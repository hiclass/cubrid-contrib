/*
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * cub_top - CUBRID memory observer (single-file C99, no external libraries)
 *
 * Portability target: source-compatible down to CentOS 6 / kernel 2.6.32.
 *   - C99 (gnu99) only; no C11 features, anonymous unions or _Static_assert.
 *   - No libdw/libelf; <elf.h> is used for its declarations only.
 *   - gettimeofday rather than clock_gettime (no -lrt on old glibc).
 *   - process_vm_readv is called through syscall() rather than the glibc
 *     wrapper, so it compiles on glibc < 2.15 and degrades to pread when the
 *     kernel does not provide it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

/* Old kernels and glibc: define the syscall numbers we need directly */
#ifndef __NR_process_vm_readv
# if defined(__x86_64__)
#  define __NR_process_vm_readv 310
# elif defined(__i386__)
#  define __NR_process_vm_readv 347
# elif defined(__aarch64__)
#  define __NR_process_vm_readv 270
# endif
#endif

#define MAXREG   4096
#define MAXPARAM  512
#define MAXSYM      32
#define MAXHIST   2048

/* ---------------------- Common utilities ---------------------- */
static double now_ms(void){
    struct timeval tv; gettimeofday(&tv,NULL);
    return tv.tv_sec*1000.0 + tv.tv_usec/1000.0;
}
static void human(double b,char *o,size_t n){
    static const char *u[]={"B","K","M","G","T","P"};
    int i=0; double v=b;
    while(v>=1024.0 && i<5){ v/=1024.0; i++; }
    if(i<2) snprintf(o,n,"%.0f%s",v,u[i]);
    else    snprintf(o,n,"%.1f%s",v,u[i]);
}
static const char *H(double b){                 /* Ring buffer, for output convenience */
    static char ring[8][32]; static int k=0;
    char *p=ring[k++ & 7]; human(b,p,32); return p;
}
/* Fixed M unit, for places like a total that must show it is measured on the same
   scale as the rows below - promoting to 1.1G stops it being added to 4.9M by eye. */
static const char *HM(double b){
    static char ring[4][24]; static int k=0;
    char *p=ring[k++ & 3];
    snprintf(p,24,"%.1fM",b/(1024.0*1024.0));
    return p;
}
/* Round up with no decimal, for narrow places where the digit count must not move
   (204.7M -> 205M).  Rounding down would understate the value. */
static const char *HC(double b){
    static char ring[4][24]; static int k=0;
    static const char *u[]={"B","K","M","G","T","P"};
    char *p=ring[k++ & 3];
    int i=0; double v=b;
    while(v>=1024.0 && i<5){ v/=1024.0; i++; }
    double c=(double)(long long)v; if(v>c) c+=1.0;
    snprintf(p,24,"%.0f%s",c,u[i]);
    return p;
}
static int read_file(const char *path,char *buf,size_t cap){
    int fd=open(path,O_RDONLY); if(fd<0) return -1;
    size_t got=0; ssize_t n;
    while(got+1<cap && (n=read(fd,buf+got,cap-1-got))>0) got+=n;
    close(fd); buf[got]=0; return (int)got;
}
static int file_exists(const char *p){ struct stat st; return stat(p,&st)==0; }
/* Delta of a cumulative unsigned counter.  One that went backwards - a server that
   exited mid-window, a device whose counters were reset - counts as no change rather
   than wrapping to ~1.8e19. */
#define UDELTA(a_,b_) ((a_)>=(b_) ? (double)((a_)-(b_)) : 0.0)

/* Value-with-unit string ("512.0M", "32768") to bytes */
static long long parse_size(const char *s){
    char *end; double v=strtod(s,&end);
    if(end==s) return -1;
    switch(*end){
        case 'K': case 'k': v*=1024.0; break;
        case 'M': case 'm': v*=1024.0*1024.0; break;
        case 'G': case 'g': v*=1024.0*1024.0*1024.0; break;
        default: break;
    }
    return (long long)v;
}

/* ---------------------- Portability layer ---------------------- */
static struct {
    int vm_readv;        /* process_vm_readv available */
    int mem_pread;       /* /proc/pid/mem pread available */
    int rollup;          /* smaps_rollup present */
    int proc_io;         /* /proc/pid/io present */
    int clear_refs;      /* clear_refs writable */
    char note[256];
} CAP;
static int g_ascii=0;   /* --ascii: English canvas labels, for terminals whose input method follows Hangul rendering */   /* Korean/English label, toggled by l or --ascii */

static int g_mem_fd = -1;   /* For the /proc/pid/mem fallback */
static int g_mem_pid = -1;  /* The pid g_mem_fd belongs to: a switch or restart must not read the old process */

static ssize_t vmread(int pid,unsigned long addr,void *buf,size_t len){
    size_t got=0;
    if(CAP.vm_readv){
#ifdef __NR_process_vm_readv
        while(got<len){
            struct iovec l,r;
            l.iov_base=(char*)buf+got; l.iov_len=len-got;
            r.iov_base=(void*)(addr+got); r.iov_len=len-got;
            long n=syscall(__NR_process_vm_readv,pid,&l,1UL,&r,1UL,0UL);
            if(n<=0){ if(got==0 && errno==ENOSYS) CAP.vm_readv=0; break; }
            got+=n;
        }
        if(got) return got;
#endif
    }
    if(CAP.mem_pread){                        /* Fallback: /proc/pid/mem */
        if(g_mem_fd>=0 && g_mem_pid!=pid){ close(g_mem_fd); g_mem_fd=-1; }
        if(g_mem_fd<0){
            char p[64]; snprintf(p,sizeof p,"/proc/%d/mem",pid);
            g_mem_fd=open(p,O_RDONLY);
            if(g_mem_fd<0){ CAP.mem_pread=0; return 0; }
            g_mem_pid=pid;
        }
        got=0;
        while(got<len){
            ssize_t n=pread(g_mem_fd,(char*)buf+got,len-got,(off_t)(addr+got));
            if(n<=0) break;
            got+=n;
        }
    }
    return got;
}
static int g_rd_ok=1;   /* Whether the last batch read succeeded, so a failed 0 is not passed off as measured */
static int32_t rd_i32(int pid,unsigned long a){
    int32_t v=0;
    if(vmread(pid,a,&v,4)==4) return v;
    g_rd_ok=0; return 0;
}

/* The one-line capability summary shown with the fallback warning */
static void cap_note(void){
    snprintf(CAP.note,sizeof CAP.note,
        "vm_readv=%s rollup=%s proc_io=%s clear_refs=%s",
        CAP.vm_readv?"yes":(g_ascii?"no(pread fallback)":"no(pread 폴백)"),
        CAP.rollup?"yes":(g_ascii?"no(smaps sum)":"no(smaps 합산)"),
        CAP.proc_io?"yes":"no", CAP.clear_refs?"yes":"no");
}
static void cap_probe(int pid){
    char p[64], buf[64];
    memset(&CAP,0,sizeof CAP);
    CAP.mem_pread=1;
#ifdef __NR_process_vm_readv
    {   /* Read one real byte to confirm kernel support */
        char t; struct iovec l,r; unsigned long self=(unsigned long)&t;
        l.iov_base=&buf[0]; l.iov_len=1; r.iov_base=(void*)self; r.iov_len=1;
        long n=syscall(__NR_process_vm_readv,getpid(),&l,1UL,&r,1UL,0UL);
        CAP.vm_readv = (n==1);
    }
#endif
    snprintf(p,sizeof p,"/proc/%d/smaps_rollup",pid); CAP.rollup=file_exists(p);
    snprintf(p,sizeof p,"/proc/%d/io",pid);           CAP.proc_io=file_exists(p);
    snprintf(p,sizeof p,"/proc/%d/clear_refs",pid);   CAP.clear_refs=file_exists(p);
    /* Old-kernel simulation switch, to exercise the fallback paths without a real
       CentOS 6 (2.6.32) machine.
       CUB_TOP_COMPAT="no-vm_readv,no-rollup,no-clear_refs,no-proc_io" */
    { const char *cm=getenv("CUB_TOP_COMPAT");
      if(cm){
          if(strstr(cm,"no-vm_readv"))   CAP.vm_readv=0;
          if(strstr(cm,"no-rollup"))     CAP.rollup=0;
          if(strstr(cm,"no-clear_refs")) CAP.clear_refs=0;
          if(strstr(cm,"no-proc_io"))    CAP.proc_io=0;
      } }
    cap_note();
}

/* Permission scope: for a target owned by another user the ptrace check blocks
 * smaps/maps/io/clear_refs (regions, PSS, I/O, hot set), while status and stat
 * stay readable (totals, CPU).  "Not permitted" is reported distinctly from
 * "measured zero" - a 0 would read as "uses no memory". */
static int  g_perm_limited=0;
static char g_perm_why[192]="";

/* ---------------------- /proc parsers ---------------------- */
typedef struct {
    unsigned long start,end;
    char perm[8];
    char path[192];
    unsigned long rss_kb,pss_kb,ref_kb;
    char is_stack;    /* Marked in the first classify_regions loop when the pthread stack pattern matches */
} region_t;

typedef struct {
    int pid;
    char db[64];
    region_t reg[MAXREG];
    int nreg;
    int reg_dropped;   /* Regions past MAXREG: not classified, so the breakdown falls short of the total */
    unsigned long rss_kb,pss_kb,swap_kb,vsize_kb;
    unsigned long minflt,majflt,blkio_ticks;
    unsigned long utime,stime;     /* Cumulative CPU in clock ticks; utilization comes from the delta */
    int nthreads;
    /* /proc/pid/io */
    unsigned long rchar,wchar_,syscr,syscw,read_b,write_b;
} proc_t;


/* Instance (database) enumeration.  /proc order is arbitrary, so instances are
   sorted by db name to keep the target stable across runs.  cub_pl is attributed
   by the db name in its cmdline; cub_master belongs to no database. */
#define MAXINST 8
typedef struct {
    int pid; char db[64];
    double pss, rss;      /* Summary, collected for every instance */
    double filemap;       /* Mapped file pages (Shared_Clean+Private_Clean): the share resident in the page cache */
    double swap;          /* Swapped out (Swap) */
    double pl_pss; int pl_n;
} inst_t;
static inst_t g_inst[MAXINST]; static int g_ninst=0, g_cur=0;
static char g_cur_db[64]="";       /* The db scan_tiers attributes PL to */

/* One smaps_rollup read per process (<1ms).
   fm_out = Shared_Clean + Private_Clean: this process's share of the page cache,
     which is not the kernel-wide "Cached" figure.
   sw_out = Swap. */
static double rollup_pss_rss(int pid,double *rss_out,double *fm_out,double *sw_out){
    char p[64],buf[4096];
    snprintf(p,sizeof p,"/proc/%d/smaps_rollup",pid);
    double pss=0,rss=0,fm=0,sw=0;
    if(read_file(p,buf,sizeof buf)>0){
        char*q; unsigned long v;
        if((q=strstr(buf,"Pss:"))&&sscanf(q+4,"%lu",&v)==1) pss=v*1024.0;
        if((q=strstr(buf,"Rss:"))&&sscanf(q+4,"%lu",&v)==1) rss=v*1024.0;
        if((q=strstr(buf,"Shared_Clean:"))&&sscanf(q+13,"%lu",&v)==1) fm+=v*1024.0;
        if((q=strstr(buf,"Private_Clean:"))&&sscanf(q+14,"%lu",&v)==1) fm+=v*1024.0;
        if((q=strstr(buf,"Swap:"))&&sscanf(q+5,"%lu",&v)==1) sw=v*1024.0;
    }
    if(rss_out) *rss_out=rss;
    if(fm_out) *fm_out=fm;
    if(sw_out) *sw_out=sw;
    return pss;
}
/* Take the second cmdline token (the db name) */
static void cmd_db(int pid,char*out,size_t n){
    char p[64],b[512]; snprintf(p,sizeof p,"/proc/%d/cmdline",pid);
    int r=read_file(p,b,sizeof b); out[0]=0;
    if(r>0){ int i=0; while(i<r&&b[i]) i++; if(++i<r) snprintf(out,n,"%s",b+i); }
}
typedef struct { double master,broker,cas,pl,server_rss; int npl,ncas;
    /* CPU per tier in cores, so "who is using CPU" reads on the same row as memory */
    double cpu_master,cpu_broker,cpu_cas,cpu_pl;
} tier_t;
static double tcpu_delta(int pid,double dt);
static tier_t *g_enum_T=NULL; static double g_enum_dt=0;   /* For the pass 2 tier aggregation */
static int enum_instances(void){
    g_ninst=0;
    DIR *d=opendir("/proc"); if(!d) return 0;
    struct dirent *e; char p[300],b[256];
    while((e=readdir(d))){
        if(e->d_name[0]<'0'||e->d_name[0]>'9') continue;
        snprintf(p,sizeof p,"/proc/%.20s/comm",e->d_name);
        if(read_file(p,b,sizeof b)<=0) continue;
        char*nl=strchr(b,'\n'); if(nl)*nl=0;
        if(strcmp(b,"cub_server")||g_ninst>=MAXINST) continue;
        inst_t*x=&g_inst[g_ninst];
        memset(x,0,sizeof *x);
        x->pid=atoi(e->d_name);
        cmd_db(x->pid,x->db,sizeof x->db);
        if(!x->db[0]) snprintf(x->db,sizeof x->db,"-");
        x->pss=rollup_pss_rss(x->pid,&x->rss,&x->filemap,&x->swap);
        g_ninst++;
    }
    rewinddir(d);
    while((e=readdir(d))){                      /* pass 2: PL attribution and tier aggregation together */
        if(e->d_name[0]<'0'||e->d_name[0]>'9') continue;
        snprintf(p,sizeof p,"/proc/%.20s/comm",e->d_name);
        if(read_file(p,b,sizeof b)<=0) continue;
        char*nl=strchr(b,'\n'); if(nl)*nl=0;
        int pid=atoi(e->d_name);
        if(!strncmp(b,"cub_pl",6)){
            char db[64]; cmd_db(pid,db,sizeof db);
            double pl=rollup_pss_rss(pid,NULL,NULL,NULL);
            for(int i=0;i<g_ninst;i++)
                if(!strcmp(g_inst[i].db,db)){ g_inst[i].pl_pss+=pl; g_inst[i].pl_n++; break; }
            /* Tier aggregation, finished in this same walk: running it separately repeats the
               whole /proc walk and the same rollups twice per frame (25ms measured).
               Only the current DB's PL is counted. */
            if(g_enum_T && (!g_cur_db[0] || !strcmp(db,g_cur_db))){
                g_enum_T->pl+=pl; g_enum_T->npl++;
                g_enum_T->cpu_pl+=tcpu_delta(pid,g_enum_dt);
            }
            continue;
        }
        if(!g_enum_T) continue;
        int is_master=!strcmp(b,"cub_master"), is_broker=!strcmp(b,"cub_broker");
        int is_cas=!strcmp(b,"cub_cas");
        if(!is_master&&!is_broker&&!is_cas) continue;
        double pss=rollup_pss_rss(pid,NULL,NULL,NULL);
        if(is_master){ g_enum_T->master+=pss; g_enum_T->cpu_master+=tcpu_delta(pid,g_enum_dt); }
        else if(is_broker){ g_enum_T->broker+=pss; g_enum_T->cpu_broker+=tcpu_delta(pid,g_enum_dt); }
        else { g_enum_T->cas+=pss; g_enum_T->ncas++; g_enum_T->cpu_cas+=tcpu_delta(pid,g_enum_dt); }
    }
    closedir(d);
    for(int i=0;i<g_ninst;i++) for(int j=i+1;j<g_ninst;j++)    /* Alphabetical by db */
        if(strcmp(g_inst[i].db,g_inst[j].db)>0){
            inst_t t=g_inst[i]; g_inst[i]=g_inst[j]; g_inst[j]=t; }
    return g_ninst;
}
static int inst_find(const char*db){
    for(int i=0;i<g_ninst;i++) if(!strcmp(g_inst[i].db,db)) return i;
    return -1;
}

/* Full smaps parse: regions plus Rss/Pss/Referenced */
static void parse_smaps(proc_t *P){
    char f[64]; snprintf(f,sizeof f,"/proc/%d/smaps",P->pid);
    FILE *fp=fopen(f,"r"); if(!fp) return;
    char line[1024]; region_t *cur=NULL;
    P->nreg=0; P->reg_dropped=0;
    while(fgets(line,sizeof line,fp)){
        /* First-character gate: a header starts with lower-case hex, an attribute line with
           an upper-case letter.  96% are attribute lines, so trying sscanf on each. */
        char c0=line[0];
        if((c0>='0'&&c0<='9')||(c0>='a'&&c0<='f')){
            unsigned long s,e; char perm[8];
            if(sscanf(line,"%lx-%lx %7s",&s,&e,perm)!=3) continue;
            if(P->nreg>=MAXREG){ P->reg_dropped++; cur=NULL; continue; }   /* counted, not hidden */
            cur=&P->reg[P->nreg++];
            memset(cur,0,sizeof *cur);
            cur->start=s; cur->end=e;
            snprintf(cur->perm,sizeof cur->perm,"%s",perm);
            /* The path starts at field 6 and may contain spaces; copied directly, no intermediate buffer */
            {   char *q=line; int fld=0; size_t n=0;
                while(*q && fld<5){ while(*q==' ')q++; while(*q && *q!=' ')q++; fld++; }
                while(*q==' ')q++;
                while(*q && *q!='\n' && n+1<sizeof cur->path) cur->path[n++]=*q++;
                cur->path[n]=0;
            }
        } else if(cur){
            if(!strncmp(line,"Rss:",4))              cur->rss_kb=strtoul(line+4,NULL,10);
            else if(!strncmp(line,"Pss:",4))         cur->pss_kb=strtoul(line+4,NULL,10);
            else if(!strncmp(line,"Referenced:",11)) cur->ref_kb=strtoul(line+11,NULL,10);
        }
    }
    fclose(fp);
}

/* Use rollup where present, otherwise sum smaps (old-kernel fallback) */
static void parse_rollup(proc_t *P){
    P->rss_kb=P->pss_kb=P->swap_kb=0;
    if(CAP.rollup){
        char f[64]; snprintf(f,sizeof f,"/proc/%d/smaps_rollup",P->pid);
        FILE *fp=fopen(f,"r");
        if(fp){
            char line[256]; unsigned long v;
            while(fgets(line,sizeof line,fp)){
                if(!strncmp(line,"Rss:",4)&&sscanf(line+4,"%lu",&v)==1) P->rss_kb=v;
                else if(!strncmp(line,"Pss:",4)&&sscanf(line+4,"%lu",&v)==1) P->pss_kb=v;
                else if(!strncmp(line,"Swap:",5)&&sscanf(line+5,"%lu",&v)==1) P->swap_kb=v;
            }
            fclose(fp);
            if(P->rss_kb) return;
        }
    }
    if(P->nreg==0){
        /* No region read at all means maps/smaps is inaccessible (owned by another user).
           VmRSS from status salvages the total; PSS is impossible without smaps. */
        char f2[64]; snprintf(f2,sizeof f2,"/proc/%d/status",P->pid);
        FILE*sf=fopen(f2,"r");
        if(sf){ char ln[256]; unsigned long v;
            while(fgets(ln,sizeof ln,sf)){
                if(!strncmp(ln,"VmRSS:",6)&&sscanf(ln+6,"%lu",&v)==1) P->rss_kb=v;
                else if(!strncmp(ln,"VmSwap:",7)&&sscanf(ln+7,"%lu",&v)==1) P->swap_kb=v;
            }
            fclose(sf); }
        if(!g_perm_limited){
            g_perm_limited=1;
            snprintf(g_perm_why,sizeof g_perm_why,
                     g_ascii?"target owned by another user: maps/smaps/io/clear_refs blocked - totals from status only (run as the server owner or root for the full view)"
                            :"대상이 다른 사용자 소유: maps/smaps/io/clear_refs 차단 - status 기반 총량만 (전체는 서버 소유자 또는 root 로 실행)");
        }
        return;
    }
    for(int i=0;i<P->nreg;i++){                 /* Fallback sum */
        P->rss_kb+=P->reg[i].rss_kb;
        P->pss_kb+=P->reg[i].pss_kb;
    }
}

static void parse_status_stat(proc_t *P){
    char f[64],buf[8192];
    snprintf(f,sizeof f,"/proc/%d/status",P->pid);
    if(read_file(f,buf,sizeof buf)>0){
        char *p=strstr(buf,"VmSize:"); unsigned long v;
        if(p && sscanf(p+7,"%lu",&v)==1) P->vsize_kb=v;
    }
    snprintf(f,sizeof f,"/proc/%d/stat",P->pid);
    if(read_file(f,buf,sizeof buf)>0){
        char *q=strrchr(buf,')');            /* Safe even when comm contains spaces or parentheses */
        if(q && *(q+1)){
            q+=2;
            unsigned long fld[64]; int n=0;
            char *tok=strtok(q," ");
            while(tok && n<64){ fld[n++]=strtoul(tok,NULL,10); tok=strtok(NULL," "); }
            /* q starts at field 3, so index i is field(i+3) */
            if(n>10) P->minflt=fld[7];        /* field10 */
            if(n>12) P->majflt=fld[9];        /* field12 */
            if(n>11) P->utime=fld[11];        /* field 14: user CPU in ticks */
            if(n>12) P->stime=fld[12];        /* field 15: kernel CPU in ticks */
            if(n>39) P->blkio_ticks=fld[39];  /* field42 */
        }
    }
    snprintf(f,sizeof f,"/proc/%d/task",P->pid);
    DIR *d=opendir(f); P->nthreads=0;
    if(d){ struct dirent *e; while((e=readdir(d))) if(e->d_name[0]>='0'&&e->d_name[0]<='9') P->nthreads++;
           closedir(d); }
}

static void parse_procio(proc_t *P){
    if(!CAP.proc_io) return;
    char f[64],buf[1024];
    snprintf(f,sizeof f,"/proc/%d/io",P->pid);
    if(read_file(f,buf,sizeof buf)<=0) return;
    char *p;
    if((p=strstr(buf,"rchar:")))       P->rchar =strtoul(p+6,NULL,10);
    if((p=strstr(buf,"wchar:")))       P->wchar_=strtoul(p+6,NULL,10);
    if((p=strstr(buf,"syscr:")))       P->syscr =strtoul(p+6,NULL,10);
    if((p=strstr(buf,"syscw:")))       P->syscw =strtoul(p+6,NULL,10);
    if((p=strstr(buf,"read_bytes:")))  P->read_b =strtoul(p+11,NULL,10);
    if((p=strstr(buf,"write_bytes:"))) P->write_b=strtoul(p+12,NULL,10);
}

/* Count cub_server threads in D state (uninterruptible sleep = storage wait).
   Field 3 of task/<tid>/stat is the state character; comm can contain spaces or
   parentheses, so parse after the last ')'.  53 threads cost 1.1ms. */
static unsigned long *g_blk_baseline_slot=NULL;   /* The frame's closing scan becomes the next baseline */
static void count_d_threads(int pid,int *d_out,int *n_out,unsigned long *blk_out){
    *d_out=0; *n_out=0; *blk_out=0;
    if(pid<0) return;
    char dp[64]; snprintf(dp,sizeof dp,"/proc/%d/task",pid);
    DIR *d=opendir(dp); if(!d) return;
    struct dirent *e; char f[400],b[512];
    while((e=readdir(d))){
        if(e->d_name[0]<'0'||e->d_name[0]>'9') continue;
        snprintf(f,sizeof f,"%s/%s/stat",dp,e->d_name);
        if(read_file(f,b,sizeof b)<=0) continue;
        char *rp=strrchr(b,')'); if(!rp||!rp[1]) continue;
        char *s=rp+1; while(*s==' ') s++;
        if(!*s) continue;
        (*n_out)++;
        if(*s=='D') (*d_out)++;
        /* Sum delayacct_blkio_ticks (field 42) over threads: the process-level value
           covers the main thread only, which is always 0 because the workers do
           the I/O.  s points at field 3, so skip 39 more. */
        { char *p=s; int k=0;
          while(*p && k<39){ while(*p && *p!=' ') p++; while(*p==' ') p++; k++; }
          if(*p) *blk_out += strtoul(p,NULL,10); }
    }
    closedir(d);
}

/* clear_refs auto-suppression.  Writing clear_refs walks the target's whole PTE
 * set, so the cost is linear in resident size (~27-31ms/GB) and the target then
 * takes a burst of minor faults.  It is off by default (--hot requests it) and
 * suppressed above a resident threshold, with the reason reported. */
static double g_hot_limit_gb=2.0;     /* Above this resident size, hot measurement disables itself */
static double g_hot_every=60.0;       /* Minimum call interval, seconds */
static char   g_hot_off_why[128]="";  /* Auto-suppression reason, shown on screen and in terse */
static double g_hot_last_ms=-1e18;    /* Time of the last call */
static double g_hot_cost_ms=-1;       /* Measured cost of the last call */
static int    g_hot_measured=0;       /* Whether hot was actually measured this run */
static void do_clear_refs(int pid){
    if(!CAP.clear_refs) return;
    /* Resident size gate: not called at all with a large buffer pool */
    { double rss_gb=0;
      char rp[64]; snprintf(rp,sizeof rp,"/proc/%d/smaps_rollup",pid);
      FILE*rf=fopen(rp,"r");
      if(rf){ char ln[256];
              while(fgets(ln,sizeof ln,rf)){
                  if(strncmp(ln,"Rss:",4)) continue;
                  unsigned long k=0;
                  if(sscanf(ln+4,"%lu",&k)==1) rss_gb=(double)k/1048576.0;
                  break;
              }
              fclose(rf); }
      /* Old kernels without smaps_rollup: fall back to VmRSS from status */
      if(rss_gb<=0){
          char sp[64]; snprintf(sp,sizeof sp,"/proc/%d/status",pid);
          FILE*sf=fopen(sp,"r");
          if(sf){ char ln[256];
                  while(fgets(ln,sizeof ln,sf)){
                      if(strncmp(ln,"VmRSS:",6)) continue;
                      unsigned long k=0;
                      if(sscanf(ln+6,"%lu",&k)==1) rss_gb=(double)k/1048576.0;
                      break;
                  }
                  fclose(sf); }
      }
      if(rss_gb>g_hot_limit_gb){
          snprintf(g_hot_off_why,sizeof g_hot_off_why,
                   g_ascii?"hot off: resident %.1fGB > %.1fGB limit (clear_refs walks every PTE; ~30ms/GB and induces minor faults on the target)"
                          :"핫 자동 해제: 상주 %.1fGB > 한도 %.1fGB (clear_refs 는 PTE 전수 순회 — 약 30ms/GB, 대상에 minor fault 유발)",
                   rss_gb,g_hot_limit_gb);
          CAP.clear_refs=0; return; }
    }
    /* Minimum interval; not called every frame */
    { double now=now_ms();
      if(now-g_hot_last_ms < g_hot_every*1000.0) return;
      g_hot_last_ms=now; }
    char f[64]; snprintf(f,sizeof f,"/proc/%d/clear_refs",pid);
    int fd=open(f,O_WRONLY); if(fd<0){ CAP.clear_refs=0; return; }
    double t0=now_ms();
    if(write(fd,"1\n",2)<0) CAP.clear_refs=0; else g_hot_measured=1;
    g_hot_cost_ms=now_ms()-t0;
    close(fd);
}

/* meminfo */
#define MAXCORE 256          /* Beyond this, only the first MAXCORE go into the distribution */
typedef struct { unsigned long total,avail,cached,swtotal,swfree;
    /* Host CPU from a /proc/stat delta; cores is the online core count.
       busy_cores is how many cores' worth is in use (0..cores), the same unit as the
       process-side figure, so the two can be compared directly. */
    double cpu_us,cpu_sy,cpu_wa,busy_cores; int cores,pcores;
    /* Per-core usage, sorted descending.  A total alone cannot tell "12 cores' worth
       spread over 12" from "6 saturated", and the latter is the bottleneck.
       The same information as btop's per-core meters, compressed into one row. */
    double core_pct[MAXCORE]; int ncore_pct, sat_cores, hi_cores;
    double load1,load5,load15;
} mem_t;
/* Per-field deltas of one /proc/stat cpu line, the way procps top takes them: a
   field that steps backwards (iowait does, proc(5)) contributes 0, and the total
   is the sum of the deltas, so the shares still add up. */
static double cpu_deltas(const unsigned long long *v,const unsigned long long *pv,int n,double *d){
    double t=0;
    for(int i=0;i<n;i++){ d[i]= v[i]>=pv[i] ? (double)(v[i]-pv[i]) : 0.0; t+=d[i]; }
    return t;
}
/* First /proc/stat line: user nice system idle iowait irq softirq steal ... */
static void parse_hostcpu(mem_t *M){
    static unsigned long long pv[10]; static int pv_ok=0;
    static char cbuf[MAXCORE*128];      /* All per-core lines, about 6KB at 64 cores */
    char buf[512];
    M->cores = (int)sysconf(_SC_NPROCESSORS_ONLN); if(M->cores<1) M->cores=1;
    /* Physical cores = sockets x cores per socket; differing from logical means SMT.
       cpuinfo has one block per core and is large, but socket ids are few, so they
       are counted in a bitmask. */
    if(M->pcores<=0){                       /* Does not change while booted; read once */
        static int cached=0;
        if(!cached){
            FILE *f=fopen("/proc/cpuinfo","r");
            if(f){ char ln[256]; unsigned long long sock=0; int percore=0;
                   while(fgets(ln,sizeof ln,f)){
                       int id;
                       if(sscanf(ln,"physical id : %d",&id)==1 && id>=0 && id<64) sock|=1ULL<<id;
                       else if(percore<=0) sscanf(ln,"cpu cores : %d",&percore);
                   }
                   fclose(f);
                   int ns=0; for(int i=0;i<64;i++) if(sock&(1ULL<<i)) ns++;
                   if(ns>0&&percore>0) cached=ns*percore;
            }
            if(cached<=0) cached=M->cores;   /* Unknown: same as logical */
        }
        M->pcores=cached;
    }
    if(read_file("/proc/stat",buf,sizeof buf)>0){
        unsigned long long v[10]={0};
        char *p=strchr(buf,' ');
        if(p){ int n=0; char *tok=strtok(p," \n");
               while(tok&&n<10){ v[n++]=strtoull(tok,NULL,10); tok=strtok(NULL," \n"); }
               double d[10];
               double dt = pv_ok ? cpu_deltas(v,pv,10,d) : 0;
               if(dt>0){
                   M->cpu_us=(d[0]+d[1])/dt*100.0;
                   M->cpu_sy=(d[2]+d[5]+d[6])/dt*100.0;
                   M->cpu_wa=d[4]/dt*100.0;
                   M->busy_cores=(1.0-(d[3]+d[4])/dt)*M->cores;
                   if(M->busy_cores<0) M->busy_cores=0;
               }
               memcpy(pv,v,sizeof pv); pv_ok=1; }
    }
    /* Per-core usage from the cpuN lines of /proc/stat.  The buffer read above holds
       only the first line, so read again with room here (about 6KB for 64 cores).
       Measured 0.27ms, 2% of the frame's work. */
    { static unsigned long long pc[MAXCORE][10];   /* the previous cpuN fields */
      static int primed=0;
      char *big=(char*)cbuf; size_t bn=sizeof cbuf;
      M->ncore_pct=0; M->sat_cores=0; M->hi_cores=0;
      if(read_file("/proc/stat",big,bn)>0){
          char *p=big; int idx=0;
          while((p=strstr(p,"cpu"))){
              p+=3;
              if(*p<'0'||*p>'9'){ continue; }          /* Skip the aggregate "cpu " line */
              int cid=(int)strtol(p,&p,10);
              if(cid<0||cid>=MAXCORE) continue;
              unsigned long long v[10]={0}; int n=0;
              while(n<10){
                  char *e; unsigned long long x=strtoull(p,&e,10);
                  if(e==p) break;
                  v[n++]=x; p=e;
              }
              if(n<5) continue;
              double d[10]={0};
              double dt = primed ? cpu_deltas(v,pc[cid],n,d) : 0;
              if(dt>0){
                  double b=(1.0-(d[3]+d[4])/dt)*100.0;
                  if(b<0) b=0;
                  if(b>100) b=100;
                  if(idx<MAXCORE) M->core_pct[idx++]=b;
                  if(b>=95.0) M->sat_cores++;
                  else if(b>=50.0) M->hi_cores++;
              }
              memcpy(pc[cid],v,sizeof pc[cid]);
          }
          M->ncore_pct=idx; primed=1;
          /* Sort descending, so the bars run busiest-first and the saturated width is visible.
             Few enough cores that insertion sort suffices (microseconds at 64). */
          for(int i=1;i<M->ncore_pct;i++){ double k=M->core_pct[i]; int j=i-1;
              while(j>=0 && M->core_pct[j]<k){ M->core_pct[j+1]=M->core_pct[j]; j--; }
              M->core_pct[j+1]=k; }
      } }
    if(read_file("/proc/loadavg",buf,sizeof buf)>0)
        sscanf(buf,"%lf %lf %lf",&M->load1,&M->load5,&M->load15);
}
static void parse_meminfo(mem_t *M){
    char buf[8192]; memset(M,0,sizeof *M);
    if(read_file("/proc/meminfo",buf,sizeof buf)<=0) return;
    char *p;
    if((p=strstr(buf,"MemTotal:")))     M->total  =strtoul(p+9,NULL,10);
    if((p=strstr(buf,"MemAvailable:"))) M->avail  =strtoul(p+13,NULL,10);
    else if((p=strstr(buf,"MemFree:"))) M->avail  =strtoul(p+8,NULL,10); /* Old-kernel fallback */
    if((p=strstr(buf,"Cached:")))       M->cached =strtoul(p+7,NULL,10);
    if((p=strstr(buf,"SwapTotal:")))    M->swtotal=strtoul(p+10,NULL,10);
    if((p=strstr(buf,"SwapFree:")))     M->swfree =strtoul(p+9,NULL,10);
}

/* diskstats: whole devices present in /sys/block only */
/* diskstats fields (1-based): 1 rd_ios 2 rd_merges 3 rd_sect 4 rd_ticks
   5 wr_ios 6 wr_merges 7 wr_sect 8 wr_ticks 9 in_flight 10 io_ticks 11 time_in_queue.
   await = (rd_ticks+wr_ticks)/(rd_ios+wr_ios), the same definition as iostat's. */
typedef struct { char name[32]; unsigned long rsect,wsect,io_ms;
                 unsigned long rios,wios,rticks,wticks,inflight,tiq; } dstat_t;
#define MAXDISK 256
static int g_disk_dropped=0;   /* Whole devices past MAXDISK, left out of the busiest-device pick */
static int parse_diskstats(dstat_t *out,int max){
    FILE *fp=fopen("/proc/diskstats","r"); if(!fp) return 0;
    char line[512]; int n=0, dropped=0;
    while(fgets(line,sizeof line,fp)){
        char nm[32]; unsigned long f[16];
        int k=sscanf(line,"%*u %*u %31s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                     nm,&f[0],&f[1],&f[2],&f[3],&f[4],&f[5],&f[6],&f[7],&f[8],&f[9],&f[10]);
        if(k<11) continue;
        if(k<12) f[10]=0;
        char sp[96]; snprintf(sp,sizeof sp,"/sys/block/%.31s",nm);
        if(!file_exists(sp)) continue;         /* Partitions excluded */
        if(n>=max){ dropped++; continue; }
        snprintf(out[n].name,sizeof out[n].name,"%s",nm);
        out[n].rsect=f[2]; out[n].wsect=f[6]; out[n].io_ms=f[9];
        out[n].rios=f[0]; out[n].wios=f[4]; out[n].rticks=f[3]; out[n].wticks=f[7];
        out[n].inflight=f[8]; out[n].tiq=f[10];
        n++;
    }
    fclose(fp); g_disk_dropped=dropped; return n;
}

/* ────────────────────── paramdump ────────────────────── */
typedef struct { char name[64]; long long val; } param_t;
static param_t g_prm[MAXPARAM]; static int g_nprm=0;
static long long g_pagesize=0;

static long long prm_get(const char *n){
    for(int i=0;i<g_nprm;i++) if(!strcmp(g_prm[i].name,n)) return g_prm[i].val;
    return -1;
}
/* Server-down fallback: parse $CUBRID/conf/cubrid.conf directly (settings only).
   The engine reads [common] and then the database's own [@db]
   section, which wins; any other section ([@otherdb], [service]) does not apply.
   As in the engine, blanks inside the brackets are ignored, [common] matches in
   any case and [@db] matches the name exactly.  db may be NULL or "-" when the
   target is unknown: [common] only. */
static int params_from_conf(const char *cubrid,const char *db){
    char path[512]; snprintf(path,sizeof path,"%s/conf/cubrid.conf",cubrid?cubrid:"");
    FILE *fp=fopen(path,"r"); if(!fp) return 0;
    const char *want = (db && db[0] && strcmp(db,"-")) ? db : NULL;
    char line[512];
    int sect=0;                     /* 0 other, 1 [common], 2 [@db] */
    for(int pass=1;pass<=2;pass++){ /* [common] first, then [@db] over it */
        rewind(fp); sect=0;
        while(fgets(line,sizeof line,fp)){
            char *h=strchr(line,'#'); if(h) *h=0;
            char *p=line; while(*p==' '||*p=='\t') p++;
            if(*p=='['){
                char *e=p+strlen(p);
                while(e>p && isspace((unsigned char)e[-1])) e--;
                sect=0;
                if(e-p>=2 && e[-1]==']'){
                    char *a=p+1, *z=e-1;
                    while(a<z && isspace((unsigned char)*a)) a++;
                    while(z>a && isspace((unsigned char)z[-1])) z--;
                    if(*a=='@'){
                        char *n=a+1; while(n<z && isspace((unsigned char)*n)) n++;
                        if(want && (size_t)(z-n)==strlen(want) && !strncmp(n,want,(size_t)(z-n))) sect=2;
                    } else if(z-a==6 && !strncasecmp(a,"common",6)) sect=1;
                }
                continue;
            }
            if(sect!=pass||*p==0||*p=='\n') continue;
            char *eq=strchr(p,'='); if(!eq) continue;
            *eq=0;
            char nm[64]; int n=0;
            for(char *q=p; *q && n<63; q++)
                if(*q!=' '&&*q!='\t'&&*q!='\n'&&*q!='\r') nm[n++]=(char)tolower((unsigned char)*q);
            nm[n]=0;
            char *v=eq+1; while(*v==' '||*v=='\t') v++;
            long long val=parse_size(v);
            if(val<0||!nm[0]) continue;
            /* A repeated name overwrites, so terse has no duplicate keys and [@db] wins */
            { int hit=-1;
              for(int i=0;i<g_nprm;i++) if(!strcmp(g_prm[i].name,nm)){ hit=i; break; }
              if(hit>=0) g_prm[hit].val=val;
              else if(g_nprm<MAXPARAM){
                  snprintf(g_prm[g_nprm].name,sizeof g_prm[g_nprm].name,"%s",nm);
                  g_prm[g_nprm].val=val; g_nprm++;
              } }
        }
    }
    fclose(fp);
    long long dbs=prm_get("data_buffer_size"), dbp=prm_get("data_buffer_pages");
    if(dbs>0&&dbp>0) g_pagesize=dbs/dbp;
    if(g_pagesize<=0) g_pagesize=16384;
    return g_nprm;
}

/* The paramdump child (~0.3s) dominates wall clock, so its start and harvest
   straddle the sample window.  No shell: the db name comes from cmdline and
   CUBRID accepts quotes, ; , $ and backticks in it, so execvp passes arguments
   as an array rather than composing a command string. */
static pid_t g_pd_pid=-1;          /* The child to reap or time out */
static char  g_pd_why[128]="";     /* Why the child could not start, kept for the screen and terse */

/* Room for one more process?  In a container the cgroup pids limit binds before
   ulimit -u, and at the limit the casualty is whatever else shares the
   container.  Stand down when the margin is thin; if the limit cannot be read,
   do not interfere. */
#define PD_PIDS_MARGIN 64
static int pids_headroom_ok(char *why,size_t wn){
    static const char *MAXP[]={"/sys/fs/cgroup/pids/pids.max","/sys/fs/cgroup/pids.max"};
    static const char *CURP[]={"/sys/fs/cgroup/pids/pids.current","/sys/fs/cgroup/pids.current"};
    char b[64];
    for(int i=0;i<2;i++){
        if(read_file(MAXP[i],b,sizeof b)<=0) continue;
        if(!strncmp(b,"max",3)) return 1;                  /* Unlimited */
        long mx=strtol(b,NULL,10); if(mx<=0) continue;
        if(read_file(CURP[i],b,sizeof b)<=0) continue;
        long cu=strtol(b,NULL,10); if(cu<0) continue;
        if(mx-cu < PD_PIDS_MARGIN){
            snprintf(why,wn,"pids 여유 %ld (한도 %ld) — paramdump 생략",mx-cu,mx);
            return 0;
        }
        return 1;
    }
    return 1;                                              /* Unknown: proceed */
}

static FILE *paramdump_start(const char *cubrid,const char *db){
    char bin[512], envc[512], envl[1024];
    g_pd_why[0]=0;
    if(!pids_headroom_ok(g_pd_why,sizeof g_pd_why)) return NULL;
    snprintf(bin,sizeof bin,"%s/bin/cubrid",cubrid);
    snprintf(envc,sizeof envc,"CUBRID=%s",cubrid);
    { const char *old=getenv("LD_LIBRARY_PATH");
      snprintf(envl,sizeof envl,"LD_LIBRARY_PATH=%s/lib%s%s",
               cubrid, (old&&*old)?":":"", (old&&*old)?old:""); }
    int fd[2];
    if(pipe(fd)<0){ snprintf(g_pd_why,sizeof g_pd_why,"pipe 실패: %s",strerror(errno));
                    return NULL; }
    pid_t pid=fork();
    if(pid<0){ /* EAGAIN means the host process limit; passing over it silently would leave no way
                  to tell why the parameters are empty. */
               snprintf(g_pd_why,sizeof g_pd_why,"fork 실패: %s",strerror(errno));
               close(fd[0]); close(fd[1]); return NULL; }
    if(pid==0){                                  /* Child */
        setpgid(0,0);                            /* Own pgid, so killing takes the grandchildren too */
        close(fd[0]);
        dup2(fd[1],STDOUT_FILENO); close(fd[1]);
        int n=open("/dev/null",O_WRONLY);        /* Stands in for 2>/dev/null */
        if(n>=0){ dup2(n,STDERR_FILENO); close(n); }
        putenv(envc); putenv(envl);
        char *av[4]; av[0]=bin; av[1]=(char*)"paramdump"; av[2]=(char*)db; av[3]=NULL;
        execvp(bin,av);
        _exit(127);                              /* exec failed; the parent sees empty output */
    }
    close(fd[1]);
    setpgid(pid,pid);                            /* Race guard: the parent sets it once too */
    g_pd_pid=pid;
    return fdopen(fd[0],"r");
}
/* Reap the child, waiting up to PD_WAIT_MS for a clean exit, then TERM and KILL.
   Polling with waitpid(WNOHANG) avoids a signal handler or alarm. */
#define PD_WAIT_MS 5000
static void paramdump_reap(void){
    if(g_pd_pid<0) return;
    int st;
    for(int i=0;i<PD_WAIT_MS/10;i++){
        pid_t r=waitpid(g_pd_pid,&st,WNOHANG);
        if(r==g_pd_pid || (r<0 && errno==ECHILD)){ g_pd_pid=-1; return; }
        usleep(10000);
    }
    kill(-g_pd_pid,SIGTERM);         /* The whole group, so a grandchild left behind is still reaped */
    for(int i=0;i<50;i++){
        if(waitpid(g_pd_pid,&st,WNOHANG)==g_pd_pid){ g_pd_pid=-1; return; }
        usleep(10000);
    }
    kill(-g_pd_pid,SIGKILL);
    waitpid(g_pd_pid,&st,0);
    g_pd_pid=-1;
}
static int paramdump_finish(FILE *fp){
    if(!fp){ if(g_pagesize<=0) g_pagesize=16384; return -1; }
    /* Keep the pipe non-blocking and read only until the deadline, so a hung paramdump
       cannot stall cub_top on its first frame. */
    int fd=fileno(fp);
    int fl=fcntl(fd,F_GETFL,0);
    if(fl>=0) fcntl(fd,F_SETFL,fl|O_NONBLOCK);
    double deadline=now_ms()+PD_WAIT_MS;

    char buf[65536]; size_t used=0; int timed_out=0;
    while(used+1<sizeof buf){
        ssize_t n=read(fd,buf+used,sizeof buf-1-used);
        if(n>0){ used+=(size_t)n; continue; }
        if(n==0) break;                                   /* EOF: the child exited */
        if(errno==EAGAIN||errno==EWOULDBLOCK){
            if(now_ms()>=deadline){ timed_out=1; break; }
            usleep(5000); continue;
        }
        if(errno==EINTR) continue;
        break;
    }
    buf[used]=0;
    fclose(fp);
    if(timed_out && g_pd_pid>0){ kill(-g_pd_pid,SIGKILL); }
    paramdump_reap();

    long long dbp=-1;
    for(char *line=buf,*nl; *line; line=nl){
        nl=strchr(line,'\n');
        if(nl) *nl++=0; else nl=line+strlen(line);
        /* 11.x prints "[S ] name=value", 10.x prints "name=value" - accept both.
           Requiring ']' discarded all 10.0 output, leaving zero parameters.  The
           prefix test only looks for ']' within the first 8 columns, so a ']'
           inside a value is not mistaken for one. */
        char *b=line;
        { char *br=strchr(line,']');
          if(br && br-line<8){ b=br+1; while(*b==' ')b++; } }
        char *eq=strchr(b,'='); if(!eq) continue;
        *eq=0;
        char nm[64]; snprintf(nm,sizeof nm,"%s",b);
        char *v=eq+1; while(*v==' ')v++;
        long long val=parse_size(v);
        if(val<0) continue;
        if(g_nprm<MAXPARAM){
            snprintf(g_prm[g_nprm].name,sizeof g_prm[g_nprm].name,"%s",nm);
            g_prm[g_nprm].val=val; g_nprm++;
        }
        if(!strcmp(nm,"data_buffer_pages")) dbp=val;
    }
    long long dbs=prm_get("data_buffer_size");
    if(dbs>0 && dbp>0) g_pagesize=dbs/dbp;
    if(g_pagesize<=0) g_pagesize=16384;
    return g_nprm;
}

/* ---------------- Direct ELF .symtab parsing (no nm) ---------------- */
/* ======= Builtin DWARF engine (derived from tools/dwoff.c; keep both in sync) ======= */

/* dw_now_ms timed the standalone version; unused here */
/* The .so comes from whatever process is named cub_server, so its DWARF is not
   trusted any more than its symbol table (see sym_lookup).  Every read is bounded
   by the end of the area being parsed: DW_END is set by the caller, and a read that
   would cross it stops and raises DW_BAD, which the caller turns into a failure. */
static const unsigned char *DW_END; static int DW_BAD;
static uint64_t uleb(const unsigned char**p){ uint64_t r=0; int s=0; unsigned char b;
    do{ if(*p>=DW_END){ DW_BAD=1; return r; }
        b=*(*p)++; if(s<64) r|=(uint64_t)(b&0x7f)<<s; s+=7; }while(b&0x80); return r; }
static int64_t sleb(const unsigned char**p){ int64_t r=0; int s=0; unsigned char b;
    do{ if(*p>=DW_END){ DW_BAD=1; return r; }
        b=*(*p)++; if(s<64) r|=(int64_t)(b&0x7f)<<s; s+=7; }while(b&0x80);
    if(s<64&&(b&0x40)) r|=-((int64_t)1<<s);
    return r; }

/* DWARF tags and attributes */
#define T_struct   0x13
#define T_class    0x02
#define T_union    0x17
#define T_member   0x0d
#define T_var      0x34
#define T_typedef  0x16
#define T_const    0x26
#define T_volatile 0x35
#define T_base     0x24
#define A_name     0x03
#define A_bsize    0x0b
#define A_dml      0x38
#define A_type     0x49
#define A_loc      0x02
#define A_decl     0x3c
#define A_lnk      0x6e
#define A_mips_lnk 0x2007

static const unsigned char *DI,*AB,*DS; static size_t DI_SZ, AB_SZ, DS_SZ;

typedef struct { unsigned long start,first,end; uint64_t ab_off; int ver,addr_sz,off_sz; } cu_t;
static cu_t *CU; static int NCU;

typedef struct { uint64_t at,form; int64_t ic; } af_t;
typedef struct { uint64_t code,tag; int kids,n; af_t a[160]; } ab_t;
/* TAB is ab_t (about 3.9KB) x 32768 = 126MB; reserving that in BSS for a one-time
   extraction would inflate the static binary's core dumps and potential RSS.  The
   extraction path already mallocs, so borrow this from the heap and return it. */
#define NTAB_MAX 32768
static ab_t *TAB=NULL; static int NTAB;
static long G_desync=0;

static int load_abbrev(uint64_t off){
    NTAB=0;
    if(off>=AB_SZ) return -1;
    const unsigned char*a=AB+off;
    DW_END=AB+AB_SZ; DW_BAD=0;
    while(NTAB<NTAB_MAX){
        uint64_t code=uleb(&a); if(DW_BAD) return -1; if(!code) return 0;
        ab_t*t=&TAB[NTAB++]; t->code=code; t->tag=uleb(&a);
        if(DW_BAD||a>=DW_END) return -1;
        t->kids=*a++; t->n=0;
        for(;;){
            uint64_t at=uleb(&a), fm=uleb(&a); int64_t ic=0;
            if(fm==0x21) ic=sleb(&a);
            if(DW_BAD) return -1;
            if(!at&&!fm) break;
            if(t->n>=160) return -1;
            t->a[t->n].at=at; t->a[t->n].form=fm; t->a[t->n].ic=ic; t->n++;
        }
    }
    return -1;
}
static ab_t* ab_find(uint64_t code){
    for(int i=0;i<NTAB;i++) if(TAB[i].code==code) return &TAB[i];
    return NULL;
}
/* Consume one form; an unknown form fails at once rather than desyncing silently */
static int form_skip(const unsigned char**p,const unsigned char*lim,uint64_t form,int64_t ic,
                     const cu_t*cu,uint64_t*uv,const char**sv){
    const unsigned char*q=*p; uint64_t v=0;
    if(q>=lim&&form!=0x19&&form!=0x21) return -1;
    /* Room for n more bytes, checked before they are read */
    #define NEED(n_) do{ if((size_t)(lim-q)<(size_t)(n_)) return -1; }while(0)
    #define BLOCK(n_) do{ uint64_t bn_=(n_); if(DW_BAD||bn_>(uint64_t)(lim-q)) return -1; \
        if(bn_&&*q==0x23){ const unsigned char*r=q+1; v=uleb(&r); } q+=bn_; }while(0)
    switch(form){
    case 0x01: NEED(cu->addr_sz); v=cu->addr_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->addr_sz; break;
    case 0x03: NEED(2); v=*(uint16_t*)q; q+=2; NEED(v); q+=v; break;     /* block2: value = length */
    case 0x04: NEED(4); v=*(uint32_t*)q; q+=4; NEED(v); q+=v; break;     /* block4: value = length */
    case 0x05: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x06: NEED(4); v=*(uint32_t*)q; q+=4; break;
    case 0x07: NEED(8); v=*(uint64_t*)q; q+=8; break;
    case 0x08: { const unsigned char*z=memchr(q,0,(size_t)(lim-q));
                 if(!z) return -1;
                 if(sv) *sv=(const char*)q;
                 q=z+1; } break;
    case 0x09: { uint64_t n=uleb(&q); BLOCK(n); } break;
    case 0x0a: { NEED(1); uint64_t n=*q++; BLOCK(n); } break;
    case 0x0b: NEED(1); v=*q++; break;
    case 0x0c: NEED(1); v=*q++; break;
    case 0x0d: v=(uint64_t)sleb(&q); break;
    case 0x0e: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz;
               if(sv&&DS){ if(v>=DS_SZ||!memchr(DS+v,0,DS_SZ-(size_t)v)) return -1;
                           *sv=(const char*)(DS+v); }
               break;
    case 0x0f: v=uleb(&q); break;
    case 0x10: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break; /* Section-relative */
    case 0x11: NEED(1); v=cu->start+*q; q+=1; break;                                      /* CU-relative from here */
    case 0x12: NEED(2); v=cu->start+*(uint16_t*)q; q+=2; break;
    case 0x13: NEED(4); v=cu->start+*(uint32_t*)q; q+=4; break;
    case 0x14: NEED(8); v=cu->start+*(uint64_t*)q; q+=8; break;
    case 0x15: { uint64_t r=uleb(&q); v=cu->start+r; } break;
    case 0x16: { uint64_t f2=uleb(&q); if(DW_BAD||f2==0x16) return -1;   /* indirect to indirect: no end */
                 *p=q; return form_skip(p,lim,f2,0,cu,uv,sv); }
    case 0x17: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break;
    case 0x18: { uint64_t n=uleb(&q); BLOCK(n); } break;
    case 0x19: v=1; break;
    case 0x1a: v=uleb(&q); break;
    case 0x1b: v=uleb(&q); break;
    case 0x1c: NEED(4); q+=4; break;
    case 0x1d: NEED(cu->off_sz); q+=cu->off_sz; break;
    case 0x1e: NEED(16); q+=16; break;
    case 0x1f: NEED(cu->off_sz); v=cu->off_sz==8?*(uint64_t*)q:*(uint32_t*)q; q+=cu->off_sz; break;
    case 0x20: NEED(8); q+=8; break;
    case 0x21: v=(uint64_t)ic; break;
    case 0x22: v=uleb(&q); break;
    case 0x23: v=uleb(&q); break;
    case 0x24: NEED(8); q+=8; break;
    case 0x25: NEED(1); v=*q++; break;
    case 0x26: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x27: NEED(3); v=(uint64_t)q[0]|((uint64_t)q[1]<<8)|((uint64_t)q[2]<<16); q+=3; break;
    case 0x28: NEED(4); v=*(uint32_t*)q; q+=4; break;
    case 0x29: NEED(1); v=*q++; break;
    case 0x2a: NEED(2); v=*(uint16_t*)q; q+=2; break;
    case 0x2b: NEED(3); v=(uint64_t)q[0]|((uint64_t)q[1]<<8)|((uint64_t)q[2]<<16); q+=3; break;
    case 0x2c: NEED(4); v=*(uint32_t*)q; q+=4; break;
    default: return -1;
    }
    #undef NEED
    #undef BLOCK
    if(DW_BAD||q>lim) return -1;
    if(uv)*uv=v;
    *p=q;
    return 0;
}
static int build_index(void){
    int cap=4096; CU=malloc(sizeof(cu_t)*cap); NCU=0;
    if(!CU) return 0;
    const unsigned char*p=DI,*end=DI+DI_SZ;
    while(p+11<end){
        unsigned long cs=(unsigned long)(p-DI);
        uint32_t len=*(uint32_t*)p; p+=4; int osz=4;
        if(len==0xffffffff){ len=(uint32_t)*(uint64_t*)p; p+=8; osz=8; }
        if(len>(uint64_t)(end-p)) break;
        const unsigned char*ce=p+len;
        if((size_t)(ce-p) < (size_t)(2+2+osz)) break;    /* version, unit type/addr size, abbrev offset */
        uint16_t ver=*(uint16_t*)p; p+=2;
        uint64_t ao; int asz;
        if(ver>=5){ p++; asz=*p++; ao=osz==8?*(uint64_t*)p:*(uint32_t*)p; p+=osz; }
        else { ao=osz==8?*(uint64_t*)p:*(uint32_t*)p; p+=osz; asz=*p++; }
        if(asz!=4 && asz!=8) break;                        /* form_skip reads addresses of this size */
        if(NCU==cap){ cu_t *t=realloc(CU,sizeof(cu_t)*(size_t)cap*2);
                      if(!t) break;           /* keep what is indexed so far */
                      CU=t; cap*=2; }
        cu_t*c=&CU[NCU++];
        c->start=cs; c->first=(unsigned long)(p-DI); c->end=(unsigned long)(ce-DI);
        c->ab_off=ao; c->ver=ver; c->addr_sz=asz; c->off_sz=osz;
        p=ce;
    }
    return NCU;
}
static cu_t* cu_of(unsigned long off){
    int lo=0,hi=NCU-1;
    while(lo<=hi){ int m=(lo+hi)/2;
        if(off<CU[m].start) hi=m-1; else if(off>=CU[m].end) lo=m+1; else return &CU[m]; }
    return NULL;
}

/* ---- One DIE ---- */
typedef struct {
    int tag; const char*name; long bsize; unsigned long type; int is_decl; int has_loc;
    int nmem; char mname[128][80]; long moff[128]; unsigned long mtype[128];
} die_t;

/* Walk the CUs to either collect variables matching a name or resolve a DIE at an offset */
typedef struct {
    const char*var;                 /* Variable name to find (mangled or source form) */
    unsigned long cand[32]; int ncand, cand_def[32];
    unsigned long want;             /* DIE offset to resolve */
    die_t out; int found;
    const char*type_name;           /* Find a struct by name */
} job_t;

static int scan_cu(cu_t*cu, job_t*J){
    if(load_abbrev(cu->ab_off)<0){ G_desync++; return -1; }
    const unsigned char*p=DI+cu->first, *lim=DI+cu->end;
    DW_END=lim; DW_BAD=0;
    int depth=0, in=-1;
    while(p<lim){
        unsigned long doff=(unsigned long)(p-DI);
        uint64_t code=uleb(&p);
        if(DW_BAD){ G_desync++; return -1; }
        if(!code){ if(depth>0)depth--; if(in>=0&&depth<=in) in=-1; continue; }
        ab_t*t=ab_find(code); if(!t){ G_desync++; return -1; }
        const char*nm=NULL,*lnk=NULL; uint64_t bsz=0,ty=(uint64_t)-1,dml=(uint64_t)-1;
        int is_decl=0,has_loc=0,have_dml=0;
        for(int i=0;i<t->n;i++){
            uint64_t v=0; const char*sv=NULL;
            if(form_skip(&p,lim,t->a[i].form,t->a[i].ic,cu,&v,&sv)<0){ G_desync++; return -1; }
            switch(t->a[i].at){
            case A_name: nm=sv; break;
            case A_lnk: case A_mips_lnk: lnk=sv; break;
            case A_bsize: bsz=v; break;
            case A_type: ty=v; break;
            case A_dml: dml=v; have_dml=1; break;
            case A_loc: has_loc=1; break;
            case A_decl: is_decl=(v!=0); break;
            default: break; }
        }
        if(J->var && t->tag==T_var && ty!=(uint64_t)-1 &&
           ((nm&&!strcmp(nm,J->var))||(lnk&&!strcmp(lnk,J->var)))){
            if(J->ncand<32){ J->cand[J->ncand]=(unsigned long)ty;
                             J->cand_def[J->ncand]=(has_loc&&!is_decl); J->ncand++; }
        }
        if(J->type_name && (t->tag==T_struct||t->tag==T_class||t->tag==T_union) &&
           nm && !strcmp(nm,J->type_name) && bsz>0 && !J->found){
            J->out.tag=t->tag; J->out.name=nm; J->out.bsize=(long)bsz; J->found=1;
            J->out.type=(unsigned long)-1; J->out.nmem=0; in=depth;
        }
        if(J->want && doff==J->want){
            J->out.tag=(int)t->tag; J->out.name=nm; J->out.bsize=(long)bsz;
            J->out.type=(unsigned long)ty; J->out.is_decl=is_decl; J->out.has_loc=has_loc;
            J->out.nmem=0; J->found=1; in=depth;
        }
        else if(in>=0 && t->tag==T_member && depth==in+1 && nm && have_dml && J->out.nmem<128){
            int k=J->out.nmem;
            snprintf(J->out.mname[k],80,"%s",nm);
            J->out.moff[k]=(long)dml; J->out.mtype[k]=(unsigned long)ty; J->out.nmem++;
        }
        if(t->kids) depth++;
    }
    if(p!=lim){ G_desync++; return -1; }      /* L1: must land exactly to be trusted */
    return 0;
}

/* Resolve the type chain through typedef/const/volatile to a struct or base DIE */
static int resolve_type(unsigned long off, die_t*out){
    for(int g=0; g<16; g++){
        cu_t*c=cu_of(off); if(!c) return -1;
        job_t J; memset(&J,0,sizeof J); J.want=off;
        if(scan_cu(c,&J)<0) return -1;
        if(!J.found) return -1;
        if(J.out.tag==T_struct||J.out.tag==T_class||J.out.tag==T_union||J.out.tag==T_base){
            *out=J.out; return 0;
        }
        if(J.out.type==(unsigned long)-1) return -1;
        off=J.out.type;
    }
    return -1;
}
/* L2 value check */
static int sane_struct(const die_t*d){
    if(d->bsize<=0||d->bsize>(1<<20)) return 0;
    for(int i=0;i<d->nmem;i++)
        if(d->moff[i]<0||d->moff[i]>=d->bsize) return 0;
    return 1;
}
/* Symbol to type DIE; with several candidates, prefer the definition and discard those that fail */
static int resolve_symbol(const char*sym, die_t*out, unsigned long*type_off){
    job_t J; memset(&J,0,sizeof J); J.var=sym;
    for(int c=0;c<NCU;c++){ scan_cu(&CU[c],&J); }        /* Collect candidates from every CU */
    if(!J.ncand) return -1;
    /* Do not adopt when several DIEs share the name: a DIE picked by name alone is
       not guaranteed to be the symtab symbol (static variables repeat per CU).
       An ambiguous key is left empty; the runtime gate decides in any case. */
    if(J.ncand>1){
        /* Exception 1: an extern global declared in a header has many candidates but one
           definition (has a location, not a declaration) - use it if unique.
           Exception 2: if every candidate resolves to the same (type, sizeof) there
           is only one entity, so take the first.  Otherwise still refuse. */
        int ndef=0, di=-1;
        for(int i=0;i<J.ncand;i++) if(J.cand_def[i]){ ndef++; di=i; }
        if(ndef==1){ J.cand[0]=J.cand[di]; J.cand_def[0]=1; J.ncand=1; }
        else {
            die_t d0; int same=1;
            if(resolve_type(J.cand[0],&d0)<0) return -1;
            for(int i=1;i<J.ncand && same;i++){
                die_t di2;
                if(resolve_type(J.cand[i],&di2)<0 || di2.bsize!=d0.bsize ||
                   !d0.name || !di2.name || strcmp(d0.name,di2.name)) same=0;
            }
            if(!same) return -1;
            J.ncand=1;
        }
    }
    for(int pass=0;pass<2;pass++){                        /* Definitions first, then the rest */
        for(int i=0;i<J.ncand;i++){
            if(pass==0 && !J.cand_def[i]) continue;
            if(pass==1 &&  J.cand_def[i]) continue;
            die_t d;
            if(resolve_type(J.cand[i],&d)<0) continue;     /* Discard invalid candidates */
            if(d.tag!=T_base && !sane_struct(&d)) continue;
            *out=d; if(type_off)*type_off=J.cand[i]; return 0;
        }
    }
    return -1;
}
/* Search member names recursively, nested included, accumulating the offset */
static int find_member(const die_t*d,const char*want,long base,int depth,long*res,char*path,size_t plen){
    if(depth>4) return -1;
    for(int i=0;i<d->nmem;i++){
        if(!strcmp(d->mname[i],want)){ *res=base+d->moff[i];
            snprintf(path,plen,"%s",d->mname[i]); return 0; }
    }
    for(int i=0;i<d->nmem;i++){
        if(d->mtype[i]==(unsigned long)-1) continue;
        die_t sub;
        if(resolve_type(d->mtype[i],&sub)<0) continue;
        if(sub.tag==T_base) continue;
        if(!sane_struct(&sub)) continue;
        char p2[160];
        if(find_member(&sub,want,base+d->moff[i],depth+1,res,p2,sizeof p2)==0){
            snprintf(path,plen,"%.80s.%.160s",d->mname[i],p2); return 0;
        }
    }
    return -1;
}
/* sizeof a struct by name */
static int sizeof_type(const char*name,long*sz,const char**real){
    job_t J; memset(&J,0,sizeof J); J.type_name=name;
    for(int c=0;c<NCU;c++){
        memset(&J.out,0,sizeof J.out); J.found=0; J.type_name=name;
        if(scan_cu(&CU[c],&J)<0) continue;
        if(J.found && J.out.bsize>0 && J.out.bsize<=(1<<20)){
            *sz=J.out.bsize; if(real)*real=name; return 0; }
    }
    return -1;
}

/* Member offset within a struct named directly (nested included), for types with no
   variable root - a BCB array element beyond a pointer.  Definitions only (bsize>0). */
static int member_of_type(const char*name,const char*member,long*off){
    job_t J; memset(&J,0,sizeof J);
    for(int c=0;c<NCU;c++){
        memset(&J.out,0,sizeof J.out); J.found=0; J.type_name=name;
        if(scan_cu(&CU[c],&J)<0) continue;
        if(J.found && J.out.bsize>0 && J.out.nmem>0){
            char path[320];
            if(find_member(&J.out,member,0,0,off,path,sizeof path)==0) return 0;
        }
    }
    return -1;
}

/* ---- Extraction targets: the values actually read ----
   sym+member = member offset from a variable root; type alone = sizeof;
   type+member = member offset within that type. */
typedef struct { const char*key,*sym,*member,*type; long expect; } tgt_t;
static tgt_t TG[]={
  {"OFF_PGBUF_NBUF",    "pgbuf_Pool","num_buffers",          NULL,  0},
  {"OFF_XCACHE_ENTRIES","xcache_Global","entry_count",       NULL,440},
  {"OFF_XCACHE_USAGE",  "xcache_Global","memory_usage_cache",NULL,484},
  {"OFF_XCACHE_CLONE",  "xcache_Global","memory_usage_clone",NULL,488},
  {"OFF_QLIST_NPAGES",  "qfile_List_cache","n_pages",        NULL, 28},
  {"OFF_QPOOL_NENT",    "qfile_List_cache_entry_pool","n_entries",NULL,8},
  {"OFF_LK_NUMTRANS",   "lk_Gl","num_trans",                 NULL,  0},
  {"OFF_LF_ALLOCCNT",   "sessions","alloc_cnt",              NULL, 12},
  {"OFF_LF_ALLOCCNT2",  "catalog_Hashmap","alloc_cnt",       NULL, 12},
  {"SZ_SESSION_STATE",  NULL,NULL,"session_state",  312},
  {"SZ_CATALOG_ENTRY",  NULL,NULL,"catalog_entry",   48},
  {"SZ_FPCACHE_ENT",    NULL,NULL,"fpcache_ent",    120},
  {"SZ_LK_TRAN_LOCK",   NULL,NULL,"lk_tran_lock",   152},
  {"SZ_CSS_CONN_ENTRY", NULL,NULL,"css_conn_entry", 480},
  {"SZ_VACUUM_DATA",    NULL,NULL,"vacuum_data",    184},
  /* ---- Buffer pool BCB direct read (pgbuf section, volmap --bufmap); measured on 11.5 ---- */
  {"OFF_PGBUF_BCBTAB",  "pgbuf_Pool","BCB_table",            NULL,  8},
  {"SZ_PGBUF_BCB",      NULL,NULL,"pgbuf_bcb",               144},
  {"OFF_BCB_VPID",      NULL,"vpid","pgbuf_bcb",             44},
  {"OFF_BCB_FLAGS",     NULL,"flags","pgbuf_bcb",            64},
  {"OFF_BCB_OLDEST_LSA",NULL,"oldest_unflush_lsa","pgbuf_bcb",128},
  {"OFF_BCB_IOPAGE",    NULL,"iopage_buffer","pgbuf_bcb",    136},
  {"OFF_IOBUF_IOPAGE",  NULL,"iopage","pgbuf_iopage_buffer",   8},
  {"OFF_LOG_NXIO_LSA",  "log_Gl","nxio_lsa",                 NULL, 48},
  {"OFF_LOG_APPEND_LSA","log_Gl","append_lsa",               NULL,280},
  {"OFF_LOG_EOF_LSA",   "log_Gl","eof_lsa",                  NULL,488},
  /* Engine statistics (perfmon) for the capacity axis.  global_stats is a pointer to a
     UINT64 array whose indices pstat_Metadata[psid].start_offset fixes at runtime. */
  {"OFF_PSTAT_NVALS",   "pstat_Global","n_stat_values",       NULL,  0},
  {"OFF_PSTAT_GSTATS",  "pstat_Global","global_stats",        NULL,  8},
  {"OFF_PSTAT_INIT",    "pstat_Global","initialized",         NULL, 44},
  {"SZ_PSTAT_METADATA", NULL,NULL,"pstat_metadata",             56},
  {"OFF_PSTATMD_PSID",  NULL,"psid","pstat_metadata",            0},
  {"OFF_PSTATMD_START", NULL,"start_offset","pstat_metadata",   20},
};
#define NTG ((int)(sizeof TG/sizeof TG[0]))
/* The standalone dwoff's GOT/OKF/NOTE are unused here; out[] replaces them */


/* ---- Builtin DWARF extraction API ----
   Returns the number of keys resolved (>0 on success); out[NTG] holds the values
   (-1 unresolved) and why the failure reason.  A full .debug_info scan takes
   seconds (8s / 125MB measured), so the caller should cache it. */
static int dwoff_extract(const char*so_path,long out[],char*why,size_t wn){
    /* Reset globals in case of re-entry */
    DI=AB=DS=NULL; DI_SZ=AB_SZ=DS_SZ=0; NCU=0; NTAB=0; G_desync=0;
    int fd=open(so_path,O_RDONLY);
    if(fd<0){ snprintf(why,wn,"open 실패"); return 0; }
    struct stat st;
    if(fstat(fd,&st)!=0 || st.st_size<(off_t)sizeof(Elf64_Ehdr)){ close(fd); snprintf(why,wn,"ELF 아님"); return 0; }
    unsigned char*m=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(m==MAP_FAILED){ snprintf(why,wn,"mmap 실패"); return 0; }
    size_t fsz=(size_t)st.st_size;
    #define DW_IN_FILE(off_,len_) ((uint64_t)(off_) <= (uint64_t)fsz && \
                                   (uint64_t)(len_) <= (uint64_t)fsz - (uint64_t)(off_))
    int got=0;
    TAB=(ab_t*)calloc(NTAB_MAX,sizeof(ab_t));
    if(!TAB){ snprintf(why,wn,"TAB 할당 실패"); munmap(m,st.st_size); return 0; }
    Elf64_Ehdr*eh=(Elf64_Ehdr*)m;
    if(memcmp(eh->e_ident,ELFMAG,SELFMAG)||eh->e_ident[EI_CLASS]!=ELFCLASS64){
        snprintf(why,wn,"ELF64 아님"); goto done; }
    if(eh->e_shentsize!=sizeof(Elf64_Shdr) || eh->e_shnum==0 || eh->e_shstrndx>=eh->e_shnum ||
       !DW_IN_FILE(eh->e_shoff,(uint64_t)eh->e_shnum*sizeof(Elf64_Shdr))){
        snprintf(why,wn,"섹션 표가 파일 밖"); goto done; }
    { Elf64_Shdr*sh=(Elf64_Shdr*)(m+eh->e_shoff);
      Elf64_Shdr*ssh=&sh[eh->e_shstrndx];
      if(!DW_IN_FILE(ssh->sh_offset,ssh->sh_size)){ snprintf(why,wn,"섹션 이름 표가 파일 밖"); goto done; }
      const char*ss=(const char*)(m+ssh->sh_offset);
      for(int i=0;i<eh->e_shnum;i++){
          if(sh[i].sh_name>=ssh->sh_size || !memchr(ss+sh[i].sh_name,0,ssh->sh_size-sh[i].sh_name)) continue;
          if(!DW_IN_FILE(sh[i].sh_offset,sh[i].sh_size)) continue;   /* a section outside the file is not used */
          const char*nm2=ss+sh[i].sh_name;
          if(!strcmp(nm2,".debug_info")){ DI=m+sh[i].sh_offset; DI_SZ=sh[i].sh_size; }
          else if(!strcmp(nm2,".debug_abbrev")){ AB=m+sh[i].sh_offset; AB_SZ=sh[i].sh_size; }
          else if(!strcmp(nm2,".debug_str")){    DS=m+sh[i].sh_offset; DS_SZ=sh[i].sh_size; }
      } }
    if(!DI||!AB){ snprintf(why,wn,"디버그 섹션 없음(strip)"); goto done; }
    build_index();
    for(int i=0;i<NTG;i++){
        out[i]=-1;
        if(TG[i].type && TG[i].member){
            long off=-1; if(member_of_type(TG[i].type,TG[i].member,&off)==0){ out[i]=off; got++; }
        } else if(TG[i].type){
            long sz; if(sizeof_type(TG[i].type,&sz,NULL)==0){ out[i]=sz; got++; }
        } else {
            die_t d; unsigned long toff=0;
            if(resolve_symbol(TG[i].sym,&d,&toff)==0){
                long off=-1; char path[320]={0};
                if(find_member(&d,TG[i].member,0,0,&off,path,sizeof path)==0){ out[i]=off; got++; }
            }
        }
    }
    if(got==0) snprintf(why,wn,"키 0개 해소");
done:
    free(TAB); TAB=NULL; NTAB=0;
    if(CU){ free(CU); CU=NULL; NCU=0; }   /* The CU index has the same lifetime */
    #undef DW_IN_FILE
    munmap(m,st.st_size);
    return got;
}
/* TG[] indices; the order fills the table, so it is fixed by this enum */
enum { DW_PGBUF=0, DW_XC_ENT, DW_XC_USE, DW_XC_CLN, DW_QLIST, DW_QPOOL,
       DW_LK_NTRAN, DW_LF, DW_LF2,
       DW_SZ_SESS, DW_SZ_CAT, DW_SZ_FP, DW_SZ_LK, DW_SZ_CONN, DW_SZ_VAC,
       /* pgbuf/log, one to one with the tail of TG[] */
       DW_BCBTAB, DW_SZ_BCB, DW_BCB_VPID, DW_BCB_FLAGS, DW_BCB_OLSA, DW_BCB_IOPG,
       DW_IOBUF_IOPG, DW_LOG_NXIO, DW_LOG_APPEND, DW_LOG_EOF,
       DW_PS_NVALS, DW_PS_GSTATS, DW_PS_INIT, DW_PS_SZMD, DW_PS_MDPSID, DW_PS_MDSTART, DW_N };

/* Extraction takes about 8 seconds over 125MB of .debug_info, so its result is cached
   as text in /var/tmp/cub_top.dw-<uid>-<size>-<mtime>.tbl, keyed by the .so's size and
   mtime.  /var/tmp is writable by every user.  A table read from there
   decides which addresses of the server are read, so only a regular file this user
   owns, and nobody else can write, is trusted; and it is written to a fresh 0600 file
   that is renamed into place, so a planted name - a symlink to some other file, or a
   file of another user - is never opened for writing.  The uid in the name keeps one
   user's cache from blocking another's. */
static FILE *dw_cache_open_trusted(const char *path){
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0) return NULL;
    struct stat cs;
    if(fstat(fd,&cs)!=0 || !S_ISREG(cs.st_mode) || cs.st_uid!=geteuid() || (cs.st_mode&022)){
        close(fd); return NULL; }
    FILE *f=fdopen(fd,"r");
    if(!f) close(fd);
    return f;
}
static void dw_cache_write(const char *path,const long out[]){
    char tmp[300]; snprintf(tmp,sizeof tmp,"%s.%d.tmp",path,(int)getpid());
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0) return;
    FILE *f=fdopen(fd,"w");
    if(!f){ close(fd); unlink(tmp); return; }
    for(int i=0;i<NTG;i++) if(out[i]>=0) fprintf(f,"%s %ld\n",TG[i].key,out[i]);
    if(fclose(f)==0) { if(rename(tmp,path)!=0) unlink(tmp); }
    else unlink(tmp);
}
static int dw_load_or_extract(const char*so,long out[],char*why,size_t wn){
    struct stat st;
    if(stat(so,&st)!=0){ snprintf(why,wn,"stat 실패"); return 2; }
    char cpath[256];
    snprintf(cpath,sizeof cpath,"/var/tmp/cub_top.dw-%d-%lld-%lld.tbl",
             (int)geteuid(),(long long)st.st_size,(long long)st.st_mtime);
    FILE*cf=dw_cache_open_trusted(cpath);
    if(cf){
        int got=0; char k[64]; long v;
        for(int i=0;i<DW_N;i++) out[i]=-1;
        while(fscanf(cf,"%63s %ld",k,&v)==2)
            for(int i=0;i<NTG;i++)
                if(!strcmp(k,TG[i].key)){ out[i]=v; got++; break; }
        fclose(cf);
        if(got>0) return 1;
    }
    fprintf(stderr,"cub_top: 오프셋 표가 이 환경과 안 맞아 %s 의 DWARF 에서 직접 추출합니다"
                   " (1회, 수 초 — 결과는 %s 에 캐시)\n",so,cpath);
    if(dwoff_extract(so,out,why,wn)<=0) return 2;
    dw_cache_write(cpath,out);
    return 1;
}
/* Anchor probing - last resort on a stripped build with no table and no DWARF.
   Symbol addresses come from .symtab; the field offset is fixed by the one
   position where a paramdump-known value appears exactly once.  Non-unique
   matches are rejected.  Item sizes cannot be probed, so those become
   "count only". */
#define PROBE_WIN 64
static int probe_i32(int pid,unsigned long base,long lo,long hi,int *out_off,int32_t *out_v){
    /* If exactly one int32 in [lo,hi] lies in the window, that offset is fixed */
    int hits=0,off=-1; int32_t val=0;
    for(int o=0;o<PROBE_WIN;o+=4){
        int32_t v=rd_i32(pid,base+o);
        if(v>=lo && v<=hi){ hits++; off=o; val=v; }
    }
    if(hits!=1) return 0;
    *out_off=off; *out_v=val; return 1;
}
/* ======= End of the builtin DWARF engine ======= */

/* CUBRID version detection: the mapped .so name suffix, else a "1X.Y.Z.build"
   string in its rodata.  This only picks a candidate table - adoption still
   requires the runtime paramdump cross-check. */
static char g_cver[24]="";           /* "11.5" or "11.5.0.2374" */
static void detect_cubrid_ver(const char *so_path){
    g_cver[0]=0;
    const char *p=strstr(so_path,".so.");
    if(p && ((p[4]>='1'&&p[4]<='9'))){
        snprintf(g_cver,sizeof g_cver,"%s",p+4);          /* "11.5" */
    }
    /* Promote to a more precise string (with build number, e.g. 11.5.0.2374) if one is
       in the file.  Found at 12.8MB in rodata on 11.5, so the first 16MB is streamed
       in 256KB chunks (static buffer, no allocation).  Otherwise the filename-derived
       value stands. */
    FILE *fp=fopen(so_path,"rb");
    if(fp){
        static char buf[256*1024+32];
        long limit=16L*1024*1024, done=0; size_t carry=0;
        while(done<limit){
            size_t got=fread(buf+carry,1,sizeof buf-32-carry,fp);
            if(got==0) break;
            size_t total=carry+got;
            for(size_t i=0;i+8<total;i++){
                if(buf[i]=='1' && (buf[i+1]=='0'||buf[i+1]=='1') && buf[i+2]=='.'
                   && buf[i+3]>='0'&&buf[i+3]<='9'){
                    size_t j=i,dots=0;
                    while(j<total && j<i+20 &&
                          ((buf[j]>='0'&&buf[j]<='9')||buf[j]=='.')){ if(buf[j]=='.')dots++; j++; }
                    if(dots>=3 && j<total && buf[j]==0){
                        snprintf(g_cver,sizeof g_cver,"%.*s",(int)(j-i),buf+i);
                        fclose(fp); return;
                    }
                    i=j;
                }
            }
            /* Carry 20 trailing bytes across the boundary */
            carry = total>20 ? 20 : total;
            memmove(buf,buf+total-carry,carry);
            done+=(long)got;
        }
        fclose(fp);
    }
}

/* Library load base = the first mapping with file offset 0, not the r-xp
   segment (whose file offset is non-zero).  Symbol st_value is relative to the
   file virtual address, so a wrong base shifts every address and disables
   method B.  Fall back to the file's lowest start. */
static unsigned long lib_base(int pid,const char *needle,char *path,size_t plen){
    char f[64]; snprintf(f,sizeof f,"/proc/%d/maps",pid);
    FILE *fp=fopen(f,"r"); if(!fp) return 0;
    char line[1024]; unsigned long base=0, first=0, s0=0,e0=0;
    char pth0[512]={0};
    while(fgets(line,sizeof line,fp)){
        if(!strstr(line,needle)) continue;
        unsigned long s,e=0,off=0; char perm[8],pth[512];
        pth[0]=0;
        if(sscanf(line,"%lx-%lx %7s %lx %*s %*s %511s",&s,&e,perm,&off,pth)<4) continue;
        if(!first || s<first){ first=s; }                 /* Lowest start address (fallback) */
        if(off==0 && !base){ base=s; s0=s; e0=e; snprintf(pth0,sizeof pth0,"%s",pth); }
        if(!s0){ s0=s; e0=e; snprintf(pth0,sizeof pth0,"%s",pth); }   /* Path and map_files candidates */
    }
    fclose(fp);
    if(!base) base=first;
    if(base){
        {
            unsigned long s=s0,e=e0; const char*pth=pth0;
            if(path){
                /* Read symbols from the bytes the server is running: a rebuild leaves maps
                   showing "(deleted)" while the old inode stays in use.
                   map_files/<range> opens that inode; failing that the path is
                   used and the gate catches a replaced file. */
                char mf[96]; snprintf(mf,sizeof mf,"/proc/%d/map_files/%lx-%lx",pid,s,e);
                int tfd=open(mf,O_RDONLY);
                if(tfd>=0){ close(tfd); snprintf(path,plen,"%s",mf); }
                else snprintf(path,plen,"%s",pth);
            }
        }
    }
    return base;
}
/* ELF symbol lookup.  The input file is not trusted: comm can be faked with
   prctl, so any process could map a crafted ELF.  Every pointer derived from
   header offset/count/size fields is checked against [m, m+size) before it is
   dereferenced. */
static int sym_lookup(const char *so,const char **names,unsigned long *out,int n){
    int fd=open(so,O_RDONLY); if(fd<0) return -1;
    struct stat st; if(fstat(fd,&st)<0){ close(fd); return -1; }
    if(st.st_size<(off_t)sizeof(Elf64_Ehdr)){ close(fd); return -1; }
    size_t sz=(size_t)st.st_size;
    unsigned char *m=(unsigned char*)mmap(NULL,sz,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd); if(m==MAP_FAILED) return -1;
    int found=0;
    /* Does [off, off+len) lie entirely within the file (overflow checked) */
    #define IN_FILE(off_,len_) ((uint64_t)(off_) <= (uint64_t)sz && \
                                (uint64_t)(len_) <= (uint64_t)sz - (uint64_t)(off_))
    Elf64_Ehdr *eh=(Elf64_Ehdr*)m;
    if(memcmp(eh->e_ident,ELFMAG,SELFMAG) || eh->e_ident[EI_CLASS]!=ELFCLASS64){
        munmap(m,sz); return -1;   /* Guard against misreading ELFCLASS32; the structs assume Elf64 */
    }
    if(eh->e_shentsize!=sizeof(Elf64_Shdr) || eh->e_shnum==0 ||
       !IN_FILE(eh->e_shoff,(uint64_t)eh->e_shnum*sizeof(Elf64_Shdr))){
        munmap(m,sz); return -1;                       /* Section header table outside the file */
    }
    Elf64_Shdr *sh=(Elf64_Shdr*)(m+eh->e_shoff);
    for(int i=0;i<eh->e_shnum && found<n;i++){
        if(sh[i].sh_type!=SHT_SYMTAB && sh[i].sh_type!=SHT_DYNSYM) continue;
        if(sh[i].sh_entsize!=sizeof(Elf64_Sym)) continue;          /* Guard against division by zero and misreading */
        if(!IN_FILE(sh[i].sh_offset,sh[i].sh_size)) continue;      /* Symbol table outside the file */
        if(sh[i].sh_link>=eh->e_shnum) continue;                   /* Invalid string section index */
        Elf64_Shdr *strsh=&sh[sh[i].sh_link];
        if(!IN_FILE(strsh->sh_offset,strsh->sh_size)) continue;    /* String table outside the file */
        Elf64_Sym *sy=(Elf64_Sym*)(m+sh[i].sh_offset);
        const char *str=(const char*)(m+strsh->sh_offset);
        const char *strend=str+strsh->sh_size;                     /* The section end, not the file end */
        long cnt=(long)(sh[i].sh_size/sizeof(Elf64_Sym));
        /* Four-byte prefilter: the first character alone is useless (65% of symbols start
           with '_'), while _ZL (local static) and _ZN (namespace) diverge at byte 4. */
        uint32_t want[MAXSYM];
        for(int j=0;j<n;j++) memcpy(&want[j],names[j],4);
        for(long k=0;k<cnt && found<n;k++){
            if(sy[k].st_name>=strsh->sh_size) continue;            /* Name offset outside the table */
            const char *nm=str+sy[k].st_name;
            if(!*nm || nm+4>strend) continue;
            /* Confirm the string is NUL-terminated, so strcmp cannot run past the section */
            if(!memchr(nm,0,(size_t)(strend-nm))) continue;
            uint32_t nm4; memcpy(&nm4,nm,4);
            for(int j=0;j<n;j++)
                if(!out[j] && nm4==want[j] && !strcmp(nm,names[j])){
                    out[j]=sy[k].st_value; found++; break; }
        }
    }
    #undef IN_FILE
    munmap(m,sz); return found;
}

/* Method B: walk the root globals.  Offsets come from a table extracted from
 * DWARF.  Self-check: pgbuf_Pool.num_buffers must equal paramdump's
 * data_buffer_pages, else method B is disabled. */
enum { S_PGBUF, S_XCACHE, S_QLIST, S_QPOOL, S_FPCNT, S_LKGL, S_SESS, S_CAT,
       S_CONNMAX, S_CONNACT, S_VACUUM, S_LOGGL, S_PSTATG, S_PSTATMD, S_NSYM };
static const char *SYMN[S_NSYM]={
    "_ZL10pgbuf_Pool","xcache_Global","_ZL16qfile_List_cache",
    "_ZL27qfile_List_cache_entry_pool","_ZL21fpcache_Entry_counter",
    "lk_Gl","_ZL8sessions","_ZL15catalog_Hashmap",
    "_ZL16css_Num_max_conn","_ZL19css_Num_active_conn","_ZL11vacuum_Data",
    "log_Gl",   /* extern "C" globals: unmangled */
    "pstat_Global","pstat_Metadata"   /* Engine statistics (capacity axis); both are non-static globals */
};
static const char *SYMN10[S_NSYM]={
    /* 10.x (C build, unmangled).  Eight of these exist in the 10.0 symtab;
       xcache_Global and fpcache_Entry_counter arrived in 10.2, and catalog_Hashmap
       is a different structure (catalog_Hash_table) in 10.x.  A missing name blanks
       only its own item - the pgbuf, lk and conn anchors survive. */
    "pgbuf_Pool","xcache_Global","qfile_List_cache",
    "qfile_List_cache_entry_pool","fpcache_Entry_counter",
    "lk_Gl","sessions","catalog_Hashmap",
    "css_Num_max_conn","css_Num_active_conn","vacuum_Data",
    "log_Gl",
    "pstat_Global","pstat_Metadata"
};

/* DWARF-derived offsets and sizes (the 11.5 golden set; check-offsets.sh compares it with rules-11.5.h) */
#define OFF_XCACHE_ENTRIES 440
#define OFF_XCACHE_USAGE   484
#define OFF_XCACHE_CLONE   488
#define OFF_QLIST_NPAGES    28
#define OFF_QPOOL_NENT       8
#define OFF_LF_ALLOCCNT     12
#define SZ_LK_TRAN_LOCK    152
#define SZ_SESSION_STATE   312
#define SZ_CATALOG_ENTRY    48
#define SZ_FPCACHE_ENT     120
#define SZ_CSS_CONN_ENTRY  480
#define SZ_VACUUM_DATA     184

/* Version tables are candidates only.  Adoption requires the .so symtab lookup
   plus the paramdump cross-check in that environment, so every candidate is
   tried regardless of the detected version. */
typedef struct {
    const char *tag;                 /* Table name (its source), shown on screen and in terse */
    const char *ver_prefix;          /* Tried first when it prefix-matches g_cver */
    const char **syms;               /* S_NSYM symbol names */
    int off_pgbuf_nbuf;
    int off_xc_entries,off_xc_usage,off_xc_clone;
    int off_qlist_npages,off_qpool_nent,off_lf_alloccnt;
    int sz_lk_tran,sz_session,sz_catalog,sz_fpcache,sz_conn,sz_vacuum;   /* -1 means the size is unknown */
    /* Offset of num_trans within lk_Gl.  The structure differs by release, so a fixed
       +0 could read garbage that happens to be in range (10.2 is at +456).
       Keep this at the END of the struct: BTABS[] uses positional literals, so an
       insertion in the middle silently shifts every field. */
    int off_lk_ntran;
    /* ---- BCB direct read offsets; all -1 disables the pgbuf section.  Append at the struct end only ---- */
    struct { int bcbtab, sz_bcb, vpid, flags, olsa, iopg, iobuf_iopg, log_nxio, log_append, log_eof; } pg;
    /* perfmon offsets; -1 disables only the statistics direct read, leaving the rest */
    struct { int nvals, gstats, init, sz_md, md_psid, md_start; } ps;
} btab_t;
static const btab_t BTABS[]={
    {"table-11.5","11.5",SYMN,0,
     OFF_XCACHE_ENTRIES,OFF_XCACHE_USAGE,OFF_XCACHE_CLONE,
     OFF_QLIST_NPAGES,OFF_QPOOL_NENT,OFF_LF_ALLOCCNT,
     SZ_LK_TRAN_LOCK,SZ_SESSION_STATE,SZ_CATALOG_ENTRY,
     SZ_FPCACHE_ENT,SZ_CSS_CONN_ENTRY,SZ_VACUUM_DATA,
     0 /* off_lk_ntran: on 11.x num_trans is at the head of lk_Gl */,
     {8,144,44,64,128,136,8,48,280,488} /* pg, measured with dwoff on 11.5.0.2374 */,
     {0,8,44,56,0,20} /* ps, measured with dwoff on 11.5.0.2374 */},
};
/* DWARF or offset-file values into the pg group.  If any is missing all become -1 - no partial trust. */
static void ps_from_dw(btab_t*bt,const long dw[]){
    long v[6]={dw[DW_PS_NVALS],dw[DW_PS_GSTATS],dw[DW_PS_INIT],dw[DW_PS_SZMD],dw[DW_PS_MDPSID],dw[DW_PS_MDSTART]};
    int ok=1; for(int i=0;i<6;i++) if(v[i]<0) ok=0;
    int *f=(int*)&bt->ps;
    for(int i=0;i<6;i++) f[i]= ok ? (int)v[i] : -1;
}
static void pg_from_dw(btab_t*bt,const long dw[]){
    long v[10]={dw[DW_BCBTAB],dw[DW_SZ_BCB],dw[DW_BCB_VPID],dw[DW_BCB_FLAGS],dw[DW_BCB_OLSA],
                dw[DW_BCB_IOPG],dw[DW_IOBUF_IOPG],dw[DW_LOG_NXIO],dw[DW_LOG_APPEND],dw[DW_LOG_EOF]};
    int ok=1; for(int i=0;i<10;i++) if(v[i]<0) ok=0;
    int *f=(int*)&bt->pg;
    for(int i=0;i<10;i++) f[i]= ok ? (int)v[i] : -1;
}
#define NBTABS ((int)(sizeof BTABS/sizeof BTABS[0]))
static const btab_t *g_bt=&BTABS[0];   /* Set by method_b to the candidate that passed validation */
static char g_offs_file[256]="";       /* --offsets <file>: the output of dwoff --emit-tbl */
/* Bumped when the mapped libcubrid.so changes (another instance or a restart on a
   different install): the version and every table chosen for it belong to that file. */
static unsigned g_lib_gen=0;
static char g_so_path[512]="";          /* The lib path symbols and DWARF were read from, for diagnostics and regression */

/* Offset file loader, the same key=val format as the dw cache (plus #meta comments).
   The metadata is informational: extracting on another machine is the point, so a
   (size,mtime) mismatch does not block - the runtime gates decide either way. */
#include "offsets_embedded.h"

/* Parse the table text.  Files and the built-in tables share this one parser, so an
   embedded table cannot be read differently from the same table on disk. */
static int offs_text_load(const char*txt,long out[]){
    for(int i=0;i<DW_N;i++) out[i]=-1;
    char k[64]; long v; int got=0; char line[256];
    const char *p=txt;
    while(*p){
        const char *e=strchr(p,'\n'); size_t n=e?(size_t)(e-p):strlen(p);
        if(n>=sizeof line) n=sizeof line-1;
        memcpy(line,p,n); line[n]=0;
        p = e ? e+1 : p+strlen(p);
        if(line[0]=='#') continue;
        if(sscanf(line,"%63s %ld",k,&v)!=2) continue;
        for(int i=0;i<NTG;i++)
            if(!strcmp(k,TG[i].key)){ out[i]=v; got++; break; }
    }
    return got;
}
static int offs_file_load(const char*path,long out[]){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    long sz; char *buf;
    fseek(f,0,SEEK_END); sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0 || sz>(1<<20)){ fclose(f); return 0; }
    buf=malloc((size_t)sz+1);
    if(!buf){ fclose(f); return 0; }
    size_t rd=fread(buf,1,(size_t)sz,f); buf[rd]=0;
    fclose(f);
    int got=offs_text_load(buf,out);
    free(buf);
    return got;
}

/* The table file's #meta version= value, used to pick a matching table from a
   directory.  Only the first few lines are read (dwoff writes the meta at the top). */
static int offs_tbl_version(const char*path,char*ver,size_t vn){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    char line[256]; int ok=0;
    for(int i=0;i<10 && fgets(line,sizeof line,f);i++)
        if(sscanf(line,"#meta version=%31s",ver)==1 && (size_t)strlen(ver)<vn){ ok=1; break; }
    fclose(f); return ok;
}
/* Matching component count between two version strings: "11.4.5.1865" against
   "11.4.4.1832" gives 2.  Below major.minor there is no basis for sharing a table. */
static int ver_match_comp(const char*a,const char*b){
    int n=0;
    for(;;){
        const char*da=strchr(a,'.'), *db=strchr(b,'.');
        size_t la=da?(size_t)(da-a):strlen(a), lb=db?(size_t)(db-b):strlen(b);
        if(la==0||lb==0||la!=lb||strncmp(a,b,la)) return n;
        n++;
        if(!da||!db) return n;
        a=da+1; b=db+1;
    }
}

/* Pick the embedded table whose version best matches, and parse it with the same
   parser files use.  Embedded tables make the binary self-contained: without them a
   copy of cub_top on another host falls back to DWARF extraction or probing on any
   version that has no built-in table. */
static int offs_embedded_load(const char*ver,long out[],char *vout,size_t vn){
    int best=-1,bn=0;
    for(int i=0;i<EMB_TBL_N;i++){
        int n=ver_match_comp(ver,EMB_TBL[i].ver);
        if(n>bn){ bn=n; best=i; }
    }
    if(best<0||bn<2) return 0;        /* below major.minor there is no basis */
    if(vout) snprintf(vout,vn,"%s",EMB_TBL[best].ver);
    return offs_text_load(EMB_TBL[best].text,out);
}

/* Pick the *.tbl in a directory that best matches the detected version.
   Returns 1 on a choice (out filled), 0 if none; ties go to the longer match. */
static int offs_dir_pick(const char*dir,const char*cver,char*out,size_t on){
    DIR*d=opendir(dir); if(!d) return 0;
    struct dirent*e; int best=-1; char bestp[512]="";
    while((e=readdir(d))){
        size_t l=strlen(e->d_name);
        if(l<5 || strcmp(e->d_name+l-4,".tbl")) continue;
        char p[512]; snprintf(p,sizeof p,"%s/%s",dir,e->d_name);
        char v[32];
        if(!offs_tbl_version(p,v,sizeof v)) continue;
        int m=ver_match_comp(v,cver);
        if(m>=2 && m>best){ best=m; snprintf(bestp,sizeof bestp,"%s",p); }
    }
    closedir(d);
    if(best<0) return 0;
    snprintf(out,on,"%s",bestp); return 1;
}

typedef struct { char name[40]; double bytes; char note[72]; char grade;
    /* count-only: anchor probing on a stripped foreign build fixes the count alone.
       The byte figure (count x size) is not invented when the size is unknown, and it
       is left out of the total. */
    long count; char count_only;
} bitem_t;
/* The heap item's previous-frame value, found by name: B rebuilds its array every
   frame, so remembering an index would misalign everything the moment one drops. */
#define HPREVMAX 24
static struct { char name[40]; double bytes; } g_hprev[HPREVMAX];
static int g_hprevn=0;
static double hprev_get(const char *nm){
    for(int i=0;i<g_hprevn;i++) if(!strcmp(g_hprev[i].name,nm)) return g_hprev[i].bytes;
    return -1.0;                       /* No baseline */
}
static void hprev_set(const char *nm,double v){
    for(int i=0;i<g_hprevn;i++) if(!strcmp(g_hprev[i].name,nm)){ g_hprev[i].bytes=v; return; }
    if(g_hprevn<HPREVMAX){ snprintf(g_hprev[g_hprevn].name,40,"%s",nm);
                           g_hprev[g_hprevn].bytes=v; g_hprevn++; }
}
/* Delta string, placed left of the value; empty when unchanged, to keep the screen quiet */
static const char *hdelta(const char *nm,double cur){
    static char b[4][24]; static int k=0; char *o=b[k++&3];
    double p=hprev_get(nm);
    o[0]=0;
    if(p>=0){ double d=cur-p, a=d<0?-d:d;
              if(a>=1.0) snprintf(o,24,"%s%s",d>0?"+":"-",H(a)); }
    hprev_set(nm,cur);
    return o;
}
static struct {
    int ok; int partial; char why[128];   /* partial marks anchor-probing count-only mode */
    unsigned long base, sym[S_NSYM];
    bitem_t it[16]; int n;
    double total;      /* Sum of valid items only */
    int n_bad;         /* Items failing validation, excluded from the sum */
} B;

static void method_b(proc_t *P){
    memset(&B,0,sizeof B);
    char so[512]={0};
    B.base=lib_base(P->pid,"libcubrid.so.",so,sizeof so);   /* Common to 10.x-11.x.  lib_base is a substring match, and neither
       "libcubrid_timezones.so." nor "libcubridsa" contains "libcubrid.so.",
       so there is no false match. */
    snprintf(g_so_path,sizeof g_so_path,"%s",so);        /* terse heapB.lib: the file actually read (the running inode via map_files) */
    if(!B.base){ snprintf(B.why,sizeof B.why,g_ascii?"libcubrid.so not mapped":"libcubrid.so 매핑 없음"); return; }
    /* Version and table choices belong to the library file, not to this run.  Switching
       to a server of another release kept the first one's version and tables, and a
       table that passed the gates by chance would be adopted with the wrong sizes. */
    { static dev_t ld; static ino_t li; static int lk=0; struct stat ls;
      if(stat(so,&ls)==0 && (!lk || ls.st_dev!=ld || ls.st_ino!=li)){
          ld=ls.st_dev; li=ls.st_ino; lk=1; g_lib_gen++; g_cver[0]=0; } }
    if(!g_cver[0]) detect_cubrid_ver(so);   /* From the very file the process mapped */
    if(!CAP.vm_readv && !CAP.mem_pread){
        snprintf(B.why,sizeof B.why,g_ascii?"cannot read process memory":"프로세스 메모리 읽기 불가"); return; }

    /* Cache across frames: symbol addresses and the adopted table are constant for
       the same .so and pid.  The L4 self-check is still re-read every frame. */
    {
        static char c_so[512]; static long long c_mt=-1; static int c_pid=-1;
        static const btab_t *c_bt=NULL;
        static unsigned long c_sym[S_NSYM];
        struct stat cst; long long mt = stat(so,&cst)==0 ? (long long)cst.st_mtime : -2;
        if(c_bt && c_pid==P->pid && c_mt==mt && !strcmp(c_so,so)){
            long long want=prm_get("data_buffer_pages");
            memcpy(B.sym,c_sym,sizeof B.sym);
            int32_t nbuf=rd_i32(P->pid,B.base+B.sym[S_PGBUF]+c_bt->off_pgbuf_nbuf);
            /* Even on a cache hit the value is sanity-checked every frame, for the reason above */
            if((nbuf>0 && nbuf<=(1<<26)) && (want<=0 || nbuf==(int32_t)want)){
                g_bt=c_bt; B.ok=1;
                if(!B.partial)
                    snprintf(B.why,sizeof B.why,g_ascii?"self-check passed (num_buffers=%d, %s)"
                                                     :"자기검증 통과(num_buffers=%d, %s)",nbuf,g_bt->tag);
                goto adopted;
            }
            c_bt=NULL;   /* Validation failed: invalidate the cache and run the whole ladder again */
        }
        /* Filled when the ladder below adopts a candidate */
        #define BT_CACHE_FILL() do{ \
            snprintf(c_so,sizeof c_so,"%s",so); c_mt=mt; c_pid=P->pid; \
            c_bt=g_bt; memcpy(c_sym,B.sym,sizeof c_sym); }while(0)

    /* Candidate selection: matching table first, then the rest.  Each must pass
       L1 symbol present, L2 offset aligned, L4 paramdump cross-check.  Without
       paramdump there is nothing to stand in for L4, so a version match is
       required instead. */
    long long want=prm_get("data_buffer_pages");
    int32_t nbuf=0; int tried=0; char lastwhy[96]; snprintf(lastwhy,sizeof lastwhy,"후보 없음");
    /* Run one candidate through the three gates.  Macro-internal names use tb_/tm_
       so they cannot collide with the caller's argument. */
    #define TRY_BTAB(bt_,match_) do{ const btab_t*tb_=(bt_); int tm_=(match_); tried++; \
        memset(B.sym,0,sizeof B.sym);   /* sym_lookup treats out[j]!=0 as already found, so
                                           clear it for every candidate, or the second
                                           candidate dies with found=0. */ \
        if(sym_lookup(so,tb_->syms,B.sym,S_NSYM)<=0){ \
            snprintf(lastwhy,sizeof lastwhy,"[%s] 심볼 조회 실패",tb_->tag); break; } \
        if(!B.sym[S_PGBUF]){ \
            snprintf(lastwhy,sizeof lastwhy,"[%s] pgbuf_Pool 심볼 없음",tb_->tag); break; } \
        { const int offs[]={tb_->off_pgbuf_nbuf,tb_->off_xc_entries,tb_->off_xc_usage, \
                            tb_->off_xc_clone,tb_->off_qlist_npages,tb_->off_qpool_nent, \
                            tb_->off_lf_alloccnt,tb_->off_lk_ntran}; \
          int bad=0; \
          for(unsigned i2=0;i2<sizeof offs/sizeof offs[0];i2++) \
              if(offs[i2]>=0 && (offs[i2]&3)) bad=1; \
          if(bad){ snprintf(lastwhy,sizeof lastwhy,"[%s] 오프셋 정렬 위반",tb_->tag); break; } } \
        nbuf=rd_i32(P->pid,B.base+B.sym[S_PGBUF]+tb_->off_pgbuf_nbuf); \
        if(want>0){ \
            if(nbuf!=(int32_t)want){ \
                snprintf(lastwhy,sizeof lastwhy,"[%s] num_buffers=%d != data_buffer_pages=%lld", \
                         tb_->tag,nbuf,want); \
                break; } \
        } else if(!tm_){ \
            snprintf(lastwhy,sizeof lastwhy,"[%s] paramdump 없음 — 비일치 버전 표는 미검증 채택 불가",tb_->tag); \
            break; } \
        /* Without paramdump, sanity-check the value instead: num_buffers is
           preallocated at boot, so it cannot be 0 or implausibly large on a live
           server.  A corrupted offset returning 0 would otherwise be adopted. */ \
        if(nbuf<=0 || nbuf>(1<<26)){ \
            snprintf(lastwhy,sizeof lastwhy,"[%s] num_buffers=%d 위생 위반(paramdump 부재)",tb_->tag,nbuf); \
            break; } \
        g_bt=tb_; B.ok=1; }while(0)

    /* Promotion check for a non-builtin table (file or DWARF): beyond the pgbuf
       anchor, its lk.num_trans offset and conn max symbol value are compared
       against the range implied by paramdump's max_clients.  One clear mismatch
       rejects the whole table. */
    #define EXTRA_ANCHORS(tag_,lk_off_) do{ \
        long mxc_=prm_get("max_clients"); \
        if(B.ok && want>0 && mxc_>0){ \
            int f_=0; \
            if(B.sym[S_CONNMAX]){ \
                int32_t mx_=rd_i32(P->pid,B.base+B.sym[S_CONNMAX]); \
                if(!(mx_>=mxc_ && mx_<=mxc_*2+32)) f_=1; } \
            if(!f_ && (lk_off_)>=0 && B.sym[S_LKGL]){ \
                int32_t nt_=rd_i32(P->pid,B.base+B.sym[S_LKGL]+(lk_off_)); \
                if(!(nt_>=mxc_ && nt_<=mxc_+16)) f_=2; } \
            if(f_){ B.ok=0; \
                snprintf(lastwhy,sizeof lastwhy,"[%s] 이차 앵커 불일치(%s)", \
                         tag_, f_==1?"conn max":"lk num_trans"); } \
        } }while(0)

    /* Pass 1 tries only the builtin table matching the version, ahead of a
       version-matched offsets file.  A non-matching table can pass the paramdump
       gate (the structures are mostly identical) and be adopted carrying a size
       error such as sz_conn 480 vs 400.  A matching table is always right. */
    for(int ti=0; ti<NBTABS && !B.ok; ti++){
        const btab_t *bt=&BTABS[ti];
        int match = g_cver[0] && !strncmp(g_cver,bt->ver_prefix,strlen(bt->ver_prefix));
        if(match) TRY_BTAB(bt,1);
    }

    /* --offsets: a portable table extracted where DWARF was available; preferred
       on a stripped host.  Given a directory, the *.tbl best matching the
       detected version is chosen.  An offsets/ beside the executable is used
       automatically. */
    if(!B.ok){
        static char resolved[512]=""; static int rst=0;   /* 0 unparsed, 1 file, 2 none */
        static unsigned rgen=0;
        if(rgen!=g_lib_gen){ rgen=g_lib_gen; rst=0; resolved[0]=0; }
        if(rst==0){
            char cand[512]="";
            if(g_offs_file[0]) snprintf(cand,sizeof cand,"%s",g_offs_file);
            else{                       /* Auto-discovery: offsets beside the executable, or one level up */
                char exe[400]; ssize_t el=readlink("/proc/self/exe",exe,sizeof exe-1);
                if(el>0){ exe[el]=0; char*sl=strrchr(exe,'/'); if(sl)*sl=0;
                    char d1[512]; snprintf(d1,sizeof d1,"%s/offsets",exe);
                    char d2[512]; snprintf(d2,sizeof d2,"%s/../offsets",exe);
                    struct stat ds;
                    if(!stat(d1,&ds)&&S_ISDIR(ds.st_mode)) snprintf(cand,sizeof cand,"%s",d1);
                    else if(!stat(d2,&ds)&&S_ISDIR(ds.st_mode)) snprintf(cand,sizeof cand,"%s",d2);
                }
            }
            rst=2;
            if(cand[0]){
                struct stat ds;
                if(!stat(cand,&ds)&&S_ISDIR(ds.st_mode)){
                    if(offs_dir_pick(cand,g_cver,resolved,sizeof resolved)) rst=1;
                    else if(g_offs_file[0])   /* No matching table in the given directory leaves a reason */
                        snprintf(lastwhy,sizeof lastwhy,
                                 "--offsets 디렉터리에 %.20s 와 맞는 표 없음",g_cver);
                } else { snprintf(resolved,sizeof resolved,"%s",cand); rst=1; }
            }
        }
        /* A file, when given, wins over the embedded copy: a newer patch release can be
           handed over as a table without rebuilding the binary.  With no file, the
           embedded table keeps a lone binary working on older versions. */
        static char embver[32]=""; static long fv_emb[DW_N];
        if(rst!=1 && g_cver[0]){
            static long ev[DW_N]; static int est=0; static unsigned egen=0;
            if(egen!=g_lib_gen){ egen=g_lib_gen; est=0; }
            if(est==0) est = offs_embedded_load(g_cver,ev,embver,sizeof embver)>0 ? 1 : 2;
            if(est==1){ memcpy(fv_emb,ev,sizeof ev); rst=3; }
        }
        if(rst==1||rst==3){
        static long fv[DW_N]; static int fst=0; static btab_t fbt; static unsigned fgen=0;
        if(fgen!=g_lib_gen){ fgen=g_lib_gen; fst=0; }
        if(rst==3){ memcpy(fv,fv_emb,sizeof fv); fst=1; }
        else if(fst==0) fst = offs_file_load(resolved,fv)>0 ? 1 : 2;
        if(fst==1){
            fbt.tag = (rst==3) ? "offsets-embedded" : "offsets-file";
            fbt.ver_prefix=(rst==3)?embver:g_cver; fbt.syms=SYMN;
            fbt.off_pgbuf_nbuf =(int)fv[DW_PGBUF];
            fbt.off_xc_entries =(int)fv[DW_XC_ENT];  fbt.off_xc_usage=(int)fv[DW_XC_USE];
            fbt.off_xc_clone   =(int)fv[DW_XC_CLN];
            fbt.off_qlist_npages=(int)fv[DW_QLIST];  fbt.off_qpool_nent=(int)fv[DW_QPOOL];
            fbt.off_lf_alloccnt=(int)fv[DW_LF];
            fbt.sz_lk_tran =(int)fv[DW_SZ_LK];   fbt.sz_session=(int)fv[DW_SZ_SESS];
            fbt.sz_catalog =(int)fv[DW_SZ_CAT];  fbt.sz_fpcache=(int)fv[DW_SZ_FP];
            fbt.sz_conn    =(int)fv[DW_SZ_CONN]; fbt.sz_vacuum =(int)fv[DW_SZ_VAC];
            fbt.off_lk_ntran=(int)fv[DW_LK_NTRAN];
            pg_from_dw(&fbt,fv); ps_from_dw(&fbt,fv);
            /* Symbol names differ by release family (11.2+ mangled vs 10.x C symbols), and the
               table may come from another version, so both sets are tried in turn. */
            { char why1[96]="";
              for(int si=0; si<2 && !B.ok; si++){
                fbt.syms = si ? (const char**)SYMN10 : (const char**)SYMN;
                TRY_BTAB(&fbt,1);
                if(si==0 && !B.ok) snprintf(why1,sizeof why1,"%s",lastwhy);
              }
              /* The second set (10.x symbols) always ends in "symbol not found" on 11.x; letting
                 that overwrite the first set's real rejection reason would kill the
                 diagnostic. */
              if(!B.ok && why1[0] && strstr(lastwhy,"심볼")) snprintf(lastwhy,sizeof lastwhy,"%s",why1); }
            EXTRA_ANCHORS("offsets-file",fv[DW_LK_NTRAN]);
        } else if(fst==2)
            snprintf(lastwhy,sizeof lastwhy,"--offsets 파일을 읽을 수 없음");
        }
    }

    /* Pass 2, builtin tables not matching the version: meaningful only with a paramdump
       gate, and never reached once a version-matched offsets file is adopted. */
    for(int ti=0; ti<NBTABS && !B.ok; ti++){
        const btab_t *bt=&BTABS[ti];
        int match = g_cver[0] && !strncmp(g_cver,bt->ver_prefix,strlen(bt->ver_prefix));
        if(!match) TRY_BTAB(bt,0);
    }

    /* If every builtin table fails, extract from this environment's .so DWARF and apply the same gates. */
    if(!B.ok){
        static long dwv[DW_N]; static int dwst=0; static char dwwhy[64]; static unsigned dgen=0;
        if(dgen!=g_lib_gen){ dgen=g_lib_gen; dwst=0; dwwhy[0]=0; }
        static btab_t dwbt;
        if(dwst==0) dwst=dw_load_or_extract(so,dwv,dwwhy,sizeof dwwhy);
        if(dwst==1){
            dwbt.tag="dwarf-runtime"; dwbt.ver_prefix=g_cver; dwbt.syms=SYMN;
            dwbt.off_pgbuf_nbuf =(int)dwv[DW_PGBUF];
            dwbt.off_xc_entries =(int)dwv[DW_XC_ENT];
            dwbt.off_xc_usage   =(int)dwv[DW_XC_USE];
            dwbt.off_xc_clone   =(int)dwv[DW_XC_CLN];
            dwbt.off_qlist_npages=(int)dwv[DW_QLIST];
            dwbt.off_qpool_nent =(int)dwv[DW_QPOOL];
            dwbt.off_lf_alloccnt=(int)dwv[DW_LF];
            dwbt.sz_lk_tran =(int)dwv[DW_SZ_LK];   dwbt.sz_session=(int)dwv[DW_SZ_SESS];
            dwbt.sz_catalog =(int)dwv[DW_SZ_CAT];  dwbt.sz_fpcache=(int)dwv[DW_SZ_FP];
            dwbt.sz_conn    =(int)dwv[DW_SZ_CONN]; dwbt.sz_vacuum =(int)dwv[DW_SZ_VAC];
            dwbt.off_lk_ntran=(int)dwv[DW_LK_NTRAN];
            pg_from_dw(&dwbt,dwv); ps_from_dw(&dwbt,dwv);
            /* DWARF comes from this environment and counts as a match; the rejection reason is
               kept in lastwhy.  Symbol names differ by family, so both sets are tried. */
            { char why1[96]="";
              for(int si=0; si<2 && !B.ok; si++){
                dwbt.syms = si ? (const char**)SYMN10 : (const char**)SYMN;
                TRY_BTAB(&dwbt,1);
                if(si==0 && !B.ok) snprintf(why1,sizeof why1,"%s",lastwhy);
              }
              if(!B.ok && why1[0] && strstr(lastwhy,"심볼")) snprintf(lastwhy,sizeof lastwhy,"%s",why1); }
            EXTRA_ANCHORS("dwarf-runtime",dwv[DW_LK_NTRAN]);
        } else if(dwst==2 && dwwhy[0])
            snprintf(lastwhy,sizeof lastwhy,"DWARF: %.56s",dwwhy);
    }

    /* Anchor probing - last resort when both the table and DWARF are unavailable
       on a stripped build of another version. */
    if(!B.ok && want>0){
        /* Symbol names themselves differ by release family (11.2+ C++ mangling vs 10.x C
           symbols), so both sets are tried in turn. */
        const char **SYMSETS[2]; SYMSETS[0]=(const char**)SYMN; SYMSETS[1]=(const char**)SYMN10;
        for(unsigned si=0; si<2 && !B.ok; si++){
        memset(B.sym,0,sizeof B.sym);
        if(sym_lookup(so,SYMSETS[si],B.sym,S_NSYM)>0 && B.sym[S_PGBUF]){
            int off; int32_t v;
            if(probe_i32(P->pid,B.base+B.sym[S_PGBUF],want,want,&off,&v)){
                /* Fix pgbuf num_buffers uniquely within the window - both the proof that the symbol
                   and read paths work here and the gate itself.  Only items with an
                   anchor produce a count. */
                B.ok=1; B.partial=1;
                snprintf(B.why,sizeof B.why,
                         g_ascii?"anchor probe partial mode (num_buffers=%d @+%d, %s) - size unknown, count only"
                                :"앵커 탐사 부분 모드(num_buffers=%d @+%d, %s) — 크기 미상, 개수만",
                         v,off,si==0?(g_ascii?"11.x symbols":"11.x 심볼")
                                    :(g_ascii?"10.x symbols":"10.x 심볼"));
            }
        }
        }
    }
    if(!B.ok){
        snprintf(B.why,sizeof B.why,
                 g_ascii?"%d tables+DWARF+probe rejected (detected %.12s) - %.48s"
                        :"표 %d개+DWARF+탐사 탈락(감지 %.12s) — %.48s",
                 tried,g_cver[0]?g_cver:"?",lastwhy);
        return;
    }
    if(!B.partial)   /* Do not overwrite the probing mode's own reason */
        snprintf(B.why,sizeof B.why,g_ascii?"self-check passed (num_buffers=%d, %s%s)"
                                           :"자기검증 통과(num_buffers=%d, %s%s)",
                 nbuf,g_bt->tag,(want>0)?"":(g_ascii?", no paramdump; conditional on version match"
                                                    :", paramdump 부재·버전일치 조건부"));
    if(!B.partial) BT_CACHE_FILL();   /* Probing mode is not a table, so it is excluded from the cache */
    }
adopted:;

    bitem_t *it; double v;
    #define ADD_COUNT(nm_,cnt_,...) do{ if(B.n<16){ it=&B.it[B.n++]; \
        snprintf(it->name,sizeof it->name,"%s",nm_); \
        it->bytes=0.0; it->grade='M'; it->count=(long)(cnt_); it->count_only=1; \
        snprintf(it->note,sizeof it->note,__VA_ARGS__); } }while(0)
    if(B.partial){
        /* Items with an anchor only.  An item that fails uniqueness cannot carry a note
           saying why (it does not exist at all), so the reason is in the help and docs. */
        long mxc=prm_get("max_clients"); if(mxc<0) mxc=0;
        if(B.sym[S_LKGL] && mxc>0){
            int off; int32_t nt;
            if(probe_i32(P->pid,B.base+B.sym[S_LKGL],mxc,mxc+16,&off,&nt))
                ADD_COUNT("lock table (tran)",nt,"num_trans=%d @+%d — 크기 미상",nt,off);
        }
        if(B.sym[S_CONNMAX] && mxc>0){
            int32_t mx=rd_i32(P->pid,B.base+B.sym[S_CONNMAX]);
            if(mx>=mxc && mx<=mxc*2+32)
                ADD_COUNT("conn entries (css)",mx,"max=%d — 크기 미상",mx);
        }
        for(int i=0;i<B.n;i++) B.total+=0;   /* count-only is excluded from the sum (obvious, but stated) */
        return;
    }
    /* No single subsystem uses 64GB - the guard against reading a pointer as a count.
       Out-of-range values are downgraded to '?' and left out of totals. */
    #define SANE_MAX (64.0*1024.0*1024.0*1024.0)
    #define ADD(nm_,val_,gr_,...) do{ if(B.n<16){ it=&B.it[B.n++]; \
        snprintf(it->name,sizeof it->name,"%s",nm_); it->bytes=(val_); it->grade=(gr_); \
        it->count=0; it->count_only=0; \
        snprintf(it->note,sizeof it->note,__VA_ARGS__); \
        if(!(it->bytes>=0.0 && it->bytes<=SANE_MAX)){ \
            it->grade='U'; it->bytes=0.0; \
            snprintf(it->note,sizeof it->note,g_ascii?"out of plausible range - excluded" \
                                                     :"값 범위 벗어남 — 신뢰 불가, 합산 제외"); } } }while(0)

    /* Per item: on a read failure the grade drops to '?' so a 0 is not passed off as measured */
    /* An item at 0 was measured as 0, not missing - the screen says so */
    #define GRD (g_rd_ok?'M':'U')
    /* L4 count check: no negatives, and an upper bound from max_clients with 8x slack.
       Catches an offset that is aligned but wrong (entries far above the limit). */
    long mxcli=prm_get("max_clients"); if(mxcli<=0) mxcli=1000;
    #define CNT_OK(c) ((c)>=0)
    #define CNT_IN_RANGE(c) ((c)>=0 && (double)(c) <= (double)mxcli*8.0)
    if(B.sym[S_XCACHE]){
        unsigned long a=B.base+B.sym[S_XCACHE];
        g_rd_ok=1;
        int32_t ent=rd_i32(P->pid,a+g_bt->off_xc_entries);
        int32_t uc =rd_i32(P->pid,a+g_bt->off_xc_usage);
        int32_t ucl=rd_i32(P->pid,a+g_bt->off_xc_clone);
        ADD("plan cache (xcache)",(double)uc+ucl,GRD,"entries=%d, engine-tracked",ent);
    }
    /* terse keys are separate from screen labels: a Hangul label makes slugify produce
       an empty string and keys like `heapB._bytes`.  The screen changes with L() but
       keys must stay fixed ASCII or monitoring integrations break. */
    if(B.sym[S_QLIST]||B.sym[S_QPOOL]){
        g_rd_ok=1;
        int32_t np=B.sym[S_QLIST]?rd_i32(P->pid,B.base+B.sym[S_QLIST]+g_bt->off_qlist_npages):0;
        int32_t pe=B.sym[S_QPOOL]?rd_i32(P->pid,B.base+B.sym[S_QPOOL]+g_bt->off_qpool_nent):0;
        v=(double)np*g_pagesize + (double)pe*4096;
        /* The manual's term is "query result cache" (list cache is the internal name).
           The feature is off entirely when max_query_cache_entries=0 (the 11.5
           default), so the screen distinguishes "off, hence 0" from "on but empty". */
        { long qce=prm_get("max_query_cache_entries");
          if(qce==0)
              ADD("result cache (query)",0.0,GRD,g_ascii?"disabled (max_query_cache_entries=0)"
                                                   :"비활성 (max_query_cache_entries=0)");
          else
              ADD("result cache (query)",(CNT_OK(np)&&CNT_OK(pe))?v:-1.0,GRD,"pages=%d pool=%d",np,pe); }
    }
    if(B.sym[S_FPCNT]){
        g_rd_ok=1;
        int32_t c=rd_i32(P->pid,B.base+B.sym[S_FPCNT]);
        ADD("filter pred cache",CNT_OK(c)?(double)c*g_bt->sz_fpcache:-1.0,GRD,"counter=%d x%d",c,g_bt->sz_fpcache);
    }
    if(B.sym[S_LKGL] && g_bt->off_lk_ntran>=0){
        g_rd_ok=1;
        int32_t nt=rd_i32(P->pid,B.base+B.sym[S_LKGL]+g_bt->off_lk_ntran);
        /* lk_Gl.tran_lock_table = per-transaction lock slots: num_trans x 152B
           preallocated from the connection limit.  Not lockdb's "Object Lock
           Table" (an LK_ENTRY/LK_RES pool), so the label names its subject. */
        ADD("lock table (tran)",CNT_IN_RANGE(nt)?(double)nt*g_bt->sz_lk_tran:-1.0,GRD,
            g_ascii?"num_trans=%d x%dB (not object locks)":"num_trans=%d x%dB (객체락 아님)",nt,g_bt->sz_lk_tran);
    }
    if(B.sym[S_SESS]){
        g_rd_ok=1;
        int32_t c=rd_i32(P->pid,B.base+B.sym[S_SESS]+g_bt->off_lf_alloccnt);
        ADD("session table",CNT_IN_RANGE(c)?(double)c*g_bt->sz_session:-1.0,GRD,g_ascii?"alloc_cnt=%d x%dB, cap %ld":"alloc_cnt=%d x%dB, 한도 %ld",c,g_bt->sz_session,mxcli*8);
    }
    if(B.sym[S_CAT]){
        g_rd_ok=1;
        int32_t c=rd_i32(P->pid,B.base+B.sym[S_CAT]+g_bt->off_lf_alloccnt);
        ADD("catalog cache",CNT_OK(c)?(double)c*g_bt->sz_catalog:-1.0,GRD,"alloc_cnt=%d x%d",c,g_bt->sz_catalog);
    }
    if(B.sym[S_CONNMAX]){
        g_rd_ok=1;
        int32_t mx=rd_i32(P->pid,B.base+B.sym[S_CONNMAX]);
        int32_t ac=B.sym[S_CONNACT]?rd_i32(P->pid,B.base+B.sym[S_CONNACT]):0;
        ADD("conn entries (css)",CNT_IN_RANGE(mx)?(double)mx*g_bt->sz_conn:-1.0,GRD,"max=%d act=%d x%d",mx,ac,g_bt->sz_conn);
    }
    #undef GRD
    #undef CNT_OK
    #undef CNT_IN_RANGE
    if(B.sym[S_VACUUM]) ADD("vacuum data",(double)g_bt->sz_vacuum,'E',"struct only");
    #undef ADD
    /* Sum valid items only; read failures and out-of-range values are excluded, leaving the count */
    B.total=0; B.n_bad=0;
    for(int i=0;i<B.n;i++){
        if(B.it[i].grade=='U'){ B.n_bad++; continue; }
        B.total+=B.it[i].bytes;
    }
}

/* ---------------- Method A: glibc chunk histogram ---------------- */
#define CHUNK_MASK  (~(unsigned long)7)
#define CHUNK_ALIGN (2*sizeof(size_t)-1)   /* 15 on x86_64, 7 on i386; generalizes the alignment check */
static struct {
    long chunks; double total;
    double cat[7];               /* Category total */
    long trunc; double unwalked; /* Regions walked only up to the cap, and the bytes left out */
} A;
static const char *ACAT[7]={
    /* LEA_HEAP_BASE_SIZE (64KB): one mspace base per hl_register_lea_heap() call.
       A thread may create several, so the count does not match the thread count
       (measured 430 vs 151), hence "each" rather than "/thread". */
    "db_private heap base (64K each)","page-pair block (32K)","MVCC snapshot (est, 80B)",
    "large work block (>=1M)","query working mem (64K~1M)","working buffer (8K~64K)",
    "small/misc (<8K)"
};
static int classify(long chunk){
    long ud=chunk-16;
    if(ud==65536) return 0;
    if(ud==32768) return 1;
    if(ud==80)    return 2;
    if(chunk>=(1L<<20)) return 3;
    if(chunk>=(1L<<16)) return 4;
    if(chunk>=8192)     return 5;
    return 6;
}
/* Read one chunk header (8B) outside the probe window, for the verification hop only */
static unsigned long chunk_hdr(int pid,unsigned long s,size_t span,
                               const unsigned char *head,size_t hn,size_t o,int *rok){
    if(o+16<=hn){ *rok=1; return *(const unsigned long*)(head+o+8)&CHUNK_MASK; }
    if(o+16>span){ *rok=0; return 0; }
    unsigned long z=0;
    *rok=(vmread(pid,s+o+8,&z,8)==8);
    return z&CHUNK_MASK;
}
static void walk_region(int pid,unsigned long s,unsigned long e){
    /* Instead of copying the whole region up front (up to 768MB, 593MB peak measured):
       probe the first 128K, then move a 64K window along the chunk boundaries.
       Only 16B per chunk is needed, so the copy comes to a few windows. */
    size_t span=e-s;
    if(span>(768UL<<20)){            /* Bounded for time; say so, or the rest reads as "unexplained" */
        A.trunc++; A.unwalked+=(double)(span-(768UL<<20)); span=768UL<<20; }
    static unsigned char head[0x20000+64];
    static unsigned char win[1<<16];
    size_t hn=vmread(pid,s,head,span<sizeof head?span:sizeof head);
    if(hn<64) return;
    long start=-1;
    for(size_t off=0; off+64<hn && off<0x20000; off+=16){
        size_t o=off; int ok=1;
        for(int k=0;k<4;k++){
            int rok; unsigned long z=chunk_hdr(pid,s,span,head,hn,o,&rok);
            if(!rok||z<16||z>span||(z&CHUNK_ALIGN)){ ok=0; break; }
            o+=z;
        }
        if(ok){ start=(long)off; break; }
    }
    if(start<0) return;
    size_t o=(size_t)start, boff=0, blen=hn;
    const unsigned char *buf=head;
    long w=0;
    unsigned long prev_z=0;
    while(o+16<=span && w<5000000){
        if(o+16>boff+blen){
            /* If the previous chunk is larger than the window, fetch just the next 16B header.
               On a heap with many large chunks the windowed walk copies the whole heap
               and takes seconds (measured 1,617ms -> 165ms, same chunks and bytes). */
            size_t want = (prev_z>=4096) ? 16 : (span-o);
            if(want>sizeof win) want=sizeof win;
            blen=vmread(pid,s+o,win,want); boff=o; buf=win;
            if(blen<16) break;
        }
        unsigned long z=*(const unsigned long*)(buf+o-boff+8)&CHUNK_MASK;
        prev_z=z;
        if(z<16||z>span||(z&CHUNK_ALIGN)) break;
        A.total+=z; A.chunks++;
        A.cat[classify((long)z)]+=z;
        o+=z; w++;
    }
}
static const region_t *g_reg_db=NULL,*g_reg_lg=NULL;   /* Filled by classify_regions */

/* ---- Buffer pool BCB direct read (pgbuf) ----
 * Method B applied to an array: pgbuf_Pool.BCB_table -> read the whole BCB array,
 * then gather each entry's page header (prv: lsa/pageid/volid).  Latch-free, so
 * the snapshot is graded as an estimate.
 *   L4 self-check: disable if bad VPIDs exceed 1%, or if header (pageid,volid)
 *   disagrees with the BCB vpid beyond a threshold (pages under replacement
 *   legitimately differ a little).
 *   Cost: 512MB buffer = 32,768 BCB x 144B plus a 16B gather - a few ms.
 *   Diluted to one frame in N above 262,144 BCBs.
 *   --bcb-dump FILE writes the snapshot (volmap --bufmap overlays it).
 */
typedef struct { int64_t pageid; int offset; } lsa_t;   /* struct log_lsa: pageid:48 | offset:16 (little-endian bitfield) */
static lsa_t lsa_dec(uint64_t raw){ lsa_t l; l.pageid=((int64_t)(raw<<16))>>16; l.offset=(int)(int16_t)(raw>>48); return l; }
static int   lsa_isnull(lsa_t a){ return a.pageid==-1 && a.offset==-1; }
static int   lsa_cmp(lsa_t a,lsa_t b){ if(a.pageid!=b.pageid) return a.pageid<b.pageid?-1:1;
                                       return a.offset<b.offset?-1:(a.offset>b.offset); }
#define PGBUF_F_DIRTY    0x80000000u
#define PGBUF_F_FLUSHING 0x40000000u
#define PGBUF_ZONE_MASK  0x000F0000u   /* LRU1=1<<16 LRU2=2<<16 LRU3=3<<16 INVALID=1<<18 VOID=2<<18 */
typedef struct {            /* Dump record, fixed 32B little-endian: the contract shared with volmap */
    int16_t volid; int16_t zone;  /* zone: 1/2/3 = LRU, 4 = invalid, 8 = void */
    int32_t pageid; uint32_t flags; int32_t hdr_ok;   /* hdr_ok: the page header was read and matched */
    uint64_t page_lsa;        /* Raw LSA from the in-memory page header; comparing it with the disk copy shows whether it was flushed */
    uint64_t oldest_lsa;      /* Raw BCB.oldest_unflush_lsa: when it became dirty */
} pgrec_t;
/* The dump contract (32B) stays as it is and the BCB index the recheck needs lives in
   a memory-only array - growing the record would break the file contract with volmap. */
static int *pg_bidx=NULL; static int pg_bidx_cap=0;
#define PG_MAXVOL 256
static struct {
    int ok; char why[128];
    int nbuf, resident, dirty, flushing, z1,z2,z3, zvoid, zinv, mismatch, unread, badvpid;
    int mismatch_raw;   /* Mismatches before the recheck, including replacements between the two reads */
    int recheck;        /* Number rechecked */
    int degraded;       /* Frames where remaining mismatches crossed the threshold and dropped to valid-only mode */
    int tempvol;      /* Resident pages of temporary volumes (volid at or above PG_MAXVOL); this is normal */
    int vol_res[PG_MAXVOL], vol_dirty[PG_MAXVOL];
    lsa_t oldest_dirty; int have_oldest;
    lsa_t log_nxio, log_append, log_eof; int have_log;
    uint64_t raw_oldest, raw_nxio, raw_append, raw_eof;
    double ms; long frame; int every;
    pgrec_t *rec; int nrec, reccap;
    struct timespec when;
} PG;
static char g_bcb_dump[256]="";

/* Gather n scattered addresses, each bytes wide.  process_vm_readv takes up to 1024
   iovs at once; on a partial success (remote fault) the rest are read individually.
   okflag[i]=1 on success. */
static void vm_gather(int pid,const uint64_t*addr,int n,size_t each,unsigned char*out,unsigned char*okflag){
    memset(okflag,0,(size_t)n);
#ifdef __NR_process_vm_readv
    if(CAP.vm_readv){
        static struct iovec l[1024],r[1024];
        for(int i=0;i<n;i+=1024){
            int m=(n-i<1024)?(n-i):1024;
            for(int k=0;k<m;k++){ l[k].iov_base=out+(size_t)(i+k)*each; l[k].iov_len=each;
                                  r[k].iov_base=(void*)(uintptr_t)addr[i+k]; r[k].iov_len=each; }
            long got=syscall(__NR_process_vm_readv,pid,l,(unsigned long)m,r,(unsigned long)m,0UL);
            int okn = got>0 ? (int)((size_t)got/each) : 0;
            if(okn>m) okn=m;
            for(int k=0;k<okn;k++) okflag[i+k]=1;
            for(int k=okn;k<m;k++)
                okflag[i+k] = vmread(pid,(unsigned long)addr[i+k],out+(size_t)(i+k)*each,each)==(ssize_t)each;
        }
        return;
    }
#endif
    for(int i=0;i<n;i++)
        okflag[i] = vmread(pid,(unsigned long)addr[i],out+(size_t)i*each,each)==(ssize_t)each;
}

static int cap_turnover_from_dump(void);   /* Must be read as the baseline before the dump is overwritten */
static void pgbuf_dump(const char*db){
    if(!g_bcb_dump[0]||!PG.ok) return;
    char tmp[300]; snprintf(tmp,sizeof tmp,"%s.tmp",g_bcb_dump);
    FILE*f=fopen(tmp,"wb"); if(!f) return;
    struct { char magic[8]; uint32_t version, reclen; int64_t ts_sec, ts_nsec;
             int32_t nbuf, nrec, dirty, pagesize;
             uint64_t log_append, log_nxio, log_eof, oldest_dirty; char db[32]; } h;
    memset(&h,0,sizeof h); memcpy(h.magic,"CBCBMAP1",8); h.version=1; h.reclen=(uint32_t)sizeof(pgrec_t);
    h.ts_sec=PG.when.tv_sec; h.ts_nsec=PG.when.tv_nsec;
    h.nbuf=PG.nbuf; h.nrec=PG.nrec; h.dirty=PG.dirty; h.pagesize=(int32_t)g_pagesize;
    h.log_append=PG.have_log?PG.raw_append:~(uint64_t)0; h.log_nxio=PG.have_log?PG.raw_nxio:~(uint64_t)0;
    h.log_eof=PG.have_log?PG.raw_eof:~(uint64_t)0; h.oldest_dirty=PG.have_oldest?PG.raw_oldest:~(uint64_t)0;
    snprintf(h.db,sizeof h.db,"%s",db?db:"");
    int ok = fwrite(&h,sizeof h,1,f)==1 && (PG.nrec==0 || fwrite(PG.rec,sizeof(pgrec_t),(size_t)PG.nrec,f)==(size_t)PG.nrec);
    ok = (fclose(f)==0) && ok;
    if(ok) rename(tmp,g_bcb_dump); else unlink(tmp);
}

static void pgbuf_scan(proc_t*P){
    PG.frame++;
    if(!B.ok){ PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"method B off — %s":"방법 B 비활성 — %s",B.why); return; }
    if(B.partial||g_bt->pg.sz_bcb<0){ PG.ok=0;
        snprintf(PG.why,sizeof PG.why,g_ascii?"table lacks BCB offsets (probe mode or old offsets/*.tbl — re-run tools/collect-offsets.sh)"
                                            :"표에 BCB 오프셋 없음 (탐사 모드 또는 구 offsets/*.tbl — tools/collect-offsets.sh 재수집)");
        return; }
    if(PG.every>1 && PG.ok && (PG.frame%PG.every)!=1) return;   /* Large buffer: diluted, keeping the previous snapshot */
    double t0=now_ms();
    unsigned long pool=B.base+B.sym[S_PGBUF];
    int32_t nbuf=rd_i32(P->pid,pool+g_bt->off_pgbuf_nbuf);
    uint64_t tab=0;
    if(vmread(P->pid,pool+g_bt->pg.bcbtab,&tab,8)!=8 || !tab || nbuf<=0 || nbuf>(1<<26)){
        PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"BCB_table pointer unreadable":"BCB_table 포인터 읽기 실패"); return; }
    PG.every = nbuf>262144 ? (nbuf+262143)/262144 : 1;
    size_t szb=(size_t)g_bt->pg.sz_bcb, need=szb*(size_t)nbuf;
    static unsigned char*buf=NULL; static size_t cap=0;
    static uint64_t*addr=NULL; static unsigned char*hdr=NULL,*okf=NULL; static int acap=0;
    if(need>cap){ free(buf); buf=malloc(need); cap=buf?need:0; if(!buf){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    if(nbuf>pg_bidx_cap){ free(pg_bidx); pg_bidx=(int*)malloc(sizeof(int)*(size_t)nbuf);
                          pg_bidx_cap=pg_bidx?nbuf:0;
                          if(!pg_bidx){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    if(nbuf>acap){ free(addr); free(hdr); free(okf); free(PG.rec);
        addr=malloc(sizeof(uint64_t)*(size_t)nbuf); hdr=malloc(16*(size_t)nbuf); okf=malloc((size_t)nbuf);
        PG.rec=malloc(sizeof(pgrec_t)*(size_t)nbuf); acap=(addr&&hdr&&okf&&PG.rec)?nbuf:0; PG.reccap=acap;
        if(!acap){ PG.ok=0; snprintf(PG.why,sizeof PG.why,"malloc"); return; } }
    if(vmread(P->pid,(unsigned long)tab,buf,need)!=(ssize_t)need){
        PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"BCB array read failed (%d x %zuB)":"BCB 배열 읽기 실패 (%d x %zuB)",nbuf,szb); return; }
    /* pass 1: decode and aggregate */
    PG.nbuf=nbuf; PG.resident=PG.dirty=PG.flushing=PG.z1=PG.z2=PG.z3=PG.zvoid=PG.zinv=PG.badvpid=0;
    PG.tempvol=0;
    memset(PG.vol_res,0,sizeof PG.vol_res); memset(PG.vol_dirty,0,sizeof PG.vol_dirty);
    PG.have_oldest=0; PG.nrec=0;
    const int o_vpid=g_bt->pg.vpid, o_flags=g_bt->pg.flags, o_olsa=g_bt->pg.olsa, o_iopg=g_bt->pg.iopg;
    for(int i=0;i<nbuf;i++){
        const unsigned char*b=buf+(size_t)i*szb;
        int32_t pageid; int16_t volid; uint32_t flags; uint64_t olsa, iop;
        memcpy(&pageid,b+o_vpid,4); memcpy(&volid,b+o_vpid+4,2); memcpy(&flags,b+o_flags,4);
        memcpy(&olsa,b+o_olsa,8); memcpy(&iop,b+o_iopg,8);
        uint32_t zone=flags&PGBUF_ZONE_MASK;
        if(zone==0x10000) PG.z1++; else if(zone==0x20000) PG.z2++; else if(zone==0x30000) PG.z3++;
        else if(zone==0x40000) PG.zinv++; else if(zone==0x80000) PG.zvoid++;
        if(volid==-1 || pageid==-1) continue;                 /* Empty BCB (NULL VPID) */
        if(volid<0 || pageid<0){ PG.badvpid++; continue; }
        /* Temporary volumes have large volids (32766); permanent ones are 0..n.  Counting
           these as abnormal crosses the 1% threshold and switches the whole buffer pool
           view off.  They cannot
           go in the per-volume array, so they are counted only in the total. */
        int vol_ok = (volid<PG_MAXVOL);
        if(!vol_ok) PG.tempvol++;
        PG.resident++; if(vol_ok) PG.vol_res[volid]++;
        if(flags&PGBUF_F_DIRTY){
            PG.dirty++; if(vol_ok) PG.vol_dirty[volid]++;
            lsa_t l=lsa_dec(olsa);
            if(!lsa_isnull(l) && (!PG.have_oldest || lsa_cmp(l,PG.oldest_dirty)<0)){ PG.oldest_dirty=l; PG.raw_oldest=olsa; PG.have_oldest=1; }
        }
        if(flags&PGBUF_F_FLUSHING) PG.flushing++;
        pgrec_t*r=&PG.rec[PG.nrec];
        r->volid=volid; r->zone=(int16_t)(zone>>16); r->pageid=pageid; r->flags=flags; r->hdr_ok=0;
        r->page_lsa=~(uint64_t)0; r->oldest_lsa=olsa;
        addr[PG.nrec]=iop+(uint64_t)g_bt->pg.iobuf_iopg;
        if(pg_bidx && PG.nrec<pg_bidx_cap) pg_bidx[PG.nrec]=i;   /* BCB index for the recheck */
        PG.nrec++;
    }
    if(PG.badvpid>nbuf/100){ PG.ok=0;
        snprintf(PG.why,sizeof PG.why,g_ascii?"abnormal VPIDs: %d — offset mismatch suspected":"비정상 VPID %d개 — 오프셋 불일치 의심",PG.badvpid); return; }
    /* pass 2: gather the page headers (prv 16B: lsa 8, pageid 4, volid 2, ptype/pflag 2) and compare */
    PG.mismatch=PG.unread=0; PG.mismatch_raw=0; PG.recheck=0; PG.degraded=0;
    if(PG.nrec>0){
        vm_gather(P->pid,addr,PG.nrec,16,hdr,okf);
        /* Collect the mismatched indices for the recheck.  addr[] is overwritten there, so
           the original iopage addresses are kept alongside. */
        static int *midx=NULL; static uint64_t *maddr=NULL; static int mcap=0;
        int nm=0;
        if(PG.nrec>mcap){ free(midx); free(maddr);
            midx=(int*)malloc(sizeof(int)*(size_t)PG.nrec);
            maddr=(uint64_t*)malloc(sizeof(uint64_t)*(size_t)PG.nrec);
            mcap=(midx&&maddr)?PG.nrec:0; }
        for(int k=0;k<PG.nrec;k++){
            pgrec_t*r=&PG.rec[k];
            if(!okf[k]){ PG.unread++; continue; }
            uint64_t lsa; int32_t hp; int16_t hv;
            memcpy(&lsa,hdr+(size_t)k*16,8); memcpy(&hp,hdr+(size_t)k*16+8,4); memcpy(&hv,hdr+(size_t)k*16+12,2);
            if(hp!=r->pageid || hv!=r->volid){
                PG.mismatch_raw++;
                if(mcap && nm<mcap){ midx[nm]=k; maddr[nm]=addr[k]; nm++; }
                continue;
            }
            r->page_lsa=lsa; r->hdr_ok=1;
        }
        /* Recheck.  Pass 1 (BCB array) and pass 2 (page headers) are taken at different
           times, so pages replaced in between necessarily disagree; a flat 5%
           threshold could not tell that from a wrong offset and disabled 40% of
           frames under traffic.  The recheck re-pairs only the mismatches: a
           replacement resolves, a wrong offset stays 100% mismatched.  Cost is
           proportional to the mismatch count alone. */
        if(nm>0 && mcap){
            static unsigned char *rvp=NULL, *rhd=NULL, *rok=NULL; static int rcap=0;
            if(nm>rcap){ free(rvp); free(rhd); free(rok);
                rvp=(unsigned char*)malloc(8*(size_t)nm);      /* vpid 8B(pageid4+volid2+pad2) */
                rhd=(unsigned char*)malloc(16*(size_t)nm);
                rok=(unsigned char*)malloc((size_t)nm*2);
                rcap=(rvp&&rhd&&rok)?nm:0; }
            if(rcap>=nm){
                /* Re-read those BCBs' vpids now */
                static uint64_t *va=NULL; static int vcap=0;
                if(nm>vcap){ free(va); va=(uint64_t*)malloc(sizeof(uint64_t)*(size_t)nm); vcap=va?nm:0; }
                if(vcap>=nm){
                    for(int j=0;j<nm;j++){
                        /* BCB element address = tab + i*szb, where i is the original BCB index, not the rec
                           index: rec skips empty BCBs, so it cannot be derived from the
                           iopage address - the BCB index is kept in rec (bidx). */
                        va[j]=(uint64_t)tab+(uint64_t)pg_bidx[midx[j]]*(uint64_t)szb+(uint64_t)o_vpid;
                    }
                    vm_gather(P->pid,va,nm,8,rvp,rok);
                    /* Re-read the same BCBs' iopage headers too (addresses unchanged) */
                    vm_gather(P->pid,maddr,nm,16,rhd,rok+nm);
                    for(int j=0;j<nm;j++){
                        PG.recheck++;
                        if(!rok[j]||!rok[nm+j]){ PG.mismatch++; continue; }
                        int32_t npid; int16_t nvid;
                        memcpy(&npid,rvp+(size_t)j*8,4); memcpy(&nvid,rvp+(size_t)j*8+4,2);
                        uint64_t lsa; int32_t hp; int16_t hv;
                        memcpy(&lsa,rhd+(size_t)j*16,8); memcpy(&hp,rhd+(size_t)j*16+8,4); memcpy(&hv,rhd+(size_t)j*16+12,2);
                        if(hp==npid && hv==nvid && npid>=0){
                            /* It was a timing gap: update with the new values and keep this record */
                            pgrec_t*r=&PG.rec[midx[j]];
                            r->pageid=npid; r->volid=nvid; r->page_lsa=lsa; r->hdr_ok=1;
                        } else PG.mismatch++;      /* Still wrong after the recheck: a genuine mismatch */
                    }
                } else for(int j=0;j<nm;j++) PG.mismatch++;
            } else PG.mismatch=PG.mismatch_raw;
        }
        /* Judge on the mismatches remaining after the recheck; a wrong offset cannot pass a
           recheck, so safety is unchanged.  Above the threshold the snapshot is not
           discarded but degraded to a verified-records-only mode with the reason kept
           (dirty state and zone distribution are valid from vpid alone). */
        /* Unread entries count toward the gate too.  Without that, a wrong iopage_buffer
           offset failing all 32,768 reads still passed as healthy (a deliberate
           corruption test returned enabled=1).  Reading no header at all empties
           the LSA axis, so it is treated as an offset error. */
        if(PG.nrec>100 && PG.unread>PG.nrec/2){ PG.ok=0;
            snprintf(PG.why,sizeof PG.why,
                     g_ascii?"page headers unreadable %d/%d — iopage offset mismatch suspected"
                            :"페이지 헤더 읽기 실패 %d/%d — iopage 오프셋 불일치 의심",PG.unread,PG.nrec);
            return; }
        if(PG.nrec>100 && PG.mismatch>PG.nrec/20){
            if(PG.mismatch>PG.nrec/2){ PG.ok=0;
                snprintf(PG.why,sizeof PG.why,
                         g_ascii?"page header vs BCB mismatch %d/%d after recheck — offset mismatch suspected"
                                :"재검 후에도 페이지 헤더·BCB 불일치 %d/%d — 오프셋 불일치 의심",PG.mismatch,PG.nrec);
                return; }
            PG.degraded=1;
        }
    }
    /* Log tail, the upper bound of WAL continuity: append >= nxio (the disk boundary) >= page LSA */
    PG.have_log=0;
    if(B.sym[S_LOGGL] && g_bt->pg.log_nxio>=0){
        unsigned long lg=B.base+B.sym[S_LOGGL]; uint64_t a=0,n=0,e=0;
        if(vmread(P->pid,lg+g_bt->pg.log_append,&a,8)==8 && vmread(P->pid,lg+g_bt->pg.log_nxio,&n,8)==8
           && vmread(P->pid,lg+g_bt->pg.log_eof,&e,8)==8){
            PG.raw_append=a; PG.raw_nxio=n; PG.raw_eof=e;
            PG.log_append=lsa_dec(a); PG.log_nxio=lsa_dec(n); PG.log_eof=lsa_dec(e);
            /* Check: pageid must be non-negative and append >= nxio */
            if(PG.log_append.pageid>=0 && PG.log_nxio.pageid>=0 && lsa_cmp(PG.log_append,PG.log_nxio)>=0) PG.have_log=1;
        }
    }
    PG.ok=1;
    if(PG.degraded)
        snprintf(PG.why,sizeof PG.why,
                 g_ascii?"%d slots; %d/%d unverified after recheck — counted from verified records only"
                        :"슬롯 %d개; 재검 후 %d/%d 미검증 — 검증된 레코드만 집계",
                 nbuf,PG.mismatch,PG.nrec);
    else if(PG.recheck>0)
        snprintf(PG.why,sizeof PG.why,
                 g_ascii?"%d page slots checked (%d rechecked, %d residual)"
                        :"페이지 슬롯 %d개 확인 (재검 %d건, 잔여 %d건)",
                 nbuf,PG.recheck,PG.mismatch);
    else
        snprintf(PG.why,sizeof PG.why,g_ascii?"%d page slots checked":"페이지 슬롯 %d개 확인",nbuf);
    PG.ms=now_ms()-t0; clock_gettime(CLOCK_REALTIME,&PG.when);
    /* Order matters: pgbuf_dump overwrites the --bcb-dump file, so a one-shot run must
       read the turnover baseline BEFORE that, or the baseline becomes this very frame
       and turnover is always 0. */
    (void)cap_turnover_from_dump();
    pgbuf_dump(P->db);
}
/* Formatting helper */
static const char* lsa_str(lsa_t l){ static char ring[6][32]; static int k=0; char*o=ring[k++%6];
    if(lsa_isnull(l)) snprintf(o,32,"-"); else snprintf(o,32,"%lld|%d",(long long)l.pageid,l.offset); return o; }

static void method_a(proc_t *P){
    memset(&A,0,sizeof A);
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        int anon = (r->path[0]==0 || !strcmp(r->path,"[heap]"));
        if(!anon) continue;
        if(!strchr(r->perm,'r')||!strchr(r->perm,'w')) continue;
        /* The data and log buffer mappings are already attributed as regions; counting them as heap would double-count */
        if(r==g_reg_db||r==g_reg_lg) continue;
        walk_region(P->pid,r->start,r->end);
    }
}

/* ---------------- Classify cub_server regions into six kinds ---------------- */
typedef struct {
    char name[20]; double mapped,rss,ref; long long cfg; char grade;
    double prev_mapped;         /* grow for view 3 (over configured), tied to the allocation-over-configuration axis */
    double prev_rss;            /* Previous-frame baseline (live) for the grow column */
    double prev_ref;            /* Previous-frame hot baseline, for view 3's hot delta column */
} srg_t;
static srg_t SR[8]; static int NSR=0;
static double g_dyn_rss=0,g_dyn_mapped=0;

/* A .so is identified by a ".so" suffix or a ".so." substring; the older
   strstr(".so") and "/lib" tests matched .sock files and locale-archive. */
static int path_is_so(const char *p){
    size_t n=strlen(p);
    if(n>=3 && !strcmp(p+n-3,".so")) return 1;
    return strstr(p,".so.")!=NULL;
}
static int path_is_heap(const char *p){ return !strcmp(p,"[heap]"); }

static void classify_regions(proc_t *P){
    double code_m=0,code_r=0,code_f=0,arena_m=0,stk_m=0,stk_r=0,stk_f=0;
    double oth_m=0,oth_r=0,oth_f=0;   /* Executable segments, message catalogues, /dev/shm and the like */
    region_t *db=NULL,*lg=NULL,*prev=NULL;
    long long cfg_db=prm_get("data_buffer_size"), cfg_lg=prm_get("log_buffer_size");
    /* Match large anonymous rw regions against the configured size (100-140%); [heap] is not a buffer candidate */
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        if(r->path[0]||!strchr(r->perm,'w')) continue;
        double sz=(double)(r->end-r->start);
        if(!db && cfg_db>0 && sz>=cfg_db && sz<=cfg_db*1.4) { db=r; continue; }
        if(!lg && cfg_lg>0 && sz>=cfg_lg && sz<=cfg_lg*1.4) { lg=r; continue; }
    }
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        double sz=(double)(r->end-r->start);
        int guard = prev && prev->path[0]==0 && !strchr(prev->perm,'w') && !strchr(prev->perm,'r');
        if(r->path[0] && path_is_so(r->path)){
            code_m+=sz; code_r+=r->rss_kb*1024.0; code_f+=r->ref_kb*1024.0; }
        else if(r->path[0]==0 && !strchr(r->perm,'w') && !strchr(r->perm,'r')) arena_m+=sz;
        else if(r->path[0]==0 && strchr(r->perm,'r') && strchr(r->perm,'w')
                && guard && (prev->end-prev->start)<(1UL<<20)
                && sz>=8384512.0 && sz<=8388608.0){
            r->is_stack=1;                                                 /* pthread stacks */
            stk_m+=sz; stk_r+=r->rss_kb*1024.0; stk_f+=r->ref_kb*1024.0;
        }
        else if(!strncmp(r->path,"[stack",6)){ stk_m+=sz; stk_r+=r->rss_kb*1024.0; stk_f+=r->ref_kb*1024.0; }
        else if(r->path[0] && !path_is_heap(r->path)){
            oth_m+=sz; oth_r+=r->rss_kb*1024.0; oth_f+=r->ref_kb*1024.0; }
        prev=r;
    }
    /* Dynamic heap = unmatched anonymous rw + [heap] - pattern stacks.
       Excluding [heap] (the glibc main arena) leaves tens of MB unattributed.
       Stacks are excluded by the is_stack marking rather than by size, so a genuine
       8MB heap block without a guard is not dropped. */
    double dyn_m=0,dyn_r=0,dyn_f=0;
    for(int i=0;i<P->nreg;i++){
        region_t *r=&P->reg[i];
        int anon = (r->path[0]==0 || path_is_heap(r->path));
        if(!anon||!strchr(r->perm,'w')||!strchr(r->perm,'r')) continue;
        if(r==db||r==lg||r->is_stack) continue;
        dyn_m+=(double)(r->end-r->start); dyn_r+=r->rss_kb*1024.0; dyn_f+=r->ref_kb*1024.0;
    }
    g_reg_db=db; g_reg_lg=lg;   /* Exposed so method_a does not double-count */
    NSR=0;
    #define PUT(nm_,mp_,rs_,rf_,cfg_,gr_) do{ if(NSR<8){ srg_t*x=&SR[NSR++]; \
        snprintf(x->name,sizeof x->name,"%s",nm_); x->mapped=(mp_); x->rss=(rs_); \
        x->ref=(rf_); x->cfg=(cfg_); x->grade=(gr_);} }while(0)
    if(db) PUT("data_buffer",(double)(db->end-db->start),db->rss_kb*1024.0,db->ref_kb*1024.0,cfg_db,'M');
    if(lg) PUT("log_buffer", (double)(lg->end-lg->start),lg->rss_kb*1024.0,lg->ref_kb*1024.0,cfg_lg,'M');
    /* When A/B explain more than half the dynamic heap the grade is "estimated", not
       "unattributed": leaving the largest row permanently at '?' reads as no
       decomposition at all. */
    PUT("dynamic-heap",dyn_m,dyn_r,dyn_f,-1,'U');
    PUT("thread stacks",stk_m,stk_r,stk_f,-1,'R');
    PUT("code (.so)",code_m,code_r,code_f,-1,'M');
    if(oth_m>0) PUT("bin/shm etc",oth_m,oth_r,oth_f,-1,'M');
    PUT("glibc arena",arena_m,0,0,-1,'R');
    #undef PUT
    g_dyn_rss=dyn_r; g_dyn_mapped=dyn_m;
}

/* Look up region values by name.  SR[] is filled conditionally (if(db)/if(lg)), so
   indexing SR[0]/SR[1]/SR[2] silently picks up another region's value when
   data_buffer was not detected - the tree, dashboard, plots and terse all go
   through this helper so they cannot disagree. */
static const srg_t *sr_find(const char *nm){
    for(int i=0;i<NSR;i++) if(!strcmp(SR[i].name,nm)) return &SR[i];
    return NULL;
}
static double sr_rss(const char *nm){ const srg_t*x=sr_find(nm); return x?x->rss:0.0; }
static double sr_mapped(const char *nm){ const srg_t*x=sr_find(nm); return x?x->mapped:0.0; }
static double sr_hotpct(const char *nm){
    const srg_t*x=sr_find(nm);
    return (x && x->rss>0) ? x->ref/x->rss*100.0 : 0.0;
}

/* Bumped whenever the target process changes (switch or restart).  Baselines kept
   across frames - engine counters, the turnover page set, per-item deltas - belong
   to one process; compared against another they yield nonsense rates. */
static unsigned g_inst_gen=0;
/* Must be called when switching instances.  g_reg_db/g_reg_lg point into P->reg[],
   so keeping the previous instance's pointers makes method_a exclude the wrong
   region and double-count.  SR[]/NSR, B and A are cleared too, so the next frame
   recomputes against the new target. */
static void reset_instance_state(void){
    g_inst_gen++;
    g_hprevn=0;                        /* heap item deltas */
    g_reg_db=NULL; g_reg_lg=NULL;
    NSR=0; memset(SR,0,sizeof SR);
    g_dyn_rss=0; g_dyn_mapped=0;
    memset(&B,0,sizeof B);
    memset(&A,0,sizeof A);
    /* cubrid.conf parameters are per database too ([@db] sections differ), so without
       clearing them the previous instance's values make a verdict like "configured
       512M" entirely wrong. */
    g_nprm=0;
}

/* Re-grade dynamic-heap once the decomposition is in (called after method_a/b) */
static void grade_dynamic_heap(void){
    for(int i=0;i<NSR;i++){
        if(strcmp(SR[i].name,"dynamic-heap")) continue;
        if(SR[i].rss<=0) return;
        double explained=B.total+A.total;
        if(explained>=SR[i].rss*0.5) SR[i].grade='E';   /* Estimated */
    }
}

/* ---------------- Process tier totals ---------------- */
/* tier_t is defined earlier in the file, for the combined enum_instances pass */
/* Cumulative process CPU ticks; the delta comes from the previous value found by name and pid. */
#define TCPUMAX 64
static struct { int pid; unsigned long ticks; } g_tcpu[TCPUMAX];
static int g_tcpun=0;
static unsigned long proc_ticks(int pid){
    char p[64],b[512]; snprintf(p,sizeof p,"/proc/%d/stat",pid);
    if(read_file(p,b,sizeof b)<=0) return 0;
    char *q=strrchr(b,')'); if(!q) return 0;
    q+=2; unsigned long f[24]; int n=0;
    char *tok=strtok(q," ");
    while(tok&&n<24){ f[n++]=strtoul(tok,NULL,10); tok=strtok(NULL," "); }
    return (n>12)? f[11]+f[12] : 0;      /* field14+15 */
}
static double tcpu_delta(int pid,double dt){
    unsigned long cur=proc_ticks(pid), prev=0; int hit=-1;
    for(int i=0;i<g_tcpun;i++) if(g_tcpu[i].pid==pid){ prev=g_tcpu[i].ticks; hit=i; break; }
    if(hit<0){
        if(g_tcpun>=TCPUMAX){
            /* Full: drop entries whose process is gone.  Without this the table
               stays full of dead CAS pids and every new process reports 0 cores
               for the rest of the run - CAS churn reaches TCPUMAX on a busy
               broker tier well before the concurrent limit does. */
            int w=0;
            for(int i=0;i<g_tcpun;i++){
                char sp[64]; snprintf(sp,sizeof sp,"/proc/%d",g_tcpu[i].pid);
                if(!access(sp,F_OK)) g_tcpu[w++]=g_tcpu[i];
            }
            g_tcpun=w;
            if(g_tcpun>=TCPUMAX) g_tcpun=0;   /* all still live: start over rather than freeze */
        }
        if(g_tcpun<TCPUMAX){ g_tcpu[g_tcpun].pid=pid; g_tcpu[g_tcpun].ticks=cur; g_tcpun++; }
        return 0.0; }
    g_tcpu[hit].ticks=cur;
    long hz=sysconf(_SC_CLK_TCK); if(hz<=0) hz=100;
    if(cur<prev||dt<=0) return 0.0;
    return (double)(cur-prev)/hz/dt;      /* Core count */
}
static void scan_tiers(tier_t *T,double dt){
    /* The work is done by enum_instances pass 2 (one /proc walk and one rollup per
       frame).  This wrapper skips a second walk when the same frame already counted. */
    if(g_enum_T==T) return;            /* The previous enum filled this T */
    memset(T,0,sizeof *T);
    g_enum_T=T; g_enum_dt=dt;
    enum_instances();
    g_enum_T=NULL;
}

/* ---- cubrid.conf parameters that consume memory ----
 * Shows the configured value regardless of whether usage is observable; tuning
 * advice and thresholds need that baseline.
 * obs: 1 if this tool also measures actual usage, 0 if it knows only the setting. */
/* unit: 'B' bytes (human), 'P' pages, 'N' a plain count */
typedef struct { const char *name, *desc, *desc_en; int obs; char unit; } prmrow_t;
static const prmrow_t PRMROWS[]={
 {"data_buffer_size",              "페이지 버퍼",                "page buffer",                1,'B'},
 {"log_buffer_size",               "로그 버퍼",                  "log buffer",                 1,'B'},
 {"double_write_buffer_size",      "DWB(이중 쓰기 버퍼)",        "DWB (double write buffer)",  0,'B'},
 {"sort_buffer_size",              "정렬 버퍼 — 세션·연산당",    "sort buffer — per session·op",0,'B'},
 {"temp_file_memory_size_in_pages","임시파일 메모리",            "temp-file memory",           0,'P'},
 {"max_plan_cache_entries",        "질의계획 캐시 항목 수",      "plan cache entries",         0,'N'},
 {"max_query_cache_entries",       "질의결과 캐시 항목 수",      "query cache entries",        0,'N'},
 {"max_filter_pred_cache_entries", "필터술어 캐시 항목 수",      "filter pred cache entries",  0,'N'},
 {"index_scan_oid_buffer_size",    "인덱스 스캔 OID 버퍼",       "index scan OID buffer",      0,'B'},
 {"index_scan_key_buffer_size",    "인덱스 스캔 키 버퍼",        "index scan key buffer",      0,'B'},
 {"thread_stacksize",              "스레드 스택 — HA 경로 전용", "thread stack — HA path only",0,'B'},
 {"max_clients",                   "최대 접속 수",               "max clients",                0,'N'},
};
#define NPRMROWS ((int)(sizeof PRMROWS/sizeof PRMROWS[0]))

/* ---------------- Render: drill-down tree ---------------- */
static const char *GR(char g){
    switch(g){ case 'M': return "●"; case 'R': return "◐";
               case 'E': return "◌"; default: return "?"; }
}

#define RCHAR_MIN (64.0*1024.0)   /* Floor below which the absorb denominator is meaningless (64KB/s) */
/* ---- Live dashboard: canvas / braille / TUI ----
 * Six-dot glyph gauges (foreground = lens metric, background = hot), four value
 * columns alloc|rss|hot|chg, three lenses, ~3 minute window, 24h time ruler.
 */
#define CV_W 120   /* Ceiling of 114 plus slack */
/* Plot mode is the tallest: nine panels (53 rows) plus the status line and the
   bottom ruler and key hints.  Overflow silently drops whole panels from the
   bottom, so leave margin (at 56 three new panels vanished). */
#define CV_H 80
typedef struct { int w,h; char ch[CV_H][CV_W][5]; char co[CV_H][CV_W][20]; } canvas_t;
static canvas_t CV;

static int g_argc=0; static char **g_argv=NULL;
static double g_interval=0.5;   /* For displaying the hot window */
/* Method A takes time proportional to heap size (10ms small, 58-508ms on a 7GB heap
   under load).  Automatic mode targets one second but never runs more often than three
   times the last measurement - self-protection. */
static int g_a_auto=0; static double g_a_dur=0.0;
/* Signals that method A must rerun immediately after a restart.  A runs periodically,
   so otherwise a dead server's measurement survives until the next period. */
static int g_a_redo=0;
/* Wall-clock time of the last method A measurement.  An elapsed count ("184s ago")
   has to be subtracted mentally and grows the longer the screen is up, so the time
   itself is shown.  Zero means not yet measured. */
static time_t g_a_when=0;
#define A_TARGET 1.0
#define A_DUTY   3.0
static double a_period(void){ double p=g_a_dur*A_DUTY; return p>A_TARGET?p:A_TARGET; }
/* g_ascii is defined earlier in the file; method_b's reason strings reference it */
/* Hangul label to English; the tree and terse outside the canvas are unaffected */
static const char* L(const char*ko){
    static const char *M[][2]={
      {"OS 컨텍스트","os context"},
      /* Symmetric with the process box's "(PSS)": the two boxes' axes (apportioned shared
         vs full physical) differ in the title itself, so the gap between the total and
         PSS is less likely to be misread. */
      {"RSS · 서버","RSS · server"},
      {"성장 추이","growth"},{"PSS · 프로세스","PSS · processes"},
      {"I/O · cub_server + 최다사용 장치","i/o · cub_server + busiest device"},
      {"동적 메모리 상세 (B) — 자동 갱신","dynamic heap · method B — per frame"},
      {"호스트 CPU · 사용 코어 · 포화 코어 · load","host CPU · busy cores · saturated · load"},
      {"I/O 대기 · iowait · D스레드 · blkio","I/O wait · iowait · D threads · blkio"},
      {"티어별 CPU(코어) · master · pl · broker · CAS","per-tier CPU (cores) · master · pl · broker · CAS"},
      {"포화=95% 이상","saturated = >=95%"},
      {"판정문 근거","verdict evidence"},
      {"server 는 위 활동 패널","server: activity panel above"},
      {"디스크 읽기","disk read"},{"디스크 쓰기","disk write"},{"캐시 흡수율","cache absorb"},
      {"유휴","idle"},{"B 합계(정확)","B total (exact)"},{"A 합계(청크)","A total (chunks)"},{"A 합계(청크·일부)","A total (chunks, partial)"},
      {"미설명 잔여","unexplained"},{"과대계상(free+비상주)","overcount(free+nonres)"},
      {"방법 B 비활성","method B disabled"},{"힙 순회 중…","walking heap…"},
      {"[a] 로 방법 A 분석","[a] run method A"},{"장치","dev"},{"쓰기","write"},{"누적","total"},{"읽기","read"},
      {"전체 메모리 대비 점유","share of system RAM"},
      {"상주 구성 · server = data_buffer + log_buffer + dynamic-heap + 기타","resident composition"},
      {"할당(VmSize) vs 상주(VmRSS) · 간격 = 예약만 하고 안 쓰는 분량","reserved (VmSize) vs resident (VmRSS)"},
      {"활동 · CPU · data_buffer 핫% · 폴트/s","activity · cpu · data_buffer hot% · faults/s"},
      {"디스크 I/O","disk i/o"},
      {"I/O 포화 · 장치 사용률 · 캐시 흡수율","i/o saturation · device util · cache absorb"},
      {"표본이 부족합니다 — 선을 그리려면 2개 이상 필요","not enough samples — need 2+ to draw a line"},
      {"측정 불가 구간은 0 으로 표기","unmeasurable spans drawn as 0"},
      {"핫 측정 꺼짐(--no-hot) — 생략","hot disabled (--no-hot) — omitted"},
      {"계열별 정규화(단위 상이)","normalized per series"},
      {"메이저=디스크","major=disk"}};
    if(!g_ascii) return ko;
    for(unsigned i=0;i<sizeof M/sizeof M[0];i++) if(!strcmp(ko,M[i][0])) return M[i][1];
    return ko;
}
static const int GRAD[11]={46,82,118,154,190,226,220,214,208,202,196};
#define HOTBG 18
static int grad_of(double f){ int i=(int)(f*10.0); if(i<0)i=0; if(i>10)i=10; return GRAD[i]; }

/* Byte length of one UTF-8 character.  A sequence counts as multi-byte only when its
   continuation bytes are really there: labels cut with %.Ns in bytes can end in a
   split syllable, and the terminating NUL must not be stepped over. */
static int u8len(const char*s){
    unsigned char c=(unsigned char)*s;
    int n = c<0x80 ? 1 : (c>>5)==6 ? 2 : (c>>4)==14 ? 3 : (c>>3)==30 ? 4 : 1;
    for(int k=1;k<n;k++) if(((unsigned char)s[k]&0xC0)!=0x80) return 1;
    return n;
}
static int u8cp(const char*s,int len){
    unsigned char c=(unsigned char)s[0];
    if(len==1) return c;
    if(len==2) return ((c&0x1f)<<6)|((unsigned char)s[1]&0x3f);
    if(len==3) return ((c&0x0f)<<12)|(((unsigned char)s[1]&0x3f)<<6)|((unsigned char)s[2]&0x3f);
    return ((c&0x07)<<18)|(((unsigned char)s[1]&0x3f)<<12)|(((unsigned char)s[2]&0x3f)<<6)|((unsigned char)s[3]&0x3f);
}
/* Wide-character test (Hangul, CJK, fullwidth symbols): the core of canvas width arithmetic */
static int u8wide(int cp){
    return (cp>=0x1100&&cp<=0x115F)||(cp>=0x2E80&&cp<=0xA4CF)||
           (cp>=0xAC00&&cp<=0xD7A3)||(cp>=0xF900&&cp<=0xFAFF)||
           (cp>=0xFE30&&cp<=0xFE6F)||(cp>=0xFF00&&cp<=0xFF60)||
           (cp>=0xFFE0&&cp<=0xFFE6)||(cp>=0x20000&&cp<=0x3FFFD);
}
static int disp_w(const char*s){
    int w=0; while(*s){ int l=u8len(s); w+=u8wide(u8cp(s,l))?2:1; s+=l; } return w;
}
static void cv_init(int w,int h){
    if(w>CV_W)w=CV_W;
    if(h>CV_H)h=CV_H;
    CV.w=w; CV.h=h;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){ CV.ch[y][x][0]=' '; CV.ch[y][x][1]=0; CV.co[y][x][0]=0; }
}
static void cv_put(int x,int y,const char*s,const char*col){
    if(y<0||y>=CV.h) return;
    int cx=x;
    while(*s && cx<CV.w){
        int l=u8len(s); int wd=u8wide(u8cp(s,l))?2:1;
        if(cx+wd>CV.w) break;
        if(cx>=0){
            /* A wide character marks its second cell consumed, and flush skips it.  If a
               later half-width glyph overwrites only the first cell, the second
               stays marked, drops out of flush and shifts the row by one cell.
               So writing half-width revives a consumed cell as a space. */
            if(wd==1 && cx+1<CV.w && !CV.ch[y][cx+1][0]){
                CV.ch[y][cx+1][0]=' '; CV.ch[y][cx+1][1]=0; CV.co[y][cx+1][0]=0;
            }
            if(l==1 && (unsigned char)*s>=0x80){ CV.ch[y][cx][0]='?'; CV.ch[y][cx][1]=0; }   /* a broken sequence */
            else { int n=l<4?l:4; memcpy(CV.ch[y][cx],s,n); CV.ch[y][cx][n]=0; }
            if(col) snprintf(CV.co[y][cx],sizeof CV.co[y][cx],"%s",col); else CV.co[y][cx][0]=0;
            if(wd==2 && cx+1<CV.w){ CV.ch[y][cx+1][0]=0; CV.co[y][cx+1][0]=0; }  /* Cell consumed by a wide character */
        }
        s+=l; cx+=wd;
    }
}
/* Truncate to display width n with no padding, so a title cannot overrun the box corner */
static const char* cutf(const char*s,int n){
    static char ring[4][256]; static int k=0;
    char*o=ring[k++&3]; int acc=0; size_t p=0;
    while(*s && p+4<sizeof ring[0]){
        int l=u8len(s), wd=u8wide(u8cp(s,l))?2:1;
        if(acc+wd>n) break;
        for(int i=0;i<l&&s[i];i++) o[p++]=s[i];
        s+=l; acc+=wd;
    }
    o[p]=0; return o;
}
/* Build a left- or right-aligned string by display width */
static const char* padf(const char*s,int n,int right){
    static char ring[8][256]; static int k=0;
    char*o=ring[k++&7]; int w=disp_w(s);
    if(w>=n){ snprintf(o,256,"%s",s); return o; }
    int sp=n-w; if(sp>200) sp=200;
    if(right){ memset(o,' ',sp); snprintf(o+sp,256-sp,"%s",s); }
    else { int L=snprintf(o,256,"%s",s); if(L>200)L=200; memset(o+L,' ',sp); o[L+sp]=0; }
    return o;
}
static void cv_box2(int x,int y,int w,int h,const char*title,const char*rtitle){
    const char*bc="38;5;240";
    cv_put(x,y,"╭",bc); cv_put(x+w-1,y,"╮",bc);
    for(int i=1;i<w-1;i++){ cv_put(x+i,y,"─",bc); cv_put(x+i,y+h-1,"─",bc); }
    cv_put(x,y+h-1,"╰",bc); cv_put(x+w-1,y+h-1,"╯",bc);
    for(int yy=y+1;yy<y+h-1;yy++){ cv_put(x,yy,"│",bc); cv_put(x+w-1,yy,"│",bc); }
    int budget=w-7;                      /* The limit at which border + title + border stays inside the corner */
    if(rtitle&&*rtitle&&budget>0){       /* Right-hand title, set apart at the right of the border */
        const char*rt=cutf(rtitle,budget);
        int rw=disp_w(rt);
        if(rw>0){
            int rx=x+w-2-(rw+4);
            cv_put(rx,y,"┤ ",bc); cv_put(rx+2,y,rt,"38;5;250");
            cv_put(rx+2+rw,y," ├",bc);
            budget-=rw+5;                /* Deduct the budget so it cannot overlap the left title */
        }
    }
    if(title&&*title&&budget>0){
        const char*tt=cutf(title,budget);
        int tw=disp_w(tt);
        if(tw>0){
            cv_put(x+2,y,"┤ ",bc); cv_put(x+4,y,tt,"1;36");
            cv_put(x+4+tw,y," ├",bc);
        }
    }
}
static void cv_box(int x,int y,int w,int h,const char*title){ cv_box2(x,y,w,h,title,NULL); }
/* Six-dot gauge: foreground is the lens metric (green to red), background the hot range (complementary blue) */
/* Overlay chg on the gauge.  The background already carries the hot range, so an
   orange background would mix two signals in one channel; instead the growth
   since the previous frame is painted orange at the end of the filled bar, where
   it is visible in place and does not collide with the hot background. */
static void cv_meter3(int x,int y,int w,double frac,double hot,double dfrac){
    if(y<0||y>=CV.h) return;
    if(frac<0)frac=0;
    if(frac>1)frac=1;
    int n=(int)(frac*w+0.5), hn=0;
    if(n==0 && frac>0 && w>0) n=1;   /* A non-zero value occupying no cells reads as "none" */
    if(hot>0){
        if(hot>1)hot=1;
        hn=(int)(hot*w+0.5);
        if(hn<1)hn=1;
    }
    /* Growth range [dn, n); at least one cell, so small changes stay visible */
    int dn=n;
    if(dfrac>0 && n>0){
        double df=dfrac>1?1:dfrac;
        int dw2=(int)(df*w+0.5); if(dw2<1) dw2=1; if(dw2>n) dw2=n;
        dn=n-dw2;
    }
    char col[24];
    for(int i=0;i<w;i++){
        if(x+i<0||x+i>=CV.w) continue;
        const char*bg=(i<hn)?";48;5;18":"";
        if(i<n){
            if(i>=dn) snprintf(col,sizeof col,"1;38;5;214%s",bg);   /* Growth in orange */
            else      snprintf(col,sizeof col,"38;5;%d%s",grad_of((double)i/(w>1?w-1:1)),bg);
            cv_put(x+i,y,"⠿",col);
        }
        else   { snprintf(col,sizeof col,"38;5;236%s",bg); cv_put(x+i,y,"⠤",col); }
    }
}
static void cv_meter(int x,int y,int w,double frac,double hot){
    cv_meter3(x,y,w,frac,hot,0);
}
/* Per-core usage bars, sorted busiest first.  A total gauge cannot tell "12
   cores' worth spread over 12" from "6 saturated", and the latter is the
   bottleneck; sorted, the width of the dark run is the saturated core count.
   With more cores than columns a cell takes the maximum, not the average -
   averaging would hide the saturation this view exists to show. */
static void cv_cores(int x,int y,int w,const double *pct,int n){
    if(w<=0||n<=0) return;
    for(int i=0;i<w;i++){
        int a=(int)((long long)i*n/w), b=(int)((long long)(i+1)*n/w);
        if(b<=a) b=a+1;
        if(b>n) b=n;
        double mx=0;
        for(int k=a;k<b;k++) if(pct[k]>mx) mx=pct[k];
        /* Only the two glyphs the other gauges use (filled and empty) - mixing in block
           characters would change the texture within one box.  The distinction is
           carried by colour alone: red saturated, orange high, green mid, grey idle. */
        const char *g,*c;
        if(mx>=95)      { g="⠿"; c="1;38;5;203"; }  /* Saturated: red, bold */
        else if(mx>=50) { g="⠿"; c="38;5;215"; }    /* High: orange */
        else if(mx>=10) { g="⠿"; c="38;5;150"; }    /* Mid: light green */
        else            { g="⠤"; c="38;5;236"; }    /* Idle: the empty glyph */
        cv_put(x+i,y,g,c);
    }
}
/* Braille 2x4 line chart, resampling the whole window to the x resolution */
static const int BRD[2][4]={{0x01,0x02,0x04,0x40},{0x08,0x10,0x20,0x80}};
/* Up to CV_NS series.  The caller supplies colours as palette indices (38;5;N) so the
   dashboard and plots draw the same metric in the same colour. */
#define CV_NS 9   /* Series limit including overlays.  Panels 3 and 4 gained OS cache, majflt and D
                     threads, and 7 truncated silently - leave margin, since overflow
                     drops the later series. */
static void cv_chart_n(int x,int y,int w,int h,const double*const*v,const int*colv,int ns,
                       int n,double vmax){
    static unsigned char cell[16][CV_W]; static unsigned char ccol[16][CV_W];
    if(h>16)h=16;
    if(w>CV_W)w=CV_W;
    if(w<2||h<1) return;                         /* Divides by px-1, so w must be at least 2 */
    if(ns>CV_NS) ns=CV_NS;
    for(int i=0;i<h;i++) for(int j=0;j<w;j++){ cell[i][j]=0; ccol[i][j]=0; }
    if(n<1||vmax<=0||ns<1) return;
    int px=w*2, py=h*4;
    for(int s=0;s<ns;s++){
        const double*vv=v[s]; if(!vv) continue;
        int lx=-1,ly=-1;
        for(int i=0;i<px;i++){
            /* Peak-preserving downsample: with n>px each pixel takes the maximum of its range.
               Even sampling would lose majflt and I/O spikes with probability
               (n/px-1)/(n/px). */
            double val;
            if(n>px){
                int a=(int)((double)i*n/px), b=(int)((double)(i+1)*n/px);
                if(b>n) b=n;
                if(b<=a) b=a+1;
                val=vv[a];
                for(int k=a+1;k<b;k++) if(vv[k]>val) val=vv[k];
            }else{
                int idx=(n>1)?(int)((double)i*(n-1)/(px-1)+0.5):0;
                val=vv[idx];
            }
            /* Written as "not >= 0" so NaN is caught too (every comparison with NaN is
               false): a NaN from a recording would otherwise become INT_MIN below and the
               line walk would run for ~2^31 steps. */
            if(!(val>=0)) val=0;
            double f=val/vmax; if(!(f>=0)) f=0; if(f>1) f=1;
            int yy=(int)((py-1)*(1.0-f)+0.5);
            if(lx>=0){                                   /* Bresenham line */
                int x0=lx,y0=ly,x1=i,y1=yy;
                        int dx=x1-x0, dy=y1>y0?y1-y0:y0-y1, sy=y1>y0?1:-1, err=dx-dy;
                while(1){
                    if(x0>=0&&x0<px&&y0>=0&&y0<py){ cell[y0/4][x0/2]|=BRD[x0%2][y0%4]; ccol[y0/4][x0/2]=(unsigned char)(s+1); }
                    if(x0==x1&&y0==y1) break;
                    int e2=2*err;
                    if(e2>-dy){ err-=dy; x0++; }
                    if(e2<dx){ err+=dx; y0+=sy; }
                }
            }
            lx=i; ly=yy;
        }
    }
    char buf[8],col[16];
    for(int cy=0;cy<h;cy++) for(int cx=0;cx<w;cx++){
        if(!cell[cy][cx]) continue;
        int cp=0x2800+cell[cy][cx];
        buf[0]=(char)(0xE0|(cp>>12)); buf[1]=(char)(0x80|((cp>>6)&0x3F)); buf[2]=(char)(0x80|(cp&0x3F)); buf[3]=0;
        int ci=ccol[cy][cx]-1; if(ci<0)ci=0; if(ci>=ns)ci=ns-1;
        snprintf(col,sizeof col,"38;5;%d",colv[ci]);
        cv_put(x+cx,y+cy,buf,col);
    }
}
/* Compatibility wrapper for the older two-series callers; same colours as before (211/203) */
/* Write the canvas out.  Each row is placed absolutely (ESC[y;1H) with no newline:
   descending by newline scrolls when the canvas is taller than the terminal, which
   pushes the top row off and back on, flickering.
   maxrows limits drawing (0 = all); returns the number of rows not drawn. */
static int cv_flush_rows(int use_color,int maxrows){
    char line[CV_W*40];
    int lim = (maxrows>0 && maxrows<CV.h) ? maxrows : CV.h;
    for(int y=0;y<lim;y++){
        int p=0; const char*cur=NULL;
        for(int x=0;x<CV.w;x++){
            if(!CV.ch[y][x][0]) continue;                 /* Cell consumed by a wide character */
            const char*c=CV.co[y][x][0]?CV.co[y][x]:NULL;
            if(use_color && c!=cur){
                if(cur) p+=snprintf(line+p,sizeof line-p,"\033[0m");
                if(c)   p+=snprintf(line+p,sizeof line-p,"\033[%sm",c);
                cur=c;
            }
            p+=snprintf(line+p,sizeof line-p,"%s",CV.ch[y][x]);
            if((size_t)p>sizeof line-64) break;
        }
        if(use_color&&cur) p+=snprintf(line+p,sizeof line-p,"\033[0m");
        printf("\033[%d;1H%s\033[K",y+1,line);
    }
    return CV.h-lim;
}

typedef struct {
    double rb,wb,ri,wi,absorb,rchar,blkpct,devutil,devr; char devnm[32];
    double minflt,majflt;          /* Faults per second; majflt is a direct signal of swapping and cache misses */
    double cpu_pct;                /* Server CPU percentage, one core = 100% */
    int ncpu;                      /* Host core count, for showing the ceiling */
    int absorb_ok;
    unsigned long blk_ticks;       /* Cumulative delayacct_blkio_ticks summed over all threads */
    int d_thr, n_thr;              /* cub_server threads in D state over total threads.  wa is divided by the core
                                      count, so a saturated disk shows as 1-2% on 64
                                      cores; the D count is not diluted and is direct
                                      evidence of waiting. */
} iod_t;

#define CAP_BUDGET_MS 500.0              /* Collection budget; over it the frame is dropped and the screen says so */
#define CAP_HASH_BITS 17                 /* 131072 slots, covering 32k-128k BCBs */
#define CAP_HASH_SZ   (1<<CAP_HASH_BITS)
typedef struct {
    /* Demand */
    double read_bps, write_bps, riops, wiops;   /* Attributed to the process, from /proc/pid/io */
    double dev_rbps, dev_wbps;                  /* Device-wide, diskstats sectors x 512 */
    double miss_pps;                            /* Buffer misses: pages loaded per second */
    double hit_pct;                             /* Hit ratio percentage; -1 means it cannot be computed */
    char   hit_src[16];                         /* "perfmon" | "turnover" | "-" */
    double turnover_pct;                        /* Share of resident pages replaced within the window */
    double readmit_pct;                         /* Share evicted and read back: the signal that the working set exceeds the buffer */
    /* Ceiling and latency */
    double await_ms, r_await_ms, w_await_ms;    /* Wait per device request */
    double qdepth;                              /* Mean queue depth (time_in_queue over elapsed) */
    double dev_iops, dev_util;                  /* Device-wide IOPS and utilization */
    double ceil_iops, ceil_bps, ceil_util;      /* Ceiling observed at saturation, for the current profile */
    time_t ceil_when; int ceil_valid; double ceil_qd;
    /* Ceiling matrix.  The IOPS ceiling for 4KB random and 128KB sequential differ
       by orders of magnitude, so a single remembered number misjudges the moment
       the profile changes.  Ceilings are kept per cell, keyed by (mean request
       size x read share), and the verdict uses the cell in force now. */
    int    prof_bs, prof_rw;                    /* The cell this frame falls in */
    double req_kb, read_ratio;                  /* Mean request size in KB and read share (0-1) */
    double headroom_pct;                        /* Current over ceiling; -1 means no ceiling observed */
    /* Engine statistics, when available */
    int    ps_ok; char ps_why[96];
    double pb_fetch_ps, pb_ioread_ps, pb_iowrite_ps, log_iowrite_ps, commit_ps;
    double ms; int over_budget;                 /* Collection time and whether the budget was exceeded */
} cap_t;
static cap_t CAP2;
/* Clear the values only, keeping the reasons and remembered ceilings, so a budget-exceeded or server-down frame shows no half values */
static void cap_reset_values(void){
    CAP2.miss_pps=-1; CAP2.hit_pct=-1; CAP2.turnover_pct=-1; CAP2.readmit_pct=-1;
    CAP2.await_ms=CAP2.r_await_ms=CAP2.w_await_ms=-1; CAP2.qdepth=-1;
    CAP2.headroom_pct=-1; CAP2.ps_ok=0;
    CAP2.dev_rbps=CAP2.dev_wbps=0; CAP2.req_kb=0; CAP2.read_ratio=0.5;
    snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"-");
}

/* ---- History ring buffer ----
   HIST_MAX 800 x 0.5s default interval = a window of about 400 seconds.
   Every field the plot panels read is filled only here - computing per screen
   would let the same metric differ between views. */
#define HIST_MAX 800
typedef struct {
    double t, srv, dyn, rbps, wbps, riops, majflt;   /* Existing fields; order preserved */
    /* Plot extension, holding both srv_rss and srv_pss: the composition total
       (db+lg+dyn+etc) must be over RSS for the identity to hold, while "CUBRID total"
       sums across processes and must therefore be PSS. */
    double srv_rss, db, lg, etc, vsz, rss, dynmap;
    double dbhot, minflt, devutil, absorb, osused, cub, ram;
    double cpu;                 /* Server CPU percentage, one core = 100 */
    /* A metric on the dashboard must also be in the plots, or finding a trend means
       working out which screen had it. */
    double busy_cores;          /* Host cores in use (the OS box's used) */
    double sat_cores;           /* Saturated cores, 95% and above (the OS box's sat); double because it is graphed */
    double cpu_wa;              /* iowait percentage (the I/O box's iowait row) */
    double load1;               /* One-minute load (in the OS box title) */
    double d_thr;               /* D (disk wait) thread count, cited by the verdict */
    double blkpct;              /* blkio wait percentage, cited by the verdict */
    double wiops;               /* Write IOPS (reads are riops) */
    double c_master,c_pl,c_broker,c_cas;   /* CPU per tier, in cores */
    /* BCB direct read - the basis for the B (fill) and D (drain) panels and the
       verdict.  A frame with the snapshot inactive has pg_ok=0 and zero values. */
    /* PSS per tier - the delta and sparkline columns of the process box (what is growing).
       These are the only graphed view of the broker/CAS/PL trends. */
    double pss_srv, pss_master, pss_pl, pss_broker, pss_cas;
    /* Capacity axis, read by plots B and D and by the verdict */
    double cap_iops, cap_await, cap_hit, cap_miss, cap_head;
    int    pg_ok;
    double pg_res_pct;          /* Resident over total, percent */
    double pg_dirty;            /* Dirty page count */
    double pg_cold_pct;         /* LRU cold zone share, percent of resident */
    double pg_span;             /* Flush lag: append.pageid - oldest_dirty.pageid, in log pages */
    double pg_lag;              /* Log fsync lag: append.pageid - nxio.pageid */
    int hot_ok, absorb_ok;
    /* Total of dynamic memory detail B (engine direct read).  It moves in steps:
       conn/lock/session are preallocated from max_clients and fixed, while only the
       plan/result/catalog caches follow actual use.  A (chunk estimate) is re-read
       only on r/a and takes hundreds of ms, so it is kept out of the time series -
       sparse samples would draw a trend that is not there. */
    double heapb;
    double cap_readmit;   /* Refill percentage: what separates a short buffer from a scan */
    char hh[12];
} hsam_t;
static hsam_t HS[HIST_MAX]; static int HN=0, HCAP=HIST_MAX;

/* One place for the value sources.  Region values go through the sr_*() name lookups without exception; never by index. */
static void hist_push(double t,const proc_t*P,const mem_t*M,const tier_t*T,const iod_t*IO,
                      int hot_ok){
    if(HN>=HCAP){ memmove(HS,HS+1,sizeof(hsam_t)*(HCAP-1)); HN=HCAP-1; }
    hsam_t*s=&HS[HN++];
    memset(s,0,sizeof *s);
    s->t=t;
    s->srv     = P->pss_kb*1024.0 + T->master;      /* The existing growth trend series (PSS) is kept */
    s->srv_rss = P->rss_kb*1024.0;                  /* Denominator of the composition total */
    s->rss     = P->rss_kb*1024.0;
    s->vsz     = P->vsize_kb*1024.0;
    s->db      = sr_rss("data_buffer");
    s->lg      = sr_rss("log_buffer");
    s->dyn     = sr_rss("dynamic-heap");
    s->dynmap  = sr_mapped("dynamic-heap");
    { double sum=s->db+s->lg+s->dyn;
      s->etc = (s->srv_rss>sum)?(s->srv_rss-sum):0.0; }
    s->dbhot   = sr_hotpct("data_buffer");
    s->hot_ok  = hot_ok;
    s->heapb   = B.ok ? B.total : 0;   /* Dynamic memory B total, which moves in steps */
    s->minflt  = IO->minflt; s->majflt = IO->majflt;
    s->rbps=IO->rb; s->wbps=IO->wb; s->riops=IO->ri;
    s->devutil = IO->devutil;
    /* Idle stretches (below the denominator floor) are 0; drawing 100% would read as a
       perfect cache.  That makes 0 and "not measurable" the same pixel, so absorb_ok is
       kept and footnoted. */
    s->absorb_ok = IO->absorb_ok;
    s->absorb  = IO->absorb_ok ? IO->absorb*100.0 : 0.0;
    s->cpu     = IO->cpu_pct;
    s->ram     = (double)M->total*1024.0;
    s->osused  = (double)(M->total-M->avail)*1024.0;
    s->cub     = P->pss_kb*1024.0 + T->master + T->broker + T->cas + T->pl;
    /* CPU and I/O extension, holding the dashboard's values in the dashboard's units. */
    s->busy_cores = M->busy_cores;
    s->sat_cores  = (double)M->sat_cores;
    s->cpu_wa     = M->cpu_wa;
    s->load1      = M->load1;
    s->d_thr      = (double)IO->d_thr;
    s->blkpct     = IO->blkpct;
    s->wiops      = IO->wi;
    s->c_master   = T->cpu_master;
    s->c_pl       = T->cpu_pl;
    s->c_broker   = T->cpu_broker;
    s->c_cas      = T->cpu_cas;
    s->pss_srv=P->pss_kb*1024.0; s->pss_master=T->master; s->pss_pl=T->pl;
    s->pss_broker=T->broker; s->pss_cas=T->cas;
    s->cap_iops=CAP2.dev_iops; s->cap_await=CAP2.await_ms<0?0:CAP2.await_ms;
    s->cap_readmit=CAP2.readmit_pct<0?0:CAP2.readmit_pct;
    s->cap_hit=CAP2.hit_pct<0?0:CAP2.hit_pct; s->cap_miss=CAP2.miss_pps<0?0:CAP2.miss_pps;
    s->cap_head=CAP2.headroom_pct<0?0:CAP2.headroom_pct;
    s->pg_ok      = PG.ok;
    if(PG.ok){
        s->pg_res_pct  = PG.nbuf>0 ? 100.0*PG.resident/PG.nbuf : 0;
        s->pg_dirty    = PG.dirty;
        s->pg_cold_pct = PG.resident>0 ? 100.0*PG.z3/PG.resident : 0;
        s->pg_span     = (PG.have_log&&PG.have_oldest) ? (double)(PG.log_append.pageid-PG.oldest_dirty.pageid) : 0;
        s->pg_lag      = PG.have_log ? (double)(PG.log_append.pageid-PG.log_nxio.pageid) : 0;
    }
    time_t tt=(time_t)t; struct tm tmv; localtime_r(&tt,&tmv);
    strftime(s->hh,sizeof s->hh,"%H:%M:%S",&tmv);        /* 24-hour clock; the date is in the top box */
}
/* Dump the last history sample to stderr, leaving the terse stdout contract clean.
   tools/check-hist.sh compares this against the terse keys to catch numbers that
   disagree between views. */
static void hist_dump_last(void){
    if(HN<1) return;
    const hsam_t*s=&HS[HN-1];
    fprintf(stderr,
        "hist.srv_rss=%.0f hist.db=%.0f hist.lg=%.0f hist.dyn=%.0f hist.etc=%.0f\n"
        "hist.dynmap=%.0f hist.vsz=%.0f hist.rss=%.0f hist.cub=%.0f hist.osused=%.0f\n"
        "hist.dbhot=%.1f hist.minflt=%.0f hist.majflt=%.0f hist.rbps=%.0f hist.wbps=%.0f\n"
        "hist.devutil=%.1f hist.absorb=%.1f hist.absorb_ok=%d hist.hot_ok=%d\n"
        "hist.pg_ok=%d hist.pg_res_pct=%.2f hist.pg_dirty=%.0f hist.pg_cold_pct=%.2f hist.pg_span=%.0f hist.pg_lag=%.0f\n"
        "hist.pss_srv=%.0f hist.pss_master=%.0f hist.pss_pl=%.0f hist.pss_broker=%.0f hist.pss_cas=%.0f\n"
        "hist.cap_iops=%.0f hist.cap_await=%.2f hist.cap_hit=%.2f hist.cap_miss=%.0f hist.cap_head=%.1f\n",
        s->srv_rss,s->db,s->lg,s->dyn,s->etc,
        s->dynmap,s->vsz,s->rss,s->cub,s->osused,
        s->dbhot,s->minflt,s->majflt,s->rbps,s->wbps,
        s->devutil,s->absorb,s->absorb_ok,s->hot_ok,
        s->pg_ok,s->pg_res_pct,s->pg_dirty,s->pg_cold_pct,s->pg_span,s->pg_lag,
        s->pss_srv,s->pss_master,s->pss_pl,s->pss_broker,s->pss_cas,
        s->cap_iops,s->cap_await,s->cap_hit,s->cap_miss,s->cap_head);
}
static double hist_peak(int which,double floor_v){
    double mx=floor_v;
    for(int i=0;i<HN;i++){
        double v = which==0?HS[i].rbps : which==1?HS[i].wbps : HS[i].riops;
        if(v>mx) mx=v;
    }
    return mx;
}

/* ---------------- TUI control ---------------- */
#include <termios.h>
#include <locale.h>
#include <langinfo.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
static struct termios g_tio; static int g_raw=0;
static volatile sig_atomic_t g_stop=0;
static void on_sig(int s){ (void)s; g_stop=1; }

/* Darken the background (live only).  The palette assumes a dark background, so
   live mode sets the terminal default foreground/background on entry and
   restores them on exit, via OSC 10/11:
     query  ESC]1N;?BEL  ->  reply  ESC]1N;rgb:RRRR/GGGG/BBBB
   Only terminals that answer both queries are changed - no reply means no
   support, so nothing is done (self-gating).  Both must change together, or a
   light theme's black foreground is lost in the dark. */
static char g_fg_orig[64]="", g_bg_orig[64]=""; static int g_bg_set=0;
static int osc_color_query(int code,char*out,size_t on){
    char q[16]; int ql=snprintf(q,sizeof q,"\033]%d;?\007",code);
    if(write(STDOUT_FILENO,q,(size_t)ql)<0) return 0;
    char b[96]; int bn=0; struct pollfd pf={STDIN_FILENO,POLLIN,0};
    while(bn<(int)sizeof b-1 && poll(&pf,1,200)>0){
        ssize_t k=read(STDIN_FILENO,b+bn,1); if(k<=0) break;
        bn+=(int)k;
        if(b[bn-1]=='\a') break;
        if(bn>=2 && b[bn-2]=='\033' && b[bn-1]=='\\'){ bn-=2; break; }
    }
    if(bn>0 && b[bn-1]=='\a') bn--;
    b[bn]=0;
    char pre[8]; snprintf(pre,sizeof pre,"]%d;",code);
    char *p=strstr(b,pre); if(!p) return 0;
    p+=strlen(pre);
    if(!*p || strlen(p)>=on) return 0;
    snprintf(out,on,"%s",p); return 1;
}
static void bg_dark_enter(void){
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)) return;
    if(!osc_color_query(10,g_fg_orig,sizeof g_fg_orig)) return;
    if(!osc_color_query(11,g_bg_orig,sizeof g_bg_orig)) return;
    printf("\033]10;#d8d8d8\007\033]11;#1c1c1c\007"); fflush(stdout);
    g_bg_set=1;
}
static void bg_dark_leave(void){
    if(!g_bg_set) return;
    g_bg_set=0;
    printf("\033]10;%s\007\033]11;%s\007",g_fg_orig,g_bg_orig);
}
static void tui_leave(void){
    static int done=0;
    if(g_raw){ tcsetattr(STDIN_FILENO,TCSADRAIN,&g_tio); g_raw=0; }
    if(done) return;                 /* Called explicitly and via atexit, but runs once */
    done=1;
    bg_dark_leave();                 /* Restore the terminal default colours, before restoring the cursor */
    /* Restore only the cursor and leave the screen, so the last frame stays on the tty.
       G0 is reset to ASCII again so the shell prompt does not inherit a CJK charset. */
    printf("\033[?25h\033[0m\033(B\017\033[?2004h\033[?2026l"); fflush(stdout);   /* Restore the input state */
}
static void tui_enter(void){
    if(isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&g_tio)==0){
        struct termios t=g_tio; t.c_lflag&=~(ICANON|ECHO); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
        tcsetattr(STDIN_FILENO,TCSANOW,&t); g_raw=1;
    }
    /* No alternate screen (?1049h) - it would erase the last frame on exit.
       ESC(B + SI resets G0 to ASCII, clearing any CJK/line-drawing charset state
       a previous program left behind (restored on exit).

       On IME: no escape sequence lets a remote process turn off the client's
       input method - the IME belongs to the user's OS and SSH carries only its
       output.  The two sequences below are a best effort on terminals that
       honour them, and tui_key() folds Hangul jamo to ASCII keys for the rest.
         ESC[?2004l  disable bracketed paste, so composition is not read as a paste
         ESC[8;1"q   DECSCA default; some terminals reset input state on it
       tui_leave() restores both. */
    bg_dark_enter();                 /* Query under raw mode; no reply means no action */
    printf("\033(B\017\033[2J\033[H\033[?25l\033[?2004l\033[?2026l"); fflush(stdout);
    atexit(tui_leave);
    signal(SIGINT,on_sig); signal(SIGTERM,on_sig);
}
/* Map Hangul typed on a 2-set layout back to the ASCII keys on the same physical
   keys, so a key pressed with the IME still in Hangul mode does what was intended.
   Jamo (U+3131-U+3163) give their key(s); a composed syllable (U+AC00-U+D7A3), which
   an IME emits for two or three keystrokes, gives each of them in order.  Shifted
   keys fold to lower case.  Returns "" for anything else. */
static const char *ko_to_keys(int cp){
    static const char *const jamo[51]={      /* U+3131..U+3163 */
        "r","r","rt","s","sw","sg","e","e","f","fr","fa","fq","ft","fx","fv","fg",
        "a","q","q","qt","t","t","d","w","w","c","z","x","v","g",
        "k","o","i","o","j","p","u","p","h","hk","ho","hl","y",
        "n","nj","np","nl","b","m","ml","l"};
    static const char *const cho[19]={
        "r","r","s","e","e","f","a","q","q","t","t","d","w","w","c","z","x","v","g"};
    static const char *const jong[28]={
        "","r","r","rt","s","sw","sg","e","f","fr","fa","fq","ft","fx","fv","fg",
        "a","q","qt","t","t","d","w","c","z","x","v","g"};
    static char k[8];
    k[0]=0;
    if(cp>=0x3131 && cp<=0x3163) snprintf(k,sizeof k,"%s",jamo[cp-0x3131]);
    else if(cp>=0xAC00 && cp<=0xD7A3){
        int sy=cp-0xAC00;
        snprintf(k,sizeof k,"%s%s%s",cho[sy/588],jamo[30+(sy%588)/28],jong[sy%28]);
    }
    return k;
}
/* Special key codes returned by tui_key, at 0x100 and above so they cannot collide with byte values */
#define TUI_KEY_UP    0x101
#define TUI_KEY_DOWN  0x102
#define TUI_KEY_RIGHT 0x103
#define TUI_KEY_LEFT  0x104
#define TUI_KEY_PGUP  0x105
#define TUI_KEY_PGDN  0x106
/* Bytes already read but not yet returned: the tail of an ESC prefix that was not a
   recognised sequence, or the further keys of a decomposed Hangul syllable. */
static unsigned char g_kq[16]; static int g_kqh, g_kqn;
/* Put n bytes back in front of the queue, so they are returned next and in order */
static void kq_unget(const unsigned char *b,int n){
    for(int i=n-1;i>=0 && g_kqn<(int)sizeof g_kq;i--){
        g_kqh=(g_kqh+(int)sizeof g_kq-1)%(int)sizeof g_kq; g_kq[g_kqh]=b[i]; g_kqn++;
    }
}
static int key_byte(unsigned char *c){
    if(g_kqn>0){ *c=g_kq[g_kqh]; g_kqh=(g_kqh+1)%(int)sizeof g_kq; g_kqn--; return 1; }
    return read(STDIN_FILENO,c,1)==1;
}
static int tui_key(void){
    unsigned char c;
    if(!key_byte(&c)) return -1;
    if(c==0x1b){
        /* Interpret "ESC [ X", "ESC O X" and "ESC [ N ~" one byte at a time; bytes
           that do not continue a known sequence are returned as the next keys. */
        unsigned char b[3]; int n=0;
        if(!key_byte(&b[n])) return 0x1b;
        n++;
        if(b[0]=='[' || b[0]=='O'){
            if(key_byte(&b[n])){
                n++;
                switch(b[1]){
                case 'A': return TUI_KEY_UP;
                case 'B': return TUI_KEY_DOWN;
                case 'C': return TUI_KEY_RIGHT;
                case 'D': return TUI_KEY_LEFT;
                }
                if(b[0]=='[' && (b[1]=='5'||b[1]=='6') && key_byte(&b[n])){
                    if(b[2]=='~') return b[1]=='5' ? TUI_KEY_PGUP : TUI_KEY_PGDN;
                    n++;
                }
            }
        }
        kq_unget(b,n);
        return 0x1b;
    }
    if(c<0x80) return c;
    /* Multi-byte: the remaining bytes must be consumed, or they are taken as the next key. */
    int need = (c>=0xF0)?3 : (c>=0xE0)?2 : (c>=0xC0)?1 : 0;
    unsigned char b[3]; int got=0;
    for(int i=0;i<need;i++){ if(!key_byte(&b[i])) break; got++; }
    if(got<need) return -1;                       /* Incomplete sequence: discarded */
    int cp=-1;
    if(need==1) cp=((c&0x1F)<<6)|(b[0]&0x3F);
    else if(need==2) cp=((c&0x0F)<<12)|((b[0]&0x3F)<<6)|(b[1]&0x3F);
    else if(need==3) cp=((c&0x07)<<18)|((b[0]&0x3F)<<12)|((b[1]&0x3F)<<6)|(b[2]&0x3F);
    const char *k=ko_to_keys(cp);
    if(!k[0]) return -1;                          /* Unmapped characters are ignored */
    kq_unget((const unsigned char*)k+1,(int)strlen(k+1));
    return (unsigned char)k[0];
}
static int term_w(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>=60)
        return ws.ws_col<114?ws.ws_col:114;              /* Ceiling of 114 */
    return 114;
}
/* Real row count, no fallback; 0 means unknown.  Used to paginate the help. */
static int term_h(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_row>0) return ws.ws_row;
    return 0;
}
/* Real column count, no fallback; 0 means unknown.  term_w() falls back to 114 below
   60, and drawing at that width wraps and breaks the screen - wide views use this
   value to decide "too narrow" first. */
static int term_cols_raw(void){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col>0) return ws.ws_col;
    return 0;
}


/* ---- Capacity axis: scale up, scale down, tune ----
 * Looks at demand, ceiling and headroom against allocated resources rather than
 * at one query.  Nothing is written to the target and no connection or lock is
 * taken.
 *   Engine statistics are read only when already counting: perfmon does not
 *   increment counters unless n_watchers > 0, and creating a watcher would write
 *   to the target and add server hot-path cost, so it is not done.
 *   Fallback: the VPID set difference between two BCB snapshots gives buffer
 *   turnover with no extra I/O, since the same array is already read.
 *   Cost: 8B x 6 reads, a hash comparison, and /proc/diskstats fields already
 *   being read - well under 1ms per frame.
 */
static char g_cap_state[256]="";          static int g_replaying=0;             /* Set during --replay so the screen says so */
static int g_rep_i=0,g_rep_n=0;       /* Replay progress, shown in the status line */
static time_t g_rep_ts=0;             /* The frame's recorded time, shown instead of now */
static const char *g_rec_path=NULL;   /* --record FILE  */
static const char *g_rep_path=NULL;   /* --replay FILE  */
static double g_rep_speed=1.0;        /* --replay-speed (0 = as fast as possible) */
/* --capacity-state FILE, for remembering ceilings */
/* Three block-size bands x three read/write bands.  The boundaries come from CUBRID's
   real I/O units: log and DWB around 4KB, data pages 16KB by default, scan read-ahead
   128KB and above. */
#define CAP_BS_N 3
#define CAP_RW_N 3
static const char *CAP_BS_NAME[CAP_BS_N]={"<8KB","8-64KB",">64KB"};
static const char *CAP_RW_NAME[CAP_RW_N]={"write-heavy","mixed","read-heavy"};
typedef struct { double iops,bps,util,qd,await_ms; time_t when; int valid; } cap_cell_t;
static cap_cell_t g_cap_mtx[CAP_BS_N][CAP_RW_N];
static int cap_bs_idx(double kb){ return (kb<8.0)?0:(kb<=64.0)?1:2; }
static int cap_rw_idx(double rr){ return (rr<0.3)?0:(rr<=0.7)?1:2; }

/* ---- Engine statistics direct read, used when already counting ----
   Adopted only after the psid self-check (metadata psid == requested psid) and a
   start_offset range check. */
enum { CPS_PB_FETCH=0, CPS_PB_IOREAD, CPS_PB_IOWRITE, CPS_LOG_IOWRITE, CPS_COMMIT, CPS_N };
/* The PSTAT_ID order from 11.5's perf_monitor.h.  A psid shifted by a version difference is caught by the self-check. */
static const int CPS_PSID[CPS_N]={ 8, 10, 11, 30, 44 };
static int cap_pstat_read(proc_t*P,uint64_t out[CPS_N]){
    if(!B.ok||B.partial) { snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"method B off":"방법 B 비활성"); return 0; }
    if(g_bt->ps.nvals<0||!B.sym[S_PSTATG]||!B.sym[S_PSTATMD]){
        snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"no perfmon offsets/symbols":"perfmon 오프셋·심볼 없음"); return 0; }
    unsigned long g=B.base+B.sym[S_PSTATG], md=B.base+B.sym[S_PSTATMD];
    int32_t nvals=0; unsigned char init=0; uint64_t gptr=0;
    if(vmread(P->pid,g+g_bt->ps.nvals,&nvals,4)!=4) return 0;
    vmread(P->pid,g+g_bt->ps.init,&init,1);
    if(vmread(P->pid,g+g_bt->ps.gstats,&gptr,8)!=8) return 0;
    if(!init||!gptr||nvals<=0||nvals>1000000){
        snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"perfmon not initialized":"perfmon 미초기화"); return 0; }
    for(int i=0;i<CPS_N;i++){
        unsigned long e=md+(unsigned long)CPS_PSID[i]*(unsigned long)g_bt->ps.sz_md;
        int32_t psid=-1, st=-1;
        if(vmread(P->pid,e+g_bt->ps.md_psid,&psid,4)!=4) return 0;
        if(vmread(P->pid,e+g_bt->ps.md_start,&st,4)!=4) return 0;
        if(psid!=CPS_PSID[i]||st<0||st>=nvals){    /* Version mismatch: give up rather than invent */
            snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"psid mismatch (other version)":"psid 불일치(타버전)"); return 0; }
        if(vmread(P->pid,(unsigned long)gptr+(unsigned long)st*8,&out[i],8)!=8) return 0;
    }
    return 1;
}

/* ---- Turnover by direct read: the VPID set difference between BCB snapshots ----
   PG.rec was already read this frame (no extra I/O).  The previous frame's set is
   held as a hash bitmap to count replacements and refills. */
static unsigned char *cap_prev=NULL, *cap_ever=NULL;   /* Resident last time / resident at any point */
static int cap_prev_n=0;
static double cap_prev_t=0;
static inline unsigned cap_hash(int volid,int pageid){
    unsigned k=(unsigned)volid*2654435761u ^ (unsigned)pageid*2246822519u;
    k^=k>>15; return k&(CAP_HASH_SZ-1);
}
/* One-shot (-t, tree) has a single frame and so no memory baseline.  If a previous
   snapshot file from --bcb-dump exists it serves as the baseline (one file read, no
   extra contact with the server), used only when the two are 0.2-60s apart: shorter
   is noise, longer is meaningless. */
static int cap_turnover_from_dump(void){
    if(!g_bcb_dump[0]||!PG.ok||PG.nrec<=0) return 0;
    FILE*f=fopen(g_bcb_dump,"rb"); if(!f) return 0;
    struct { char magic[8]; uint32_t version,reclen; int64_t ts_sec,ts_nsec;
             int32_t nbuf,nrec,dirty,pagesize;
             uint64_t la,ln,le,od; char db[32]; } h;
    if(fread(&h,sizeof h,1,f)!=1||memcmp(h.magic,"CBCBMAP1",8)||h.reclen!=32||h.nrec<=0){ fclose(f); return 0; }
    double dt=(double)PG.when.tv_sec-(double)h.ts_sec;
    if(dt<0.2||dt>60.0){ fclose(f); return 0; }
    unsigned char*prev=(unsigned char*)calloc(CAP_HASH_SZ,1);
    if(!prev){ fclose(f); return 0; }
    for(int i=0;i<h.nrec;i++){
        struct { int16_t volid,zone; int32_t pageid; uint32_t flags; int32_t hdr_ok; uint64_t a,b; } r;
        if(fread(&r,sizeof r,1,f)!=1) break;
        if(r.volid<0||r.pageid<0) continue;
        prev[cap_hash(r.volid,r.pageid)]=1;
    }
    fclose(f);
    long admitted=0;
    for(int i=0;i<PG.nrec;i++) if(!prev[cap_hash(PG.rec[i].volid,PG.rec[i].pageid)]) admitted++;
    free(prev);
    CAP2.turnover_pct = 100.0*(double)admitted/(double)PG.nrec;
    CAP2.miss_pps     = (double)admitted/dt;
    CAP2.readmit_pct  = -1;    /* A file baseline has no ever-resident history, so refill cannot be computed */
    return 1;
}
static void cap_turnover(double now_s){
    CAP2.turnover_pct=-1; CAP2.readmit_pct=-1; CAP2.miss_pps=-1;
    { static unsigned seen=0;
      if(seen!=g_inst_gen){ seen=g_inst_gen; cap_prev_n=0;
                            if(cap_ever) memset(cap_ever,0,CAP_HASH_SZ); } }
    if(!PG.ok||PG.nrec<=0) return;
    if(!cap_prev){ cap_prev=(unsigned char*)calloc(CAP_HASH_SZ,1);
                   cap_ever=(unsigned char*)calloc(CAP_HASH_SZ,1);
                   if(!cap_prev||!cap_ever){ free(cap_prev); free(cap_ever); cap_prev=cap_ever=NULL; return; } }
    static unsigned char *cur=NULL;
    if(!cur){ cur=(unsigned char*)calloc(CAP_HASH_SZ,1); if(!cur) return; }
    else memset(cur,0,CAP_HASH_SZ);
    long admitted=0, readmit=0;
    for(int i=0;i<PG.nrec;i++){
        unsigned h=cap_hash(PG.rec[i].volid,PG.rec[i].pageid);
        cur[h]=1;
        if(cap_prev_n>0 && !cap_prev[h]){ admitted++; if(cap_ever[h]) readmit++; }
        cap_ever[h]=1;
    }
    double dt=now_s-cap_prev_t;
    if(cap_prev_n>0 && dt>0.05){
        CAP2.turnover_pct = PG.nrec>0 ? 100.0*(double)admitted/(double)PG.nrec : 0;
        CAP2.readmit_pct  = admitted>0 ? 100.0*(double)readmit/(double)admitted : 0;
        CAP2.miss_pps     = (double)admitted/dt;
    }
    memcpy(cap_prev,cur,CAP_HASH_SZ);
    cap_prev_n=PG.nrec; cap_prev_t=now_s;
}

/* ---- Remembered ceilings: the peak IOPS and bandwidth of saturated frames (util >= 90%) are kept in a state file ---- */
static void cap_ceiling(void){
    CAP2.headroom_pct=-1;
    if(!g_cap_state[0]) g_cap_state[0]=0;
    static int loaded=0;
    if(!loaded && g_cap_state[0]){
        FILE*f=fopen(g_cap_state,"r");
        if(f){ char ln[256];
               while(fgets(ln,sizeof ln,f)){
                   int b=0,r=0; double io=0,bp=0,ut=0,qd=0,aw=0; long long w=0;
                   if(ln[0]=='#') continue;
                   if(sscanf(ln,"cell %d %d iops=%lf bps=%lf util=%lf qd=%lf await=%lf when=%lld",
                             &b,&r,&io,&bp,&ut,&qd,&aw,&w)>=6
                      && b>=0 && b<CAP_BS_N && r>=0 && r<CAP_RW_N){
                       cap_cell_t*c=&g_cap_mtx[b][r];
                       c->iops=io; c->bps=bp; c->util=ut; c->qd=qd; c->await_ms=aw;
                       c->when=(time_t)w; c->valid=1;
                   }
               }
               fclose(f); }
        loaded=1;
    }
    /* This frame's I/O profile: mean request size and read share */
    { double ios=CAP2.dev_iops, byt=CAP2.dev_rbps+CAP2.dev_wbps;
      CAP2.req_kb = (ios>0) ? byt/ios/1024.0 : 0;
      CAP2.read_ratio = (byt>0) ? CAP2.dev_rbps/byt : 0.5;
      CAP2.prof_bs = cap_bs_idx(CAP2.req_kb);
      CAP2.prof_rw = cap_rw_idx(CAP2.read_ratio); }
    /* Update the observed ceiling from either: a saturated frame (utilization >= 90%),
         closest to the true ceiling; or a frame where the queue began to build
         (depth >= 1), which approximates it on storage that never reaches 90%
         (network, multi-queue NVMe).  Only the current profile cell is updated -
         a ceiling measured at 128KB sequential would make the 4KB random cell
         report false headroom. */
    double cur_iops=CAP2.dev_iops;
    int sat = (CAP2.dev_util>=90.0) || (CAP2.qdepth>=1.0 && CAP2.await_ms>=1.0);
    { cap_cell_t*c=&g_cap_mtx[CAP2.prof_bs][CAP2.prof_rw];
      if(sat && cur_iops>c->iops){
          c->iops=cur_iops; c->bps=CAP2.dev_rbps+CAP2.dev_wbps; c->util=CAP2.dev_util;
          c->qd=CAP2.qdepth<0?0:CAP2.qdepth; c->await_ms=CAP2.await_ms<0?0:CAP2.await_ms;
          c->when=time(NULL); c->valid=1;
      }
      /* The ceiling used for the verdict is the current profile's cell; an empty cell means
         no ceiling observed.  Borrowing another cell compares different magnitudes. */
      if(c->valid){
          CAP2.ceil_iops=c->iops; CAP2.ceil_bps=c->bps; CAP2.ceil_util=c->util;
          CAP2.ceil_qd=c->qd; CAP2.ceil_when=c->when; CAP2.ceil_valid=1;
      } else CAP2.ceil_valid=0;
    }
    if(sat && cur_iops>0){
        if(g_cap_state[0]){    /* Writes only to our own file; the target process and DB are untouched */
            char tmp[300]; snprintf(tmp,sizeof tmp,"%s.tmp",g_cap_state);
            FILE*f=fopen(tmp,"w");
            if(f){ fprintf(f,"# cub_top capacity ceilings by profile (block size x read ratio)\n");
                   for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++){
                       cap_cell_t*c=&g_cap_mtx[b][r];
                       if(!c->valid) continue;
                       fprintf(f,"cell %d %d iops=%.0f bps=%.0f util=%.1f qd=%.2f await=%.2f when=%lld\n",
                               b,r,c->iops,c->bps,c->util,c->qd,c->await_ms,(long long)c->when);
                   }
                   if(fclose(f)==0) rename(tmp,g_cap_state); else unlink(tmp); }
        }
    }
    if(CAP2.ceil_valid && CAP2.ceil_iops>0) CAP2.headroom_pct = 100.0*cur_iops/CAP2.ceil_iops;
}

/* ---- Collection entry point, once at the end of sample_once ---- */
static void cap_collect(proc_t*P,iod_t*IO,const dstat_t*d0,int nd0,const dstat_t*d1,int nd1,double dt){
    CAP2.read_bps=IO->rb; CAP2.write_bps=IO->wb; CAP2.riops=IO->ri; CAP2.wiops=IO->wi;
    CAP2.dev_util=IO->devutil;
    CAP2.await_ms=CAP2.r_await_ms=CAP2.w_await_ms=-1; CAP2.qdepth=-1; CAP2.dev_iops=0;
    /* Latency, queue and device IOPS, from the diskstats delta of the busiest device (IO->devnm) */
    if(IO->devnm[0] && dt>0){
        const dstat_t *a=NULL,*b2=NULL;
        for(int i=0;i<nd0;i++) if(!strcmp(d0[i].name,IO->devnm)){ a=&d0[i]; break; }
        for(int i=0;i<nd1;i++) if(!strcmp(d1[i].name,IO->devnm)){ b2=&d1[i]; break; }
        if(a&&b2){
            double dr=UDELTA(b2->rios,a->rios), dw=UDELTA(b2->wios,a->wios);
            double tr=UDELTA(b2->rticks,a->rticks), tw=UDELTA(b2->wticks,a->wticks);
            CAP2.dev_iops=(dr+dw)/dt;
            /* Device bytes for the profile decision; diskstats sectors are always 512B.
               Do not mix these with the process-attributed read_bps/write_bps: a
               process numerator over a device-wide denominator gives a nonsensical
               request size (0.0KB measured). */
            CAP2.dev_rbps=UDELTA(b2->rsect,a->rsect)*512.0/dt;
            CAP2.dev_wbps=UDELTA(b2->wsect,a->wsect)*512.0/dt;
            if(dr+dw>0) CAP2.await_ms=(tr+tw)/(dr+dw);
            if(dr>0)    CAP2.r_await_ms=tr/dr;
            if(dw>0)    CAP2.w_await_ms=tw/dw;
            if(b2->tiq>=a->tiq) CAP2.qdepth=(double)(b2->tiq-a->tiq)/(dt*1000.0);
        }
    }
    /* Hit ratio from the counters when they are running, otherwise from turnover */
    CAP2.hit_pct=-1; snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"-");
    CAP2.ps_ok=0; CAP2.ps_why[0]=0;
    { static uint64_t prev[CPS_N]; static int have=0; static unsigned seen=0;
      uint64_t v[CPS_N]={0};
      if(seen!=g_inst_gen){ seen=g_inst_gen; have=0; }
      if(cap_pstat_read(P,v)){
          /* One-shot has no delta baseline, but the fact that the counters are running and the
             cumulative hit ratio are still absolute values.  Per-second figures (_ps)
             are filled only in live mode. */
          /* A watcher that just attached has only a handful of fetch samples, making the
             cumulative hit ratio a false 0%.  It is reported only above 10,000 samples -
             an invented 0% is not published as a metric. */
          if(!have && v[CPS_PB_FETCH]>=10000){
              CAP2.ps_ok=1;
              CAP2.hit_pct=(1.0-(double)v[CPS_PB_IOREAD]/(double)v[CPS_PB_FETCH])*100.0;
              snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"perfmon-cum");
              snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                       g_ascii?"counters running (watcher present) - cumulative hit ratio; rates need live mode"
                              :"카운터 동작 중(watcher 있음) — 누적 히트율, 초당값은 라이브에서");
          } else if(!have && v[CPS_PB_FETCH]>0){
              snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                       g_ascii?"counters just started (%llu fetches) - too few to rate; using turnover"
                              :"카운터 방금 시작(fetch %llu건) — 표본 부족, 회전율로 대체",
                       (unsigned long long)v[CPS_PB_FETCH]);
          }
          if(have && dt>0){
              double f=(double)(v[CPS_PB_FETCH]-prev[CPS_PB_FETCH]);
              double r=(double)(v[CPS_PB_IOREAD]-prev[CPS_PB_IOREAD]);
              CAP2.pb_fetch_ps=f/dt; CAP2.pb_ioread_ps=r/dt;
              CAP2.pb_iowrite_ps=(double)(v[CPS_PB_IOWRITE]-prev[CPS_PB_IOWRITE])/dt;
              CAP2.log_iowrite_ps=(double)(v[CPS_LOG_IOWRITE]-prev[CPS_LOG_IOWRITE])/dt;
              CAP2.commit_ps=(double)(v[CPS_COMMIT]-prev[CPS_COMMIT])/dt;
              if(f>0){ CAP2.ps_ok=1; CAP2.hit_pct=(1.0-r/f)*100.0;
                       snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"perfmon");
                       snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"counters running (watcher present)":"카운터 동작 중(watcher 있음)"); }
              else snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                            g_ascii?"no fetches this window: no watcher (statdump/monitor off) or an idle database - using turnover"
                                   :"이번 구간 fetch 증가 없음: watcher 없음(statdump·모니터 미가동) 또는 유휴 DB — 회전율로 대체");
          }
          memcpy(prev,v,sizeof prev); have=1;
      } else if(!CAP2.ps_why[0]) snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"perfmon unreadable":"perfmon 읽기 불가");
    }
    /* Live (two or more frames) uses the in-memory baseline; one-shot uses the --bcb-dump file */
    { static int frames=0;
      double nowd=(double)PG.when.tv_sec+(double)PG.when.tv_nsec/1e9;
      double keep_t=CAP2.turnover_pct, keep_m=CAP2.miss_pps, keep_r=CAP2.readmit_pct;
      cap_turnover(nowd>0?nowd:(double)time(NULL));
      /* Keep the value pgbuf_scan already produced from the dump baseline (one-shot) */
      if(CAP2.turnover_pct<0 && keep_t>=0){ CAP2.turnover_pct=keep_t; CAP2.miss_pps=keep_m; CAP2.readmit_pct=keep_r; }
      frames++; }
    if(CAP2.hit_pct<0 && CAP2.turnover_pct>=0 && PG.ok){
        /* Zero turnover means no new pages entered in the window: the working set has settled into the buffer */
        /* Turnover-based proxy for hit ratio: pages loaded per second are misses.  The
           fetch count is not observable, so the denominator is the buffer size and the
           figure reads as "what share of the buffer was newly filled in the window".
           It is not an absolute hit ratio, so its source is shown alongside. */
        CAP2.hit_pct = 100.0 - CAP2.turnover_pct;
        snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"turnover");
    }
    cap_ceiling();
}

/* ---------------- Render: time-series plots ---------------- */
/* History field selector, chosen by enum rather than index. */
enum { HK_OSUSED=0,HK_CUB,HK_SRV_RSS,HK_DB,HK_LG,HK_DYN,HK_ETC,
       HK_VSZ,HK_RSS,HK_DYNMAP,HK_DBHOT,HK_MINFLT,HK_MAJFLT,
       HK_RBPS,HK_WBPS,HK_DEVUTIL,HK_ABSORB,HK_CPU,
       /* CPU and I/O series, so every dashboard metric is also plotted */
       HK_BUSYC,HK_SATC,HK_WA,HK_LOAD1,HK_DTHR,HK_BLKPCT,HK_WIOPS,
       HK_CMASTER,HK_CPL,HK_CBROKER,HK_CCAS,
       /* Merged into the four panels: buffer pool, server PSS, server core count */
       HK_SRVPSS,HK_CSERVER,HK_PGRES,HK_PGDIRTY,HK_PGCOLD,HK_PGSPAN,HK_PGLAG,
       /* Capacity axis */
       HK_CAPIOPS,HK_CAPAWAIT,HK_CAPHIT,HK_CAPMISS,HK_CAPHEAD,
       HK_HEAPB, HK_CAPREADMIT };   /* Dynamic memory B total and refill percentage */
static double hs_val(const hsam_t*s,int k){
    switch(k){
      case HK_OSUSED: return s->osused;  case HK_CUB:    return s->cub;
      case HK_SRV_RSS:return s->srv_rss; case HK_DB:     return s->db;
      case HK_LG:     return s->lg;      case HK_DYN:    return s->dyn;
      case HK_ETC:    return s->etc;     case HK_VSZ:    return s->vsz;
      case HK_RSS:    return s->rss;     case HK_DYNMAP: return s->dynmap;
      case HK_DBHOT:  return s->dbhot;   case HK_MINFLT: return s->minflt;
      case HK_MAJFLT: return s->majflt;  case HK_RBPS:   return s->rbps;
      case HK_WBPS:   return s->wbps;    case HK_DEVUTIL:return s->devutil;
      case HK_ABSORB: return s->absorb;  case HK_CPU: return s->cpu;
      case HK_BUSYC:  return s->busy_cores; case HK_SATC:  return s->sat_cores;
      case HK_WA:     return s->cpu_wa;     case HK_LOAD1: return s->load1;
      case HK_DTHR:   return s->d_thr;      case HK_BLKPCT:return s->blkpct;
      case HK_WIOPS:  return s->wiops;
      case HK_CMASTER:return s->c_master;   case HK_CPL:   return s->c_pl;
      case HK_CBROKER:return s->c_broker;   case HK_CCAS:  return s->c_cas;
      case HK_SRVPSS: return s->srv;        case HK_CSERVER:return s->cpu/100.0;
      case HK_PGRES:  return s->pg_res_pct; case HK_PGDIRTY:return s->pg_dirty;
      case HK_PGCOLD: return s->pg_cold_pct;case HK_PGSPAN: return s->pg_span;
      case HK_PGLAG:  return s->pg_lag;
      case HK_HEAPB:  return s->heapb;
      case HK_CAPREADMIT: return s->cap_readmit;
      case HK_CAPIOPS: return s->cap_iops;  case HK_CAPAWAIT:return s->cap_await;
      case HK_CAPHIT:  return s->cap_hit;   case HK_CAPMISS: return s->cap_miss;
      case HK_CAPHEAD: return s->cap_head;
      default: return 0.0;
    }
}
/* Value format: 0 bytes, 1 percent, 2 bytes per second, 3 events per second */
static const char* pf_val(double v,int fmt){
    static char b[8][32]; static int t=0; char*o=b[t=(t+1)&7];
    if(fmt==0)      snprintf(o,32,"%s",H(v));          /* Bytes, unit chosen automatically */
    else if(fmt==1) snprintf(o,32,"%.0f%%",v);          /* Percentage */
    else if(fmt==2) snprintf(o,32,"%s/s",H(v));         /* Bytes per second */
    else if(fmt==4) snprintf(o,32,v>=10?"%.0f":"%.1f",v); /* A count: cores, threads, load */
    else            snprintf(o,32,v>=10?"%.0f/s":"%.1f/s",v);  /* Events per second */
    return o;
}
typedef struct { int key,col,fmt; const char*lab; } pser_t;
#define PLOT_AXW 9      /* Width of the left-hand vertical ruler */

/* One panel = box + three vertical ticks + multi-series chart + one legend row.
   With norm!=0 each series is normalized to its own maximum: overlaying series with
   different units (%, per second) on one axis flattens the smaller to the floor. */
/* ov overlays one secondary series in a different unit (load and the like),
   normalized to its own maximum, with the real value left in the legend.
   verdict is one line under the legend (colour vcol); NULL draws no row. */
static void ticks_bottom(int x0,int w,int yb);
/* ticks draws one time-ruler row under the chart and above the legend, so each panel
   carries its own axis and a point is dated without looking down the screen. */
/* ---- Secondary bar subchart (zoom only) ----
   Like a price chart with volume beneath it: putting the quantity that causes the
   zoomed panel's metric below it lets cause and effect be read on one time axis.
   Drawn with block glyphs, visually distinct from the braille line above.  Each
   series is normalized to its own maximum (the units differ) and the legend
   carries the real values. */
/* hrows is the row count given to ONE series: 1 draws a single-row bar, more draws
   a taller one.  Zoom grows the main chart to 40 rows, and an effect metric pinned
   to one row would look flattened beside it. */
/* Four-dot braille bars.  Block characters gave one sample per cell; braille
   carries two horizontally, doubling time resolution at the same width, and
   shares a glyph family with the line chart above.  Vertically four steps per
   cell.  The BRD[col][row] bitmap is shared with the line chart, so the two
   cannot drift apart.  Bars fill from the bottom, lighting every dot below the
   height, so they read as area rather than line. */
static void cv_subbars(int x,int y,int w,const pser_t*se,int ns,int hrows){
    if(ns<1||w<4||hrows<1) return;
    int gx=x+1+PLOT_AXW, gw=w-2-PLOT_AXW;
    if(gw<2) return;
    if(hrows<1) hrows=1;
    for(int si=0; si<ns; si++){
        /* Percentage series (fmt==1) are drawn against 100: normalizing to their own maximum
           fills the bar even when the value sits at a steady 0.4%%, which reads as
           "full".  Other units (B/s, counts) have no absolute reference and use their
           own maximum. */
        double mx = (se[si].fmt==1) ? 100.0 : 0.0;
        if(mx<=0) for(int j=0;j<HN;j++){ double v=hs_val(&HS[j],se[si].key); if(v>mx) mx=v; }
        /* Tick widths are display widths.  Cutting bytes with %.*s splits a Hangul label
           mid-sequence and leaves a broken character; cutf respects both the character
           boundary and the width. */
        const char*lab=cutf(se[si].lab?se[si].lab:"",PLOT_AXW);
        int lrow=y+si*hrows+hrows-1;          /* Bottom row of the series block */
        cv_put(x+1,lrow,padf(lab,PLOT_AXW,1),"38;5;240");
        if(HN<1) continue;
        /* Where several samples share a cell, take the maximum - a spike is not averaged away
           (the same rule as the line chart's peak-preserving downsample). */
        /* Reserve the right-hand value slot in advance so a bar cannot cover the number. */
        char vtx[24]; snprintf(vtx,sizeof vtx,"%s",pf_val(HN>0?hs_val(&HS[HN-1],se[si].key):0,se[si].fmt));
        int vw=(int)strlen(vtx)+1;
        int bw=gw-vw; if(bw<2) bw=gw;
        /* Braille cell buffer: hrows rows by bw columns for this series, two samples per cell horizontally. */
        static unsigned char bcell[8][CV_W];
        if(hrows>8) hrows=8;
        for(int r=0;r<hrows;r++) for(int c=0;c<bw && c<CV_W;c++) bcell[r][c]=0;
        int px=bw*2, py=hrows*4;                    /* Dot-level resolution */
        for(int i=0;i<px;i++){
            int a=(int)((double)i*HN/px), b=(int)((double)(i+1)*HN/px);
            if(b>HN) b=HN;
            if(b<=a) b=a+1;
            double val=0;
            for(int k=a;k<b && k<HN;k++){ double v=hs_val(&HS[k],se[si].key); if(v>val) val=v; }
            if(mx<=0||val<=0) continue;
            double fr=val/mx; if(fr>1) fr=1;
            int hpx=(int)(fr*py+0.5);               /* Height in dots from the bottom */
            if(hpx<1) hpx=1;                        /* At least one dot for a non-zero value, so it cannot vanish */
            if(hpx>py) hpx=py;
            for(int d=0;d<hpx;d++){
                int yy=py-1-d;                      /* Filled bottom up */
                int cx=i/2, cy=yy/4;
                if(cx<bw && cx<CV_W && cy<hrows) bcell[cy][cx]|=(unsigned char)BRD[i%2][yy%4];
            }
        }
        { char col[16]; snprintf(col,sizeof col,"38;5;%d",se[si].col);
          for(int r=0;r<hrows;r++) for(int c=0;c<bw && c<CV_W;c++){
              if(!bcell[r][c]) continue;
              char g[8]; unsigned cp=0x2800u|bcell[r][c];
              g[0]=(char)(0xE0|(cp>>12)); g[1]=(char)(0x80|((cp>>6)&0x3F)); g[2]=(char)(0x80|(cp&0x3F)); g[3]=0;
              cv_put(gx+c,y+si*hrows+r,g,col);
          } }
        /* Current value at the right: the bar gives the shape, the number is read here */
        int vx=x+w-2-(int)strlen(vtx);
        if(vx>gx+2){ char cc[16]; snprintf(cc,sizeof cc,"38;5;%d",se[si].col); cv_put(vx,lrow,vtx,cc); }
    }
}
/* bars/nb draws the cause-metric bars INSIDE the box, below the time axis and above
   the legend.  Outside the box they read as a separate element, detached from the
   graph. */
static void cv_panel2(int x,int y,int w,int ch,const char*title,const char*rtitle,
                      const pser_t*se,int ns,double fixed_ymax,int norm,const char*note,
                      const pser_t*ov,const char*verdict,const char*vcol,int ticks,
                      const pser_t*bars,int nb,int bpr){
    static double buf[CV_NS][HIST_MAX];
    const double*vp[CV_NS]={0}; int colv[CV_NS]={0};
    pser_t sa[CV_NS]; int nt;
    if(ns>CV_NS-1) ns=CV_NS-1;
    memcpy(sa,se,sizeof(pser_t)*(size_t)ns); nt=ns;
    if(ov){ sa[nt++]=*ov; }
    int extra = (verdict?1:0) + (ticks?1:0) + (nb>0?nb*(bpr>0?bpr:1):0);
    /* Footnotes come after the legend: when width runs short the footnote goes and the
       series values stay (the footnote is also in the help, the values are not). */
    cv_box2(x,y,w,ch+3+extra,title,rtitle);
    int gx=x+1+PLOT_AXW, gw=w-2-PLOT_AXW;
    if(gw<2) return;
    int ty_ = y+ch+1;              /* Ruler, then cause bars, then legend, then verdict */
    int by_ = ty_ + (ticks?1:0);
    int ly_ = by_ + (nb>0?nb*(bpr>0?bpr:1):0);
    if(ticks) ticks_bottom(gx,gw,ty_);
    if(nb>0) cv_subbars(x,by_,w,bars,nb,bpr>0?bpr:1);
    if(verdict) cv_put(x+2,ly_+1,cutf(verdict,w-4),vcol?vcol:"38;5;244");
    if(HN<2){
        cv_put(gx,y+1+ch/2,cutf(L("표본이 부족합니다 — 선을 그리려면 2개 이상 필요"),gw),"38;5;244");
        return;
    }
    double gmax=fixed_ymax;
    if(!norm && gmax<=0){
        for(int i=0;i<ns;i++) for(int j=0;j<HN;j++){
            double v=hs_val(&HS[j],se[i].key); if(v>gmax) gmax=v; }
        gmax*=1.15;
    }
    if(gmax<=0) gmax=1;
    for(int i=0;i<nt;i++){
        double m=0; int nrm = norm || (i>=ns);      /* Secondary series are always normalized */
        if(nrm){ for(int j=0;j<HN;j++){ double v=hs_val(&HS[j],sa[i].key); if(v>m)m=v; }
                 m*=1.15; if(m<=0)m=1; }
        for(int j=0;j<HN;j++){
            double v=hs_val(&HS[j],sa[i].key);
            buf[i][j] = nrm ? v/m*gmax : v;
        }
        vp[i]=buf[i]; colv[i]=sa[i].col;
    }
    cv_chart_n(gx,y+1,gw,ch,vp,colv,nt,HN,gmax);
    se=sa; ns=nt;   /* The legend below includes the secondary series */
    /* Vertical ruler: a normalized panel has no common axis, so drawing one would lie */
    if(!norm){
        /* The chart occupies y+1..y+ch; the mid label goes on its centre row, or the ruler lies. */
        cv_put(x+1,y+1,  padf(pf_val(gmax,se[0].fmt),PLOT_AXW,1),"38;5;240");
        /* Mid ticks sit on the cell the line actually crosses.  cv_chart_n plots with
           yy=(py-1)*(1-f)+0.5, so applying that at f=0.5 keeps label and line aligned. */
        if(ch>=3){ int mid=(int)((ch*4-1)*0.5+0.5)/4;
                   cv_put(x+1,y+1+mid,padf(pf_val(gmax/2,se[0].fmt),PLOT_AXW,1),"38;5;240"); }
        cv_put(x+1,y+ch, padf(pf_val(0,se[0].fmt),PLOT_AXW,1),"38;5;240");
    }else{
        cv_put(x+1,y+1+ch/2,padf(g_ascii?"norm":"정규화",PLOT_AXW,1),"38;5;240");
    }
    /* Legend: current value and (min-max) per series.  When it does not fit, shorten the
       names, then drop the ranges - a coloured line with no name cannot be read. */
    {
    /* The legend is a blank row below the vertical ticks, so there is no reason to reserve
       the tick width (PLOT_AXW); giving it the full inner width lets a five-series
       panel show its values. */
    int lgx=x+2, ly=ly_, lend=x+w-1;
    char item[CV_NS][96]; int need[CV_NS];
    for(int pass=0;pass<3;pass++){
        int tot=0;
        for(int i=0;i<ns;i++){
            double cur=hs_val(&HS[HN-1],se[i].key), mn=cur,mx=cur;
            for(int j=0;j<HN;j++){ double v=hs_val(&HS[j],se[i].key);
                if(v<mn)mn=v;
                if(v>mx)mx=v; }
            if(pass==0)      snprintf(item[i],96,"%s %s (%s~%s)",se[i].lab,
                                      pf_val(cur,se[i].fmt),pf_val(mn,se[i].fmt),pf_val(mx,se[i].fmt));
            else if(pass==1) snprintf(item[i],96,"%s %s",se[i].lab,pf_val(cur,se[i].fmt));
            else             snprintf(item[i],96,"%s",se[i].lab);
            need[i]=disp_w(item[i])+2;
            tot+=need[i]+1;
        }
        /* No trailing space is needed after the last entry; returning it keeps a legend that
           just fits from being abbreviated one step further. */
        if(lgx+tot-1 <= lend || pass==2) break;
    }
    /* Right-aligned, so the eye scans the same x in every panel.  When the legend does
       not fit, keep as many leading entries as do, hold that group to the right and
       mark the elision with a leading ellipsis - falling back to left alignment
       would leave one panel's legend out of line with the rest. */
    int lx=lgx;
    int nw=(note&&*note)?disp_w(note)+3:0;   /* Footnote slot plus one space */
    int room=lend-nw-lgx;                    /* Cells available to the legend */
    int nfit=0, tot=0;
    for(int i=0;i<ns;i++){
        int add=need[i]+(nfit?1:0);
        int mark=(i<ns-1)?2:0;               /* If more follows, room for an ellipsis and a space */
        if(tot+add+mark>room) break;
        tot+=add; nfit++;
    }
    if(nfit<ns && nfit>0) tot+=2;            /* "… " */
    { int start=lend-nw-tot; if(start>lgx) lx=start; }
    if(nfit<ns && nfit>0){ cv_put(lx,ly,"…","38;5;240"); lx+=2; }
    for(int i=0;i<nfit;i++){
        char c[16]; snprintf(c,sizeof c,"38;5;%d",se[i].col);
        cv_put(lx,ly,"●",c); cv_put(lx+1,ly,item[i],"38;5;250");
        lx+=need[i]+1;
    }
    if(nfit==0 && ns>0 && lx+1<lend) cv_put(lx,ly,"…","38;5;240");
    if(note && *note){
        int nw=disp_w(note);
        /* One space, so it does not touch the border */
        if(lx+nw+1 <= lend) cv_put(lend-nw-1,ly,note,"38;5;240");
    }
    }
}

/* ---- One-line verdict, per panel ----
   Follows the I/O box convention: cite only signals that actually hold, set
   thresholds relative to the window and the core count rather than absolutely, and
   state plain facts when uncertain (the warning mark is for actionable signals). */
static const char *VD_WARN="1;38;5;203", *VD_NOTE="38;5;215", *VD_OK="38;5;79", *VD_INFO="38;5;244";
static double vd_win(void){ return HN>1 ? HS[HN-1].t-HS[0].t : 0; }
static void vd_wtxt(char*o,size_t n){ double w=vd_win();
    if(w>=60) snprintf(o,n,"%dm%02ds",(int)(w/60),(int)w%60); else snprintf(o,n,"%.0fs",w); }
/* A: memory */
static void vd_mem(char*o,size_t n,const char**col){
    if(HN<4){ snprintf(o,n,"%s",g_ascii?"collecting samples...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*a=&HS[0],*b=&HS[HN-1];
    double d=b->srv-a->srv, pct=a->srv>0?d/a->srv:0, w=vd_win(), rate=w>0?d/w*3600:0;
    char wt[24]; vd_wtxt(wt,sizeof wt);
    char rt[32]; snprintf(rt,sizeof rt,"%s%s/h",rate>=0?"+":"-",H(rate<0?-rate:rate));
    int load_up = (b->load1 > a->load1*1.3 && b->load1 >= 1.0);
    double gap = b->vsz-b->rss;
    if(pct>0.05){
        if(load_up){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! server +%s (%s), load rising with it -> load-driven growth (buffers/work memory)"
                               :"⚠ 서버 +%s (%s) · load 도 함께 상승 → 부하 기인 증가(작업 메모리·캐시)",H(d),rt); }
        else { *col=VD_WARN;
            snprintf(o,n,g_ascii?"! server +%s (%s) while load is flat -> growth unrelated to load: check heap detail (B/A)"
                               :"⚠ 서버 +%s (%s) · load 평탄 → 부하와 무관한 증가 — 동적 메모리 상세(B/A)에서 어느 항목인지 확인",H(d),rt); }
    } else if(pct<-0.05){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"server -%s over %s - work memory / caches released":"· 서버 -%s (%s 동안) — 작업 메모리·캐시 반납",H(-d),wt);
    } else { *col=VD_OK;
        snprintf(o,n,g_ascii?"stable - server %s, change %s%s over %s%s":"· 안정 — 서버 %s, 변동 %s%s (%s)%s",
                 H(b->srv),d>=0?"+":"-",H(d<0?-d:d),wt,
                 (gap>2.5*b->rss && b->rss>0) ? (g_ascii?" / large reserved-only span (normal: stacks/arenas)":" / 예약만 한 분량 큼(정상: 스택·아레나)") : "");
    }
}
/* B: the fill side, volume to buffer */
static void vd_buf(char*o,size_t n,const char**col){
    if(HN<1){ snprintf(o,n,"%s",g_ascii?"collecting...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*b=&HS[HN-1];
    double rb=b->rbps;
    if(!b->pg_ok){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"buffer-pool state unavailable - disk read %s/s, cache absorb %s":"· 버퍼풀 상태 읽기 비활성 — 디스크 읽기 %s/s, 캐시 흡수율 %s",
                 H(rb), b->absorb_ok?pf_val(b->absorb,1):(g_ascii?"idle":"유휴")); return; }
    if(b->pg_res_pct<90.0){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"buffer still filling (%.0f%% resident) - hit-rate judgement deferred, read %s/s"
                           :"· 버퍼 채우는 중 (상주 %.0f%%) — 히트율 판단 보류, 읽기 %s/s",b->pg_res_pct,H(rb)); return; }
    /* Cold zone occupancy alone cannot separate a short buffer from a scan - both run
       cold.  Only the refill rate separates them: pages evicted and read back again
       mean the working set exceeds the buffer and more buffer helps, while read-once
       pages (refill near 0) do not benefit.  A run at 76%% refill was once misread
       as a scan without reuse. */
    if(rb>=8.0*1024*1024 && b->cap_readmit>=30.0){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! buffer shortage - re-admit %.0f%%, read %s/s: working set exceeds the buffer, enlarging helps"
                           :"⚠ 버퍼 부족 — 재적재 %.0f%%·읽기 %s/s: 작업집합이 버퍼보다 크다, 증설이 듣는다",b->cap_readmit,H(rb)); return; }
    if(rb>=8.0*1024*1024 && b->pg_cold_pct>=80.0){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! scan without reuse - cold zone %.0f%%, re-admit %.0f%%, read %s/s: a bigger buffer will not cut reads"
                           :"⚠ 재사용 없는 스캔 — cold 존 %.0f%%·재적재 %.0f%%·읽기 %s/s: 버퍼를 키워도 읽기가 줄지 않음",
                 b->pg_cold_pct,b->cap_readmit,H(rb)); return; }
    if(rb<1.0*1024*1024){ *col=VD_OK;
        snprintf(o,n,g_ascii?"served from buffer - disk read %s/s, resident %.0f%%, hot+warm %.0f%%"
                           :"· 버퍼에서 처리 중 — 디스크 읽기 %s/s, 상주 %.0f%%, hot+warm 존 %.0f%%",H(rb),b->pg_res_pct,100.0-b->pg_cold_pct); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"read %s/s - hot+warm %.0f%% cold %.0f%% - working set turning over":"· 읽기 %s/s · hot+warm %.0f%% cold %.0f%% — 작업집합 교체 중",
             H(rb),100.0-b->pg_cold_pct,b->pg_cold_pct);
}
/* Host-wide verdict: separates a CUBRID problem from a machine-level one.
   Device utilization is host-wide (diskstats), so it belongs here. */
static void vd_host(char*o,size_t n,const char**col,int ncores){
    if(HN<1){ snprintf(o,n,"%s",g_ascii?"collecting...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*b=&HS[HN-1];
    if(ncores<=0) ncores=1;
    double cubpct = b->ram>0 ? b->cub/b->ram*100 : 0;
    if(b->sat_cores>=1){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! %.0f core(s) saturated of %d - host CPU is the constraint (CUBRID %.0f%% of RAM)"
                           :"⚠ 포화 코어 %.0f/%d개 — 호스트 CPU 가 제약 (CUBRID 는 RAM 의 %.0f%%)",
                 b->sat_cores,ncores,cubpct); return; }
    if(b->load1 > ncores*1.2){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! load %.1f > %d cores - run queue waiting (host-wide, not only CUBRID)"
                           :"⚠ load %.1f > 코어 %d — 실행 대기 큐 존재(호스트 전체, CUBRID 만은 아님)",
                 b->load1,ncores); return; }
    if(b->devutil>=80){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! device %.0f%% busy - host storage is the constraint (see panels 3/4 for this instance)"
                           :"⚠ 장치 %.0f%% — 호스트 스토리지가 제약 (이 인스턴스 몫은 3·4번 패널에서)",
                 b->devutil); return; }
    if(b->ram>0 && b->osused/b->ram>0.92){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! host RAM %.0f%% used - little headroom left (CUBRID %.0f%%)"
                           :"⚠ 호스트 RAM %.0f%% 사용 — 여유가 적다 (CUBRID 는 %.0f%%)",
                 b->osused/b->ram*100,cubpct); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"host ok - %.1f of %d cores, RAM %.0f%%, device %.0f%%, CUBRID %.0f%% of RAM"
                       :"· 호스트 정상 — 사용 %.1f/%d 코어, RAM %.0f%%, 장치 %.0f%%, CUBRID 는 RAM 의 %.0f%%",
             b->busy_cores,ncores,b->ram>0?b->osused/b->ram*100:0,b->devutil,cubpct);
}
/* D: the drain side, buffer back to volume */
static void vd_flush(char*o,size_t n,const char**col){
    if(HN<1){ snprintf(o,n,"%s",g_ascii?"collecting...":"표본 수집 중…"); *col=VD_INFO; return; }
    const hsam_t*b=&HS[HN-1];
    double dmin=b->pg_dirty; for(int i=0;i<HN;i++) if(HS[i].pg_ok && HS[i].pg_dirty<dmin) dmin=HS[i].pg_dirty;
    if(!b->pg_ok){
        if(b->devutil>=80 && b->wbps>=1.0*1024*1024){ *col=VD_WARN;
            snprintf(o,n,g_ascii?"! device %.0f%% busy with %s/s writes - write bottleneck":"⚠ 장치 %.0f%% · 쓰기 %s/s — 쓰기 병목",b->devutil,H(b->wbps)); }
        else { *col=VD_INFO; snprintf(o,n,g_ascii?"write %s/s, device %.0f%% (buffer-pool state unavailable)":"· 쓰기 %s/s, 장치 %.0f%% (버퍼풀 상태 읽기 비활성)",H(b->wbps),b->devutil); }
        return; }
    if(b->pg_lag>0){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! fsync delay %.0f log pg (append ahead of flushed) - commit latency risk":"⚠ fsync 지연 %.0f 로그pg (append 가 flushed 앞) — 커밋 지연 위험",b->pg_lag); return; }
    if(b->devutil>=80 && b->wbps>=1.0*1024*1024){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! write bottleneck - device %.0f%%, write %s/s, dirty %.0f pages":"⚠ 쓰기 병목 — 장치 %.0f%%, 쓰기 %s/s, dirty %.0f 페이지",b->devutil,H(b->wbps),b->pg_dirty); return; }
    if(b->pg_dirty>=100 && b->pg_dirty>1.5*dmin){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! dirty pages accumulating %.0f (flush delay %.0f log pg) - grows until checkpoint/flush catches up"
                           :"⚠ dirty 누적 %.0f 페이지 (flush 지연 %.0f 로그pg) — 체크포인트·flush 가 따라잡기 전까지 증가",b->pg_dirty,b->pg_span); return; }
    if(b->pg_dirty==0){ *col=VD_OK;
        snprintf(o,n,g_ascii?"all reflected to volumes - dirty 0, no flush delay, write %s/s":"· 볼륨 반영 완료 — dirty 0, flush 지연 없음, 쓰기 %s/s",H(b->wbps)); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"dirty %.0f pages - flush delay %.0f log pages - device %.0f%% - write %s/s":"· dirty %.0f 페이지 · flush 지연 span %.0f 로그pg · 장치 %.0f%% · 쓰기 %s/s",
             b->pg_dirty,b->pg_span,b->devutil,H(b->wbps));
}


/* ---- Capacity verdict: is the answer more, less, or tuning? ----
   Same convention as the I/O verdict: only signals that hold, thresholds relative to
   the allocated resource, and the warning mark for actionable signals. */
static void vd_capacity(char*o,size_t n,const char**col){
    const cap_t*c=&CAP2;
    if(c->over_budget){ *col=VD_NOTE;
        snprintf(o,n,g_ascii?"! capacity aggregation skipped this frame (%.0fms > %.0fms) - values withheld"
                            :"⚠ 이 프레임 용량 취합 생략 (%.0fms > %.0fms) — 값 보류",c->ms,(double)CAP_BUDGET_MS);
        return; }
    /* Headroom against the ceiling is the strongest conclusion, but headroom alone
       does not establish a limit - the ceiling is an observation from some other
       moment.  A real limit comes with the device falling behind: await >= 20ms or
       queue depth >= 10.  With neither, report "near ceiling" only and do not
       recommend more capacity. */
    if(c->headroom_pct>=0){
        if(c->headroom_pct>=80){
            int pressed = (c->await_ms>=20.0) || (c->qdepth>=10.0);
            if(pressed){ *col=VD_WARN;
                snprintf(o,n,g_ascii?"! IOPS %.0f of ceiling %.0f (%.0f%%, %s/%s) with await %.1fms q%.1f - at capacity: add IOPS or cut reads"
                                    :"⚠ IOPS %.0f / 상한 %.0f (%.0f%%, %s/%s), 대기 %.1fms 큐 %.1f — 용량 한계: IOPS 증설 또는 읽기 감축",
                         c->dev_iops,c->ceil_iops,c->headroom_pct,
                         CAP_BS_NAME[c->prof_bs],CAP_RW_NAME[c->prof_rw],
                         c->await_ms>=0?c->await_ms:0,c->qdepth>=0?c->qdepth:0); return; }
            *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! IOPS %.0f near ceiling %.0f (%.0f%%, %s/%s) but await %.1fms q%.1f is not queueing - near ceiling, not proven limited"
                                :"⚠ IOPS %.0f 로 상한 %.0f 근접 (%.0f%%, %s/%s) — 대기 %.1fms·큐 %.1f 로 적체 없음: 한계 단정 불가(상한 근접)",
                     c->dev_iops,c->ceil_iops,c->headroom_pct,
                     CAP_BS_NAME[c->prof_bs],CAP_RW_NAME[c->prof_rw],
                     c->await_ms>=0?c->await_ms:0,c->qdepth>=0?c->qdepth:0); return; }
        if(c->headroom_pct<=20 && c->dev_util<50){ *col=VD_OK;
            snprintf(o,n,g_ascii?"spare: IOPS %.0f of ceiling %.0f (%.0f%%), await %.1fms - storage is not the limit"
                                :"· 여유: IOPS %.0f / 상한 %.0f (%.0f%%), 대기 %.1fms — 스토리지가 제약이 아님",
                     c->dev_iops,c->ceil_iops,c->headroom_pct,c->await_ms>=0?c->await_ms:0); return; }
    }
    /* Past the latency threshold the answer is the storage tier, whatever the IOPS headroom */
    if(c->await_ms>=20.0 && c->dev_iops>=10){ *col=VD_WARN;
        snprintf(o,n,g_ascii?"! await %.1fms at only %.0f IOPS (q%.1f) - latency-bound: faster storage, not more IOPS"
                            :"⚠ 대기 %.1fms인데 IOPS %.0f (큐 %.1f) — 지연 병목: IOPS 증설보다 빠른 스토리지",
                 c->await_ms,c->dev_iops,c->qdepth>=0?c->qdepth:0); return; }
    /* Buffer verdict: sustained misses call for more, no turnover and no use makes it a candidate to shrink */
    if(PG.ok && c->miss_pps>=0){
        double res=PG.nbuf>0?100.0*PG.resident/PG.nbuf:0;
        if(c->readmit_pct<0 && c->miss_pps>=200){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted (turnover %.0f%%) - re-read share unknown from a file baseline: run live (-b) to tell buffer shortage from scan"
                                :"⚠ 초당 %.0f페이지 적재(회전율 %.0f%%) — 파일 기준선이라 재적재 비율 미상: 라이브(-b)로 버퍼 부족과 스캔을 구분",
                     c->miss_pps,c->turnover_pct); return; }
        if(c->miss_pps>=200 && c->readmit_pct>=30){ *col=VD_WARN;
            snprintf(o,n,g_ascii?"! buffer too small: %.0f pages/s admitted, %.0f%% are re-reads - raise data_buffer_size"
                                :"⚠ 버퍼 부족: 초당 %.0f페이지 적재 중 %.0f%%가 재적재 — data_buffer_size 증설",
                     c->miss_pps,c->readmit_pct); return; }
        if(c->miss_pps>=200 && c->readmit_pct<10){ *col=VD_NOTE;
            snprintf(o,n,g_ascii?"! %.0f pages/s admitted but almost no re-reads - scan workload: bigger buffer won't help"
                                :"⚠ 초당 %.0f페이지 적재이나 재적재 거의 없음 — 스캔 워크로드: 버퍼 증설 무효",
                     c->miss_pps); return; }
        if(res<60.0 && c->miss_pps<20){ *col=VD_OK;
            snprintf(o,n,g_ascii?"over-provisioned: only %.0f%% of data_buffer ever used, %.0f pages/s - can shrink"
                                :"· 과다 할당: data_buffer 중 %.0f%%만 사용, 초당 %.0f페이지 — 축소 가능",
                     res,c->miss_pps); return; }
    }
    /* With no ceiling observed, say so rather than pass an estimate off as fact */
    if(c->headroom_pct<0){ *col=VD_INFO;
        snprintf(o,n,g_ascii?"IOPS %.0f, await %.1fms, dev %.0f%% - ceiling not yet observed (needs a >=90%% util moment)"
                            :"· IOPS %.0f, 대기 %.1fms, 장치 %.0f%% — 상한 미관측(사용률 90%% 순간이 필요)",
                 c->dev_iops,c->await_ms>=0?c->await_ms:0,c->dev_util); return; }
    *col=VD_INFO;
    snprintf(o,n,g_ascii?"IOPS %.0f (%.0f%% of ceiling), await %.1fms, hit %.0f%% (%s)"
                        :"· IOPS %.0f (상한의 %.0f%%), 대기 %.1fms, 히트 %.0f%% (%s)",
             c->dev_iops,c->headroom_pct,c->await_ms>=0?c->await_ms:0,
             c->hit_pct>=0?c->hit_pct:0,c->hit_src);
}

/* ---- Four-panel definition, shared by the plots and the dashboard ---- */
static int g_plot_zoom=0;   /* 0 is the four-panel view; 1-4 give that panel the full height */
static int g_sb_per=1;      /* Rows per cause-metric series when zoomed */
static const pser_t PL_LOAD={HK_LOAD1,227,4,"load(1분)"};
/* Host overlay on panels 3 and 4.  Its scope differs from the main series (this
   instance), so it is drawn as a secondary overlay; a solid line on the same axis
   confuses "my value" with "the environment".  The overlay is normalized to its
   own maximum and the legend carries the real values. */
static const pser_t PL_DEVUTIL={HK_DEVUTIL,240,1,"장치%(호스트)"};
static const pser_t PL_AWAIT  ={HK_CAPAWAIT,240,4,"await ms(호스트)"};
static const pser_t PL_DEVUTIL_EN={HK_DEVUTIL,240,1,"device%(host)"};
static const pser_t PL_AWAIT_EN  ={HK_CAPAWAIT,240,4,"await ms(host)"};
/* Panel A, memory on a byte axis.  server uses PSS, as the dashboard's process and
   trend boxes do (check-hist keeps testing the db+lg+dyn+etc == RSS identity on the
   RSS axis). */
static int pl_series_A(pser_t*s2,int with_vsz){
    int n=0;
    s2[n].key=HK_SRVPSS; s2[n].col=211; s2[n].fmt=0; s2[n].lab="server(PSS)"; n++;
    s2[n].key=HK_DB;     s2[n].col=79;  s2[n].fmt=0; s2[n].lab="data_buffer"; n++;
    s2[n].key=HK_LG;     s2[n].col=178; s2[n].fmt=0; s2[n].lab="log_buffer"; n++;
    s2[n].key=HK_DYN;    s2[n].col=203; s2[n].fmt=0; s2[n].lab="dynamic-heap"; n++;
    if(with_vsz){ s2[n].key=HK_VSZ; s2[n].col=117; s2[n].fmt=0; s2[n].lab=g_ascii?"reserved(VmSize)":"할당(VmSize)"; n++; }
    return n;
}
/* Panel 1 is host-wide, panels 2-4 belong to the selected instance.  Mixing the
   two scopes on one screen invites reading one axis against the other.  Panel 1
   stays on top even when zoomed, so a magnified instance metric is still seen
   against the host situation. */
/* Series for the zoomed panel's bottom bars - the quantities that cause its main
   metric.  Returns the series count (0 draws no subchart). */
/* The permanent bottom bar, used in four-panel mode too: metrics too small to leave
   the floor of a line chart get their own bars (the volume slot of a price chart).
   hot is commonly around 0.4%% and invisible as a line; bars normalize to their
   own maximum, so small values keep their shape. */
static int pl_subbars_always(int idx,pser_t*b){
    int n=0;
    if(idx==3){
        b[n].key=HK_DBHOT; b[n].col=117; b[n].fmt=1; b[n].lab=g_ascii?"hot%":"핫%"; n++;
        b[n].key=HK_ABSORB;b[n].col=228; b[n].fmt=1; b[n].lab=g_ascii?"absorb":"흡수%"; n++;
    }
    return n;
}
static int pl_subbars(int idx,pser_t*b){
    int n=0;
    switch(idx){
    case 2:   /* Why memory grew: engine structures (B) plus page demand pressure */
        b[n].key=HK_HEAPB;  b[n].col=42;  b[n].fmt=0; b[n].lab=g_ascii?"heapB":"heapB"; n++;
        b[n].key=HK_MINFLT; b[n].col=178; b[n].fmt=3; b[n].lab=g_ascii?"minflt":"마이너"; n++;
        break;
    case 3:   /* Why reads grew: loads (misses), refills (working set over buffer), OS cache absorption */
        b[n].key=HK_CAPMISS;b[n].col=178; b[n].fmt=3; b[n].lab=g_ascii?"admit":"적재"; n++;
        /* Refill%% is the share of pages evicted and read back.  High load with refill means
           the buffer is short (more helps); high load without refill is a scan (more
           does not).  It is the one criterion separating the two verdicts, so it is
           shown as a bar where the reasoning can be checked. */
        b[n].key=HK_CAPREADMIT;b[n].col=203; b[n].fmt=1; b[n].lab=g_ascii?"readmit":"재적재%"; n++;
        b[n].key=HK_ABSORB; b[n].col=228; b[n].fmt=1; b[n].lab=g_ascii?"absorb":"흡수%"; n++;
        b[n].key=HK_MAJFLT; b[n].col=140; b[n].fmt=3; b[n].lab=g_ascii?"majflt":"메이저"; n++;
        break;
    case 4:   /* Why writes fall behind: the actual write volume and the threads blocked on disk */
        b[n].key=HK_WBPS;   b[n].col=117; b[n].fmt=2; b[n].lab=g_ascii?"write":"쓰기"; n++;
        b[n].key=HK_DTHR;   b[n].col=203; b[n].fmt=4; b[n].lab=g_ascii?"D thr":"D스레드"; n++;
        b[n].key=HK_BLKPCT; b[n].col=215; b[n].fmt=1; b[n].lab=g_ascii?"blkio":"blkio%"; n++;
        break;
    default: break;   /* Panel 1 (host) is not a zoom target */
    }
    return n;
}
static void pl_panel(int idx,int y,int W,int ch,int hot_on,int ncores,
                     const char*hdr,const char*hdr_r){
    /* Cause-metric bars go inside the panel, below the time axis and above the legend.
       Zoom shows more of them; four-panel mode shows only those invisible as lines
       (hot%, absorption%). */
    pser_t bars[6]; int nb;
    if(g_plot_zoom==idx) nb=pl_subbars(idx,bars);
    else                 nb=pl_subbars_always(idx,bars);
    int bper = (g_plot_zoom==idx) ? g_sb_per : 1;   /* Zoomed: several rows per series */
    char t1[200],t2[160],vb[320]; const char*vc=VD_INFO;
    /* Sized by CV_NS, so adding series to a panel cannot overflow it */
    pser_t s2[CV_NS]; int n=0;
    switch(idx){
    case 1: {   /* Host-wide: what state this machine is in */
        s2[n].key=HK_BUSYC;  s2[n].col=117; s2[n].fmt=4; s2[n].lab=g_ascii?"busy cores":"사용 코어"; n++;
        s2[n].key=HK_SATC;   s2[n].col=203; s2[n].fmt=4; s2[n].lab=g_ascii?"saturated":"포화 코어"; n++;
        s2[n].key=HK_OSUSED; s2[n].col=140; s2[n].fmt=0; s2[n].lab=g_ascii?"host RAM used":"호스트 RAM 사용"; n++;
        s2[n].key=HK_CUB;    s2[n].col=211; s2[n].fmt=0; s2[n].lab=g_ascii?"CUBRID total":"CUBRID 총합"; n++;
        s2[n].key=HK_DEVUTIL;s2[n].col=215; s2[n].fmt=1; s2[n].lab=g_ascii?"device util":"장치 사용률"; n++;
        s2[n].key=HK_CAPIOPS;s2[n].col=178; s2[n].fmt=3; s2[n].lab=g_ascii?"device IOPS":"장치 IOPS"; n++;
        vd_host(vb,sizeof vb,&vc,ncores);
        /* The header box is gone and its contents (db, window, samples, instance, time) are
           absorbed into this panel's title, returning two contentless border rows to
           the chart. */
        if(hdr&&hdr[0]) snprintf(t1,sizeof t1,"%s",hdr);
        else snprintf(t1,sizeof t1,"%s",g_ascii?"1 host resources · CPU cores · RAM · device · load overlay (all processes)"
                                               :"1 호스트 리소스 · CPU 코어 · RAM · 장치 · load 겹침 (전체 프로세스)");
        if(hdr_r&&hdr_r[0]) snprintf(t2,sizeof t2,"%s",hdr_r);
        else snprintf(t2,sizeof t2,"%s",g_ascii?"host-wide":"호스트 전체");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,1,NULL,&PL_LOAD,vb,vc,1,NULL,0,1);
        break; }
    case 2: {   /* This instance's memory */
        n=pl_series_A(s2,1);
        /* Draw the B total alongside: a dynamic heap that grows while B stays flat means the
           part B cannot see (region A) is growing, which calls for r to remeasure A.
           B moving in steps is normal. */
        s2[n].key=HK_HEAPB; s2[n].col=42; s2[n].fmt=0; s2[n].lab=g_ascii?"heapB total":"heapB 합계"; n++;
        vd_mem(vb,sizeof vb,&vc);
        double d=HN>1?HS[HN-1].srv-HS[0].srv:0, w=vd_win(), rate=w>0?d/w*3600:0;
        snprintf(t2,sizeof t2,"%s%s/h · RAM %.1f%%",rate>=0?"+":"-",H(rate<0?-rate:rate),
                 HN>0&&HS[HN-1].ram>0?HS[HN-1].cub/HS[HN-1].ram*100:0);
        snprintf(t1,sizeof t1,"%s",g_ascii?"2 instance memory · why and where does it grow · reserved(blue) vs resident · load overlay"
                                          :"2 인스턴스 메모리 · 왜, 어디서 느나 · 할당(청) vs 상주 · load 겹침");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,0,NULL,&PL_LOAD,vb,vc,1,bars,nb,bper);
        break; }
    case 3: {   /* This instance: volume to buffer */
        s2[n].key=HK_RBPS;  s2[n].col=203; s2[n].fmt=2; s2[n].lab=g_ascii?"disk read":"디스크 읽기"; n++;
        s2[n].key=HK_PGRES; s2[n].col=79;  s2[n].fmt=1; s2[n].lab=g_ascii?"buffer resident%":"버퍼 상주%"; n++;
        s2[n].key=HK_PGCOLD;s2[n].col=215; s2[n].fmt=1; s2[n].lab=g_ascii?"cold zone%":"cold 존%"; n++;
        if(hot_on){ s2[n].key=HK_DBHOT; s2[n].col=117; s2[n].fmt=1; s2[n].lab=g_ascii?"data_buffer hot%":"data_buffer 핫%"; n++; }
        s2[n].key=HK_CAPMISS;s2[n].col=178; s2[n].fmt=3; s2[n].lab=g_ascii?"admitted pg/s":"적재 pg/s"; n++;
        /* majflt and OS cache absorption stay out of the main series: at eight series the
           right-aligned legend fills and the left entries disappear into an ellipsis
           (reproduced even at 150 columns).  Both appear in the bars below, so nothing
           is lost. */
        vd_buf(vb,sizeof vb,&vc);
        snprintf(t2,sizeof t2,"%s",g_ascii?"instance · overlay=host":"이 인스턴스 · 겹침=호스트");
        snprintf(t1,sizeof t1,"%s",g_ascii?"3 volume -> buffer · is the buffer working (per-series normalized)"
                                          :"3 올리는 쪽(볼륨→버퍼) · 버퍼가 일하고 있나 (계열별 정규화)");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,1,NULL,
                  g_ascii?&PL_DEVUTIL_EN:&PL_DEVUTIL,vb,vc,1,bars,nb,bper);
        break; }
    default: {  /* This instance: buffer to volume */
        s2[n].key=HK_PGDIRTY;s2[n].col=211; s2[n].fmt=4; s2[n].lab=g_ascii?"dirty pages":"dirty 페이지"; n++;
        s2[n].key=HK_PGSPAN; s2[n].col=215; s2[n].fmt=4; s2[n].lab=g_ascii?"flush delay(log pg)":"flush 지연(로그pg)"; n++;
        s2[n].key=HK_WBPS;   s2[n].col=117; s2[n].fmt=2; s2[n].lab=g_ascii?"disk write":"디스크 쓰기"; n++;
        /* D threads are those in uninterruptible wait for the disk.  Undiluted by the core
           count, it is the one instance metric that always registers a wait. */
        s2[n].key=HK_DTHR;   s2[n].col=203; s2[n].fmt=4; s2[n].lab=g_ascii?"D threads":"D스레드"; n++;
        vd_flush(vb,sizeof vb,&vc);
        snprintf(t2,sizeof t2,"%s",g_ascii?"instance · overlay=host":"이 인스턴스 · 겹침=호스트");
        snprintf(t1,sizeof t1,"%s",g_ascii?"4 buffer -> volume · is write-back/flush falling behind (per-series normalized)"
                                          :"4 내리는 쪽(버퍼→볼륨) · 반영·flush 가 밀리나 (계열별 정규화)");
        cv_panel2(0,y,W,ch,t1,t2,s2,n,0,1,NULL,
                  g_ascii?&PL_AWAIT_EN:&PL_AWAIT,vb,vc,1,bars,nb,bper);
        break; }
    }
}


/* ---------------- Dashboard render ---------------- */

static void ticks_bottom(int x0,int w,int yb){
    if(HN<2||w<12) return;
    int n=(w+2)/11;
    if(n>4)n=4;
    if(n<2)n=2;
    for(int i=0;i<n;i++){
        double f=(double)i/(n-1);
        const char*lbl=HS[(int)(f*(HN-1)+0.5)].hh;
        int L=(int)strlen(lbl);
        int cx=x0+(int)(f*(w-1)+0.5), lx=cx-L/2;
        if(lx<x0+1) lx=x0+1;
        if(lx>x0+w-L-1) lx=x0+w-L-1;
        cv_put(lx-1,yb,"┤","38;5;240"); cv_put(lx,yb,lbl,"38;5;250");
        cv_put(lx+L,yb,"├","38;5;240");
    }
}

/* What a retained I/O verdict needs to be worded again: the values it was drawn
   from.  The sentence is built when drawn, so after [l] past entries follow the
   new language too. */
typedef struct { double wa, devutil, blkpct, majflt, io; int d_thr, n_thr; } vdval_t;
static void vd_format(int kind,const vdval_t *v,char *vb,size_t n){
    if(kind==1){
        /* Cite only signals that actually show this instance waiting.  majflt stays 0 even
           under a bottleneck because CUBRID reads with pread rather than
           faulting on mmap, so quoting it would contradict the conclusion. */
        char ev[64]; size_t ep=0; ev[0]=0;
        if(v->d_thr>0)     ep+=snprintf(ev+ep,sizeof ev-ep,"%s%s %d/%d",
                                         ep?" · ":"",g_ascii?"D thr":"D스레드",
                                         v->d_thr,v->n_thr);
        if(v->blkpct>=1.0) ep+=snprintf(ev+ep,sizeof ev-ep,"%sblkio %.0f%%",
                                         ep?" · ":"",v->blkpct);
        if(v->majflt>=1.0) ep+=snprintf(ev+ep,sizeof ev-ep,"%smajflt %.0f/s",
                                         ep?" · ":"",v->majflt);
        if(!ep && v->io>0) snprintf(ev,sizeof ev,"%s %s/s",
                                            g_ascii?"io":"이 서버",H(v->io));
        snprintf(vb,n,"%s wa %.1f%% + %s %.0f%% %s data_buffer %s%s%s%s",
                 g_ascii?"[!]":"⚠",v->wa,g_ascii?"dev":"장치",v->devutil,
                 g_ascii?"-> disk bottleneck.":"→ 디스크 병목.",
                 g_ascii?"shortage?":"부족 의심",
                 ev[0]?" (":"",ev,ev[0]?")":"");
    } else if(kind==2){
        snprintf(vb,n,"%s %s %.0f%% %s wa %.1f%% %s (%s %d/%d · blkio %.0f%%)",
                 g_ascii?"[!]":"⚠",g_ascii?"dev":"장치",v->devutil,
                 g_ascii?"busy but":"는 바쁜데",v->wa,
                 g_ascii?"low - CPU busy hides wa":"로 낮다 — CPU 가 바빠 wa 가 가려진 상태",
                 g_ascii?"D thr":"D스레드",v->d_thr,v->n_thr,v->blkpct);
    } else if(kind==4){
        snprintf(vb,n,"%s %s %.0f%% %s (%s %d/%d · blkio %.0f%% · io %s/s) — %s",
                 g_ascii?"[i]":"·",g_ascii?"dev":"장치",v->devutil,
                 g_ascii?"busy but this instance is idle":"는 바쁘지만 이 인스턴스는 조용",
                 g_ascii?"D thr":"D스레드",v->d_thr,v->n_thr,v->blkpct,
                 H(v->io),
                 g_ascii?"load is from another process":"다른 프로세스의 부하다");
    } else {
        snprintf(vb,n,"%s wa %.1f%% %s",
                 g_ascii?"[i]":"·",v->wa,
                 g_ascii?"but device idle - non-disk wait (net/NFS) or short window"
                        :"인데 장치는 한가 — 디스크가 아닌 대기(네트워크·NFS)이거나 측정 구간이 짧다");
    }
}

static void render_dash(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int lens,int paused,
                        double work_ms,double a_age,int with_a){
    int W=term_w();
    double mt=M->total*1024.0, av=M->avail*1024.0, used=mt-av;
    double swt=M->swtotal*1024.0, swu=(M->swtotal-M->swfree)*1024.0;
    cv_init(W,CV_H);
    char t1[64],t2[160];
    /* During a replay the clock must show when the frame was RECORDED, not now. */
    time_t now=g_replaying&&g_rep_ts?g_rep_ts:time(NULL);
    struct tm tv; localtime_r(&now,&tv);
    int h12=tv.tm_hour%12; if(!h12)h12=12;
    snprintf(t1,sizeof t1,"%04d-%02d-%02d %02d:%02d:%02d %s",tv.tm_year+1900,tv.tm_mon+1,tv.tm_mday,
             h12,tv.tm_min,tv.tm_sec,tv.tm_hour<12?"AM":"PM");
    int y=0;
    /* Compact layout: if the rows a full-width layout needs exceed the terminal height,
       the process and trend boxes are set side by side, saving 6 rows.
       Below 100 columns halves do not work (sparklines, legends), so no compaction. */
    int compact=0;
    { int trows=term_h();
      int need = 1 + (4+(g_ninst>0?g_ninst:0)) + 7 + (NSR>0?NSR+3:4) + 11 + 6 + 8 + 9 + 2;
      if(trows>=8 && trows<need && W>=100) compact=1; }
    /* ---- Top time line, outside the boxes ----
       Left: this frame's collection work time.  Right: the current time.
       Both apply to the whole screen, so neither hangs off a particular box title. */
    {
        char lt[64];
        /* A replay must never be mistaken for live: the left slot says REPLAY and the
           right shows the RECORDED time, not now. */
        if(g_replaying) snprintf(lt,sizeof lt,"%s %d/%d",
                                 g_ascii?"REPLAY":"재생", g_rep_i+1, g_rep_n);
        else            snprintf(lt,sizeof lt,"%s %.0fms",g_ascii?"collect":"수집",work_ms);
        cv_put(1,y,lt,g_replaying?"1;38;5;214":"38;5;244");
        cv_put(W-1-disp_w(t1),y,t1,"38;5;250");
        y+=1;
    }
    /* ---- OS context; no lens marker, since the lens is a regions-only axis ---- */
    /* The instance list lives in the OS box: what is running on this host and how much
       each uses is an OS-level fact.  The boxes below analyse the selected one. */
    int oh = 4 + (g_ninst>0 ? g_ninst : 0);   /* +1 for the CPU row */
    /* load is host-wide and includes I/O waiting, so it goes in the title rather than a
       column.  The right of the title is taken by column headings, so it follows the
       left title. */
    { char ot[80];
      snprintf(ot,sizeof ot,"%s · load %.1f %.1f %.1f",L("OS 컨텍스트"),
               M->load1,M->load5,M->load15);
      cv_box(0,y,W,oh,ot); }
    /* Same form as the region box: column headings on the border and values in columns.
       Cached, avail and swap are gathered on one row instead of flowing as sentences. */
    /* Six columns (43 cells) need 63 with the title, so narrow terminals drop some.
       cache and avail go first: they are OS-only and read '-' on instance rows. */
    int os_wide = (W >= 78);   /* Six columns, 45 cells, plus the title */
    { int VX=W-1-(os_wide?45:29);                      /* Start x of the value columns */
      char hd[80];
      /* The six columns:
           used   OS = used,          instance = PSS + PL
           cache  OS = Cached,        instance = its share of mapped file pages
           avail  OS = MemAvailable,  instance = n/a (not a process concept)
           swap   OS = swap in use,   instance = swapped out
           total  OS = MemTotal,      instance = n/a
           pct    used/total (instance is against RAM, rounded up) */
      /* total first, then used and pct: how much of how much, and what share */
      if(os_wide) snprintf(hd,sizeof hd,"┤%7s|%7s|%4s|%7s|%7s|%6s├",
                           "total","used","pct","avail","cache","swap");
      else        snprintf(hd,sizeof hd,"┤%7s|%7s|%4s|%6s├","total","used","pct","swap");
      cv_put(VX-1,y,hd,"1;36");

      char v[160];
      /* CPU before memory - it is the first thing looked at for resource use.
         CPU uses the same columns: total = logical cores, used = cores in use,
         pct = utilization, with us/sy/wa in the remaining three.  The label reads
         "physical c / logical t" so hyperthreading is visible. */
      { char cl[64]; int nc=M->cores>0?M->cores:1, pc=M->pcores>0?M->pcores:nc;
        char cn[24];
        if(pc!=nc) snprintf(cn,sizeof cn,"%dc/%dt",pc,nc);   /* Hyperthreading */
        else       snprintf(cn,sizeof cn,"%dc",nc);
        snprintf(cl,sizeof cl,"CPU %s",cn);
        cv_put(2,y+1,padf(cl,22,0),"250");
        /* Per-core distribution instead of a total gauge - the distribution is what reveals
           saturation.  Before the first frame delivers per-core values, fall back to
           the old gauge. */
        if(M->ncore_pct>0) cv_cores(25,y+1,VX-26,M->core_pct,M->ncore_pct);
        else               cv_meter(25,y+1,VX-26,M->busy_cores/nc,0);
        { char c1[16],c2[16],c4[16],c5[16];
          snprintf(c1,sizeof c1,"%d",nc);
          snprintf(c2,sizeof c2,"%.1f",M->busy_cores);
          /* The avail cell carries the saturated core count, which matters more than us/sy:
             "12 cores' worth" does not say whether that is 12 cores or 6 at 100%, and
             the latter is the bottleneck.  hi (50-95%) is counted with it as near
             saturation. */
          /* Drop the us/sy labels as values grow, rather than overflow the cell: a truncated
             number cannot be read, while a label can be inferred from the column order.
             The last cell is 6 wide, so "sy 100%" (7 characters) would push the width. */
          #define CPUCELL(dst,lab,val,cap) do{ \
              snprintf(dst,sizeof dst,"%s %.0f%%",lab,val); \
              if((int)strlen(dst)>(cap)) snprintf(dst,sizeof dst,"%.0f%%",val); \
              if((int)strlen(dst)>(cap)) snprintf(dst,sizeof dst,"%.0f",val); \
          }while(0)
          CPUCELL(c4,"us",M->cpu_us,7);
          CPUCELL(c5,"sy",M->cpu_sy,6);
          /* These cells pad by display width rather than %7s, so CJK does not push the
             columns; the CPU row is therefore laid out cell by cell.  The last
             three carry their own labels, unrelated to the column headings above,
             so they are dimmed to keep the pairing from being misread. */
          { char c3k[16]; int cap = os_wide?7:6;
            snprintf(c3k,sizeof c3k,g_ascii?"sat %d":"포화 %d",M->sat_cores);
            /* Decide the fallback on display width: measured in bytes, a label 6 cells wide but 8
               bytes long is truncated although it fits.  padf is display width too. */
            if(disp_w(c3k)>cap) snprintf(c3k,sizeof c3k,"sat %d",M->sat_cores);
            if(disp_w(c3k)>cap) snprintf(c3k,sizeof c3k,"%d",M->sat_cores);
            char pctb[8]; snprintf(pctb,sizeof pctb,"%3.0f%%",nc>0?M->busy_cores/nc*100:0);
            int cx=VX;
            cv_put(cx,y+1,padf(c1,7,1),"250");   cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
            cv_put(cx,y+1,padf(c2,7,1),"250");   cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
            cv_put(cx,y+1,padf(pctb,4,1),"250"); cx+=4; cv_put(cx,y+1,"|","250"); cx+=1;
            if(os_wide){
                cv_put(cx,y+1,padf(c3k,7,1),"38;5;244"); cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
                cv_put(cx,y+1,padf(c4,7,1), "38;5;244"); cx+=7; cv_put(cx,y+1,"|","250"); cx+=1;
                cv_put(cx,y+1,padf(c5,6,1), "38;5;244");
            } else  /* Narrow: only one cell remains, so keep saturation - it stands in for the
                       distribution when the bar is too narrow to read. */
                cv_put(cx,y+1,padf(c3k,6,1),"38;5;244");
          } } }
      cv_put(2,y+2,padf("Memory",10,0),"250");
      cv_meter(25,y+2,VX-26,mt>0?used/mt:0,0);
      if(os_wide)
          snprintf(v,sizeof v,"%7s|%7s|%3.0f%%|%7s|%7s|%6s",
                   H(mt),H(used),mt>0?used/mt*100:0,H(av),
                   H(M->cached*1024.0),M->swtotal==0?"off":H(swu));
      else
          snprintf(v,sizeof v,"%7s|%7s|%3.0f%%|%6s",H(mt),H(used),
                   mt>0?used/mt*100:0,M->swtotal==0?"off":H(swu));
      cv_put(VX,y+2,v,"250"); (void)swt;
    }
    /* ---- Per-instance usage, on the same scale and columns as total memory ---- */
    if(g_ninst>0){
        int VX=W-1-(os_wide?45:29);
        for(int i=0;i<g_ninst;i++){
            int yy=y+3+i, cur=(i==g_cur);   /* One more row ahead, for CPU */
            double iv=g_inst[i].pss+g_inst[i].pl_pss;       /* Instance total, PL included */
            char nm[80],v[80];
            snprintf(nm,sizeof nm," %s ",g_inst[i].db);     /* Background colour padding on both sides */
            cv_put(2,yy,padf(cutf(nm,10),10,0), cur?"1;38;5;231;48;5;24":"38;5;250");
            /* PL is a breakdown of that instance's total, set beside the name to make the membership clear */
            if(g_inst[i].pl_n>0){
                char pl[24]; snprintf(pl,sizeof pl,"(PL %s)",HC(g_inst[i].pl_pss));
                cv_put(13,yy,padf(pl,11,0),"38;5;244");
            }
            cv_meter(25,yy,VX-26,mt>0?iv/mt:0,0);
            /* Instances have no avail or total.  pct rounds up: writing 0.6% as 0% reads as
               unused and makes the column width jump. */
            double pc = mt>0?iv/mt*100:0;
            int pci = (int)pc; if(pc>(double)pci) pci++;
            /* A '-' for an inapplicable column is not placed against the pipe: beside a capacity
               figure, '-|' reads as a value.  A space shows the cell is empty. */
            if(os_wide)
                snprintf(v,sizeof v,"%6s |%7s|%3d%%|%6s |%7s|%6s",
                         "-",H(iv),pci,"-",H(g_inst[i].filemap),
                         g_inst[i].swap>0?H(g_inst[i].swap):"0");
            else
                snprintf(v,sizeof v,"%6s |%7s|%3d%%|%6s","-",H(iv),pci,
                         g_inst[i].swap>0?H(g_inst[i].swap):"0");
            cv_put(VX,yy,v,"38;5;250");
        }
    }
    /* Parameter attribution describes the selected instance's region composition, not the
       whole OS (85% on one DB against 92% on another), so it moved to the region box
       title. */
    y+=oh;
    /* ---- Processes; in compact layout this draws into the left half (bw) ---- */
    int cbx=0, cbw=W;
    if(compact){ cbx=0; cbw=W/2; }
    {
        const int bx=cbx, bw=cbw;
        const int nmw = (bw>=100) ? 22 : 14;        /* Name column width, narrowed in a half-width layout */
        /* The title says PSS, so the server row is PSS too and the row total matches the
           CUBRID total exactly.  The instance list moved to the OS box; this box shows
           only the selected instance's composition. */
        double srv=P->pss_kb*1024.0;
        char n_srv[96];
        snprintf(n_srv,sizeof n_srv,"%sserver",T->master>0?"  ├ ":"▾ ");
        int nr=2+(T->npl>0?1:0)+(T->master>0?1:0)+(T->ncas>0?1:0), ph=nr+2;
        if(compact && ph<7) ph=7;                  /* Match the height of the trend mini beside it (7 rows) */
        /* Trend columns: the window delta plus a sparkline when width allows, so
           broker/CAS/PL growth is visible too. */
        /* Sparkline width: the gauge duplicates the value column to its right (PSS size), so
           it is halved and the space given to the sparkline.  A 3-minute window at 0.5s
           gives 24 cells, about 7 seconds each; 12 at 80 columns. */
        int spark = (bw>=100) ? 24 : (bw>=80) ? 12 : (bw>=54) ? 8 : 0;
        char wt2[24]; vd_wtxt(wt2,sizeof wt2);
        { char rt[64];
          double ptot=T->master+srv+T->pl+T->broker+T->cas;
          /* cv_box2's right-hand title shifts as a whole including its trailing space, so the
             text always ends at W-4.  The value column below moves from W-9 to W-11 to
             line up.  The unit stays M - promoting to 1.1G would stop the rows below
             from being added by eye. */
          if(HN>=4) snprintf(rt,sizeof rt,"%s%s │ All %8s",g_ascii?"d":"Δ",wt2,HM(ptot));
          else      snprintf(rt,sizeof rt,"All %8s",HM(ptot));
          cv_box2(bx,y,bw,ph,L("PSS · 프로세스"),rt); }
        double mx=srv;
        if(T->broker>mx)mx=T->broker;
        if(T->cas>mx)mx=T->cas;
        if(T->pl>mx)mx=T->pl;
        if(T->master>mx)mx=T->master;
        if(mx<=0)mx=1;
        /* The real startup hierarchy: master starts server and broker, and PL is a child
           of server (cub_pl's PPID is that cub_server).
             master
               |- server@db
               |    \- pl server (JVM)
               \- broker + CAS */
        char n_cas[48];
        snprintf(n_cas,sizeof n_cas,"      └ CAS[%d]",T->ncas);
        const char*nm[6]; double vv[6],cc[6]; int k=0; int hk[6];
        /* In a half-width layout (14-cell names) use short names that do not truncate mid-word */
        char n_cas2[24]; snprintf(n_cas2,sizeof n_cas2,"    └ CAS[%d]",T->ncas);
        int nar = (nmw<22);
        if(T->master>0){ nm[k]=nar?"▾ master":(g_ascii?"▾ master (shared)":"▾ master (설치본)");
                         cc[k]=T->cpu_master; hk[k]=1; vv[k++]=T->master; }
        nm[k]=n_srv; cc[k]=IO->cpu_pct/100.0; hk[k]=0; vv[k++]=srv;   /* Percent (one core = 100) to cores */
        if(T->npl>0){ nm[k]=nar?"  │ └ pl(JVM)":"  │   └ pl server(JVM)"; cc[k]=T->cpu_pl; hk[k]=2; vv[k++]=T->pl; }
        nm[k]=T->master>0?"  └ broker":"▾ broker"; cc[k]=T->cpu_broker; hk[k]=3; vv[k++]=T->broker;
        if(T->ncas>0){ nm[k]=nar?n_cas2:n_cas; cc[k]=T->cpu_cas; hk[k]=4; vv[k++]=T->cas; }
        /* Tier history accessor: hk index to hsam_t field */
        #define TIER_HS(smp,idx) ((idx)==0?(smp)->pss_srv:(idx)==1?(smp)->pss_master:(idx)==2?(smp)->pss_pl:(idx)==3?(smp)->pss_broker:(smp)->pss_cas)
        /* Same rule as panel A's verdict: growth above 5% across the window while load stays flat */
        int load_flat = !(HN>=4 && HS[HN-1].load1 > HS[0].load1*1.3 && HS[HN-1].load1>=1.0);
        /* The total goes at the right of the box title rather than in a row; see cv_box2 below */
        /* The gauge stays on the memory (PSS) scale and CPU appears only as the number before
           it: 0.4 cores on a 64-core scale would not fill one cell. */
        for(int i=0;i<k;i++){
            cv_put(bx+2,y+1+i,padf(cutf(nm[i],nmw),nmw,0),"250");
            /* Core counts truncate to one decimal.  Colour follows magnitude, as in htop's
               %CPU column, so which tier is running is visible without a gauge.
               Zero renders as a dot so idle rows do not draw the eye - five tiers
               all reading 0.0 would be noise. */
            { char cb[16]; double c=(double)((long)(cc[i]*10))/10.0;
              const char *col;
              if(c<=0)                       { snprintf(cb,sizeof cb,"·"); col="38;5;240"; }
              else { snprintf(cb,sizeof cb,"%.1f",c);
                     if(c>=M->pcores)        col="38;5;203";  /* Above the physical core count: red */
                     else if(c>=M->pcores/8.0) col="38;5;215"; /* Noticeable load: orange */
                     else if(c>=1.0)         col="38;5;228";  /* At least one core: yellow */
                     else                    col="38;5;117";  /* Small: cyan */ }
              cv_put(bx+nmw+3,y+1+i,padf(cb,4,1),col); }
            /* Columns from the right: value 8, delta 8, sparkline, then the gauge */
            int xval=bx+bw-12, xdel=xval-9, xsp=xdel-(spark?spark+1:0);
            int mx0=bx+nmw+8, mw=xsp-mx0-1;
            if(mw>=4) cv_meter(mx0,y+1+i,mw,vv[i]/mx,0);   /* Narrow: drop the gauge, the value column stands in */
            if(HN>=4){
                double d=TIER_HS(&HS[HN-1],hk[i])-TIER_HS(&HS[0],hk[i]);
                double base=TIER_HS(&HS[0],hk[i]);
                double ad=d<0?-d:d;
                char db[16]; const char*dc="38;5;244"; int dcol=244;
                /* Delta threshold: a dot only below 0.1% or 1MB; (1% of a 950MB server is 9.5MB) */
                if(base>0 && ad < base*0.001 && ad < 1.0*1024*1024){ snprintf(db,sizeof db,"·"); }
                else { snprintf(db,sizeof db,"%s%s",d>=0?"+":"-",H(ad));
                       if(base>0 && d>base*0.05){ dc = load_flat ? "1;38;5;203" : "38;5;215"; dcol = load_flat?203:215; }
                       else if(d<0){ dc="38;5;117"; dcol=117; }
                       else { dc="38;5;250"; dcol=250; } }
                cv_put(xdel,y+1+i,padf(db,8,1),dc);
                if(spark){
                    /* min-max scale: against 0, a value that only moves near its maximum (resident
                       memory) sits at the top and cannot distinguish "steady" from
                       "10% swing".  Stretching to the window's min-max shows the
                       shape, while the delta beside it gives the magnitude.
                       Under 0.5% amplitude draws a flat grey line. */
                    static double sv[HIST_MAX]; double smin=1e300,smax=-1e300;
                    for(int j=0;j<HN;j++){ sv[j]=TIER_HS(&HS[j],hk[i]); if(sv[j]>smax) smax=sv[j]; if(sv[j]<smin) smin=sv[j]; }
                    double span=smax-smin;
                    int flat = (smax<=0) || (span < smax*0.005);
                    const double*vp1[1]={sv}; int c1[1]={ flat ? 240 : dcol };
                    /* A flat line draws at 0.65 height (the second dot row); 0.5 lands on the same row as
                       the gauge track glyph and looked like a continuation of the gauge. */
                    if(flat){ for(int j=0;j<HN;j++) sv[j]=0.65; cv_chart_n(xsp,y+1+i,spark,1,vp1,c1,1,HN,1.0); }
                    else {   /* Map [min,max] onto [0.1,0.9] so the extremes do not sit on the cell boundary */
                        for(int j=0;j<HN;j++) sv[j]=0.1+0.8*(sv[j]-smin)/span;
                        cv_chart_n(xsp,y+1+i,spark,1,vp1,c1,1,HN,1.0); }
                }
            }
            /* Align right with the title's All value (the title text ends at W-4) */
            cv_put(xval,y+1+i,padf(HM(vv[i]),8,1),"250");
        }
        #undef TIER_HS
        y+=ph;
    }
    if(compact){
        /* Right half: the trend mini (7 rows, no verdict).  The process box just advanced y by ph, so step back to draw */
        int ty=y-7;
        pser_t s2[6]; int n=pl_series_A(s2,0);
        char vb[320]; const char*vc=VD_INFO; vd_mem(vb,sizeof vb,&vc);
        char wtxt[24]; vd_wtxt(wtxt,sizeof wtxt);
        double d=HN>1?HS[HN-1].srv-HS[0].srv:0, w=vd_win(), rate=w>0?d/w*3600:0;
        if(g_ascii) snprintf(t2,sizeof t2,"trend %s · [p]",wtxt); else snprintf(t2,sizeof t2,"메모리 추이 %s · [p]",wtxt);
        char rt[32]; snprintf(rt,sizeof rt,"%s%s/h",rate>=0?"+":"-",H(rate<0?-rate:rate));
        cv_panel2(W/2,ty,W-W/2,3,t2,rt,s2,n,0,0,NULL,&PL_LOAD,NULL,NULL,1,NULL,0,1);
        /* The verdict takes one full-width row; in a half it would lose its conclusion */
        cv_put(2,y,cutf(vb,W-4),vc); y+=1;
    }
    /* ---- cub_server regions ---- */
    /* Three lenses; the delta is the chg column, not a lens */
    static const char*LN[4]={"","할당/상주","핫","설정 초과"};
    static const char*LNA[4]={"","alloc/rss","hot","config over"};
    const char **LNS = g_ascii?LNA:LN;
    /* Four value columns: alloc | rss | hot | growth, each preceded by a space so it does
       not touch the pipe.  hot carries its delta in parentheses and growth is the rss
       delta - two different metrics. */
    /* Value width 7: just below 1024, H() produces "1023.9M", seven characters; six would
       truncate it or push the column.  grow stays at 6, being mostly small deltas. */
    int VAL_X=W-35, PCT_X=W-42, MET_X=19, MET_W=PCT_X-MET_X-1;
    if(NSR==0){
        cv_box(0,y,W,4,L("RSS · 서버"));
        cv_put(2,y+1,"? cub_server 미기동 — 실측 불가","38;5;203");
        y+=4;
    } else {
        /* The left title is the box's identity (axis, attribution).  The view label is a switch
           that changes what the value columns MEAN, so it sits just before the value
           headings - the reason next to what it changes. */
        snprintf(t2,sizeof t2,"%s",L("RSS · 서버"));
        int title_end = 4+disp_w(t2)+2;   /* The x where the border, title and closing border end */
        cv_box(0,y,W,NSR+3,t2);   /* +1 for the total row */
        /* The share of this instance's regions explained by cubrid.conf parameters.
           The rest (dynamic heap, stacks, code) is decomposed by heap [B]/[A]. */
        { double byparam=0,tsum=0;
        for(int i=0;i<NSR;i++){ tsum+=SR[i].rss; if(SR[i].cfg>0) byparam+=SR[i].rss; }
          if(tsum>0){
              char at[48];
              double pct=byparam/tsum*100;
              /* Below 70%, more than three tenths is unexplained by configuration - a signal to look
                 at the heap box, so it takes the same orange as over-counting. */
              const char *ac = (pct<70.0) ? "1;38;5;214" : "38;5;244";
              snprintf(at,sizeof at,"┤ %s %.0f%% ├",
                       g_ascii?"by-param":"설정으로 설명", pct);
              /* Not drawn if it would reach the value headings at the right; overlapping makes both unreadable */
              int ax=2+disp_w(t2)+4;
              if(ax+disp_w(at) <= PCT_X-2) cv_put(ax,y,at,ac);
          }
          /* If no configuration could be read and the reason is known, show it where the
             attribution rate goes - nothing at all reads as "this build does not expose
             parameters".  (byparam==0 means no region has a cfg, i.e. no config read.) */
          if(byparam<=0 && g_pd_why[0]){
              char at[160]; snprintf(at,sizeof at,"┤ %s ├",g_pd_why);
              int ax=2+disp_w(t2)+4;
              if(ax+disp_w(at) <= PCT_X-2) cv_put(ax,y,at,"38;5;214");
          } }
        /* Column order alloc | rss | hot | grow.  The hot delta has no column of its own, since
           lens 3 (hot) shows it as a gauge. */
        /* The heading stays "grow".  In view 2 (hot) the column carries hot deltas, and
           rather than swapping the wording the hot and grow tokens are recoloured
           (green, matching the +green/-red deltas below).  View 3 replaces the hot
           column with conf, so there the wording does change, in orange. */
        snprintf(t2,sizeof t2,"┤%7s |%7s |%7s |%6s├","alloc","rss",
                 lens==3?"conf":"hot","grow");
        /* A heading past the right corner breaks the box, so it is pulled left by the overhang.
           If it would still collide with the title (at 60 columns), it is omitted; the
           values remain. */
        { int hx=VAL_X-1, hw=disp_w(t2);
          if(hx+hw > W-1) hx = W-1-hw;
          if(hx >= title_end){
              /* View badge, just before the value headings.  Its colour matches the column it
                 changes (green for hot, orange for over-configured), so badge, heading
                 token and row value read as one colour. */
              { char vb2[48];
                snprintf(vb2,sizeof vb2,"┤%s%d · %s ",g_ascii?"lens":"보기",lens,LNS[lens]);
                int vw=disp_w(vb2), vx=hx-vw;
                if(vx>=title_end)
                    cv_put(vx,y,vb2, lens==2?"1;38;5;79":lens==3?"1;38;5;214":"1;36");
              }
              cv_put(hx,y,t2,"1;36");
              /* Token positions for columns 3 (hot/conf) and 4 (grow): border 1, field 7, separator 2 */
              if(lens==2){ cv_put(hx+19,y,padf("hot",7,1), "1;38;5;79");
                           cv_put(hx+28,y,padf("grow",6,1),"1;38;5;79"); }
              else if(lens==3) cv_put(hx+19,y,padf("conf",7,1),"1;38;5;214");
          } }
          double sm=0,ssr=0,sh=0,sd=0,sdh=0,sdm=0; int base_ok=1,hbase_ok=1,mbase_ok=1;   /* Material for the total row */
        for(int i=0;i<NSR;i++){
            srg_t*r=&SR[i]; int yy=y+1+i;
            cv_put(2,yy,GR(r->grade),
                   r->grade=='M'?"38;5;39":r->grade=='R'?"38;5;214":r->grade=='E'?"38;5;250":"38;5;203");
            cv_put(4,yy,padf(r->name,14,0),"250");
            double frac=0,hotf=0,dfrac=0; char tail[24]; int warn=0;
            /* The growth (on the rss basis) is converted to each lens's scale and painted orange at
               the end of the gauge.  A decrease already shows as a shorter bar and is
               not marked. */
            double dv=r->rss-r->prev_rss; if(dv<0) dv=0;
            if(lens==1){ frac=r->mapped>0?r->rss/r->mapped:0; hotf=r->mapped>0?r->ref/r->mapped:0;
                         dfrac=r->mapped>0?dv/r->mapped:0;
                         snprintf(tail,sizeof tail,"%.0f%%",frac*100); }
            else if(lens==2){ frac=r->rss>0?r->ref/r->rss:0; hotf=frac;   /* View 2: hot */
                   snprintf(tail,sizeof tail,"%.0f%%",frac*100); }
            else { if(r->cfg>0){ frac=r->mapped/(double)r->cfg; warn=frac>1.05;   /* View 3: over configured */
                                 hotf=r->ref/(double)r->cfg;
                                 snprintf(tail,sizeof tail,"%.0f%%%s",frac*100,warn?"!":""); }
                   else snprintf(tail,sizeof tail,"n/a"); }
            if(r->mapped>0||r->rss>0) cv_meter3(MET_X,yy,MET_W,frac,hotf,dfrac);
            cv_put(PCT_X,yy,padf(tail,6,1),warn?"38;5;203":"250");
            cv_put(VAL_X,   yy,padf(r->mapped>0?H(r->mapped):"-",7,1),"244");
            cv_put(VAL_X+8, yy,"|","38;5;240");
            /* Zero prints as "0B", as in the tree; a different zero per view invites confusion */
            cv_put(VAL_X+9, yy,padf(r->rss>0?H(r->rss):"0B",7,1),"250");
            cv_put(VAL_X+17,yy,"|","38;5;240");
            if(lens==3){
                /* View 3 (over configured) replaces hot with conf, directly comparable with alloc and rss */
                cv_put(VAL_X+18,yy,padf(r->cfg>0?H((double)r->cfg):"-",7,1),"38;5;214");
            } else {
                /* hot is the amount touched; the colour is a gradient of its share of rss */
                char hc[20]; snprintf(hc,sizeof hc,"38;5;%d",grad_of(r->rss>0?r->ref/r->rss:0));
                cv_put(VAL_X+18,yy,padf(r->ref>0?H(r->ref):"-",7,1),hc);
            }
            cv_put(VAL_X+26,yy,"|","38;5;240");
            /* growth is the rss delta against the previous frame.  No change prints a dot: 0B would
               read as "wrote zero bytes" and not be distinguishable from no change.
               One-shot mode has no baseline and prints '-'. */
            { char cg[24]; const char *cc="38;5;244";
              /* grow follows the view's axis; the heading is fixed and colour signals which.
                   View 1 = rss delta (orange).  View 2 (hot) = hot delta.
                   View 3 (over configured) = alloc delta (orange) - the percentage
                   is allocation over configuration, so it tracks that directly. */
              double base = lens==2 ? r->prev_ref : lens==3 ? r->prev_mapped : r->prev_rss;
              double cur  = lens==2 ? r->ref      : lens==3 ? r->mapped      : r->rss;
              double d    = cur-base;
              double a=d<0?-d:d;
              /* With no baseline (previous frame) the delta is undefined - print '-'.
                 Adding d==0 to the condition would let the first frame's full-value d
                 through and report the whole RSS as "+512.3M growth". */
              if(base<=0)             snprintf(cg,sizeof cg,"-");
              else if(a<1.0)          snprintf(cg,sizeof cg,"·");   /* Genuinely unchanged only */
              else { snprintf(cg,sizeof cg,"%s%s",d>0?"+":"-",H(a));
                     cc = lens==2 ? (d>0?"1;38;5;79":"1;38;5;203")   /* Green for positive, red for negative */
                                  : "1;38;5;214"; }                  /* rss delta in orange */
              cv_put(VAL_X+27,yy,padf(cg,6,1),cc); }
            sm+=r->mapped; ssr+=r->rss; sh+=r->ref;
            if(r->prev_ref>0 || r->ref<=0) sdh+=r->ref-r->prev_ref; else hbase_ok=0;
            if(r->prev_mapped>0 || r->mapped<=0) sdm+=r->mapped-r->prev_mapped; else mbase_ok=0;
            /* Baseline test: a region where both rss and prev are 0 (an arena) has no change - it
               is not missing a baseline.  Treating it as such would leave the total's
               grow at '-' forever. */
            if(r->prev_rss<=0){ if(r->rss>0) base_ok=0; }
            else sd+=r->rss-r->prev_rss;
            r->prev_rss=r->rss; r->prev_ref=r->ref; r->prev_mapped=r->mapped;
        }
        /* ---- Total row ----
           The rss total equals process RSS by definition (all smaps mappings, held by
           a check-hist regression).  It always differs from the process box's PSS by
           the shared-page apportionment (measured: almost entirely .so) - stating
           that here keeps the two boxes from being read as inconsistent. */
        { int yy=y+1+NSR;
          char lab[96]; double pss=P->pss_kb*1024.0, df=ssr-pss;
          if(pss>0)
              snprintf(lab,sizeof lab,g_ascii?"Σ sum · PSS %s (shared %s%s)"
                                             :"Σ 합계 · PSS %s (공유 .so 배분 %s%s)",
                       H(pss), df>=0?"-":"+", H(df<0?-df:df));
          else snprintf(lab,sizeof lab,g_ascii?"Σ sum":"Σ 합계");
          cv_put(2,yy,cutf(lab,PCT_X-3),"38;5;244");
          cv_put(VAL_X,   yy,padf(sm>0?H(sm):"-",7,1),"244");
          cv_put(VAL_X+8, yy,"|","38;5;240");
          cv_put(VAL_X+9, yy,padf(ssr>0?H(ssr):"0B",7,1),"1;38;5;250");
          cv_put(VAL_X+17,yy,"|","38;5;240");
          cv_put(VAL_X+18,yy,padf(sh>0?H(sh):"-",7,1),"250");
          cv_put(VAL_X+26,yy,"|","38;5;240");
          { char cg[24]; const char *cc="38;5;244";
            double dv = lens==2 ? sdh : lens==3 ? sdm : sd;
            int ok    = lens==2 ? hbase_ok : lens==3 ? mbase_ok : base_ok;
            double a=dv<0?-dv:dv;
            if(!ok)        snprintf(cg,sizeof cg,"-");
            else if(a<1.0) snprintf(cg,sizeof cg,"·");
            else { snprintf(cg,sizeof cg,"%s%s",dv>0?"+":"-",H(a));
                   cc = lens==2 ? (dv>0?"1;38;5;79":"1;38;5;203") : "1;38;5;214"; }
            cv_put(VAL_X+27,yy,padf(cg,6,1),cc); }
        }
        y+=NSR+3;
    }
    /* ── heap B / A ── */
    int hh=11, Lw=W/2;
    cv_box(0,y,Lw,hh,L("동적 메모리 상세 (B) — 자동 갱신"));
    /* The title carries both the key hint and the last measurement time.  An elapsed
       count ("184s ago") must be subtracted mentally and grows the longer the screen
       is up, so the time is shown.  In automatic mode the period is appended so the
       next refresh can be anticipated. */
    /* A title wider than the box is cut from the end, losing the time.  The values (time,
       period) are kept and the key hints shortened in stages - the keys are in the help
       too, but "when was this measured" is only here. */
    { char chk[24];
      if(g_a_when>0){ struct tm at; localtime_r(&g_a_when,&at);
                      /* States that these are values as of that instant, not now.  An elapsed count needs
                         mental subtraction, so the time itself is printed. */
                      if(g_ascii){ char hm[16]; strftime(hm,sizeof hm,"%H:%M:%S",&at);
                                   snprintf(chk,sizeof chk,"as of %s",hm); }
                      else strftime(chk,sizeof chk,"%H:%M:%S 기준",&at); }
      else            snprintf(chk,sizeof chk,"%s",g_ascii?"not run":"미측정");
      int avail = (W-Lw) - 6;                  /* Border, title borders and corner slack */
      const char *base = g_ascii ? "dynamic heap · method A" : "동적 메모리 상세 · 크기 추정(A)";
      char per[16]; per[0]=0;
      if(g_a_auto) snprintf(per,sizeof per,"[a]uto %.0fs",a_period());
      /* Try the widest form first and take the first that fits */
      const char *cand[4]; int nc=0;
      char c0[128],c1[128],c2[128];
      if(per[0]){
          snprintf(c0,sizeof c0,"%s — %s [r]eflash %s",base,per,chk);
          snprintf(c1,sizeof c1,"%s — %s %s",base,per,chk);
          snprintf(c2,sizeof c2,"%s %s",per,chk);
      } else {
          snprintf(c0,sizeof c0,"%s — [a]uto [r]eflash %s",base,chk);
          snprintf(c1,sizeof c1,"%s — %s",base,chk);
          snprintf(c2,sizeof c2,"%s",chk);
      }
      cand[nc++]=c0; cand[nc++]=c1; cand[nc++]=c2; cand[nc++]=chk;
      snprintf(t2,sizeof t2,"%s",cand[nc-1]);   /* Last resort: values only */
      for(int i=0;i<nc;i++)
          if(disp_w(cand[i])<=avail){ snprintf(t2,sizeof t2,"%s",cand[i]); break; }
    }
    (void)a_age;
    cv_box(Lw,y,W-Lw,hh,t2);
    if(!B.ok){ cv_put(2,y+2,L("방법 B 비활성"),"38;5;203"); cv_put(4,y+3,B.why,"38;5;240"); }
    else {
        double bs=0; int nb=B.n>7?7:B.n;
        for(int i=0;i<nb;i++){
            int yy=y+1+i;
            if(B.it[i].grade!='U') bs+=B.it[i].bytes;   /* Invalid items are excluded from the sum */
            cv_put(2,yy,GR(B.it[i].grade),B.it[i].grade=='M'?"38;5;39":"38;5;250");
            cv_put(4,yy,padf(B.it[i].name,22,0),"250");
            /* The delta goes left of the value: colour alone does not say how much changed.
               No change prints nothing, keeping the screen quiet. */
            { const char *dl=hdelta(B.it[i].name,B.it[i].bytes);
              if(dl[0]) cv_put(Lw-19,yy,padf(dl,9,1),"1;38;5;214"); }
            if(B.it[i].count_only){
                char cb[16]; snprintf(cb,sizeof cb,g_ascii?"%ldx":"%ld개",B.it[i].count);
                cv_put(Lw-9,yy,padf(cb,7,1),"38;5;244");   /* Size unknown: dimmed */
            } else
                cv_put(Lw-9,yy,padf(H(B.it[i].bytes),7,1),"250");
        }
        for(int i=2;i<Lw-2;i++) cv_put(i,y+hh-3,"─","38;5;240");
        if(B.n_bad>0){ char bt[80]; snprintf(bt,sizeof bt,"B 합계(유효 %d, 제외 %d)",B.n-B.n_bad,B.n_bad);
                       cv_put(2,y+hh-2,bt,"1;97"); }
        else if(B.partial) cv_put(2,y+hh-2,g_ascii?"B partial (count-only)":"B 부분(개수만 — 크기 미상)","1;38;5;214");
        else cv_put(2,y+hh-2,L("B 합계(정확)"),"1;97");
        cv_put(Lw-9,y+hh-2,padf(H(bs),7,1),"38;5;42");
    }
    if(!with_a || A.chunks==0){
        cv_put(Lw+2,y+2,with_a?L("힙 순회 중…"):L("[a] 로 방법 A 분석"),"38;5;240");
    } else {
        int row=0;
        for(int c=0;c<7&&row<6;c++){
            if(A.cat[c]<=0) continue;
            int yy=y+1+row++;
            cv_put(Lw+2,yy,"◌","38;5;250"); cv_put(Lw+4,yy,padf(ACAT[c],22,0),"250");
            { char an[48]; snprintf(an,sizeof an,"A:%s",ACAT[c]);   /* Kept distinct from B's names */
              const char *dl=hdelta(an,A.cat[c]);
              if(dl[0]) cv_put(W-19,yy,padf(dl,9,1),"1;38;5;214"); }
            cv_put(W-9,yy,padf(H(A.cat[c]),7,1),"250");
        }
        for(int i=Lw+2;i<W-2;i++) cv_put(i,y+hh-4,"─","38;5;240");
        { int aw=W-9-(Lw+2)-1;                       /* The label may run up to the value cell */
          if(aw<1) aw=1;
          cv_put(Lw+2,y+hh-3,cutf(L(A.trunc?"A 합계(청크·일부)":"A 합계(청크)"),aw),"1;97"); }
        cv_put(W-9,y+hh-3,padf(H(A.total),7,1),"38;5;42");
        double diff=g_dyn_rss-A.total;
        { int aw=W-9-(Lw+2)-1; if(aw<1) aw=1;
          if(diff>0){ char ud[24]; snprintf(ud,sizeof ud,"+%s",H(diff));
                      cv_put(Lw+2,y+hh-2,cutf(L("미설명 잔여"),aw),"38;5;203");
                      cv_put(W-9,y+hh-2,padf(ud,7,1),"38;5;203"); }
          else { char od[24]; snprintf(od,sizeof od,"-%s",H(-diff));
                 cv_put(Lw+2,y+hh-2,cutf(L("과대계상(free+비상주)"),aw),"38;5;214");
                 cv_put(W-9,y+hh-2,padf(od,7,1),"38;5;214"); } }
    }
    y+=hh;
    /* ---- Buffer pool, BCB direct read ---- */
    {
        char pt[96];
        snprintf(pt,sizeof pt,g_ascii?"buffer pool · page slots — buffer control blocks (◌, %.0fms)":"버퍼풀 · 페이지 슬롯 = 버퍼 제어블록(BCB) (◌, %.0fms)",PG.ok?PG.ms:0.0);
        cv_box(0,y,W,6,pt);
        if(!PG.ok){ cv_put(2,y+1,g_ascii?"off":"비활성","38;5;203"); cv_put(2,y+2,cutf(PG.why,W-4),"38;5;240"); }
        else {
            char ln[200];
            double rf = PG.nbuf>0 ? (double)PG.resident/PG.nbuf : 0, df = PG.nbuf>0 ? (double)PG.dirty/PG.nbuf : 0;
            int mw = W>90 ? 30 : 16;
            cv_put(2,y+1,g_ascii?"resident":"상주","250");
            cv_meter3(12,y+1,mw,rf,df,0);           /* Foreground is the resident share, blue background the dirty share */
            snprintf(ln,sizeof ln,"%d/%d %.0f%%   dirty %d (%.1f%%)   flushing %d",PG.resident,PG.nbuf,rf*100,PG.dirty,df*100,PG.flushing);
            cv_put(12+mw+2,y+1,ln,"250");
            snprintf(ln,sizeof ln,g_ascii?"zones  hot %d  warm %d  cold %d  void %d  free %d":"존  hot %d  warm %d  cold %d  void %d  free %d",
                     PG.z1,PG.z2,PG.z3,PG.zvoid,PG.zinv);
            cv_put(2,y+2,ln,"250");
            if(PG.have_log){
                snprintf(ln,sizeof ln,g_ascii?"log append %s  flushed %s  fsync delay %lld pg   oldest dirty %s  flush delay %lld pg"
                                             :"로그 append %s  flushed %s  fsync 지연 %lld pg   최고령 dirty %s  flush 지연 %lld pg",
                         lsa_str(PG.log_append),lsa_str(PG.log_nxio),(long long)(PG.log_append.pageid-PG.log_nxio.pageid),
                         PG.have_oldest?lsa_str(PG.oldest_dirty):"-",
                         PG.have_oldest?(long long)(PG.log_append.pageid-PG.oldest_dirty.pageid):0LL);
            } else snprintf(ln,sizeof ln,g_ascii?"(log LSA unavailable)":"(로그 LSA 미확보)");
            cv_put(2,y+3,cutf(ln,W-4),"250");
            { int pos=0; ln[0]=0;
              for(int rank=0;rank<4;rank++){ int best=-1;
                  for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]>0 && (best<0||PG.vol_res[v]>PG.vol_res[best])) best=v;
                  if(best<0) break;
                  pos+=snprintf(ln+pos,sizeof ln-(size_t)pos,"%svol%d %d/%dd",rank?"  ":"",best,PG.vol_res[best],PG.vol_dirty[best]);
                  PG.vol_res[best]=-PG.vol_res[best]; if(pos>=(int)sizeof ln-1) break; }
              for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]<0) PG.vol_res[v]=-PG.vol_res[v];
              char l2[220]; snprintf(l2,sizeof l2,g_ascii?"per volume (resident/dirty): %s":"볼륨별(상주/dirty): %s",ln);
              cv_put(2,y+4,cutf(l2,W-4),"38;5;244"); }
        }
        y+=6;
    }
    /* ---- Memory trend: a reduced form of the time-series panel A ----
       (same render function, same verdict).  In compact layout it was already drawn
       beside the process box above. */
    if(!compact){
        pser_t s2[6]; int n=pl_series_A(s2,0);
        char vb[320]; const char*vc=VD_INFO; vd_mem(vb,sizeof vb,&vc);
        char wtxt[24]; vd_wtxt(wtxt,sizeof wtxt);
        double d=HN>1?HS[HN-1].srv-HS[0].srv:0, w=vd_win(), rate=w>0?d/w*3600:0, mj=0;
        for(int i=0;i<HN;i++) if(HS[i].majflt>mj) mj=HS[i].majflt;
        if(g_ascii) snprintf(t2,sizeof t2,"memory trend · load(OS) · window %s (%d samples) · [p] full timeseries",wtxt,HN);
        else        snprintf(t2,sizeof t2,"메모리 추이 · load(OS) · 관측창 %s (샘플 %d개) · [p] 시계열 전체",wtxt,HN);
        char rt[64];
        if(mj>0) snprintf(rt,sizeof rt,"%s%s/h · mf%.0f/s",rate>=0?"+":"-",H(rate<0?-rate:rate),mj);
        else     snprintf(rt,sizeof rt,"%s%s/h",rate>=0?"+":"-",H(rate<0?-rate:rate));
        cv_panel2(0,y,W,3,t2,rt,s2,n,0,0,NULL,&PL_LOAD,vb,vc,1,NULL,0,1);
        y+=8;
    }
    /* ---- I/O; absolute gauge floors of 8MB/s and 200 IOPS stop idle flicker ---- */
    {
        int yy;
        /* Read, write, IOPS and absorption come from /proc/<pid> and are per instance,
           while device utilization and iowait come from diskstats and /proc/stat and
           are host-wide; the row labels say which (only the D-thread count beside
           iowait is this server's).  Row count varies, so count before drawing. */
        int has_dev = IO->devnm[0]?1:0;
        /* Verdict from wa combined with device utilization.  wa is diluted by the core count
           (1-2% on 64 cores with the disk saturated), so read alone it misses the
           bottleneck; it is crossed with utilization, majflt and D threads. */
        /* The wa threshold must scale with the core count.  Three dd streams taking a device
           to 69% still left wa at 1.5% on 64 cores, so a fixed 3% is unreachable there.
           The threshold is half of one core waiting outright (100/ncpu). */
        double wa_hi = (IO->ncpu>0 ? 100.0/IO->ncpu : 100.0)*0.5;
        if(wa_hi<0.5) wa_hi=0.5;           /* A floor applies even with very many cores */
        int vd_hi_wa = (M->cpu_wa>=wa_hi), vd_hi_dev = (has_dev&&IO->devutil>=80);
        /* Evidence that this instance is the one using the disk: a busy device driven by
           some other process is not a CUBRID bottleneck.  rb/wb (/proc/<pid>/io)
           must be included, because under an overlay filesystem the backing device
           of the DB files cannot be identified in diskstats - the server can read
           hundreds of MB/s with the matched device showing zero.  Waiting signals
           alone (D threads, blkio) would misattribute that load. */
        int vd_mine = (IO->majflt>=1.0) || (IO->d_thr>0) || (IO->blkpct>=1.0)
                      || (IO->rb+IO->wb >= 1.0*1024*1024);   /* Above 1MB/s this instance is doing I/O too */
        int verdict = 0;   /* 0 none, 1 certain bottleneck, 2 masked, 3 non-disk wait, 4 someone else's load */
        /* Verdicts persist.  I/O anomalies are brief, so a warning cleared by the next
           frame reads as "I saw it and it vanished".  The two most recent are kept
           with their timestamps and pushed up by newer ones; a repeat of the same
           verdict updates the time rather than adding a line. */
        #define VDKEEP 2
        static struct { int kind; vdval_t v; char hm[8]; int live; } g_vd[VDKEEP];
        static int g_vdn=0;
        if(vd_hi_wa && vd_hi_dev && vd_mine)         verdict=1;
        else if(!vd_hi_wa && vd_hi_dev && vd_mine)   verdict=2;
        else if(vd_hi_dev && !vd_mine)               verdict=4;
        else if(vd_hi_wa && !vd_hi_dev)              verdict=3;
        /* Verdicts include retained ones, so their row count is counted in advance for the box
           height; a new kind arriving this frame can add one. */
        int vdrows = g_vdn;
        if(verdict && (g_vdn==0 || g_vd[0].kind!=verdict)) vdrows++;
        if(vdrows>VDKEEP) vdrows=VDKEEP;
        int nrow = 5 + has_dev + vdrows;
        cv_box(0,y,W,nrow+2,L("I/O · cub_server + 최다사용 장치"));
        double pr=hist_peak(0,8.0*1024*1024), pw=hist_peak(1,8.0*1024*1024), pi=hist_peak(2,200);
        struct { const char*g,*nm; double frac; char prim[24],sec[28]; int warn; } R[8];
        int n=0;
        /* CPU utilization itself is not repeated here - the OS and process boxes show it
           twice already.  Only iowait stays: it is an I/O metric and belongs beside the
           disk rows where it can be compared quickly. */
        /* iowait leads the disk rows.  Its gauge is scaled to 10%, not 100%: wa is
           divided by the core count, so on 64 cores a saturated device still
           measured 0.8% and would not fill one cell.  The D-thread count at the
           right is direct evidence, undiluted. */
        { R[n].g="●"; R[n].nm=g_ascii?"iowait (host)":"iowait(OS)";
          R[n].frac=M->cpu_wa/10.0; if(R[n].frac>1) R[n].frac=1;
          R[n].warn=(M->cpu_wa>=3.0)||(IO->d_thr>0);
          snprintf(R[n].prim,24,"%.1f%%",M->cpu_wa);
          if(IO->n_thr>0) snprintf(R[n].sec,28,"%s %d/%d",g_ascii?"D thr":"D스레드",
                                   IO->d_thr,IO->n_thr);
          else            snprintf(R[n].sec,28,"%s",g_ascii?"D thr -":"D스레드 -");
          n++; }
        R[n].g="●"; R[n].nm=L("디스크 읽기"); R[n].frac=IO->rb/pr; R[n].warn=0;
        snprintf(R[n].prim,24,"%s/s",H(IO->rb)); snprintf(R[n].sec,28,"%s %s",L("누적"),H((double)P->read_b)); n++;
        R[n].g="●"; R[n].nm=L("디스크 쓰기"); R[n].frac=IO->wb/pw; R[n].warn=0;
        snprintf(R[n].prim,24,"%s/s",H(IO->wb)); snprintf(R[n].sec,28,"%s %s",L("누적"),H((double)P->write_b)); n++;
        R[n].g="●"; R[n].nm="IOPS"; R[n].frac=IO->ri/pi; R[n].warn=0;
        snprintf(R[n].prim,24,"%.0f/s",IO->ri); snprintf(R[n].sec,28,"%s %.0f/s",L("쓰기"),IO->wi); n++;
        if(IO->absorb_ok){ R[n].g="◌"; R[n].nm=L("캐시 흡수율"); R[n].frac=IO->absorb;
            R[n].warn=(IO->absorb<0.05&&IO->rb>0);
            snprintf(R[n].prim,24,"%.0f%%",IO->absorb*100); }
        else { R[n].g="◌"; R[n].nm=L("캐시 흡수율"); R[n].frac=0; R[n].warn=0;
            snprintf(R[n].prim,24,"%s",L("유휴")); }
        snprintf(R[n].sec,28,"rchar %s/s",H(IO->rchar)); n++;
        if(IO->devnm[0]){ R[n].g="●"; R[n].frac=IO->devutil/100.0; R[n].warn=IO->devutil>=90;
            static char dn[80]; snprintf(dn,sizeof dn,"%s %s%s",L("장치"),IO->devnm,
                                         g_ascii?"(host)":"(OS)"); R[n].nm=dn;
            snprintf(R[n].prim,24,"%.0f%%",IO->devutil);
            snprintf(R[n].sec,28,"%s %s/s",L("읽기"),H(IO->devr)); n++; }
        /* Unlike the region box's four columns the I/O box has two values, so reusing those
           positions leaves 15 cells at the right - the values go to the right edge and
           the gauge takes the rest. */
        { int SEC_X=W-2-16, PRI_X=SEC_X-8;
          for(int i=0;i<n;i++){
            yy=y+1+i;
            cv_put(2,yy,R[i].g,R[i].g[0]=='?'?"38;5;203":"38;5;39");
            cv_put(4,yy,padf(R[i].nm,14,0),"250");
            cv_meter(MET_X,yy,PRI_X-MET_X-1,R[i].frac,0);
            cv_put(PRI_X,yy,padf(R[i].prim,7,1),R[i].warn?"38;5;203":"250");
            cv_put(SEC_X,yy,padf(R[i].sec,16,1),"38;5;244");
          } }
        /* The verdict states in one line what the rows above imply together, so that wa
           is not read on its own: it is diluted by core count and hidden when the CPU
           saturates. */
        if(verdict){
            vdval_t vv={ M->cpu_wa, IO->devutil, IO->blkpct, IO->majflt, IO->rb+IO->wb, IO->d_thr, IO->n_thr };
            /* A repeat of the same kind updates the latest entry rather than adding a row - the
               same warning across two rows would push the previous one out. */
            if(g_vdn>0 && g_vd[0].kind==verdict){
                g_vd[0].v=vv;
                strftime(g_vd[0].hm,sizeof g_vd[0].hm,"%H:%M",&tv);
                g_vd[0].live=1;
            } else {
                /* An entry being pushed down no longer holds, so live is cleared and it cannot be read
                   as something happening now. */
                if(g_vdn>0) g_vd[0].live=0;
                for(int k=VDKEEP-1;k>0;k--) g_vd[k]=g_vd[k-1];
                g_vd[0].kind=verdict;
                g_vd[0].v=vv;
                strftime(g_vd[0].hm,sizeof g_vd[0].hm,"%H:%M",&tv);
                g_vd[0].live=1;
                if(g_vdn<VDKEEP) g_vdn++;
            }
        } else if(g_vdn>0) g_vd[0].live=0;   /* The condition cleared; the record stays */
        /* Draw retained verdicts newest first: those still holding in their own colour, past
           ones dimmed to grey. */
        for(int k=0;k<g_vdn;k++){
            char vb[256], line[288];
            vd_format(g_vd[k].kind,&g_vd[k].v,vb,sizeof vb);
            snprintf(line,sizeof line,"%s %s",g_vd[k].hm,vb);
            const char *c;
            if(!g_vd[k].live)      c="38;5;240";           /* Past */
            else if(g_vd[k].kind==1) c="1;38;5;203";
            else if(g_vd[k].kind==2) c="38;5;215";
            else                     c="38;5;244";
            cv_put(2,y+1+n,cutf(line,W-4),c);
            n++;
        }
        y+=n+2;
    }
    CV.h=y;                                   /* Output only up to the height actually used */
    /* Re-assert cursor hiding every frame: a paramdump child inheriting the terminal, or
       a resize or mode change, can restore it, and it then blinks for one frame at the
       top left right after the move to home. */
    /* Atomic frames (DECSET 2026, synchronized output).  Even with ?25l some
       terminals track the cursor while drawing, which shows as a cursor artifact
       on wide cells.  Output between 2026h and 2026l lands at once on terminals
       that support it, and unsupported ones ignore the sequence. */
    printf("\033[?2026h\033[?25l\033[H");
    /* Fill the screen: the canvas draws only above the last two rows (overflow would clip
       the lower boxes) and those two always carry the key hints.  No newline is used,
       so there is no scrolling or flicker. */
    int trows=term_h(); if(trows<8) trows=CV.h+2;
    int clipped=cv_flush_rows(1,trows-2);
    printf("\033[%d;1H\033[J",(CV.h<trows-2?CV.h:trows-2)+1);   /* Clear leftovers below the canvas */
    /* The status line is outside the canvas and unbounded, so it is cut to avoid wrapping
       on a narrow terminal, with the PAUSED marker's width budgeted in advance. */
    /* The separator is grey so the key groups stand out; printing one string would not
       allow that, hence the piecewise output.
       With a single instance < and > do nothing, so they are only hinted when several
       exist. */
    {
      const char *seg[8]; int nseg=0;
      if(g_ascii){
          seg[nseg++]="[1/2/3] alloc/hot/over";
          seg[nseg++]="heap [r]eflash [a]uto";
          if(g_ninst>1) seg[nseg++]="[<>]inst";
          seg[nseg++]="[p]plot";
          seg[nseg++]="[l]lang"; seg[nseg++]="[h]help"; seg[nseg++]="[q]quit";
      } else {
          seg[nseg++]="[1/2/3] 할당/핫/초과";
          seg[nseg++]="힙 [r]재측정 [a]자동";
          if(g_ninst>1) seg[nseg++]="[<>]인스턴스";
          seg[nseg++]="[p]시계열";
          seg[nseg++]="[l]한/영"; seg[nseg++]="[h]도움말"; seg[nseg++]="[q]종료";
      }
      char clip[24]=""; int clipw=0;
      /* Keep the clipped-row count short, or the help and quit segments lose their budget */
      if(clipped>0){ snprintf(clip,sizeof clip,g_ascii?"v%d":"\xe2\x96\xbc%d",clipped); clipw=disp_w(clip)+1; }
      int budget = W-2-(paused?9:0)-clipw;   /* One leading space plus one of slack */
      if(budget<0) budget=0;
      /* Bottom two-row overlay: one blank row and one key row, placed absolutely and ended
         without a newline: a newline on the last row would scroll the screen every frame. */
      printf("\033[%d;1H\033[K\033[%d;1H %s",trows-1,trows, paused?"\033[7m PAUSED \033[0m ":"");
      /* When width runs short, drop middle segments rather than cutting from the end: help
         and quit must survive at any width (the way out for a first-time user). */
      int keep[8]; for(int i=0;i<nseg;i++) keep[i]=1;
      for(;;){
          int tot=0,first=1;
          for(int i=0;i<nseg;i++) if(keep[i]){ tot+=(first?0:3)+disp_w(seg[i]); first=0; }
          if(tot<=budget) break;
          int drop=-1;
          for(int i=nseg-3;i>=1;i--) if(keep[i]){ drop=i; break; }   /* Middle, later ones first */
          if(drop<0){ for(int i=0;i<nseg-2;i++) if(keep[i]){ drop=i; break; } }
          if(drop<0) break;
          keep[drop]=0;
      }
      int used=0, first=1;
      for(int i=0;i<nseg;i++){
          if(!keep[i]) continue;
          int sw=disp_w(seg[i]);
          if(!first){ if(used+3+sw>budget) break; printf("\033[38;5;240m · \033[0m"); used+=3; }
          else if(sw>budget) break;
          first=0;
          printf("\033[38;5;250m%s\033[0m",seg[i]); used+=sw;
      }
      if(clip[0]) printf(" \033[38;5;214m%s\033[0m",clip);   /* Clipped row count, reported rather than hidden */
      printf("\033[K"); }
    printf("\033[?2026l");           /* End the atomic frame; everything appears at once here */
    fflush(stdout);
}

static void render_tree(proc_t *P,mem_t *M,tier_t *T,int with_a,
                        double io_rbps,double io_wbps,double io_riops,double io_wiops,
                        double absorb,double blkpct,const char *devnm,double devutil,
                        double devr,const char *ts,double rchar_bps,double work_ms,
                        double cpu_pct,int ncpu){
    double mt=M->total*1024.0, av=M->avail*1024.0, used=mt-av;
    /* PSS on the same basis as the dashboard's process box, so one label never carries two
       values.  RSS double-counts shared pages and the tier totals would not match. */
    double srv=P->pss_kb*1024.0+T->master;
    double brk=T->broker+T->cas;
    double cub=P->pss_kb*1024.0+T->master+T->broker+T->cas+T->pl;

    printf(g_ascii?"CUBRID memory drilldown (C)  db=%s  pid=%d  ⏱ %s\n"
                  :"CUBRID 메모리 드릴다운 (C)  db=%s  pid=%d  ⏱ %s\n",P->db,P->pid,ts);
    if(g_ninst>1){
        /* States that only the db above is examined in detail; the rest give totals only. */
        printf(g_ascii?"  %d instances:":"  인스턴스 %d개:",g_ninst);
        for(int i=0;i<g_ninst;i++)
            printf(" %s%s %s%s", i==g_cur?"▶":"", g_inst[i].db,
                   H(g_inst[i].pss+g_inst[i].pl_pss), i<g_ninst-1?" ·":"");
        printf(g_ascii?"   (detail = %s — other DBs: 'cub_top <db>')\n"
                      :"   (상세는 %s — 다른 DB 는 'cub_top <db명>')\n",P->db);
    }
    if(P->pid>=0 && NSR>0){
        /* This tool's premise is labelling by cubrid.conf parameter name; without stating the
           share actually attributable to parameters, the premise reads as an
           overpromise. */
        double byparam=0, tot=0;
        for(int i=0;i<NSR;i++){ tot+=SR[i].rss; if(SR[i].cfg>0) byparam+=SR[i].rss; }
        if(tot>0)
            printf(g_ascii?"explained by config: %.0f%% (%s) / unexplained %.0f%%"
                          " — the latter is split by heap detail below (B=engine, A=estimate)\n"
                          :"설정으로 설명: cubrid.conf 파라미터 %.0f%% (%s) / 설명 안 되는 영역 %.0f%%"
                          " — 후자는 아래 동적 메모리 상세(B=정확, A=크기 추정)로 분해\n",
                   byparam/tot*100, H(byparam), (tot-byparam)/tot*100);
    }
    /* Grade marks appear from the first line while the full legend is at the foot (for
       attaching to a ticket), so a summary comes first and '?' is not misread as an
       error by someone reading downward. */
    printf(g_ascii?"marks  ● measured  ◐ reserved  ◌ estimated  ? unattributed (split by heap detail)\n"
                  :"기호  ● 측정  ◐ 예약  ◌ 추정  ? 미귀속(아래 동적 메모리 상세가 분해)\n");
    /* Always yes in a normal environment and thus uninformative; reported only when a fallback applies */
    if(!(CAP.vm_readv && CAP.rollup && CAP.proc_io && CAP.clear_refs))
        printf(g_ascii?"⚠ fallback active: %s   (see -h)\n"
               :"⚠ 폴백 동작 중: %s   (의미는 -h 참조)\n",CAP.note);
    if(P->reg_dropped)
        printf(g_ascii?"⚠ %d memory regions beyond the %d tracked were not classified - the breakdown is short of the total\n"
               :"⚠ 메모리 영역 %d개가 한도 %d개를 넘어 분류되지 않음 — 구성 합계가 총량보다 작게 나옴\n",P->reg_dropped,MAXREG);
    if(g_disk_dropped)
        printf(g_ascii?"⚠ %d block devices beyond %d were not read\n":"⚠ 블록 장치 %d개가 한도 %d개를 넘어 읽지 않음\n",g_disk_dropped,MAXDISK);
    printf("OS RAM  %s\n",H(mt));
    printf("├─ used  %s (%.0f%%)\n",H(used),used/(mt>0?mt:1)*100);
    printf("├─ cached  %s\n",H(M->cached*1024.0));
    printf("├─ avail  %s\n",H(av));
    if(M->swtotal==0) printf("├─ swap  off\n");
    else printf("├─ swap  %s / %s\n",H((M->swtotal-M->swfree)*1024.0),H(M->swtotal*1024.0));
    printf("├─ CUBRID engine  %s (%.2f%% of RAM)\n",H(cub),cub/(mt>0?mt:1)*100);
    if(P->pid<0){
        printf(g_ascii?"│  ├─ ? cub_server  [not running] — no measurement, showing cubrid.conf only\n"
               :"│  ├─ ? cub_server  [미기동] — 실측 불가, cubrid.conf 설정값만 표시\n");
        static const char *cf[]={"data_buffer_size","log_buffer_size","sort_buffer_size",
                                 "temp_file_memory_size_in_pages","max_subquery_cache_size"};
        for(unsigned i=0;i<sizeof cf/sizeof cf[0];i++){
            long long v=prm_get(cf[i]);
            if(v>0) printf(g_ascii?"│  │  ├─ ◐ %s (config)  %s\n":"│  │  ├─ ◐ %s (설정)  %s\n",cf[i],H((double)v));
        }
        printf(g_ascii?"│  └─ ● broker+CAS (separate 3-tier layer)  %s\n"
               :"│  └─ ● broker+CAS (3-tier 별도 계층)  %s\n",H(brk));
        if(T->npl>0) printf("│     └─ ◐ PL server(JVM)  %s\n",H(T->pl));
        printf("└─ I/O\n");
        if(devnm[0]) printf(g_ascii?"   ├─ ● dev %s (host-wide)  util %.0f%%  r %s/s\n"
                                   :"   ├─ ● dev %s (시스템 전체)  util %.0f%%  r %s/s\n",devnm,devutil,H(devr));
        printf(g_ascii?"   └─ ? cub_server I/O  not running — cannot attribute\n"
               :"   └─ ? cub_server I/O  미기동 — 프로세스 귀속 불가\n");
        return;
    }
    /* Collection time is the tool's own business; reported only when slow */
    if(work_ms>=1000.0)
        printf(g_ascii?"│  ├─ ● server+master  %s  (VmSize %s, worker %d, collect %.1fs)\n"
                      :"│  ├─ ● server+master  %s  (VmSize %s, worker %d, 수집 %.1fs)\n",
               H(srv),H(P->vsize_kb*1024.0),P->nthreads,work_ms/1000.0);
    else
        printf("│  ├─ ● server+master  %s  (VmSize %s, worker %d)\n",
               H(srv),H(P->vsize_kb*1024.0),P->nthreads);
    for(int i=0;i<NSR;i++){
        srg_t *r=&SR[i];
        int last=(i==NSR-1) && T->npl==0;
        printf("│  │  %s %s %-14s %s",last?"└─":"├─",GR(r->grade),r->name,
               r->rss>0?H(r->rss):"0B");
        if(r->mapped>0) printf(" / alloc %s",H(r->mapped));
        if(!strcmp(r->name,"glibc arena"))
            printf(g_ascii?"  (virtual reservation — 0 physical)":"  (가상 예약 — 물리 메모리 0)");
        if(!strcmp(r->name,"thread stacks")){
            long long ts=prm_get("thread_stacksize");
            if(ts>0) printf(g_ascii?"  (thread_stacksize %s is HA-path only — workers use OS default 8MB)"
                                   :"  (thread_stacksize %s 는 HA 경로 전용 — 워커는 OS 기본 8MB)",
                            H((double)ts));
        }
        if(r->cfg>0){
            double ov=r->mapped/(double)r->cfg*100;
            /* Exceeding the configured size is normal - the BCB and victim arrays and other
               management structures share the mapping.  The normal range is given so it
               is not read as the setting being ignored. */
            printf(g_ascii?"  (config %s, alloc %.0f%%%s)":"  (설정 %s, 할당 %.0f%%%s)",
                   H((double)r->cfg),ov,
                   (ov<=120.0)?(g_ascii?" ok":" 정상"):(g_ascii?" ⚠over":" ⚠초과"));
        }
        if(r->rss>0&&r->ref>0){
            /* "hot" is the share of resident pages touched in the recent window.  A low value is
               not necessarily a problem - the workload may simply be quiet - so a
               reading accompanies it. */
            double hp=r->ref/r->rss*100;
            printf(g_ascii?"  recent-access %.0f%%%s":"  최근접근 %.0f%%%s",
                   hp,(hp<1.0)?(g_ascii?"(idle)":"(유휴)"):"");
        }
        printf("\n");
        if(!strcmp(r->name,"dynamic-heap")){
            if(B.ok){
                for(int k=0;k<B.n;k++)
                    printf("│  │  │  ├─ %s [B] %-22s %s   (%s%s)\n",
                           GR(B.it[k].grade),B.it[k].name,
                           B.it[k].count_only?"-":H(B.it[k].bytes),B.it[k].note,
                           (B.it[k].bytes==0.0 && B.it[k].grade!='U' && !B.it[k].count_only)
                               ? (g_ascii?" — unused":" — 미사용") : "");
            } else printf(g_ascii?"│  │  │  ├─ ? [B] disabled — %s\n"
                                 :"│  │  │  ├─ ? [B] 비활성 — %s\n",B.why);
            if(with_a){
                for(int c=0;c<7;c++) if(A.cat[c]>0)
                    printf("│  │  │  ├─ ◌ [A] %-22s %s\n",ACAT[c],H(A.cat[c]));
                double diff=g_dyn_rss-A.total;
                /* A walks the heap after the smaps window, so its instant differs from dyn.
                   The unexplained and over-counted figures carry that gap together with
                   A's structural limits. */
                double ratio = (g_dyn_rss>0) ? (diff>=0?diff:-diff)/g_dyn_rss*100 : 0;
                printf("│  │  │  └─ ◌ [A] total %s  (%s %s = %.0f%% %s)\n",
                       H(A.total),
                       diff>=0?(g_ascii?"unexplained +":"미설명 잔여 +")
                              :(g_ascii?"overcount -":"과대계상 -"),H(diff>=0?diff:-diff),
                       ratio, (ratio<=30.0)?(g_ascii?"— ok (query/thread contexts unreachable)"
                                                    :"— 정상 (질의·스레드 컨텍스트는 도달 불가)")
                                           :(g_ascii?"— >30%, re-run advised"
                                                    :"— 30% 초과, 재분석 권고"));
            }
        }
    }
    if(T->npl>0) printf(g_ascii?"│  │  └─ ◐ PL server(JVM) — separate process started by server  %s\n"
                               :"│  │  └─ ◐ PL server(JVM) — 서버가 기동하는 별도 프로세스  %s\n",H(T->pl));
    printf(g_ascii?"│  └─ ● broker+CAS (separate 3-tier layer)  %s\n"
               :"│  └─ ● broker+CAS (3-tier 별도 계층)  %s\n",H(brk));
    if(P->pid>=0){
        /* ---- Capacity: demand, ceiling and headroom against the allocated resource ---- */
        printf(g_ascii?"├─ ◌ capacity (demand vs ceiling, %.0fms)\n":"├─ ◌ 용량 (수요 대비 상한, %.0fms)\n",CAP2.ms);
        if(CAP2.over_budget){
            printf(g_ascii?"│  └─ ? skipped: %s\n":"│  └─ ? 취합 생략: %s\n",CAP2.ps_why);
        } else {
            printf(g_ascii?"│  ├─ ● demand  dev %.0f IOPS · this server %.0f IOPS · read %s/s write %s/s\n"
                          :"│  ├─ ● 수요  장치 %.0f IOPS · 이 서버 %.0f IOPS · 읽기 %s/s 쓰기 %s/s\n",
                   CAP2.dev_iops,CAP2.riops+CAP2.wiops,H(CAP2.read_bps),H(CAP2.write_bps));
            if(CAP2.await_ms>=0)
                printf(g_ascii?"│  ├─ ● latency  await %.2fms (r %.2f / w %.2f) · queue %.2f · util %.0f%%\n"
                              :"│  ├─ ● 지연  요청당 %.2fms (읽기 %.2f / 쓰기 %.2f) · 큐 %.2f · 사용률 %.0f%%\n",
                       CAP2.await_ms,CAP2.r_await_ms<0?0:CAP2.r_await_ms,CAP2.w_await_ms<0?0:CAP2.w_await_ms,
                       CAP2.qdepth<0?0:CAP2.qdepth,CAP2.dev_util);
            /* This frame's I/O profile, stating which ceiling cell the comparison uses */
            printf(g_ascii?"│  ├─ ● profile  avg request %.1fKB · read %.0f%% → cell [%s / %s]\n"
                          :"│  ├─ ● 프로파일  평균 요청 %.1fKB · 읽기 %.0f%% → 상한 칸 [%s / %s]\n",
                   CAP2.req_kb,CAP2.read_ratio*100.0,CAP_BS_NAME[CAP2.prof_bs],CAP_RW_NAME[CAP2.prof_rw]);
            if(CAP2.ceil_valid){
                struct tm ct; char cw[32]="?";
                if(localtime_r(&CAP2.ceil_when,&ct)) strftime(cw,sizeof cw,"%m-%d %H:%M",&ct);
                printf(g_ascii?"│  ├─ ◌ ceiling  %.0f IOPS observed at util %.0f%% (%s) in this cell → now using %.0f%%\n"
                              :"│  ├─ ◌ 상한  %.0f IOPS (같은 칸, 사용률 %.0f%% 시점 관측, %s) → 현재 %.0f%% 사용\n",
                       CAP2.ceil_iops,CAP2.ceil_util,cw,CAP2.headroom_pct<0?0:CAP2.headroom_pct);
            } else
                printf(g_ascii?"│  ├─ ? ceiling  not observed for this cell (needs a saturated moment at this profile; --capacity-state persists it)\n"
                              :"│  ├─ ? 상한  이 칸은 미관측 (같은 프로파일에서 포화 순간 필요; --capacity-state 로 보존)\n");
            /* Ceilings remembered in other cells are shown too, making clear that the ceiling varies by profile */
            { int shown=0;
              for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++){
                  const cap_cell_t*cc=&g_cap_mtx[b][r];
                  if(!cc->valid || (b==CAP2.prof_bs && r==CAP2.prof_rw)) continue;
                  if(!shown++) printf(g_ascii?"│  ├─ ◌ other cells\n":"│  ├─ ◌ 다른 칸 상한\n");
                  printf(g_ascii?"│  │    [%s / %s] %.0f IOPS · await %.2fms · q %.2f\n"
                                :"│  │    [%s / %s] %.0f IOPS · 대기 %.2fms · 큐 %.2f\n",
                         CAP_BS_NAME[b],CAP_RW_NAME[r],cc->iops,cc->await_ms,cc->qd);
              } }
            if(CAP2.hit_pct>=0)
                printf(g_ascii?"│  ├─ ◌ buffer  hit %.1f%% (source: %s) · admitted %.0f pages/s · re-read %.0f%%\n"
                              :"│  ├─ ◌ 버퍼  히트 %.1f%% (출처: %s) · 적재 초당 %.0f페이지 · 재적재 %.0f%%\n",
                       CAP2.hit_pct,CAP2.hit_src,CAP2.miss_pps<0?0:CAP2.miss_pps,CAP2.readmit_pct<0?0:CAP2.readmit_pct);
            if(CAP2.ps_ok)
                printf(g_ascii?"│  ├─ ● engine  fetch %.0f/s ioread %.0f/s iowrite %.0f/s log-write %.0f/s commit %.0f/s\n"
                              :"│  ├─ ● 엔진  fetch %.0f/s ioread %.0f/s iowrite %.0f/s 로그쓰기 %.0f/s 커밋 %.0f/s\n",
                       CAP2.pb_fetch_ps,CAP2.pb_ioread_ps,CAP2.pb_iowrite_ps,CAP2.log_iowrite_ps,CAP2.commit_ps);
            else
                printf(g_ascii?"│  ├─ ◌ engine counters idle — %s\n":"│  ├─ ◌ 엔진 카운터 정지 — %s\n",CAP2.ps_why);
            { char vb[320]; const char*vc=VD_INFO; vd_capacity(vb,sizeof vb,&vc);
              printf(g_ascii?"│  └─ verdict  %s\n":"│  └─ 판정  %s\n",vb); }
        }
        /* Buffer pool: which pages are resident and how much has not reached disk (dirty).
           volmap --bufmap overlays the same snapshot on the volume map (--bcb-dump). */
        if(PG.ok){
            double rp = PG.nbuf>0 ? 100.0*PG.resident/PG.nbuf : 0, dp = PG.resident>0 ? 100.0*PG.dirty/PG.resident : 0;
            printf(g_ascii?"├─ ◌ page buffer (page slots/BCB read directly, %.0fms)  resident %d/%d (%.0f%%)  dirty %d (%.1f%% of resident)  flushing %d\n"
                          :"├─ ◌ 버퍼풀 (페이지 슬롯 %.0fms)  상주 %d/%d (%.0f%%)  dirty %d (상주의 %.1f%%)  flushing %d\n",
                   PG.ms,PG.resident,PG.nbuf,rp,PG.dirty,dp,PG.flushing);
            printf(g_ascii?"│  ├─ zones  hot %d · warm %d · cold %d · void %d · free %d   (victims are taken from cold; dirty ones are skipped)\n"
                          :"│  ├─ 존  hot %d · warm %d · cold %d · void %d · free %d   (victim 은 cold 에서, dirty 는 건너뜀)\n",
                   PG.z1,PG.z2,PG.z3,PG.zvoid,PG.zinv);
            { int shown=0;
              for(int rank=0;rank<6;rank++){ int best=-1;
                  for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]>0 && (best<0||PG.vol_res[v]>PG.vol_res[best])) best=v;
                  if(best<0) break;
                  printf(g_ascii?"│  ├─ vol %-3d resident %d (%s)  dirty %d\n":"│  ├─ vol %-3d 상주 %d (%s)  dirty %d\n",
                         best,PG.vol_res[best],H((double)PG.vol_res[best]*g_pagesize),PG.vol_dirty[best]);
                  PG.vol_res[best]=-PG.vol_res[best];   /* Marked as shown (negative), restored below */
                  shown++; }
              for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]<0) PG.vol_res[v]=-PG.vol_res[v];
              (void)shown; }
            if(PG.have_log){
                printf(g_ascii?"│  ├─ log  append %s  flushed(nxio) %s  → %lld log pages not yet on disk\n"
                              :"│  ├─ 로그  append %s  flushed(nxio) %s  → 아직 디스크에 안 내려간 로그 %lld 페이지\n",
                       lsa_str(PG.log_append),lsa_str(PG.log_nxio),(long long)(PG.log_append.pageid-PG.log_nxio.pageid));
                if(PG.have_oldest)
                    printf(g_ascii?"│  └─ oldest dirty page LSA %s  → flush delay %lld log pg behind append (recovery redo length)\n"
                                  :"│  └─ 가장 오래된 dirty 페이지 LSA %s  → append 보다 %lld 로그 페이지 뒤 = flush 지연(리커버리 redo 길이)\n",
                           lsa_str(PG.oldest_dirty),(long long)(PG.log_append.pageid-PG.oldest_dirty.pageid));
                else printf(g_ascii?"│  └─ no dirty page — buffer fully reflected to volumes\n":"│  └─ dirty 없음 — 버퍼 내용이 볼륨에 전부 반영됨\n");
            } else printf(g_ascii?"│  └─ (log LSA unavailable)\n":"│  └─ (로그 LSA 미확보)\n");
        } else printf(g_ascii?"├─ ? page buffer (BCB)  off — %s\n":"├─ ? 버퍼풀 (BCB)  비활성 — %s\n",PG.why);
    }
    if(CAP.proc_io){
        printf("└─ CPU · I/O (cub_server + device)\n");
        /* CPU counts one core as 100%, so a multicore process can exceed it */
        printf(g_ascii?"   ├─ ● cpu  %.0f%%  (1 core = 100%%, host %d cores)\n"
               :"   ├─ ● cpu  %.0f%%  (코어 1개=100%%, 호스트 %d코어)\n",cpu_pct,ncpu);
        printf(g_ascii?"   ├─ ● disk read  %s/s  (total %s)\n":"   ├─ ● disk read  %s/s  (누적 %s)\n",H(io_rbps),H((double)P->read_b));
        printf(g_ascii?"   ├─ ● disk write  %s/s  (total %s)\n":"   ├─ ● disk write  %s/s  (누적 %s)\n",H(io_wbps),H((double)P->write_b));
        printf("   ├─ ● iops  read %.0f/s  write %.0f/s\n",io_riops,io_wiops);
        /* Below the rchar floor (64KB/s) the ratio is meaningless, and 100% must not be read
           as a perfect cache (absorb 100% at rchar 95B/s). */
        if(absorb>=0 && rchar_bps>=RCHAR_MIN) printf("   ├─ ◌ cache absorb  %.0f%%\n",absorb*100);
        else printf(g_ascii?"   ├─ ◌ cache absorb  idle (rchar %s/s — too little requested reading to judge)\n"
                          :"   ├─ ◌ cache absorb  idle (rchar %s/s — 판단하기엔 요청 읽기가 너무 적음)\n",H(rchar_bps));
        printf(g_ascii?"   ├─ ● blkio wait  %.2f%%  (sum of all threads — can exceed 100%%;"
                      " async waits not captured)\n"
                      :"   ├─ ● blkio wait  %.2f%%  (전 스레드 합 — 100%%를 넘을 수 있음;"
                      " 비동기 대기는 안 잡힘)\n", blkpct);
        if(devnm[0]) printf("   └─ ● dev %s  util %.0f%% %s  r %s/s\n",devnm,devutil,
                            (devutil>=90.0)?(g_ascii?"⚠ saturated":"⚠ 포화")
                                           :(g_ascii?"(ok <90%)":"(정상 <90%)"),H(devr));
        else         printf(g_ascii?"   └─ (no device stats)\n":"   └─ (장치 통계 없음)\n");
    } else printf(g_ascii?"└─ I/O  (no /proc/pid/io — kernel IO accounting off)\n"
                  :"└─ I/O  (/proc/pid/io 없음 — 커널 IO 회계 비활성)\n");
    if(P->pid>=0){
        int shown=0;
        for(int i=0;i<NPRMROWS;i++){
            long long v=prm_get(PRMROWS[i].name);
            if(v<0) continue;
            if(!shown){
                printf(g_ascii?"\nmemory parameters (cubrid.conf)   ● usage observed   ◐ config only"
                              " — usage folds into dynamic-heap\n"
                              :"\n메모리 파라미터 (cubrid.conf)   ● 사용량까지 관측   ◐ 설정만"
                              " — 사용량은 dynamic-heap 에 합산됨\n");
                shown=1;
            }
            char vs[32];
            switch(PRMROWS[i].unit){
            case 'B': snprintf(vs,sizeof vs,"%s",H((double)v)); break;
            case 'P': snprintf(vs,sizeof vs,"%lld p",v); break;
            default:  snprintf(vs,sizeof vs,"%lld",v); break;
            }
            printf("  %s %-32s %10s   %s\n",
                   PRMROWS[i].obs?"●":"◐", PRMROWS[i].name, vs,
                   g_ascii?PRMROWS[i].desc_en:PRMROWS[i].desc);
        }
    }
    /* A legend is kept so the output alone can be pasted into a ticket and still read (grade marks appear on every row) */
    printf(g_ascii?"\nmarks  ● measured (OS/engine reported)  ◐ reserved (mapped only)  ◌ estimated  ? unattributed\n"
                  :"\n등급  ● 측정(OS·엔진 직접 보고)  ◐ 예약(매핑만 확인)  ◌ 추정(패턴)  ? 미귀속\n");
    if(g_hot_measured)
        printf(g_ascii?"hot   share of rss touched in the last %.1fs window (clear_refs + Referenced)\n"
                      :"핫    최근 %.1f초 창에서 접근된 상주 비율 (clear_refs + Referenced)\n", g_interval);
    else if(g_hot_off_why[0])
        printf(g_ascii?"hot   not measured — %s\n":"핫    측정 안 함 — %s\n",g_hot_off_why);
    else
        printf(g_ascii?"hot   not measured (default off; --hot enables it — writes clear_refs, ~30ms/GB on the target's PTEs)\n"
                      :"핫    측정 안 함 (기본 OFF; --hot 으로 켬 — clear_refs 쓰기, 대상 PTE 순회 약 30ms/GB)\n");
}

/* Name to a key-safe slug (lower-case alphanumeric and '_').  Repeated keys
   (heapB.item, heapA.cat, bytes) leave only the last in a dict parser, defeating the
   monitoring integration this exists for. */
static const char *slugify(const char *s){
    static char out[64];
    size_t n=0; int us=1;
    for(; *s && n+1<sizeof out; s++){
        unsigned char c=(unsigned char)*s;
        if((c>='a'&&c<='z')||(c>='0'&&c<='9')){ out[n++]=(char)c; us=0; }
        else if(c>='A'&&c<='Z'){ out[n++]=(char)(c-'A'+'a'); us=0; }
        else if(!us){ out[n++]='_'; us=1; }
    }
    while(n>0 && out[n-1]=='_') n--;
    out[n]=0;
    return out;
}

#define PLOT_MIN_COLS 60
static void render_plot(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int paused,int hot_on){
    (void)IO; (void)T;
    int raw=term_cols_raw();
    if(raw>0 && raw<PLOT_MIN_COLS){
        /* Better to say why it cannot draw than to present a broken screen. */
        printf("\033[H\033[2J");
        printf("%s\n", g_ascii?"plot view needs at least 60 columns."
                              :"플롯 화면은 최소 60칸이 필요합니다.");
        printf(g_ascii?"  now %d — widen it, or press p.\n"
                      :"  현재 %d칸 — 넓히거나 p 를 누르세요.\n", raw);
        printf(g_ascii?"  [p] dashboard  [q] quit\n":"  [p] 대시보드  [q] 종료\n");
        fflush(stdout);
        return;
    }
    int W=term_w(); if(W>CV_W)W=CV_W;
    if(raw>0 && W>raw) W=raw;
    char t1[160],t2[160];
    cv_init(W,CV_H);
    int y=0;
    /* Four panels (A memory, B fill, C CPU, D drain).  Each = title 1 + chart ch +
       legend 1 + verdict 1 + border 1 = ch+4 rows.  On a short screen ch shrinks (2-8)
       rather than dropping a panel.  Keys 1-4 zoom one panel to full height. */
    int rows=term_h(); if(rows<10) rows=24;
    if(rows>CV_H) rows=CV_H;
    int budget = rows - 1 - (P->pid<0?1:0);   /* No header box: one key row at the foot, with the rulers inside the panels */
    /* Zoom keeps panel 1 (host) on top and gives the rest to the zoomed panel: without
       the host situation, a magnified instance metric cannot be attributed.
       Panel 1 is not itself a zoom target (key 1 restores the four-panel view). */
    int host_ch = g_plot_zoom ? 4 : 0;   /* When zoomed, panel 1 (host) stays on top at a reduced height */
    /* Cause-metric bars go inside the panel box, below the time axis and above the legend.
       Here only their row count is needed, to yield that much chart height. */
    pser_t sb[6]; int nsb = g_plot_zoom ? pl_subbars(g_plot_zoom,sb) : 0;
    /* Zoom gives several rows per series: the cause metrics must grow with the main chart
       to keep the proportion.  About a third of the budget goes to them (2-5 rows each). */
    int sb_per = 1;
    if(nsb){
        /* The main chart can take 40 rows, but braille gives four steps per cell so 16 rows
           are already dense (cv_chart_n clips above 16).  The remaining height goes to
           the cause metrics; otherwise the main chart is left with empty space. */
        int main_cap=16;
        int avail = budget-10-4-main_cap;           /* Excluding the host summary, borders and the main chart's share */
        sb_per = (avail>0) ? avail/nsb : 2;
        /* Braille bars give four vertical steps per cell against a block's eight, so they need
           fewer rows - one less per series, returned to the main chart and margins. */
        sb_per -= 1;
        if(sb_per<1) sb_per=1;
        if(sb_per>5) sb_per=5;
    }
    int sb_rows = nsb ? nsb*sb_per : 0;
    g_sb_per = sb_per;
    /* Four-panel mode keeps permanent bars (hot%, absorption%) below panel 3: as lines
       those values are too small and sit on the floor.  Each panel's chart shrinks to
       make room. */
    pser_t ab[4]; int nab = g_plot_zoom ? 0 : pl_subbars_always(3,ab);
    int ab_rows = nab ? nab : 0;
    int ch = g_plot_zoom ? budget-5-(host_ch+5)-sb_rows : (budget-ab_rows)/4-5;
    if(ch>(g_plot_zoom?16:8)) ch=(g_plot_zoom?16:8);   /* cv_chart_n saturates at 16 rows */
    if(ch<2) ch=2;

    /* The header always shows the window and the sample count, so a short line reads as
       "still collecting" rather than a bug. */
    double win = HN>1 ? HS[HN-1].t-HS[0].t : 0;
    char wtxt[24];
    if(win>=60) snprintf(wtxt,sizeof wtxt,"%dm%02ds",(int)(win/60),(int)win%60);
    else        snprintf(wtxt,sizeof wtxt,"%.0fs",win);
    /* These two strings become panel 1's title and right-hand badge; there is no separate
       header box, so its rows go to the chart. */
    if(g_ascii) snprintf(t1,sizeof t1,"1 host · db=%.32s · window %s (%d samples, %.1fs)",
                         P->db[0]?P->db:"-",wtxt,HN,g_interval);
    else        snprintf(t1,sizeof t1,"1 호스트 · db=%.32s · 관측창 %s (샘플 %d개, 간격 %.1f초)",
                         P->db[0]?P->db:"-",wtxt,HN,g_interval);   /* The pid is on the dashboard */
    if(g_ninst>1) snprintf(t2,sizeof t2,"%d/%d %s · %s  %s",g_cur+1,g_ninst,
                           g_ascii?"inst":"인스턴스",g_ascii?"host-wide":"호스트 전체",
                           HN>0?HS[HN-1].hh:"--:--:--");
    else          snprintf(t2,sizeof t2,"%s  %s",g_ascii?"host-wide":"호스트 전체",
                           HN>0?HS[HN-1].hh:"--:--:--");

    if(P->pid<0){
        cv_put(2,y,g_ascii?"cub_server is not running — no server timeseries":
                           "cub_server 미기동 — 서버 시계열 없음","38;5;203");
        y+=1;
    }
    if(g_plot_zoom){
        pl_panel(1,y,W,host_ch,hot_on,M->cores,t1,t2); y+=host_ch+5;
        pl_panel(g_plot_zoom,y,W,ch,hot_on,M->cores,NULL,NULL); y+=ch+5+sb_rows;
    }
    else for(int k=1;k<=4;k++){
        pl_panel(k,y,W,ch,hot_on,M->cores,k==1?t1:NULL,k==1?t2:NULL);
        y+=ch+5+(k==3?ab_rows:0);   /* The bar rows are inside the panel box */
    }

    if(g_ascii) snprintf(t1,sizeof t1," [p] dashboard  [1-4] %s  [space] %s  [l] ko/en  [h] help  [q] quit",
                         g_plot_zoom?"back to 4 panels":"zoom one panel",paused?"resume":"pause");
    else        snprintf(t1,sizeof t1," [p] 대시보드  [1-4] %s  [space] %s  [l] 한/영  [h] 도움말  [q] 종료",
                         g_plot_zoom?"4패널 복귀":"패널 확대",paused?"재개":"일시정지");
    cv_put(0,y,cutf(t1,W),paused?"38;5;178":"38;5;244"); y+=1;

    CV.h=y;
    { int trows=term_h(); if(trows<8) trows=CV.h;
      printf("\033[?2026h\033[?25l\033[H");
      (void)cv_flush_rows(1,trows);          /* Absolute coordinates, no newline: no scroll flicker */
      if(CV.h<trows) printf("\033[%d;1H\033[J",CV.h+1);
      printf("\033[?2026l"); }
    fflush(stdout);
}

/* Reduce a database name to [A-Za-z0-9_] for the terse key=value stream.
   A db name only has to avoid whitespace, '/' and '\' (check_database_name_local
   in util_common.c), so '=' and '"' are legal in one and would otherwise split a
   value into further keys - json_from_terse turns those into real JSON keys.
   Returns out. */
static const char *terse_slug(const char *db,char *out,size_t n){
    size_t a=0,b=0; int us=0;
    if(!db) db="";
    for(;db[a]&&b+1<n;a++){ char c=db[a];
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')){out[b++]=c;us=0;}
        else if(!us&&b){out[b++]='_';us=1;} }
    while(b&&out[b-1]=='_')b--;
    out[b]=0;
    if(!b) snprintf(out,n,"%s","unknown");
    return out;
}

/* The key an instance is filed under.  Two servers can share a database name (two
   installs during an upgrade), and two names can slug alike; the second and later
   get the pid appended, or their keys would repeat and a JSON reader keep only the
   last.  instance.current uses the same rule, so it always names an existing key. */
static const char *inst_key(int i,char *out,size_t n){
    char a[64],b[64];
    terse_slug(g_inst[i].db,a,sizeof a);
    for(int j=0;j<i;j++){
        terse_slug(g_inst[j].db,b,sizeof b);
        if(!strcmp(a,b)){ snprintf(out,n,"%.40s_%d",a,g_inst[i].pid); return out; }
    }
    snprintf(out,n,"%s",a); return out;
}

/* ---------------- Render: terse ---------------- */
static void render_terse(proc_t *P,mem_t *M,tier_t *T,int with_a,
                         double rb,double wb,double ri,double wi,double absorb,
                         double blkpct,const char *devnm,double devutil,double devr,
                         const char *ts,long tsec,double rchar_bps,
                         double minflt_ps,double majflt_ps,
                         double cpu_pct,int ncpu,int IO_dthr,int IO_nthr){
    double mt=M->total*1024.0, av=M->avail*1024.0;
    printf("ts=\"%s\" ts_epoch=%ld\n",ts,tsec);
    printf("caps.vm_readv=%d caps.rollup=%d caps.proc_io=%d caps.clear_refs=%d\n",
           CAP.vm_readv,CAP.rollup,CAP.proc_io,CAP.clear_refs);
    printf("perm.limited=%d",g_perm_limited);
    if(g_perm_why[0]) printf(" perm.reason=\"%s\"",g_perm_why);
    printf("\n");
    if(P->reg_dropped || g_disk_dropped)
        printf("limits.regions_dropped=%d limits.regions_max=%d limits.disks_dropped=%d limits.disks_max=%d\n",
               P->reg_dropped,MAXREG,g_disk_dropped,MAXDISK);
    /* Hot measurement state: off by default, and released automatically above the resident
       limit even with --hot.  Why hot is empty must be visible on screen and in
       monitoring. */
    printf("hot.enabled=%d hot.limit_gb=%.1f hot.every_s=%.0f",
           g_hot_measured,g_hot_limit_gb,g_hot_every);
    if(g_hot_cost_ms>=0) printf(" hot.last_cost_ms=%.1f",g_hot_cost_ms);
    if(g_hot_off_why[0])  printf(" hot.reason=\"%s\"",g_hot_off_why);
    printf("\n");
    printf("os.ram_total_kb=%lu os.ram_avail_kb=%lu os.cached_kb=%lu "
           "os.swap_total_kb=%lu os.swap_free_kb=%lu\n",
           M->total,M->avail,M->cached,M->swtotal,M->swfree);
    /* Host CPU, the same values as the TUI's OS box.  us/sy/wa are percentages
       averaged over all cores (top's %Cpu(s) convention) and busy_cores converts
       that to cores.  server.cpu_pct uses top's "one core = 100" convention, so
       compare cpu_pct/100 against busy_cores. */
    printf("os.cpu_us_pct=%.1f os.cpu_sy_pct=%.1f os.cpu_wa_pct=%.1f"
           " os.cpu_busy_cores=%.2f os.cores_logical=%d os.cores_physical=%d\n",
           M->cpu_us,M->cpu_sy,M->cpu_wa,M->busy_cores,M->cores,M->pcores);
    printf("os.load1=%.2f os.load5=%.2f os.load15=%.2f\n",
           M->load1,M->load5,M->load15);
    /* Core saturation distribution: busy_cores alone cannot say whether the load is spread
       evenly or a few cores are at 100%.  sat is 95% and above, hi is 50-95%, max is the
       busiest core. */
    printf("os.cores_saturated=%d os.cores_high=%d os.core_max_pct=%.1f\n",
           M->sat_cores,M->hi_cores,M->ncore_pct>0?M->core_pct[0]:0.0);
    /* The sorted per-core distribution, which the core bars draw and which sat/hi counts
       alone cannot reconstruct.  Descending, so replay needs no re-sort. */
    if(M->ncore_pct>0){
        printf("os.core_pct_n=%d os.core_pct=\"",M->ncore_pct);
        for(int i=0;i<M->ncore_pct;i++) printf("%s%.1f",i?",":"",M->core_pct[i]);
        printf("\"\n");
    }
    /* The detected CUBRID version and the adopted offset table - the only mechanical way to
       check support status on a foreign deployment. */
    printf("cubrid.version=\"%s\" heapB.table=\"%s\" heapB.lib=\"%s\"\n",
           g_cver[0]?g_cver:"unknown",
           !B.ok?"-":(B.partial?"probe-runtime":g_bt->tag),
           g_so_path[0]?g_so_path:"-");   /* A map_files path means the running inode was read */
    printf("server.up=%d\n",P->pid<0?0:1);
    /* All instances.  The detailed keys (server, region) cover only the current one, so the
       other databases' totals are available nowhere else.  Keys are namespaced by db. */
    { char cs[64];
      printf("instance.count=%d instance.current=%s\n",g_ninst,
             g_ninst>0?inst_key(g_cur,cs,sizeof cs):"-"); }
    for(int i=0;i<g_ninst;i++){
        char k[64];
        inst_key(i,k,sizeof k);
        printf("instance.%s.pid=%d instance.%s.pss_bytes=%.0f instance.%s.rss_bytes=%.0f"
               " instance.%s.pl_pss_bytes=%.0f instance.%s.pl_count=%d instance.%s.tracked=%d\n",
               k,g_inst[i].pid,k,g_inst[i].pss,k,g_inst[i].rss,
               k,g_inst[i].pl_pss,k,g_inst[i].pl_n,k,i==g_cur?1:0);
    }
    if(P->pid<0){
        for(int i=0;i<g_nprm;i++)
            printf("conf.%s=%lld\n",g_prm[i].name,g_prm[i].val);
        if(devnm[0]) printf("hostdev.name=%s hostdev.util_pct=%.1f\n",devnm,devutil);
        printf("tier.server_pss_bytes=0 tier.master_pss_bytes=%.0f tier.broker_pss_bytes=%.0f"
               " tier.cas_pss_bytes=%.0f tier.cas_count=%d tier.pl_pss_bytes=%.0f tier.pl_count=%d\n",
               T->master,T->broker,T->cas,T->ncas,T->pl,T->npl);
        /* CPU per tier, in cores (the same unit as os.cpu_busy_cores). */
        printf("tier.master_cores=%.2f tier.broker_cores=%.2f"
               " tier.cas_cores=%.2f tier.pl_cores=%.2f tier.server_cores=0.00\n",
               T->cpu_master,T->cpu_broker,T->cpu_cas,T->cpu_pl);
        printf("total.cubrid_pss_bytes=%.0f\n",T->master+T->broker+T->cas+T->pl);
        return;
    }
    { char ds[64]; terse_slug(P->db,ds,sizeof ds);
    printf("server.db=%s server.pid=%d server.mapped_kb=%lu server.rss_kb=%lu "
           "server.pss_kb=%lu server.swap_kb=%lu server.workers=%d"
           " server.minflt_per_s=%.0f server.majflt_per_s=%.0f"
           " server.cpu_pct=%.1f server.host_cores=%d\n",
           ds,P->pid,P->vsize_kb,P->rss_kb,P->pss_kb,P->swap_kb,P->nthreads,
           minflt_ps,majflt_ps,cpu_pct,ncpu); }
    for(int i=0;i<NSR;i++){
        srg_t *r=&SR[i];
        /* Strip spaces and parentheses from keys so a key=value parser does not break */
        char k[24]; { size_t a=0,b=0; int us=0;
            for(;r->name[a]&&b+1<sizeof k;a++){ char c=r->name[a];
                if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')){k[b++]=c;us=0;}
                else if(!us&&b){k[b++]='_';us=1;} }
            while(b&&k[b-1]=='_')b--;
            k[b]=0; }
        printf("region.%s.mapped_bytes=%.0f region.%s.rss_bytes=%.0f",
               k,r->mapped,k,r->rss);
        if(r->cfg>0) printf(" region.%s.cfg_bytes=%lld region.%s.over_pct=%.1f",
                            k,r->cfg,k,r->mapped/(double)r->cfg*100);
        if(r->rss>0) printf(" region.%s.hot_pct=%.1f",k,r->ref/r->rss*100);
        printf("\n");
    }
    printf("heapB.enabled=%d heapB.reason=\"%s\" heapB.total_bytes=%.0f heapB.excluded=%d\n",
           B.ok,B.why,B.total,B.n_bad);
    for(int i=0;i<B.n;i++)
    {   const char *sl=slugify(B.it[i].name);
        char kb[64];
        if(!sl[0]){ snprintf(kb,sizeof kb,"item%d",i); sl=kb; }   /* Safety net for non-ASCII labels */
        if(B.it[i].count_only)
             printf("heapB.%s_count=%ld\n",sl,B.it[i].count);
        else printf("heapB.%s_bytes=%.0f\n",sl,B.it[i].bytes); }
    /* Capacity axis: demand, ceiling, headroom.  Snapshot-based estimate. */
    printf("cap.aggregate_ms=%.1f cap.over_budget=%d cap.budget_ms=%.0f\n",CAP2.ms,CAP2.over_budget,(double)CAP_BUDGET_MS);
    printf("cap.perfmon_counters_running=%d cap.hit_source=\"%s\" cap.reason=\"%s\"\n",
           CAP2.ps_ok,CAP2.hit_src,CAP2.ps_why);
    if(!CAP2.over_budget){
        if(CAP2.hit_pct>=0)       printf("cap.buffer_hit_pct=%.2f\n",CAP2.hit_pct);
        if(CAP2.turnover_pct>=0){ printf("cap.turnover_pct=%.2f cap.miss_pages_per_s=%.0f\n",CAP2.turnover_pct,CAP2.miss_pps);
                                  if(CAP2.readmit_pct>=0) printf("cap.readmit_pct=%.2f\n",CAP2.readmit_pct);
                                  else printf("cap.readmit_available=0\n"); }
        if(CAP2.await_ms>=0)      printf("cap.await_ms=%.2f cap.read_await_ms=%.2f cap.write_await_ms=%.2f\n",
                                         CAP2.await_ms,CAP2.r_await_ms<0?0:CAP2.r_await_ms,CAP2.w_await_ms<0?0:CAP2.w_await_ms);
        if(CAP2.qdepth>=0)        printf("cap.queue_depth=%.2f\n",CAP2.qdepth);
        printf("cap.dev_iops=%.0f cap.proc_iops=%.0f\n",CAP2.dev_iops,CAP2.riops+CAP2.wiops);
        /* I/O profile: ceilings are compared only within this cell (block size x read share) */
        printf("cap.req_kb=%.1f cap.read_ratio=%.2f cap.profile=\"%s/%s\"\n",
               CAP2.req_kb,CAP2.read_ratio,CAP_BS_NAME[CAP2.prof_bs],CAP_RW_NAME[CAP2.prof_rw]);
        { int nc=0; for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++) if(g_cap_mtx[b][r].valid) nc++;
          printf("cap.ceiling_cells=%d\n",nc);
          for(int b=0;b<CAP_BS_N;b++) for(int r=0;r<CAP_RW_N;r++){
              const cap_cell_t*cc=&g_cap_mtx[b][r]; if(!cc->valid) continue;
              printf("cap.cell%d%d.profile=\"%s/%s\" cap.cell%d%d.iops=%.0f cap.cell%d%d.util_pct=%.1f"
                     " cap.cell%d%d.await_ms=%.2f cap.cell%d%d.queue_depth=%.2f cap.cell%d%d.epoch=%lld\n",
                     b,r,CAP_BS_NAME[b],CAP_RW_NAME[r],b,r,cc->iops,b,r,cc->util,
                     b,r,cc->await_ms,b,r,cc->qd,b,r,(long long)cc->when);
          } }
        if(CAP2.ceil_valid)       printf("cap.ceiling_iops=%.0f cap.ceiling_bps=%.0f cap.ceiling_util_pct=%.1f"
                                         " cap.ceiling_qdepth=%.2f cap.ceiling_epoch=%lld\n",
                                         CAP2.ceil_iops,CAP2.ceil_bps,CAP2.ceil_util,CAP2.ceil_qd,(long long)CAP2.ceil_when);
        if(CAP2.headroom_pct>=0)  printf("cap.used_of_ceiling_pct=%.1f\n",CAP2.headroom_pct);
        if(CAP2.ps_ok)            printf("cap.pb_fetch_per_s=%.0f cap.pb_ioread_per_s=%.0f cap.pb_iowrite_per_s=%.0f"
                                         " cap.log_iowrite_per_s=%.0f cap.commit_per_s=%.0f\n",
                                         CAP2.pb_fetch_ps,CAP2.pb_ioread_ps,CAP2.pb_iowrite_ps,
                                         CAP2.log_iowrite_ps,CAP2.commit_ps);
    }
    { char vb[320]; const char*vc=VD_INFO; vd_capacity(vb,sizeof vb,&vc);
      printf("cap.verdict=\"%s\"\n",vb); }
    /* BCB direct read, graded as an estimate (a latch-free snapshot).  The keys share their source (the dump) with volmap --bufmap. */
    printf("pgbuf.enabled=%d pgbuf.reason=\"%s\"\n",PG.ok,PG.why);
    if(PG.ok){
        printf("pgbuf.num_buffers=%d pgbuf.resident=%d pgbuf.dirty=%d pgbuf.flushing=%d"
               " pgbuf.zone1_hot=%d pgbuf.zone2_warm=%d pgbuf.zone3_cold=%d pgbuf.zone_void=%d pgbuf.zone_free=%d"
               " pgbuf.hdr_mismatch=%d pgbuf.hdr_unread=%d pgbuf.scan_ms=%.1f\n",
               PG.nbuf,PG.resident,PG.dirty,PG.flushing,PG.z1,PG.z2,PG.z3,PG.zvoid,PG.zinv,PG.mismatch,PG.unread,PG.ms);
        printf("pgbuf.hdr_mismatch_raw=%d pgbuf.rechecked=%d pgbuf.degraded=%d\n",
               PG.mismatch_raw,PG.recheck,PG.degraded);
        for(int v=0;v<PG_MAXVOL;v++) if(PG.vol_res[v]>0)
            printf("pgbuf.vol%d.resident=%d pgbuf.vol%d.dirty=%d\n",v,PG.vol_res[v],v,PG.vol_dirty[v]);
        if(PG.have_oldest) printf("pgbuf.oldest_dirty_pageid=%lld pgbuf.oldest_dirty_offset=%d\n",
                                  (long long)PG.oldest_dirty.pageid,PG.oldest_dirty.offset);
        if(PG.have_log){
            printf("log.append_pageid=%lld log.append_offset=%d log.nxio_pageid=%lld log.nxio_offset=%d"
                   " log.eof_pageid=%lld log.eof_offset=%d log.flush_lag_pages=%lld",
                   (long long)PG.log_append.pageid,PG.log_append.offset,(long long)PG.log_nxio.pageid,PG.log_nxio.offset,
                   (long long)PG.log_eof.pageid,PG.log_eof.offset,(long long)(PG.log_append.pageid-PG.log_nxio.pageid));
            if(PG.have_oldest) printf(" log.dirty_span_pages=%lld",(long long)(PG.log_append.pageid-PG.oldest_dirty.pageid));
            printf("\n");
        }
    }
    if(with_a){
        printf("heapA.chunks=%ld heapA.total_bytes=%.0f\n",A.chunks,A.total);
        if(A.trunc) printf("heapA.truncated_regions=%ld heapA.unwalked_bytes=%.0f\n",A.trunc,A.unwalked);
        for(int c=0;c<7;c++)
            if(A.cat[c]>0) printf("heapA.%s_bytes=%.0f\n",slugify(ACAT[c]),A.cat[c]);
    }
    printf("io.read_bps=%.0f io.write_bps=%.0f io.read_iops=%.0f io.write_iops=%.0f ",
           rb,wb,ri,wi);
    /* Below the rchar floor the ratio is meaningless, so the key is omitted entirely rather
       than a non-numeric "NA" placed in a numeric field. */
    if(absorb>=0 && rchar_bps>=RCHAR_MIN) printf("io.cache_absorb_pct=%.1f ",absorb*100);
    /* absorb_ok states WHY the ratio is absent, so a replay does not read a missing key as
       "measured 0"; rchar is its denominator. */
    printf("io.cache_absorb_ok=%d io.rchar_bps=%.0f ",
           (absorb>=0 && rchar_bps>=RCHAR_MIN)?1:0, rchar_bps);
    printf("io.blkio_wait_pct=%.2f io.total_read_bytes=%lu io.total_write_bytes=%lu\n",
           blkpct,P->read_b,P->write_b);
    /* D (disk wait) threads: wa is diluted by the core count, this is not.
       The same value that appears right of the iowait row on screen. */
    printf("server.d_threads=%d server.threads_total=%d\n",IO_dthr,IO_nthr);
    /* Why paramdump could not start - the only clue to why the parameter keys are empty. */
    if(g_pd_why[0]) printf("param.skipped_reason=\"%s\"\n",g_pd_why);
    /* io.* comes from /proc/<pid>/io and belongs to the current instance, while the device
       figures come from diskstats and are host-wide - separate key prefixes keep the
       two scopes from being confused. */
    if(devnm[0]) printf("hostdev.name=%s hostdev.util_pct=%.1f hostdev.read_bps=%.0f\n",
                        devnm,devutil,devr);
    for(int i=0;i<NPRMROWS;i++){
        long long v=prm_get(PRMROWS[i].name);
        if(v>=0) printf("param.%s=%lld param.%s.observed=%d\n",
                        PRMROWS[i].name,v,PRMROWS[i].name,PRMROWS[i].obs);
    }
    /* A total alone cannot say which tier grew, so PSS is emitted per tier */
    printf("tier.server_pss_bytes=%.0f tier.master_pss_bytes=%.0f tier.broker_pss_bytes=%.0f"
           " tier.cas_pss_bytes=%.0f tier.cas_count=%d tier.pl_pss_bytes=%.0f tier.pl_count=%d\n",
           P->pss_kb*1024.0,T->master,T->broker,T->cas,T->ncas,T->pl,T->npl);
    /* CPU per tier, in cores (the same unit as os.cpu_busy_cores).
       server is server.cpu_pct (one core = 100) divided by 100 to match. */
    printf("tier.server_cores=%.2f tier.master_cores=%.2f tier.broker_cores=%.2f"
           " tier.cas_cores=%.2f tier.pl_cores=%.2f\n",
           cpu_pct/100.0,T->cpu_master,T->cpu_broker,T->cpu_cas,T->cpu_pl);
    printf("total.cubrid_pss_bytes=%.0f total.cubrid_pct_ram=%.2f\n",
           P->pss_kb*1024.0+T->master+T->broker+T->cas+T->pl,
           (P->pss_kb*1024.0+T->master+T->broker+T->cas+T->pl)/(mt>0?mt:1)*100);
    (void)av;
}

/* ---- JSON output (--json) ----
 * terse (key=value) is the single source: producing the same content twice would
 * drift, so terse output is mechanically converted, keeping keys, values and order
 * identical to -t.  Quoted values become strings, numerals become numbers, and any
 * other bare token (a device name) becomes a string. */
static void json_escape_out(FILE*o,const char*s,size_t n){
    for(size_t i=0;i<n;i++){ unsigned char c=(unsigned char)s[i];
        if(c=='"'||c=='\\') fprintf(o,"\\%c",c);
        else if(c<0x20) fprintf(o,"\\u%04x",c);
        else fputc(c,o); }
}
static void render_terse(proc_t*,mem_t*,tier_t*,int,double,double,double,double,double,
                         double,const char*,double,double,const char*,long,double,
                         double,double,double,int,int,int);
static void json_from_terse(FILE*,FILE*);
static void json_line_from_terse(FILE*,FILE*);

/* ---- Record and replay ----
 * A recording is the terse/JSON stream itself, one object per line (JSONL): the
 * same keys -t and --json already emit, so nothing is serialized twice and a
 * recording stays readable with jq.
 *
 * Replay reconstructs the structs the renderers take and calls the same
 * render_dash()/render_plot(), so what is drawn is what the recorder saw.  No
 * /proc is read: rp_apply() below is the only place a frame is rebuilt.
 */
#define RP_MAXLINE (1<<20)

typedef struct { char *buf; size_t cap; FILE *f; int n; } rp_rec_t;
static rp_rec_t RPR={0,0,NULL,0};

/* Look up one key in a recorded frame (a JSON object on one line). */
static int rp_find(const char *txt,const char *key,char *out,size_t on){
    /* A recorded frame is one JSON object: "key": value.  The key is matched with its
       quotes so "os.load1" cannot match "os.load15", and the value is taken whether it
       is quoted or bare. */
    char pat[128];
    int pl=snprintf(pat,sizeof pat,"\"%s\":",key);
    if(pl<=0 || (size_t)pl>=sizeof pat) return 0;
    const char *p=strstr(txt,pat);
    if(!p) return 0;
    const char *v=p+pl;
    while(*v==' '||*v=='\t') v++;
    size_t vl;
    if(*v=='"'){
        v++; const char *e=v;
        while(*e && *e!='"'){ if(*e=='\\' && e[1]) e++; e++; }
        vl=(size_t)(e-v);
    } else {
        const char *e=v;
        while(*e && *e!=',' && *e!='}' && *e!='\n' && *e!='\r') e++;
        while(e>v && (e[-1]==' '||e[-1]=='\t')) e--;
        vl=(size_t)(e-v);
    }
    if(vl>=on) vl=on-1;
    memcpy(out,v,vl); out[vl]=0; return 1;
}
static double rp_num(const char *txt,const char *key,double dflt){
    char b[128]; if(!rp_find(txt,key,b,sizeof b)) return dflt;
    char *end; double d=strtod(b,&end); return (end==b)?dflt:d;
}
static void rp_str(const char *txt,const char *key,char *out,size_t on){
    if(!rp_find(txt,key,out,on)) out[0]=0;
}

/* Rebuild one frame's structs from a recorded line.  This is the ONLY place a
   replayed frame comes from - no /proc is touched, so a replay on another host
   shows exactly what the recorder saw. */
/* Append one frame to the recording.  The frame is produced by render_terse() and
   converted by json_from_terse(), the same pair -t and --json use, so a recording
   cannot drift from those outputs. */
static void rp_record(proc_t*P,mem_t*M,tier_t*T,iod_t*IO,int with_a,const char*ts,long tsec){
    if(!RPR.f){
        RPR.f=fopen(g_rec_path,"we");
        if(!RPR.f){ fprintf(stderr,"cub_top: --record %s: %s\n",g_rec_path,strerror(errno));
                    g_rec_path=NULL; return; }
    }
    FILE *tf=tmpfile(); if(!tf) return;
    fflush(stdout);
    int save=dup(1); if(save<0){ fclose(tf); return; }
    if(dup2(fileno(tf),1)<0){ close(save); fclose(tf); return; }
    render_terse(P,M,T,with_a,IO->rb,IO->wb,IO->ri,IO->wi,
                 IO->absorb_ok?IO->absorb:-1.0,IO->blkpct,IO->devnm,IO->devutil,IO->devr,
                 ts,tsec,IO->rchar,IO->minflt,IO->majflt,IO->cpu_pct,IO->ncpu,
                 IO->d_thr,IO->n_thr);
    fflush(stdout); dup2(save,1); close(save);
    /* One object per line: json_from_terse writes it multi-line, so newlines are folded. */
    FILE *lf=tmpfile();
    if(lf){
        json_from_terse(tf,lf);
        long n=ftell(lf); fseek(lf,0,SEEK_SET);
        char *b=(n>0)?malloc((size_t)n+1):NULL;   /* ftell -1 would make this malloc(0) */
        if(b){ size_t rd=fread(b,1,(size_t)n,lf); b[rd]=0;
               for(size_t i=0;i<rd;i++) if(b[i]=='\n') b[i]=' ';
               fprintf(RPR.f,"%s\n",b); fflush(RPR.f); RPR.n++; free(b); }
        fclose(lf);
    }
    fclose(tf);
}

static void rp_apply(const char *t,proc_t*P,mem_t*M,tier_t*T,iod_t*IO){
    char b[512];
    memset(P,0,sizeof *P); memset(M,0,sizeof *M);
    memset(T,0,sizeof *T); memset(IO,0,sizeof *IO);

    M->total  =rp_num(t,"os.ram_total_kb",0);   M->avail =rp_num(t,"os.ram_avail_kb",0);
    M->cached =rp_num(t,"os.cached_kb",0);
    M->swtotal=rp_num(t,"os.swap_total_kb",0);  M->swfree=rp_num(t,"os.swap_free_kb",0);
    M->cores  =(int)rp_num(t,"os.cores_logical",0);
    M->pcores =(int)rp_num(t,"os.cores_physical",0);
    M->busy_cores=rp_num(t,"os.cpu_busy_cores",0);
    M->sat_cores =(int)rp_num(t,"os.cores_saturated",0);
    M->hi_cores  =(int)rp_num(t,"os.cores_high",0);
    M->cpu_us=rp_num(t,"os.cpu_us_pct",0); M->cpu_sy=rp_num(t,"os.cpu_sy_pct",0);
    M->cpu_wa=rp_num(t,"os.cpu_wa_pct",0);
    M->load1 =rp_num(t,"os.load1",0); M->load5=rp_num(t,"os.load5",0);
    M->load15=rp_num(t,"os.load15",0);
    /* Per-core distribution, recorded already sorted descending. */
    M->ncore_pct=0;
    if(rp_find(t,"os.core_pct",b,sizeof b)){
        char *q=b; int i=0;
        while(*q && i<MAXCORE){ M->core_pct[i++]=strtod(q,&q); if(*q==',') q++; else break; }
        M->ncore_pct=i;
    }

    P->pid     =(int)rp_num(t,"server.pid",-1);
    P->rss_kb  =(unsigned long)rp_num(t,"server.rss_kb",0);
    P->pss_kb  =(unsigned long)rp_num(t,"server.pss_kb",0);
    P->vsize_kb=(unsigned long)rp_num(t,"server.mapped_kb",0);
    P->swap_kb =(unsigned long)rp_num(t,"server.swap_kb",0);
    P->nthreads=(int)rp_num(t,"server.threads_total",0);
    P->read_b  =(unsigned long)rp_num(t,"io.total_read_bytes",0);
    P->write_b =(unsigned long)rp_num(t,"io.total_write_bytes",0);
    rp_str(t,"server.db",P->db,sizeof P->db);

    IO->rb=rp_num(t,"io.read_bps",0);  IO->wb=rp_num(t,"io.write_bps",0);
    IO->ri=rp_num(t,"io.read_iops",0); IO->wi=rp_num(t,"io.write_iops",0);
    IO->blkpct=rp_num(t,"io.blkio_wait_pct",0);
    IO->rchar =rp_num(t,"io.rchar_bps",0);
    IO->absorb_ok=(int)rp_num(t,"io.cache_absorb_ok",0);
    IO->absorb=IO->absorb_ok?rp_num(t,"io.cache_absorb_pct",0)/100.0:-1.0;
    IO->devutil=rp_num(t,"hostdev.util_pct",0);
    IO->devr   =rp_num(t,"hostdev.read_bps",0);
    rp_str(t,"hostdev.name",IO->devnm,sizeof IO->devnm);
    IO->minflt=rp_num(t,"server.minflt_per_s",0);
    IO->majflt=rp_num(t,"server.majflt_per_s",0);
    IO->cpu_pct=rp_num(t,"server.cpu_pct",0);
    IO->ncpu  =(int)rp_num(t,"server.host_cores",0);
    IO->d_thr =(int)rp_num(t,"server.d_threads",0);
    IO->n_thr =(int)rp_num(t,"server.threads_total",0);

    T->server_rss=rp_num(t,"tier.server_pss_bytes",0);
    T->master=rp_num(t,"tier.master_pss_bytes",0);
    T->broker=rp_num(t,"tier.broker_pss_bytes",0);
    T->cas   =rp_num(t,"tier.cas_pss_bytes",0);
    T->pl    =rp_num(t,"tier.pl_pss_bytes",0);
    T->ncas  =(int)rp_num(t,"tier.cas_count",0);
    T->npl   =(int)rp_num(t,"tier.pl_count",0);
    T->cpu_master=rp_num(t,"tier.master_cores",0);
    T->cpu_broker=rp_num(t,"tier.broker_cores",0); T->cpu_cas=rp_num(t,"tier.cas_cores",0);
    T->cpu_pl    =rp_num(t,"tier.pl_cores",0);

    /* Regions: the JSON slugifies names ("thread stacks" -> thread_stacks), so the
       map back is explicit rather than guessed. */
    static const struct { const char *slug, *name; char grade; } RGN[]={
        {"data_buffer","data_buffer",'M'}, {"log_buffer","log_buffer",'M'},
        {"dynamic_heap","dynamic-heap",'U'},{"thread_stacks","thread stacks",'R'},
        {"code_so","code (.so)",'M'},      {"bin_shm_etc","bin/shm etc",'M'},
        {"glibc_arena","glibc arena",'R'},
    };
    /* SR[] is rebuilt every frame, but the grow column's baseline lives in it
       (render_dash writes prev_* after drawing).  Rebuilding blindly would zero that
       baseline and every delta would read "no change".  The old values are therefore
       carried over BY NAME - by index would misalign the moment a region appears or
       disappears between frames. */
    srg_t old[8]; int nold=NSR;
    memcpy(old,SR,sizeof old);
    NSR=0;
    for(unsigned i=0;i<sizeof RGN/sizeof RGN[0] && NSR<8;i++){
        char k[96]; snprintf(k,sizeof k,"region.%s.mapped_bytes",RGN[i].slug);
        if(!rp_find(t,k,b,sizeof b)) continue;
        srg_t*x=&SR[NSR++];
        snprintf(x->name,sizeof x->name,"%s",RGN[i].name);
        x->mapped=strtod(b,NULL);
        x->prev_rss=x->prev_ref=x->prev_mapped=0;
        for(int j=0;j<nold;j++)
            if(!strcmp(old[j].name,x->name)){
                x->prev_rss=old[j].prev_rss; x->prev_ref=old[j].prev_ref;
                x->prev_mapped=old[j].prev_mapped; break;
            }
        snprintf(k,sizeof k,"region.%s.rss_bytes",RGN[i].slug);   x->rss=rp_num(t,k,0);
        snprintf(k,sizeof k,"region.%s.hot_pct",RGN[i].slug);
        x->ref=x->rss*rp_num(t,k,0)/100.0;
        snprintf(k,sizeof k,"region.%s.cfg_bytes",RGN[i].slug);   x->cfg=(long long)rp_num(t,k,-1);
        x->grade=RGN[i].grade;
    }
    g_dyn_rss   =rp_num(t,"region.dynamic_heap.rss_bytes",0);
    g_dyn_mapped=rp_num(t,"region.dynamic_heap.mapped_bytes",0);

    /* Buffer pool and capacity: the plot panels and verdicts read these. */
    PG.ok=(int)rp_num(t,"pgbuf.enabled",0);
    rp_str(t,"pgbuf.reason",PG.why,sizeof PG.why);     /* shown on screen beside the state */
    PG.nbuf=(int)rp_num(t,"pgbuf.num_buffers",0);
    PG.resident=(int)rp_num(t,"pgbuf.resident",0);
    PG.dirty=(int)rp_num(t,"pgbuf.dirty",0);
    PG.z1=(int)rp_num(t,"pgbuf.zone1_hot",0); PG.z2=(int)rp_num(t,"pgbuf.zone2_warm",0);
    PG.z3=(int)rp_num(t,"pgbuf.zone3_cold",0);
    PG.flushing=(int)rp_num(t,"pgbuf.flushing",0);
    PG.zvoid=(int)rp_num(t,"pgbuf.zone_void",0);
    PG.mismatch=(int)rp_num(t,"pgbuf.hdr_mismatch",0);
    PG.unread=(int)rp_num(t,"pgbuf.hdr_unread",0);
    PG.ms=rp_num(t,"pgbuf.scan_ms",0);
    PG.zinv=(int)rp_num(t,"pgbuf.zone_free",0);   /* emitted as zone_free */
    PG.mismatch_raw=(int)rp_num(t,"pgbuf.hdr_mismatch_raw",0);
    PG.recheck=(int)rp_num(t,"pgbuf.rechecked",0);
    PG.degraded=(int)rp_num(t,"pgbuf.degraded",0);
    /* The LSA axis.  Both are emitted only when present, so presence is the flag; a
       frame without them must not inherit the previous frame's values. */
    PG.have_oldest=rp_find(t,"pgbuf.oldest_dirty_pageid",b,sizeof b);
    if(PG.have_oldest){
        PG.oldest_dirty.pageid=(int64_t)strtoll(b,NULL,10);
        PG.oldest_dirty.offset=(int)rp_num(t,"pgbuf.oldest_dirty_offset",0);
    }
    PG.have_log=rp_find(t,"log.append_pageid",b,sizeof b);
    if(PG.have_log){
        PG.log_append.pageid=(int64_t)strtoll(b,NULL,10);
        PG.log_append.offset=(int)rp_num(t,"log.append_offset",0);
        PG.log_nxio.pageid=(int64_t)rp_num(t,"log.nxio_pageid",0);
        PG.log_nxio.offset=(int)rp_num(t,"log.nxio_offset",0);
        PG.log_eof.pageid=(int64_t)rp_num(t,"log.eof_pageid",0);
        PG.log_eof.offset=(int)rp_num(t,"log.eof_offset",0);
    }
    /* Per-volume residency: only volumes that had pages are emitted, so the array is
       cleared first and filled from whichever keys exist. */
    memset(PG.vol_res,0,sizeof PG.vol_res); memset(PG.vol_dirty,0,sizeof PG.vol_dirty);
    for(int v=0;v<PG_MAXVOL;v++){
        char k[48]; snprintf(k,sizeof k,"pgbuf.vol%d.resident",v);
        if(!rp_find(t,k,b,sizeof b)) continue;
        PG.vol_res[v]=(int)strtod(b,NULL);
        snprintf(k,sizeof k,"pgbuf.vol%d.dirty",v); PG.vol_dirty[v]=(int)rp_num(t,k,0);
    }
    B.ok=(int)rp_num(t,"heapB.enabled",0);
    rp_str(t,"heapB.reason",B.why,sizeof B.why);
    B.total=rp_num(t,"heapB.total_bytes",0);
    B.n_bad=(int)rp_num(t,"heapB.excluded",0);
    /* Heap detail B items.  The keys are slugified screen labels, so the map back is
       explicit; without it the box draws an empty table and "B total 0B". */
    { static const struct { const char *slug, *name; } BIT[]={
        {"plan_cache_xcache","plan cache (xcache)"},
        {"result_cache_query","result cache (query)"},
        {"filter_pred_cache","filter pred cache"},
        {"lock_table_tran","lock table (tran)"},
        {"session_table","session table"},
        {"catalog_cache","catalog cache"},
        {"conn_entries_css","conn entries (css)"},
        {"vacuum_data","vacuum data"},
      };
      B.n=0;
      for(unsigned i=0;i<sizeof BIT/sizeof BIT[0] && B.n<16;i++){
          char k[80]; snprintf(k,sizeof k,"heapB.%s_bytes",BIT[i].slug);
          int cnt_only=0;
          if(!rp_find(t,k,b,sizeof b)){
              snprintf(k,sizeof k,"heapB.%s_count",BIT[i].slug);
              if(!rp_find(t,k,b,sizeof b)) continue;
              cnt_only=1;
          }
          bitem_t*x=&B.it[B.n++];
          memset(x,0,sizeof *x);
          snprintf(x->name,sizeof x->name,"%s",BIT[i].name);
          if(cnt_only){ x->count=(long)strtod(b,NULL); x->count_only=1; x->grade='?'; }
          else        { x->bytes=strtod(b,NULL); x->grade='M'; }
      } }
    /* Capacity axis.  Without this the verdict is recomputed from zeros and states the
       OPPOSITE of the recording ("no headroom problem" for a frame that was at its
       ceiling), so every field the verdict reads is restored. */
    memset(&CAP2,0,sizeof CAP2);
    CAP2.read_bps =rp_num(t,"io.read_bps",0);   CAP2.write_bps=rp_num(t,"io.write_bps",0);
    CAP2.riops    =rp_num(t,"io.read_iops",0);  CAP2.wiops    =rp_num(t,"io.write_iops",0);
    CAP2.dev_iops =rp_num(t,"cap.dev_iops",0);  CAP2.dev_util =rp_num(t,"hostdev.util_pct",0);
    CAP2.await_ms =rp_num(t,"cap.await_ms",0);
    CAP2.r_await_ms=rp_num(t,"cap.read_await_ms",0);
    CAP2.w_await_ms=rp_num(t,"cap.write_await_ms",0);
    CAP2.qdepth   =rp_num(t,"cap.queue_depth",0);
    CAP2.miss_pps =rp_num(t,"cap.miss_pages_per_s",0);
    CAP2.hit_pct  =rp_num(t,"cap.buffer_hit_pct",-1);
    CAP2.turnover_pct=rp_num(t,"cap.turnover_pct",0);
    CAP2.readmit_pct =rp_num(t,"cap.readmit_pct",0);
    CAP2.ceil_iops=rp_num(t,"cap.ceiling_iops",0);
    CAP2.ceil_bps =rp_num(t,"cap.ceiling_bps",0);
    CAP2.ceil_util=rp_num(t,"cap.ceiling_util_pct",0);
    CAP2.ceil_qd  =rp_num(t,"cap.ceiling_qdepth",0);
    CAP2.ceil_when=(time_t)rp_num(t,"cap.ceiling_epoch",0);
    CAP2.ceil_valid=CAP2.ceil_iops>0;
    CAP2.headroom_pct=rp_num(t,"cap.used_of_ceiling_pct",-1);
    CAP2.req_kb   =rp_num(t,"cap.req_kb",0);
    CAP2.read_ratio=rp_num(t,"cap.read_ratio",0);
    /* The profile cell is derived from the restored figures, not guessed - otherwise the
       verdict names the wrong cell (">64KB" recorded, "<8KB" replayed). */
    CAP2.prof_bs=cap_bs_idx(CAP2.req_kb); CAP2.prof_rw=cap_rw_idx(CAP2.read_ratio);
    CAP2.ps_ok    =(int)rp_num(t,"cap.perfmon_counters_running",0);
    CAP2.ms       =rp_num(t,"cap.aggregate_ms",0);
    CAP2.over_budget=(int)rp_num(t,"cap.over_budget",0);
    rp_str(t,"cap.hit_source",CAP2.hit_src,sizeof CAP2.hit_src);
    rp_str(t,"cap.reason",CAP2.ps_why,sizeof CAP2.ps_why);
    /* The remembered ceilings are per profile cell, so the whole matrix is restored -
       a verdict compares against the cell in force, not a single number. */
    memset(g_cap_mtx,0,sizeof g_cap_mtx);
    for(int bs=0;bs<CAP_BS_N;bs++) for(int rw=0;rw<CAP_RW_N;rw++){
        char k[64]; snprintf(k,sizeof k,"cap.cell%d%d.iops",bs,rw);
        if(!rp_find(t,k,b,sizeof b)) continue;
        cap_cell_t*cc=&g_cap_mtx[bs][rw];
        cc->iops=strtod(b,NULL); cc->valid=1;
        snprintf(k,sizeof k,"cap.cell%d%d.util_pct",bs,rw);    cc->util=rp_num(t,k,0);
        snprintf(k,sizeof k,"cap.cell%d%d.await_ms",bs,rw);    cc->await_ms=rp_num(t,k,0);
        snprintf(k,sizeof k,"cap.cell%d%d.queue_depth",bs,rw); cc->qd=rp_num(t,k,0);
        snprintf(k,sizeof k,"cap.cell%d%d.epoch",bs,rw);       cc->when=(time_t)rp_num(t,k,0);
    }
    /* Version and instance identity belong to the recording, not to this host. */
    rp_str(t,"cubrid.version",g_cver,sizeof g_cver);
    if(!strcmp(g_cver,"unknown")) g_cver[0]=0;
    g_ninst=1;
    snprintf(g_inst[0].db,sizeof g_inst[0].db,"%s",P->db);
    g_inst[0].pid=P->pid;
    g_inst[0].pss=rp_num(t,"tier.server_pss_bytes",0)+rp_num(t,"tier.pl_pss_bytes",0);
    g_cur=0;
    /* Method A: present when the recording was taken with it (live always is).  Its
       absence leaves A empty, which the A box shows as "not measured". */
    memset(&A,0,sizeof A);
    if(rp_find(t,"heapA.chunks",b,sizeof b)){
        A.chunks=(long)strtod(b,NULL);
        A.total=rp_num(t,"heapA.total_bytes",0);
        for(int c=0;c<7;c++){ char k[96]; snprintf(k,sizeof k,"heapA.%s_bytes",slugify(ACAT[c])); A.cat[c]=rp_num(t,k,0); }
        A.trunc=(long)rp_num(t,"heapA.truncated_regions",0);
        A.unwalked=rp_num(t,"heapA.unwalked_bytes",0);
    }
    /* The recording host's capabilities, or the replay would warn of fallbacks it never had */
    CAP.vm_readv=(int)rp_num(t,"caps.vm_readv",0);  CAP.rollup=(int)rp_num(t,"caps.rollup",0);
    CAP.proc_io=(int)rp_num(t,"caps.proc_io",0);    CAP.clear_refs=(int)rp_num(t,"caps.clear_refs",0);
    cap_note();
    P->reg_dropped=(int)rp_num(t,"limits.regions_dropped",0);
    g_disk_dropped=(int)rp_num(t,"limits.disks_dropped",0);
    /* Parameters: the verdicts compare usage against these, so a replay without them
       would judge against nothing.  Only the rows terse emits are looked for. */
    g_nprm=0;
    for(int i=0;i<NPRMROWS && g_nprm<MAXPARAM;i++){
        char k[96]; snprintf(k,sizeof k,"param.%s",PRMROWS[i].name);
        if(!rp_find(t,k,b,sizeof b)) continue;
        snprintf(g_prm[g_nprm].name,sizeof g_prm[g_nprm].name,"%s",PRMROWS[i].name);
        g_prm[g_nprm].val=strtoll(b,NULL,10); g_nprm++;
    }
    /* Instances: rebuilt from every "instance.<key>.pid".  The key becomes the name, so
       re-emitting the frame produces the same keys. */
    g_ninst=0; g_cur=0;
    for(const char *q=t; (q=strstr(q,"\"instance."))!=NULL && g_ninst<MAXINST; q++){
        const char *ks=q+10, *ke=strstr(ks,".pid\":");
        const char *qe=strchr(ks,'"');
        if(!ke || !qe || ke>qe) continue;            /* not a .pid key */
        inst_t *x=&g_inst[g_ninst]; memset(x,0,sizeof *x);
        snprintf(x->db,sizeof x->db,"%.*s",(int)(ke-ks),ks);
        char k[128];
        snprintf(k,sizeof k,"instance.%s.pid",x->db);          x->pid=(int)rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.pss_bytes",x->db);    x->pss=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.rss_bytes",x->db);    x->rss=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.pl_pss_bytes",x->db); x->pl_pss=rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.pl_count",x->db);     x->pl_n=(int)rp_num(t,k,0);
        snprintf(k,sizeof k,"instance.%s.tracked",x->db);      if(rp_num(t,k,0)>0) g_cur=g_ninst;
        g_ninst++;
    }
}

/* Replay loop.  Reads the recording, rebuilds each frame with rp_apply() and calls
   the same renderers the live loop calls, so nothing about the drawing differs.
   Keys are the live ones; [space] pauses, [,] and [.] step while paused. */
static int rp_run(const char *path,int view0,int no_hot,int terse,int with_a,int jsonout){
    FILE *f=fopen(path,"re");
    if(!f){ fprintf(stderr,"cub_top: --replay %s: %s\n",path,strerror(errno)); return 1; }
    char *line=malloc(RP_MAXLINE);
    if(!line){ fclose(f); return 1; }
    /* The whole recording is held so [,] can step backwards. */
    size_t cap=256,n=0; char **fr=malloc(cap*sizeof *fr);
    if(!fr){ free(line); fclose(f); return 1; }
    while(fgets(line,RP_MAXLINE,f)){
        if(line[0]!='{') continue;
        if(n==cap){ cap*=2; char **t=realloc(fr,cap*sizeof *fr); if(!t) break; fr=t; }
        fr[n++]=strdup(line);
    }
    fclose(f); free(line);
    if(!n){ fprintf(stderr,"cub_top: --replay %s: no frames\n",path); free(fr); return 1; }

    static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
    /* -t / --json on a replay re-emit each frame instead of drawing it, so a recording
       can be post-processed or compared without a terminal. */
    if(terse||jsonout){
        for(size_t j=0;j<n;j++){
            char ts[32]; time_t rt=(time_t)rp_num(fr[j],"ts_epoch",0); struct tm tmv;
            localtime_r(&rt,&tmv); strftime(ts,sizeof ts,"%Y-%m-%d %H:%M:%S",&tmv);
            rp_apply(fr[j],&P,&M,&T,&IO);
            g_replaying=1;
            if(jsonout){
                FILE *tf=tmpfile(); if(!tf) continue;
                fflush(stdout); int sv=dup(1);
                if(sv<0||dup2(fileno(tf),1)<0){ if(sv>=0) close(sv); fclose(tf); continue; }
                render_terse(&P,&M,&T,with_a||A.chunks>0,IO.rb,IO.wb,IO.ri,IO.wi,
                             IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,
                             ts,(long)rt,IO.rchar,IO.minflt,IO.majflt,IO.cpu_pct,IO.ncpu,
                             IO.d_thr,IO.n_thr);
                fflush(stdout); dup2(sv,1); close(sv);
                json_line_from_terse(tf,stdout); fclose(tf);
            } else {
                render_terse(&P,&M,&T,with_a||A.chunks>0,IO.rb,IO.wb,IO.ri,IO.wi,
                             IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,
                             ts,(long)rt,IO.rchar,IO.minflt,IO.majflt,IO.cpu_pct,IO.ncpu,
                             IO.d_thr,IO.n_thr);
            }
        }
        for(size_t j=0;j<n;j++) free(fr[j]);
        free(fr);
        return 0;
    }
    int view=view0, paused=0, lens=1, rebuild=0; size_t i=0;
    size_t pushed=(size_t)-1;      /* the last frame added to the history */
    double t0=rp_num(fr[0],"ts_epoch",0);
    tui_enter();
    while(!g_stop && i<n){
        if(rebuild){
            /* Stepping back: the plots must show the window as it stood at frame i, so
               the history is rebuilt from the frames before it (at most one window). */
            HN=0;
            for(size_t j = i>(size_t)HCAP ? i-(size_t)HCAP : 0; j<i; j++){
                rp_apply(fr[j],&P,&M,&T,&IO);
                hist_push(rp_num(fr[j],"ts_epoch",0),&P,&M,&T,&IO,!no_hot);
            }
            rebuild=0; pushed=(size_t)-1;
        }
        rp_apply(fr[i],&P,&M,&T,&IO);
        /* The history the plots draw is rebuilt frame by frame, so a replay shows the
           window growing exactly as it did live.  Only a new frame is added: while
           paused the loop redraws the same frame every 80ms, and pushing it each time
           would scroll the plots with copies (live mode guards the same way). */
        if(pushed!=i){ hist_push(rp_num(fr[i],"ts_epoch",0),&P,&M,&T,&IO,!no_hot); pushed=i; }
        g_replaying=1; g_rep_i=(int)i; g_rep_n=(int)n;
        g_rep_ts=(time_t)rp_num(fr[i],"ts_epoch",0);
        if(view) render_plot(&P,&M,&T,&IO,paused,!no_hot);
        else     render_dash(&P,&M,&T,&IO,lens,paused,0,-1,A.chunks>0);
        int k, step=0;
        while((k=tui_key())>=0){
            if(k=='q'){ g_stop=1; break; }
            else if(k==' ') paused=!paused;
            else if(k=='p'||k=='g') view=!view;
            else if(k=='l') g_ascii=!g_ascii;
            else if(k>='1'&&k<='4'){ if(view) g_plot_zoom=(k=='1'||g_plot_zoom==k-'0')?0:k-'0';
                                     else if(k<='3') lens=k-'0'; }
            else if(k=='.') step=1;                       /* step forward while paused */
            else if(k==','){ if(i>0){ i--; rebuild=1; step=-1; } }   /* step back: history is rebuilt */
        }
        if(g_stop) break;
        if(paused && !step){ usleep(80000); continue; }
        if(step<0){ /* already moved */ }
        else {
            double dt=0;
            if(i+1<n && g_rep_speed>0){
                dt=(rp_num(fr[i+1],"ts_epoch",0)-rp_num(fr[i],"ts_epoch",0))/g_rep_speed;
                if(dt<0) dt=0;
                if(dt>5) dt=5;   /* a long gap in the recording is not replayed in real time */
            }
            i++;
            if(!paused && dt>0) usleep((useconds_t)(dt*1e6));
        }
    }
    tui_leave();
    (void)t0;
    fprintf(stderr,"\nreplay: %zu frames from %s\n",n,path);
    for(size_t j=0;j<n;j++) free(fr[j]);
    free(fr);
    return 0;
}

static void json_from_terse(FILE*tf,FILE*o){
    fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
    if(sz<=0){ fputs("{}\n",o); return; }
    char *b=malloc((size_t)sz+1); if(!b){ fputs("{}\n",o); return; }
    size_t rd=fread(b,1,(size_t)sz,tf); b[rd]=0;
    fputs("{\n",o); int first=1;
    char *p=b;
    while(*p){
        while(*p==' '||*p=='\n'||*p=='\t'||*p=='\r') p++;
        if(!*p) break;
        char *k=p; while(*p && *p!='=' && *p!=' ' && *p!='\n') p++;
        if(*p!='='){ continue; }               /* Tokens without '=' are discarded (there should be none) */
        size_t kl=(size_t)(p-k); p++;
        char *v=p; size_t vl; int quoted=0;
        if(*p=='"'){ quoted=1; v=++p; while(*p && *p!='"') p++;
                     vl=(size_t)(p-v); if(*p=='"') p++; }
        else       { while(*p && *p!=' ' && *p!='\n' && *p!='\r') p++;
                     vl=(size_t)(p-v); }
        if(!first) fputs(",\n",o);
        first=0;
        fputs("  \"",o); json_escape_out(o,k,kl); fputs("\": ",o);
        int numeric=0;
        if(!quoted && vl>0){                    /* Treated as numeric only when the whole token is a finite number */
            char tmp[64];
            if(vl<sizeof tmp){ memcpy(tmp,v,vl); tmp[vl]=0;
                char*end; double d=strtod(tmp,&end);
                if(end==tmp+vl && d==d && d<1e308 && d>-1e308) numeric=1; }
        }
        if(numeric){ fwrite(v,1,vl,o); }
        else { fputc('"',o); json_escape_out(o,v,vl); fputc('"',o); }
    }
    fputs("\n}\n",o); free(b);
}


/* One frame as a single JSONL line.  --replay prints a frame per recorded sample;
   emitting the pretty form repeatedly yields concatenated objects that are neither
   a JSON document nor JSONL, so the newlines are folded the way rp_record does. */
static void json_line_from_terse(FILE*tf,FILE*o){
    FILE *m=tmpfile();
    if(!m){ json_from_terse(tf,o); return; }
    json_from_terse(tf,m);
    fflush(m); fseek(m,0,SEEK_END); long n=ftell(m); fseek(m,0,SEEK_SET);
    char *b=(n>0)?malloc((size_t)n+1):NULL;
    if(b){ size_t rd=fread(b,1,(size_t)n,m); b[rd]=0;
           for(size_t k=0;k<rd;k++) if(b[k]=='\n') b[k]=' ';
           fprintf(o,"%s\n",b); free(b); }
    fclose(m);
}static int g_json=0;

/* ---- One sample (shared by one-shot and live) ----
 * Returns collection work time in ms, excluding waits.  Window:
 *   clear_refs -> baseline -> interval -> smaps/rollup/io -> classify/method B
 * The paramdump child is harvested before the window when measuring hot, so it
 * does not pollute Referenced.
 */
static double sample_once(proc_t*P,proc_t*P0,mem_t*M,tier_t*T,iod_t*IO,
                          double interval,int no_hot,int with_a,FILE**pd){
    double w0=now_ms();
    P0->pid=P->pid;
    if(P->pid>=0){
        if(*pd && !no_hot){ paramdump_finish(*pd); *pd=NULL; }
        if(!no_hot) do_clear_refs(P->pid);
        parse_status_stat(P0); parse_procio(P0);
    }
    dstat_t d0[MAXDISK]; int nd0=parse_diskstats(d0,MAXDISK);
    /* Baseline for host CPU: /proc/stat accumulates since boot, so two points are needed
       for an interval.  Taking the window's start here lets the call at the end produce
       the interval value, so CPU is non-zero even in one-shot mode. */
    { mem_t base; memset(&base,0,sizeof base); parse_hostcpu(&base); }
    /* blkio baseline.  It is a sum over all threads, so P0's stat cannot give it, and
       the threads are not walked twice per frame: the previous frame's closing scan
       is this frame's baseline.  Only a changed pid or the first frame measures
       afresh, so one-shot (-t/-1) also produces a value. */
    unsigned long blk0=0;
    { static unsigned long last_blk=0; static int last_pid=-1;
      if(P->pid==last_pid && last_blk>0) blk0=last_blk;
      else { int dd,nn; count_d_threads(P->pid,&dd,&nn,&blk0); }
      last_pid=P->pid; g_blk_baseline_slot=&last_blk; }
    double t0=now_ms();
    usleep((useconds_t)(interval*1000000));
    if(*pd){ paramdump_finish(*pd); *pd=NULL; }
    if(P->pid>=0){ parse_smaps(P); parse_rollup(P); parse_status_stat(P); parse_procio(P); }
    dstat_t d1[MAXDISK]; int nd1=parse_diskstats(d1,MAXDISK);
    double dt=(now_ms()-t0)/1000.0; if(dt<0.001) dt=0.001;
    parse_meminfo(M);
    parse_hostcpu(M);                  /* Host CPU and load; a delta, so called every frame */
    /* Refresh the instance summary every frame (one rollup per process, under 1ms).
       The current instance was already measured precisely this frame, so that value
       overwrites it and the OS and process boxes use the same instant - measuring
       separately would differ by whatever changed between the two reads. */
    memset(T,0,sizeof *T);
    g_enum_T=T; g_enum_dt=dt;          /* This single enum serves as both the instance summary and the tier aggregation */
    enum_instances();
    /* Detect a server that died or restarted under a new pid.  Without this, /proc
       reads on the dead pid fail silently and the previous values are redrawn, so
       the screen appears frozen (measured: rss stuck at 912.6M for 20+ frames
       across a restart).  A new pid under the same db name is adopted. */
    {
        /* The check must run even at pid<0 (not running), or a restart is never noticed and the
           instance stays down forever once it has died. */
        int alive=0;
        if(P->pid>=0)
            for(int i=0;i<g_ninst;i++) if(g_inst[i].pid==P->pid){ alive=1; break; }
        if(!alive){
            /* Keep the db name: clearing it with memset loses the name while waiting for a restart,
               so the server can never be picked up again when it returns. */
            char keep[64]; snprintf(keep,sizeof keep,"%s",P->db[0]?P->db:g_cur_db);
            int newpid=-1;
            for(int i=0;i<g_ninst;i++)
                if(keep[0] && !strcmp(g_inst[i].db,keep)){ newpid=g_inst[i].pid; break; }
            /* Move to the new pid on a restart, or to not-running if it has not come up.
               Either way the previous instance's analysis must be discarded, or a dead
               server's region and heap values appear to belong to the new one. */
            reset_instance_state();
            memset(P,0,sizeof *P);
            memset(P0,0,sizeof *P0);
            snprintf(P->db,sizeof P->db,"%s",keep);
            P->pid=newpid;
            if(newpid>=0){
                for(int i=0;i<g_ninst;i++)
                    if(g_inst[i].pid==newpid){ g_cur=i; break; }
                snprintf(g_cur_db,sizeof g_cur_db,"%s",keep);
                cap_probe(newpid);             /* The new process: re-probe rather than inherit the old flags */
                parse_smaps(P); parse_rollup(P); parse_status_stat(P); parse_procio(P);
                /* Heap A/B analyse a process address space and must be redone after a restart.
                   B runs every frame below, but A runs periodically, so without this a
                   dead server's measurement would linger - force an immediate remeasure. */
                g_a_redo=1;
            }
            P0->pid=P->pid;                    /* The delta baseline moves to the new process too */
        }
    }
    if(P->pid>=0){
        for(int i=0;i<g_ninst;i++)
            if(g_inst[i].pid==P->pid){
                g_inst[i].pss=P->pss_kb*1024.0;
                g_inst[i].rss=P->rss_kb*1024.0;
                g_cur=i;                       /* Keep the selection when the list changes */
                break;
            }
    }
    scan_tiers(T,dt);               /* Already filled by enum; the wrapper skips the second walk */
    g_enum_T=NULL;
    if(P->pid>=0){
        classify_regions(P);
        method_b(P);
        pgbuf_scan(P);
        if(with_a) method_a(P);
        grade_dynamic_heap();          /* Re-grade in light of the decomposition */
    } else { NSR=0; memset(&B,0,sizeof B); snprintf(B.why,sizeof B.why,g_ascii?"cub_server not running":"cub_server 미기동");
             PG.ok=0; snprintf(PG.why,sizeof PG.why,g_ascii?"cub_server not running":"cub_server 미기동"); }
    memset(IO,0,sizeof *IO);
    IO->rb=UDELTA(P->read_b,P0->read_b)/dt; IO->wb=UDELTA(P->write_b,P0->write_b)/dt;
    double drchar=UDELTA(P->rchar,P0->rchar);
    IO->rchar=drchar/dt;
    IO->ri=UDELTA(P->syscr,P0->syscr)/dt;
    IO->wi=UDELTA(P->syscw,P0->syscw)/dt;
    IO->absorb = drchar>0 ? 1.0-((IO->rb*dt)/drchar>1.0?1.0:(IO->rb*dt)/drchar) : -1.0;
    /* Below the rchar floor the ratio is meaningless, so it counts as idle */
    IO->absorb_ok = (IO->absorb>=0 && IO->rchar>=RCHAR_MIN);
    long hz=sysconf(_SC_CLK_TCK); if(hz<=0) hz=100;
    /* blkio wait, as the delta of the all-thread sum.  The main thread alone always
       reads 0 because the workers do the I/O.  Being a sum it can exceed one core
       (100%), the same convention as CPU usage, so it reads as "total time this
       server waited on disk".  The first frame has no baseline and is 0. */
    /* Thread scan at the end of the window; blkpct uses this, so it must come before that calculation. */
    count_d_threads(P->pid,&IO->d_thr,&IO->n_thr,&IO->blk_ticks);
    if(g_blk_baseline_slot) *g_blk_baseline_slot=IO->blk_ticks;   /* Baseline for the next frame */
    IO->blkpct = (IO->blk_ticks>=blk0)
               ? ((double)(IO->blk_ticks-blk0)/hz)/dt*100.0 : 0.0;
    IO->minflt=UDELTA(P->minflt,P0->minflt)/dt;
    IO->majflt=UDELTA(P->majflt,P0->majflt)/dt;
    /* CPU: the utime+stime delta over the window length.  One core is 100%, so a multicore
       process can exceed 100% - the ceiling (cores x 100) is shown alongside. */
    { unsigned long c1=P->utime+P->stime, c0=P0->utime+P0->stime;
      IO->cpu_pct = (c1>=c0) ? ((double)(c1-c0)/hz)/dt*100.0 : 0.0;
      long nc=sysconf(_SC_NPROCESSORS_ONLN); IO->ncpu = (nc>0)?(int)nc:1; }
    for(int i=0;i<nd1;i++){
        for(int j=0;j<nd0;j++) if(!strcmp(d1[i].name,d0[j].name)){
            double u=UDELTA(d1[i].io_ms,d0[j].io_ms)/(dt*1000.0)*100.0;   /* a device reset rewinds its counters */
            if(u>100) u=100;
            if(IO->devnm[0]==0 || u>IO->devutil){
                IO->devutil=u;
                snprintf(IO->devnm,sizeof IO->devnm,"%.31s",d1[i].name);
                IO->devr=UDELTA(d1[i].rsect,d0[j].rsect)*512.0/dt;
            }
            break;
        }
    }
    /* ---- Capacity axis collection ----
       Collection for interpretation must not affect operation.  It measures under
       1ms, but slow storage or a huge buffer could stretch it, so a 0.5s budget
       applies: over it, the frame's collection is dropped and the reason shown. */
    if(P->pid>=0){
        double c0=now_ms();
        cap_collect(P,IO,d0,nd0,d1,nd1,dt);
        CAP2.ms=now_ms()-c0;
        if(CAP2.ms>CAP_BUDGET_MS){
            CAP2.over_budget=1;
            snprintf(CAP2.ps_why,sizeof CAP2.ps_why,
                     g_ascii?"skipped: aggregation took %.0fms > %.0fms budget"
                            :"취합 생략: %.0fms 소요 > 예산 %.0fms",CAP2.ms,(double)CAP_BUDGET_MS);
            cap_reset_values();          /* Half values are not published as metrics */
        } else CAP2.over_budget=0;
    } else { memset(&CAP2,0,sizeof CAP2); cap_reset_values();
             snprintf(CAP2.ps_why,sizeof CAP2.ps_why,g_ascii?"server down":"cub_server 미기동"); }
    return now_ms()-w0-interval*1000.0;
}

/* ---------------- Option specification: the single source for the banner and help ---------------- */
typedef struct { const char*key,*meta,*desc; } opt_t;
/* ---- Detect whether Hangul can be displayed ----
   A server process cannot know the user's font, so it uses what it can know.
   If the locale codeset is not UTF-8, Hangul bytes are certain to break -> English.
   On a UTF-8 interactive tty, print one Hangul character and ask for the cursor
   position (CPR): if it advanced by one cell the terminal treats Hangul as
   half-width and every column would shift -> English.  No reply within 200ms
   leaves Hangul in place.  --ascii / --ko always win. */
static int tty_hangul_wide(void){
    struct termios o,r;
    if(tcgetattr(STDIN_FILENO,&o)) return -1;
    r=o; r.c_lflag&=~(unsigned)(ICANON|ECHO); r.c_cc[VMIN]=0; r.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&r)) return -1;
    /* At the start of a row, print one Hangul character and query the cursor: col 3 in the ESC[row;colR reply means width 2 */
    if(write(STDOUT_FILENO,"\r\xEA\xB0\x80\x1b[6n",9)<0){ tcsetattr(STDIN_FILENO,TCSANOW,&o); return -1; }
    char b[32]; int bn=0;
    struct pollfd pf={STDIN_FILENO,POLLIN,0};
    while(bn<(int)sizeof b-1 && poll(&pf,1,200)>0){
        ssize_t k=read(STDIN_FILENO,b+bn,1);
        if(k<=0) break;
        bn+=(int)k;
        if(b[bn-1]=='R') break;
    }
    b[bn]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&o);
    { ssize_t k=write(STDOUT_FILENO,"\r\x1b[K",4); (void)k; }   /* Erase the test character */
    int row=0,col=0;
    if(sscanf(b,"\x1b[%d;%dR",&row,&col)==2 && col>0) return col>=3?1:0;
    return -1;
}
static void auto_lang(int force_ko){
    if(g_ascii||force_ko) return;                 /* An explicit setting wins */
    setlocale(LC_CTYPE,"");
    const char *cs=nl_langinfo(CODESET);
    if(!cs || !strstr(cs,"UTF")){ g_ascii=1; return; }
    if(isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO))
        if(tty_hangul_wide()==0) g_ascii=1;
}

static const opt_t OPTS[]={
 {"[db명]",      "", "볼 데이터베이스 (생략 시 알파벳순 첫 번째, 라이브에서 < > 로 이동)"},
 {"(없음)",      "", "드릴다운 트리 (기본, 1회 출력)"},
 {"-t",          "", "terse key=value (모니터링 연동·백데이터)"},
 {"--json",      "", "-t 와 같은 내용을 JSON 오브젝트로 (키·값·순서 동일, -t 자동 포함)"},
 {"-b, --live",  "", "btop 대시보드 (라이브)"},
 {"--heap",      "", "방법 A(힙 히스토그램) 포함"},
 {"--ascii",     "", "라벨을 영문으로 강제 (자동: 비 UTF-8 로케일·한글 폭 미지원 tty 는 영문)"},
 {"--ko",        "", "한글 라벨 강제 (자동 판정이 영문으로 잘못 내렸을 때)"},
 {"--offsets",   "F","오프셋 표 파일/디렉터리 (tools/collect-offsets.sh 산출물 — 디렉터리면 버전 자동 선택; 생략 시 실행파일 옆 offsets/ 자동 탐색)"},
 {"--hot",       "", "최근접근(hot) 측정 켜기 — clear_refs 쓰기 필요(기본 OFF: 대상 PTE 전수 순회 ~30ms/GB + minor fault 유발)"},
 {"--hot-limit-gb","N","핫 측정 자동 해제 임계 상주 크기(기본 2GB) — 이보다 크면 --hot 이어도 끈다"},
 {"--hot-every", "S","핫 측정 최소 간격(초, 기본 60) — 매 프레임 호출하지 않는다"},
 {"--no-hot",    "", "(하위호환) 핫 측정 끄기 — 기본이 이미 OFF 다"},
 {"--bcb-dump",  "F","버퍼풀 BCB 스냅샷을 파일 F 로 (cub_volmap --bufmap F 가 지도에 얹음; 라이브는 매 프레임 갱신)"},
 {"--capacity-state","F","용량 상한(포화 시 관측 IOPS) 기억 파일 — 재실행해도 헤드룸%를 이어서 판정"},
 {"-p, --plot",  "", "시계열 플롯 (라이브, p 키로 대시보드와 상호 전환)"},
 {"--interval",  "S","샘플 창(초, 0.05~3600, 기본 0.5)"},
 {"--record", "F", "라이브(-b/-p) 프레임을 파일 F 로 기록 (JSONL — -t/--json 과 같은 키, --replay 와 병용 불가)"},
 {"--replay", "F", "기록 파일 F 를 재생 (서버·/proc 불필요, -b/-p 키 그대로)"},
 {"--replay-speed", "N", "재생 배속 (1=실측 간격, 0=최대속도, 기본 1)"},
 {"--dump-hist", "", "히스토리 1샘플을 stderr 로 덤프 (수치 대조·진단용)"},
 {"-h, --help, /h","","이 도움말"},
};
#define NOPTS ((int)(sizeof OPTS/sizeof OPTS[0]))
static int opt_known(const char*s){
    for(int i=0;i<NOPTS;i++){
        const char*k=OPTS[i].key; char buf[64]; snprintf(buf,sizeof buf,"%s",k);
        char*tok=strtok(buf,",");
        while(tok){
            while(*tok==' ')tok++;
            if(*tok=='-' && !strcmp(tok,s)) return 1;
            tok=strtok(NULL,",");
        }
    }
    return 0;
}
/* Join the recognized options with spaces, shared by terse and the banner */
static void opts_used_str(int argc,char**argv,char*out,size_t cap){
    out[0]=0;
    for(int i=1;i<argc;i++){
        if(!opt_known(argv[i])) continue;
        snprintf(out+strlen(out),cap-strlen(out),"%s%s",out[0]?" ":"",argv[i]);
        if(!strcmp(argv[i],"--interval") && i+1<argc)
            snprintf(out+strlen(out),cap-strlen(out)," %s",argv[i+1]);
    }
}
/* One line: which options ran and what else is available */
static void print_opts_line(int argc,char**argv){
    char used[256]={0};
    opts_used_str(argc,argv,used,sizeof used);
    /* Listing every option does not say what to do next (-h already has them), so this
       shows only the entry points a newcomer can run straight away. */
    printf(g_ascii?"opts %s   ·   live: cub_top -b · timeseries: -p · heap detail: --heap"
                  " · machine output: -t · help: -h\n"
                  :"옵션 %s   ·   실시간 화면: cub_top -b · 시계열: -p · 동적 메모리 상세: --heap"
                  " · 모니터링 연동: -t · 도움말: -h\n",
           used[0]?used:(g_ascii?"(default)":"(기본)"));
}

/* ────────────────── main ────────────────── */
/* Colour one help line so its structure reads first:
     "-- section --"  cyan bold
     "  key/option"   leading token bright, description plain
   Colour changes nothing, so piped output is the same text. */
static void help_line(const char *s){
    if(!s){ putchar('\n'); return; }
    if(strstr(s,"──")){ printf("\033[1;36m%s\033[0m\n",s); return; }
    /* When a line starts with a key or option, highlight just that part */
    if(s[0]==' '&&s[1]==' '&&(s[2]=='['||s[2]=='-'||(s[2]>='1'&&s[2]<='9'))){
        int i=2; while(s[i]&&s[i]!=' ') i++;
        while(s[i]==' '&&s[i+1]&&s[i+1]!=' ') i++;      /* A joined token such as "-o, --opt" */
        printf("  \033[1;37m%.*s\033[0m%s\n",i-2,s+2,s+i);
        return;
    }
    printf("%s\n",s);
}
/* Page several lines to the screen height (used by the live h screen).
   Returns 1 if the user leaves with q or ESC. */
/* Page breaks follow sections; cutting at a fixed line count leaves a section
   heading alone at the foot of a page.
     "\f" alone   an explicit break (not drawn)
     "-- title --" moves to the next page if fewer than 4 lines remain
     otherwise     break when the page is full
   The footer names the current section. */
static int help_pager(const char *const *ln,int n){
    int rows=term_h(); if(rows<10) rows=24;
    int body=rows-3;                       /* Body plus one blank, one footer and one of slack */
    static int ps[128]; int np=0;          /* Starting line of each page */
    { int used=0; ps[np++]=0;
      for(int i=0;i<n;i++){
          const char*s=ln[i];
          if(s && s[0]=='\f'){ if(used>0 && np<128){ ps[np++]=i+1; } used=0; continue; }
          int head = s && strstr(s,"──")!=NULL;
          /* A heading must start on the same page as its body: move on when few lines
             remain, and also check that at least some body fits after it.
             Otherwise a heading is left alone at the foot of one page and the page
             after it begins with no heading at all. */
          if(used>0 && (used>=body || (head && body-used<6))){ if(np<128) ps[np++]=i; used=0; }
          used++;
      } }
    int pg=0;
    for(;;){
        int a=ps[pg], b=pg+1<np?ps[pg+1]:n;
        printf("\033[H\033[2J");
        const char*title="";
        int drawn=0;
        for(int i=a;i<b;i++){
            if(ln[i] && ln[i][0]=='\f') continue;
            if(ln[i] && strstr(ln[i],"──") && !title[0]) title=ln[i];
            help_line(ln[i]); drawn++;
        }
        for(;drawn<body;drawn++) putchar('\n');   /* Fixed footer position */
        char tt[96]={0};
        if(title[0]){ const char*t=title;
                      while(*t==' ' || !strncmp(t,"─",3)) t+= (*t==' ')?1:3;   /* Strip the leading rule (3 UTF-8 bytes) */
                      snprintf(tt,sizeof tt,"%s",t);
                      char*e=strstr(tt,"──"); if(e) *e=0;
                      size_t L=strlen(tt); while(L&&tt[L-1]==' ') tt[--L]=0; }
        printf("\033[38;5;244m ── %d/%d%s%s ── \033[0m", pg+1, np, tt[0]?" · ":"", tt);
        printf("\033[38;5;250m  [space] 순환  [→/PgDn] 다음  [←/PgUp] 이전  [q] 닫기\033[0m\n");
        fflush(stdout);
        int k=-1;
        while(!g_stop && k<0){ k=tui_key(); if(k<0) usleep(40000); }
        if(g_stop) return 1;
        /* Same convention as volmap's help: only q closes (tui_key folds the Hangul jamo).
           space/enter rolls over (last page to first), right/down/PgDn advance,
           left/up/PgUp/b go back.  Other keys are ignored, so a stray key does not
           dismiss the help. */
        if(k=='q'||k=='Q') return 1;
        if(k==' '||k=='\r'||k=='\n'){ pg=(pg+1)%np; }
        else if(k=='n'||k=='j'||k==TUI_KEY_RIGHT||k==TUI_KEY_DOWN||k==TUI_KEY_PGDN){ if(pg+1<np) pg++; }
        else if(k=='b'||k=='p'||k=='k'||k==0x7f||k==0x08||k==TUI_KEY_LEFT||k==TUI_KEY_UP||k==TUI_KEY_PGUP){ if(pg>0) pg--; }
    }
}
/* Help line collector: the text usage() builds is also captured as an array for the
   pager.  Two copies of the wording would inevitably drift, so usage() stays the only
   place it is written. */
#define HELPMAX 400   /* About 206 lines today and growing; an overflow leaves a warning below */
static char  g_hbuf[HELPMAX][1024];
static const char *g_hln[HELPMAX];
static int   g_hn=0, g_hcap=0;         /* hcap 1 means collection mode: nothing is printed */
/* Upper bound on one help block.  The last block arrives whole, so leave room -
   truncation here loses entire sections and can cut mid-UTF-8, leaving a broken
   character.  Overflow is reported rather than silently trimmed. */
#define HOUT_MAX 32768
static void hout(const char *fmt,...){
    char tmp[HOUT_MAX]; va_list ap; va_start(ap,fmt);   /* A large puts block arrives whole */
    int need=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    if(need>=(int)sizeof tmp)
        fprintf(stderr,"cub_top: 도움말 블록이 %d바이트로 잘렸다(HOUT_MAX=%d) — "
                       "HOUT_MAX 를 늘릴 것\n",need,HOUT_MAX);
    /* Several lines can arrive together; split them */
    char *p=tmp;
    for(;;){
        char *nl=strchr(p,'\n');
        if(nl) *nl=0;
        /* A line never exceeds 1023 bytes; longer ones are truncated, harmless in help text */
        if(g_hcap){ if(g_hn<HELPMAX){ snprintf(g_hbuf[g_hn],1024,"%.1023s",p);
                                      g_hln[g_hn]=g_hbuf[g_hn]; g_hn++; }
                    else {                    /* Report the overflow on screen.  g_hn never exceeds HELPMAX: g_hn++ once let it
                        reach HELPMAX+1, and the pager read g_hln[HELPMAX] out of
                        bounds and segfaulted. */
                        snprintf(g_hbuf[HELPMAX-1],1024,
                                 "%s","⚠ 도움말이 HELPMAX 에서 잘렸다 — 값을 늘릴 것");
                        g_hln[HELPMAX-1]=g_hbuf[HELPMAX-1]; g_hn=HELPMAX; } }
        else       { help_line((p[0]&&p[0]!='\f')?p:NULL); }   /* CLI: a page break becomes a blank line */
        if(!nl) break;
        p=nl+1;
        if(!*p) break;                  /* A trailing newline does not create a blank line */
    }
}
/* The CLI preamble (name, usage, options) is not needed in live h - it is already
   running.  Keeping it separate from the shared concepts section lets live go straight
   from mode keys to concepts. */
static void usage_common(void);
static void usage(void){
    hout("cub_top (C) — CUBRID 메모리 관측 (단일 파일 C99, 외부 의존 0)\n\n사용: cub_top [옵션]");
    for(int i=0;i<NOPTS;i++){
        char kb[64]; snprintf(kb,sizeof kb,"%s%s%s",OPTS[i].key,OPTS[i].meta[0]?" ":"",OPTS[i].meta);
        hout("  %-18s %s",kb,OPTS[i].desc);
    }
    hout("");
    usage_common();
}
static void usage_common(void){
    hout(
"\f\n── 환경변수 CUBRID ──\n"
"  CUBRID=<설치경로>  설정 시 시작할 때 한 번 '<경로>/bin/cubrid paramdump <db>' 를 실행해\n"
"  서버가 실제로 적용 중인 파라미터를 읽는다(셸을 거치지 않고 execvp, 최대 5초).\n"
"  없으면 <경로>/conf/cubrid.conf 를 직접 파싱하고, 그것도 없으면 아래가 모두 빠진다:\n"
"    · data_buffer/log_buffer 의 '설정 대비 %' 판정 — 설정값이 없어 초과 여부를 알 수 없다\n"
"    · 파라미터 귀속률 — 설정으로 설명되는 몫을 셀 수 없어 표시되지 않는다\n"
"    · DB 페이지 크기 — data_buffer_size/pages 로 역산한다(실패 시 16KB 로 가정)\n"
"    · 방법 B 자기검증 — num_buffers 와 data_buffer_pages 대조가 불가해 힙 B 가 꺼진다\n"
"    · 렌즈2(설정초과)·하단 '메모리 파라미터' 표 전체\n"
"  즉 RSS/PSS 같은 실측은 그대로 나오지만, '왜 이만큼인가' 를 설명하는 축이 사라진다.\n"
"\f\n── 여러 데이터베이스(인스턴스) ──\n"
"  cub_top <db명>     그 DB 를 본다.  생략하면 가동 중인 것 중 알파벳순 첫 번째\n"
"  라이브에서 < > 로 이동. 상세 추적은 화면에 보이는 DB 하나뿐이고,\n"
"  나머지는 총량만 집계한다(clear_refs 를 전부에 돌리면 대상 서버가 느려지기 때문).\n"
"  terse 는 instance.<db>.* 로 모든 인스턴스를 내보내며 tracked=1 이 현재 추적 대상이다.\n"
"\f\n── 두 가지 라이브 화면 ──\n"
"  대시보드(-b)  현재 상태 한 장. p 로 시계열과 오간다. 키는 라이브 h 도움말 첫 절 참조.\n"
"  시계열(-p)    같은 값의 시간 흐름(4패널 A~D). p 로 대시보드로 돌아온다.\n"
"  배치(-t · --json · 옵션 없이)  한 번 수집해 텍스트로 낸다 — 모니터링·보고서 연동용.\n"
"\f\n── 대시보드 화면 구성 (위에서 아래로) ──\n"
"  ① OS 컨텍스트(호스트 CPU·메모리·인스턴스) ② 프로세스(티어별 PSS·코어)\n"
"  ③ RSS · 서버(영역·값 4열) ④ 동적 메모리 상세(B/A) ⑤ 메모리 추이(+load) ⑥ I/O 순으로 쌓인다.\n"
"  아래 절 설명도 같은 순서다 — 화면에서 보이는 자리 그대로 찾아 내려가면 된다.\n"
"\f\n── ① OS 컨텍스트 — CPU 와 load ──\n"
"  CPU 32c/64t     물리 32 코어 / 논리 64 스레드. 다르면 하이퍼스레딩이다.\n"
"                  물리보다 많은 부하는 같은 코어를 나눠 쓰는 것이라 선형으로 안 빨라진다.\n"
"  total|used|pct  total=논리코어, used=사용 중 코어 수, pct=전체 사용률.\n"
"                  used 는 '몇 코어어치가 돌고 있나' 라 프로세스 박스의 코어 수와 바로 비교된다.\n"
"  막대(게이지)    코어를 바쁜 순으로 정렬해 왼쪽부터 그린 것 — 합계가 아니라 분포다.\n"
"                  색으로 구분한다 — 적색=포화(≥95%%) 주황=높음(≥50%%)\n"
"                  녹색=중간(≥10%%) 회색=유휴. 글리프는 다른 게이지와 같다.\n"
"                  used 12 이 12코어에 고르게 퍼진 것인지 6개가 100%%인지는 합계로는\n"
"                  구분되지 않는다 — 뒤엣것이 병목이고, 왼쪽 붉은 구간의 폭이 그것이다.\n"
"                  ※ 코어가 막대 폭보다 많으면 한 칸이 여러 코어를 대표하며 그 구간의\n"
"                    최댓값을 그린다(포화를 평균으로 감추지 않기 위함). 그래서 칸 수에\n"
"                    코어 수를 곱해 읽으면 안 된다 — 정확한 개수는 옆의 sat 숫자를 볼 것.\n"
"  sat N           포화 코어 수 — 사용률 95%% 이상인 코어가 몇 개인가.\n"
"                  포화 직전(50~95%%)은 막대의 주황 구간이 보여준다.\n"
"                  btop 의 코어별 미터에서 100%% 칸을 세는 것과 같은 값이다.\n"
"                  이 값이 물리코어 수에 근접하면 CPU 가 병목이다. 반대로 used 는 큰데\n"
"                  sat 이 0 이면 부하가 고르게 퍼진 것이라 아직 여유가 있다.\n"
"  us  user        사용자 코드. 질의 처리·정렬·조인 등 엔진이 실제 일한 몫\n"
"  sy  system      커널. I/O 발행, 락, 컨텍스트 스위치. us 대비 과도하면 경합을 의심\n"
"  load 1/5/15     최근 1·5·15분 평균 실행대기 태스크 수. CPU 사용률이 아니다 —\n"
"                  리눅스에서는 D 상태(디스크 대기)도 포함하므로, CPU 가 한가해도\n"
"                  I/O 가 막히면 올라간다. 논리코어 수(64)를 넘으면 대기가 쌓이는 중.\n"
"                  전체 호스트 값이라 인스턴스별로 나눌 수 없어 박스 제목에 둔다.\n"
"\f\n── ② 프로세스 박스가 PSS 인 이유 · 티어별 CPU ──\n"
"  PSS = 공유 페이지를 쓰는 프로세스 수로 나눠 배분한 상주량.\n"
"  libcubrid.so 10M 을 5개가 공유하면 각자 2M 로 잡힌다 → 행을 다 더해도 이중 계상이\n"
"  없어 All 합계가 성립한다. RSS 로 재면 같은 페이지가 여러 번 세어져 합이 부풀려진다.\n"
"  프로세스 박스   각 티어(master/server/pl/broker/CAS) 앞의 숫자가 그 티어의 코어 수.\n"
"                  전부 더하면 OS 의 used 이하여야 정상이다(나머지는 CUBRID 밖 프로세스).\n"
"                  색으로 크기를 구분한다 — 회색 · =유휴, 청록 <1코어, 노랑 1코어 이상,\n"
"                  주황=눈에 띄는 부하, 적색=물리코어 수 초과. 0 은 숫자 대신 · 로 눌러\n"
"                  실제로 돌고 있는 티어만 눈에 들어오게 했다.\n"
"  ※ 다른 도구와 대조할 때\n"
"    · server 의 CPU 는 top 과 같은 '1코어=100%' 규약이라 100% 를 넘을 수 있다.\n"
"      OS 박스의 코어 수와 견주려면 100 으로 나눌 것(terse 는 tier.*_cores 로 이미 환산).\n"
"    · Memory 의 used 는 MemTotal-MemAvailable 로 top 과 같은 정의다.\n"
"      htop 은 Shmem 을 달리 처리해 수 GB 낮게 나오는데, 오차가 아니라 정의 차이다.\n"
"\f\n── ③ RSS · 서버 — 각 항목이 알려주는 것 ──\n"
"  data_buffer     페이지 버퍼. 설정보다 큰 게 정상(BCB·victim 배열이 같은 매핑에 포함)\n"
"  log_buffer      로그 버퍼. 선할당이라 설정≈상주\n"
"  dynamic-heap    정렬·캐시·세션이 섞인 동적 힙 → 아래 [B]/[A] 로 분해\n"
"  thread stacks   워커 스택. 예약 크고 상주 극소가 정상\n"
"  glibc arena     할당자 예약. 상주 0이면 물리 메모리 미사용\n"
"  (설정 X, 할당 Y%)  설정 대비 매핑 비율. >105% 면 초과\n"
"  핫 N%           상주 중 실제 접근 비율. 낮으면 올려두고 안 만지는 메모리\n"
"  Σ 합계          4열의 합. rss 합 = 프로세스 RSS(항등). 프로세스 박스의 PSS 와는\n"
"                  공유 .so 배분만큼 다르며(오류 아님), 그 값이 라벨에 함께 적힌다.\n"
"\f\n── 파라미터 귀속률 — 화면 표기 \"설정으로 설명 N%%\" (영역 박스 제목 옆) ──\n"
"  서버 메모리 중 cubrid.conf 설정으로 '왜 이만큼인지' 설명되는 비율.\n"
"    귀속   data_buffer(data_buffer_size), log_buffer(log_buffer_size)\n"
"    비귀속 dynamic-heap, thread stacks, code(.so) 등 — 설정으로 통제되지 않는 부분\n"
"  읽는 법:\n"
"    높다(90%대)   정상. 메모리 대부분이 설정대로다\n"
"    떨어진다      dynamic-heap 이 자라고 있다는 뜻 — 아래 heap 박스로 내려가\n"
"                  무엇이 늘었는지 확인할 것([a] 로 방법 A 를 돌리면 더 쪼개진다)\n"
"  즉 이 값은 'heap 박스를 들여다볼 필요가 있는가' 를 알려주는 신호다.\n"
"\f\n── 관측 등급 (값 왼쪽 1글자) ──\n"
"  ● 측정    OS/엔진이 직접 보고. 신뢰 가능\n"
"  ◐ 예약    매핑만 확인 — 실사용과 크게 다를 수 있음\n"
"  ◌ 추정    크기 패턴 추정 — 상대 비중만\n"
"  ? 미귀속  아직 분해되지 않음 — 해석하지 말 것\n"
"\f\n── ④ 동적 메모리 상세(= glibc malloc 힙) — B=정확 · A=크기 추정 ──\n"
"  ※ 여기서 '힙'은 프로세스의 동적 할당 영역(malloc)이다. 테이블 데이터가 저장되는\n"
"     스토리지의 '힙 파일'(HFID)과는 다른 것이며, 그쪽은 cub_volmap 이 다룬다.\n"
"  [B] 항목        엔진이 직접 보고하는 값 → 정확. plan·결과·필터 캐시·락·세션·카탈로그·연결\n"
"  lock table (tran)  lk_Gl.tran_lock_table — 트랜잭션별 락 보유 슬롯이다.\n"
"                  num_trans x 152B 로, 접속 상한에 비례해 미리 잡힌다.\n"
"                  ※ cubrid lockdb 의 \"Object Lock Table\" 과는 다른 구조다.\n"
"                    저쪽은 잠긴 객체의 LK_ENTRY/LK_RES 풀(예: 1000개 242K)이고\n"
"                    이쪽은 트랜잭션 슬롯(예: 101개 15K)이라 값이 다른 것이 정상이다.\n"
"  [A] 항목        glibc 청크 분포 추정. 정렬·해시 작업버퍼 등 B가 못 잡는 것\n"
"  [A] overcount   A 합계가 상주 초과(free 청크·비상주 포함) — 정상, 상한으로 읽을 것\n"
"\f\n── ⓑ 실행 계정 — 서버를 띄운 계정으로 실행할 것 ──\n"
"  root 일 필요는 없다. 서버를 띄운 계정(보통 cubrid)이면 전 기능이 동작한다.\n"
"  그 계정이 아니면 커널 ptrace 검사로 mem/smaps/io 가 막혀 55개 키가 빠진다\n"
"  (영역별 분해·PSS·동적 메모리 상세 B·버퍼풀 BCB·로그 LSA·proc_iops·hot).\n"
"  남는 것: OS 전체·장치 I/O·cubrid.conf·서버 총량(status 폴백으로 VmRSS 는 살린다).\n"
"  못 본 값은 0 으로 내지 않고 perm.limited=1 과 사유를 함께 표기한다 — 0 으로 내면\n"
"  모니터링이 '메모리를 안 쓴다' 로 오독한다. 자세한 대조표는 docs/limitations.md §15.\n"
"\f\n── ⓐ 용량(capacity) — 증설·축소·개선을 무엇으로 판단하나 ──\n"
"  숲 관점의 축이다: 수요(지금 얼마 쓰나) · 상한(이 환경이 얼마까지 되나) · 헤드룸(남은 여유).\n"
"  수요   dev IOPS = 장치 전체, this server IOPS = 이 인스턴스 귀속(/proc/<pid>/io).\n"
"  지연   await = 요청당 대기(ms). IOPS 는 낮은데 await 가 크면 'IOPS 증설'이 아니라\n"
"         '더 빠른 스토리지'가 답이다. queue = 평균 큐 깊이.\n"
"  상한   포화(사용률 90%% 이상) 또는 큐가 쌓인 순간의 IOPS 를 그 환경의 실효 상한으로 기억한다.\n"
"         **하나의 숫자로 기억하지 않는다.** 같은 장치라도 4KB 랜덤과 128KB 순차는 IOPS 상한이\n"
"         자릿수로 다르므로, I/O 프로파일 = 평균 요청 크기(<8KB / 8-64KB / >64KB) x 읽기 비중\n"
"         (write-heavy <30%% / mixed / read-heavy >70%%) 9칸으로 나눠 칸마다 따로 기억하고,\n"
"         지금 프로파일과 **같은 칸**의 상한하고만 비교한다. 다른 칸 값을 빌려 쓰면\n"
"         (128KB 로 잰 상한을 4KB 워크로드에 적용) '여유 있다' 는 거짓 판정이 나온다.\n"
"         --capacity-state FILE 로 보존하면 재실행해도 칸별 상한을 이어서 판정한다.\n"
"         그 칸이 한 번도 포화하지 않았으면 '이 칸은 미관측'으로 표기한다(추정을 사실로 위장하지 않는다).\n"
"  버퍼   히트율의 출처가 두 가지다.\n"
"         perfmon   엔진 카운터가 **이미 돌고 있을 때**(statdump·csql ;histo 등 watcher 존재)\n"
"                   그 값을 읽는다. 우리가 watcher 를 만들지는 않는다(무침습 절대 조건).\n"
"         turnover  카운터가 정지면 BCB 스냅샷 두 장의 VPID 집합 차이로 회전율을 낸다.\n"
"                   적재 pg/s = 미스, 재적재%% = 빠졌다 다시 들어온 비율(작업집합>버퍼 신호).\n"
"  판정   ⚠ 용량 한계 = 헤드룸 80%%↑ **그리고** 장치가 실제로 밀릴 때(await 20ms↑ 또는 큐 10↑).\n"
"         헤드룸만 높고 적체가 없으면 '상한 근접 — 한계 단정 불가'로만 말하고 증설을 권고하지 않는다.\n"
"         상한은 과거 다른 순간의 관측치라 워크로드가 가벼워지기만 해도 80%%를 넘길 수 있기 때문이다.\n"
"         · ⚠ 지연 병목(await 20ms↑ & IOPS 낮음)\n"
"         ⚠ 버퍼 부족(적재 많고 재적재 30%%↑) · ⚠ 스캔(적재 많고 재적재 없음 — 증설 무효)\n"
"         · 과다 할당(버퍼 60%% 미사용 & 적재 거의 없음 — 축소 가능) · · 여유\n"
"  비용   취합 예산 0.5초. 넘으면 그 프레임 값을 버리고 '취합 생략'을 화면에 남긴다.\n"
"         실측 0.2~0.9ms(예산의 0.2%% 미만). 대상 프로세스 쓰기 0, 접속·락 0.\n"
"\n── ④′ 버퍼풀 상태 — 페이지 슬롯 = 버퍼 제어블록(BCB, Buffer Control Block) (◌) ──\n"
"  pgbuf_Pool.BCB_table 배열을 방법 B 와 같은 경로로 통째 읽어 어떤 페이지가 버퍼에\n"
"  올라와 있고(resident) 몇 장이 아직 볼륨에 안 내려갔는지(dirty) 센다. 래치 없는 스냅샷.\n"
"  게이지: 전경=상주율, 배경(청색)=dirty 비율.  zones: hot/warm/cold = LRU 3구역,\n"
"  void = 적재/축출 전이 중, free = 빈 BCB.  victim 은 cold 에서 뽑고 dirty 는 건너뛴다.\n"
"  log: append = 로그 꼬리, flushed(nxio) = 디스크에 내려간 경계 → 차이 = 아직 fsync 안 된 로그.\n"
"  두 가지 '뒤처짐' 을 구분한다 — 둘 다 로그 꼬리(append) 기준이지만 대상이 다르다:\n"
"    fsync 지연 = append − flushed(nxio)     로그 자체가 디스크에 안 내려간 거리.\n"
"                 커밋 응답이 직접 느려지므로 더 급한 신호다.\n"
"    flush 지연 = append − oldest dirty LSA  데이터 페이지의 볼륨 반영이 뒤처진 거리\n"
"                 = 장애 시 redo 해야 할 길이. 정상에서도 0 보다 크며 크기보다 추세를 본다.\n"
"                 dirty 개수와 다르다 — 개수가 적어도 오래된 한 장이 남으면 이 값은 크다.  자기검증: VPID 이상치 1% · 헤더 읽기실패 50% · **재검 후** 잔여 불일치 5%(50% 초과면 비활성, 그 사이는 검증분만 집계).\n"
"  --bcb-dump F : 스냅샷을 파일로 → cub_volmap --bufmap F 로 볼륨 지도 위에 버퍼 상주/dirty 를 얹는다.\n"
"\f\n── ⑤ 메모리 추이 박스의 load(OS) 겹침 ──\n"
"  분홍=서버 전체, 적색=동적 힙, 노랑=load(OS, 1분). load 는 단위가 달라\n"
"  자체 최대 기준으로 모양만 겹치고, 실제 값·범위는 우측 범례에 있다.\n"
"  읽는 법: 메모리가 늘 때 load 도 같이 오르면 부하 기인 —\n"
"           load 는 평탄한데 메모리만 계속 오르면 누수를 의심할 것.\n"
"\f\n── ⑥ I/O 박스 — iowait · D스레드 · 항목 · 판정문 ──\n"
"  wa  iowait      ※ 'CPU 가 I/O 를 기다리며 바빴다' 가 아니다.\n"
"                  CPU 가 놀고 있었고(idle) 동시에 그 CPU 에 디스크 대기 태스크가\n"
"                  있었던 시간의 비율 — 즉 idle 의 하위 분류다. 함정 둘:\n"
"                    · 코어 수로 나뉜다. 64코어에서 한 프로세스가 디스크를 완전히\n"
"                      포화시켜도 wa 는 1~2% 로밖에 안 보인다\n"
"                    · CPU 가 바쁘면 idle 이 없어 wa 도 0 이 된다 → I/O 병목이 가려진다\n"
"                  그래서 wa 단독으로 판단하지 말고 아래 조합으로 읽을 것:\n"
"                    wa 낮음 + dev util 높음  CPU 가 바빠 wa 가 가려진 상태. 병목 가능성 있음\n"
"                    wa 높음 + dev util 높음  디스크 병목이 확실 — data_buffer 부족을 의심\n"
"                    wa 높음 + dev util 낮음  디스크가 아닌 대기이거나 측정 구간이 짧다\n"
"                    majflt 증가             버퍼에 없어 디스크에서 읽은 것 — 증설의 직접 근거\n"
"                  dev util 과 majflt 는 코어 수에 희석되지 않는 물리량이라 더 믿을 만하다.\n"
"                  ※ I/O 박스가 이 조합을 대신 판정해 한 줄로 알려준다(아래 참조).\n"
"  D스레드 n/m     iowait 행 오른쪽. cub_server 스레드 m 개 중 n 개가 D 상태\n"
"                  (디스크 응답을 기다리는 중단 불가 대기)라는 뜻이다.\n"
"                  wa 와 달리 코어 수로 희석되지 않아, 대기가 있으면 반드시 잡힌다.\n"
"                  0 이 아니면 그 순간 실제로 디스크에 막혀 있다는 직접 증거다.\n"
"  cache absorb    요청 읽기 중 디스크로 안 내려간 비율. 0% 면 캐시가 무효\n"
"  dev util        장치 포화도 — I/O 가 많은지의 1차 판정. 100%면 포화\n"
"  blkio wait      이 서버가 디스크를 기다린 시간의 비율(delayacct_blkio_ticks).\n"
"                  전 스레드 합이라 한 코어(100%%)를 넘을 수 있다 — CPU 사용률과\n"
"                  같은 규약이다. cub_server 는 워커가 I/O 를 하므로 반드시 스레드를\n"
"                  모두 더해야 한다(메인 스레드만 보면 늘 0 이다).\n"
"                  pidstat -d 의 iodelay 와 같은 출처이며, 그쪽은 기본이 메인 스레드\n"
"                  기준이라(-t 로 스레드별) 값이 다르게 보일 수 있다.\n"
"                  비동기 대기는 안 잡히므로 0 을 '문제 없음'으로 단정하지 말 것.\n"
"\f\n── I/O 판정문 (I/O 박스 맨 아래) ──\n"
"  wa·장치 사용률·인스턴스 신호 세 가지를 조합해 결론을 한 줄로 낸다. 조건이\n"
"  풀려도 지워지지 않고 시각과 함께 최근 2건까지 남는다(지난 것은 회색) —\n"
"  I/O 이상은 순간적이라 지워지면 '봤는데 없어졌다' 가 되기 때문이다.\n"
"\n"
"  판정에 쓰는 세 조건\n"
"    (1) wa 높음    iowait >= 100/코어수 x 0.5 (%%). 코어 수에 맞춰 자동 조정된다 —\n"
"                   64코어면 0.78%%, 32코어면 1.6%%, 4코어면 12.5%% (하한 0.5%%).\n"
"                   고정 3%% 같은 기준은 코어가 많으면 도달하지 않아 병목을 놓친다.\n"
"    (2) 장치 바쁨  최다사용 장치의 사용률 >= 80%%. 장치를 식별했을 때만 성립한다.\n"
"    (3) 내 신호    이 인스턴스가 실제로 I/O 중이라는 증거. 넷 중 하나면 성립:\n"
"                   majflt >= 1/s · D스레드 > 0 · blkio >= 1%% · 이 서버 io >= 1MB/s\n"
"                   ※ 컨테이너(overlay)에서는 DB 파일의 백킹 장치를 diskstats 에서\n"
"                     못 찾아 (2)가 인스턴스 부하와 이어지지 않는다. 그래서 (3)에\n"
"                     이 서버의 실제 읽기/쓰기량을 반드시 포함한다.\n"
"\n"
"  나올 수 있는 경우 — 세 조건의 8조합이 판정 5종으로 모인다\n"
"   (1)(2)(3)  판정\n"
"    O  O  O   ⚠ 디스크 병목 — 셋 다 성립. 가장 확실한 신호다.\n"
"              data_buffer 를 늘려 디스크 읽기를 줄이는 것이 1차 대응이다.\n"
"    X  O  O   ⚠ wa 가 가려짐 — 장치는 바쁜데 wa 가 낮다. CPU 가 바빠 idle 이\n"
"              없으면 wa 도 0 이 된다(wa 는 idle 의 하위 분류). 병목일 수 있다.\n"
"    O  X  O   · 비디스크 대기 — wa 는 있는데 장치는 한가. 네트워크·NFS 대기이거나\n"
"    O  X  X     측정 구간이 짧아 장치 통계가 아직 안 잡힌 경우다.\n"
"    X  O  X   · 다른 프로세스의 부하 — 장치는 바쁘지만 이 인스턴스는 조용하다.\n"
"    O  O  X     CUBRID 문제가 아니므로 같은 호스트의 다른 프로세스를 볼 것.\n"
"    X  X  O   (표시 없음) — 이 서버가 I/O 를 하지만 wa 도 장치도 한가하다.\n"
"    X  X  X   (표시 없음) — 유휴. 정상 상태다.\n"
"\n"
"  읽는 순서\n"
"    1) ⚠(적색·주황)이면 그 줄만 읽어도 결론이 난다.\n"
"    2) ·(회색)이면 이 인스턴스의 문제가 아니거나 판단하기 이른 상태다.\n"
"    3) 아무것도 없으면 I/O 는 정상이다 — 느리다면 CPU(OS 박스)나 잠금을 볼 것.\n"
"\ncaps: 줄은 이 커널에서 사용 가능한 인터페이스와 폴백 상태를 보여줍니다.\n"
"자세한 한계는 docs/limitations.md, 이식성은 docs/portability.md 참조.");
}

/* ---- Per-mode key help ----
   Live h leads with the keys of the mode in view (dashboard or time series);
   pouring in both pushes the section actually wanted further down.
   Only the switch key (p) appears in both, so either can be reached. */
/* ---- Per-mode help ----
   Each mode's help is self-contained: no description of another mode's screen,
   while what applies to both (observation grades, environment variables,
   instances) appears in each.  The order follows the screen, top to bottom. */
static void help_keys_dash(void){
    hout(
"── 대시보드(-b) · 이 화면이 무엇을 보여주나 ──\n"
"\n"
"  cub_server 프로세스의 메모리를 /proc 만 읽어 분해한다. 접속·쿼리·쓰기 0(무침습).\n"
"  핵심 질문은 하나다 — \"이 메모리가 왜 이만큼인가, 늘고 있다면 어디가 늘었나\".\n"
"\n"
"  화면은 위에서 아래로 여섯 상자다. 아래 설명도 그 순서다.\n"
"\n"
"    맨 윗줄        수집 시간(이 프레임에 든 실작업 ms) · 현재 시각\n"
"    ① OS 컨텍스트  호스트 CPU·메모리 · 가동 인스턴스 목록(▶ = 지금 보는 DB)\n"
"    ② PSS·프로세스 master / server / pl / broker / CAS 티어별 실점유와 코어\n"
"    ③ RSS·서버     영역별 alloc·rss·hot·grow — 1/2/3 키로 보기 전환\n"
"    ④ 동적 메모리  [B] 정확 · [A] 청크 추정 · ④' 버퍼풀 상태 · ⓐ 용량\n"
"    ⑤ 메모리 추이  서버 전체·동적 힙 증감 + load(OS) 겹침\n"
"    ⑥ I/O          iowait · 장치 · D스레드 → 맨 아래 한 줄 판정문\n"
"\n"
"  읽는 순서: ① 호스트가 괜찮은지 → ②③ 서버가 얼마나 쓰는지 →\n"
"  ③ 제목 옆 \"설정으로 설명 N%%\" 가 낮으면 ④ 로 내려가 무엇이 차지하는지 →\n"
"  느리다는 신고가 있으면 ⑥ 판정문부터.\n"
"\f\n"
"── ① OS 컨텍스트 — CPU 와 load ──\n"
"  CPU 32c/64t     물리 32 코어 / 논리 64 스레드. 다르면 하이퍼스레딩이다.\n"
"                  물리보다 많은 부하는 같은 코어를 나눠 쓰는 것이라 선형으로 안 빨라진다.\n"
"  total|used|pct  total=논리코어, used=사용 중 코어 수, pct=전체 사용률.\n"
"                  used 는 '몇 코어어치가 돌고 있나' 라 프로세스 박스의 코어 수와 바로 비교된다.\n"
"  막대(게이지)    코어를 바쁜 순으로 정렬해 왼쪽부터 그린 것 — 합계가 아니라 분포다.\n"
"                  색으로 구분한다 — 적색=포화(≥95%%) 주황=높음(≥50%%)\n"
"                  녹색=중간(≥10%%) 회색=유휴. 글리프는 다른 게이지와 같다.\n"
"                  used 12 이 12코어에 고르게 퍼진 것인지 6개가 100%%인지는 합계로는\n"
"                  구분되지 않는다 — 뒤엣것이 병목이고, 왼쪽 붉은 구간의 폭이 그것이다.\n"
"                  ※ 코어가 막대 폭보다 많으면 한 칸이 여러 코어를 대표하며 그 구간의\n"
"                    최댓값을 그린다(포화를 평균으로 감추지 않기 위함). 그래서 칸 수에\n"
"                    코어 수를 곱해 읽으면 안 된다 — 정확한 개수는 옆의 sat 숫자를 볼 것.\n"
"  sat N           포화 코어 수 — 사용률 95%% 이상인 코어가 몇 개인가.\n"
"                  포화 직전(50~95%%)은 막대의 주황 구간이 보여준다.\n"
"                  btop 의 코어별 미터에서 100%% 칸을 세는 것과 같은 값이다.\n"
"                  이 값이 물리코어 수에 근접하면 CPU 가 병목이다. 반대로 used 는 큰데\n"
"                  sat 이 0 이면 부하가 고르게 퍼진 것이라 아직 여유가 있다.\n"
"  us  user        사용자 코드. 질의 처리·정렬·조인 등 엔진이 실제 일한 몫\n"
"  sy  system      커널. I/O 발행, 락, 컨텍스트 스위치. us 대비 과도하면 경합을 의심\n"
"  load 1/5/15     최근 1·5·15분 평균 실행대기 태스크 수. CPU 사용률이 아니다 —\n"
"                  리눅스에서는 D 상태(디스크 대기)도 포함하므로, CPU 가 한가해도\n"
"                  I/O 가 막히면 올라간다. 논리코어 수(64)를 넘으면 대기가 쌓이는 중.\n"
"                  전체 호스트 값이라 인스턴스별로 나눌 수 없어 박스 제목에 둔다.\n"
"\f\n"
"── ② 프로세스 박스가 PSS 인 이유 · 티어별 CPU ──\n"
"  PSS = 공유 페이지를 쓰는 프로세스 수로 나눠 배분한 상주량.\n"
"  libcubrid.so 10M 을 5개가 공유하면 각자 2M 로 잡힌다 → 행을 다 더해도 이중 계상이\n"
"  없어 All 합계가 성립한다. RSS 로 재면 같은 페이지가 여러 번 세어져 합이 부풀려진다.\n"
"  프로세스 박스   각 티어(master/server/pl/broker/CAS) 앞의 숫자가 그 티어의 코어 수.\n"
"                  전부 더하면 OS 의 used 이하여야 정상이다(나머지는 CUBRID 밖 프로세스).\n"
"                  색으로 크기를 구분한다 — 회색 · =유휴, 청록 <1코어, 노랑 1코어 이상,\n"
"                  주황=눈에 띄는 부하, 적색=물리코어 수 초과. 0 은 숫자 대신 · 로 눌러\n"
"                  실제로 돌고 있는 티어만 눈에 들어오게 했다.\n"
"  ※ 다른 도구와 대조할 때\n"
"    · server 의 CPU 는 top 과 같은 '1코어=100%' 규약이라 100% 를 넘을 수 있다.\n"
"      OS 박스의 코어 수와 견주려면 100 으로 나눌 것(terse 는 tier.*_cores 로 이미 환산).\n"
"    · Memory 의 used 는 MemTotal-MemAvailable 로 top 과 같은 정의다.\n"
"      htop 은 Shmem 을 달리 처리해 수 GB 낮게 나오는데, 오차가 아니라 정의 차이다.\n"
"\f\n"
"── ③ RSS · 서버 — 각 항목이 알려주는 것 ──\n"
"  data_buffer     페이지 버퍼. 설정보다 큰 게 정상(BCB·victim 배열이 같은 매핑에 포함)\n"
"  log_buffer      로그 버퍼. 선할당이라 설정≈상주\n"
"  dynamic-heap    정렬·캐시·세션이 섞인 동적 힙 → 아래 [B]/[A] 로 분해\n"
"  thread stacks   워커 스택. 예약 크고 상주 극소가 정상\n"
"  glibc arena     할당자 예약. 상주 0이면 물리 메모리 미사용\n"
"  (설정 X, 할당 Y%)  설정 대비 매핑 비율. >105% 면 초과\n"
"  핫 N%           상주 중 실제 접근 비율. 낮으면 올려두고 안 만지는 메모리\n"
"  Σ 합계          4열의 합. rss 합 = 프로세스 RSS(항등). 프로세스 박스의 PSS 와는\n"
"                  공유 .so 배분만큼 다르며(오류 아님), 그 값이 라벨에 함께 적힌다.\n"
"\n"
"  1/2/3 키로 바꾸는 것은 게이지·퍼센트와 3·4번째 열뿐이다(영역 행은 그대로).\n"
"\n"
"    보기          alloc      rss        3번째 열         grow(4번째 열)\n"
"    ───────────   ────────   ────────   ──────────────   ─────────────────────\n"
"    1 할당·상주   할당       상주       hot(접근량)      rss 증감      (주황)\n"
"    2 핫          할당       상주       hot(접근량)      hot 증감   (+녹 / -적)\n"
"    3 설정 초과   할당       상주       conf(설정값)     alloc 증감    (주황)\n"
"\n"
"  헤더 색이 현재 보기를 알린다: hot·grow 녹색 = 보기2, conf 주황 = 보기3.\n"
"  hot 과 grow 는 다른 지표다 — hot=최근 얼마나 만졌나(Referenced), grow=얼마나 늘었나.\n"
"    · 잡아둔 버퍼를 읽기만 하면 hot 크고 grow 0   → data_buffer 의 정상 모습\n"
"    · hot 낮은데 grow 만 계속 오르면              → 누수 의심\n"
"  게이지: 전경 6점 글리프 = 보기 지표(녹→적), 배경 청색 = 핫 범위\n"
"\f\n"
"── ③ 파라미터 귀속률 \"설정으로 설명 N%%\" (③ 박스 제목 옆) ──\n"
"  서버 메모리 중 cubrid.conf 설정으로 '왜 이만큼인지' 설명되는 비율.\n"
"    귀속   data_buffer(data_buffer_size), log_buffer(log_buffer_size)\n"
"    비귀속 dynamic-heap, thread stacks, code(.so) 등 — 설정으로 통제되지 않는 부분\n"
"  읽는 법:\n"
"    높다(90%대)   정상. 메모리 대부분이 설정대로다\n"
"    떨어진다      dynamic-heap 이 자라고 있다는 뜻 — 아래 heap 박스로 내려가\n"
"                  무엇이 늘었는지 확인할 것([a] 로 방법 A 를 돌리면 더 쪼개진다)\n"
"  즉 이 값은 'heap 박스를 들여다볼 필요가 있는가' 를 알려주는 신호다.\n"
"\n"
"── 관측 등급 (값 왼쪽 1글자) ──\n"
"  ● 측정    OS/엔진이 직접 보고. 신뢰 가능\n"
"  ◐ 예약    매핑만 확인 — 실사용과 크게 다를 수 있음\n"
"  ◌ 추정    크기 패턴 추정 — 상대 비중만\n"
"  ? 미귀속  아직 분해되지 않음 — 해석하지 말 것\n"
"\f\n"
"── ④ 동적 메모리 상세(= glibc malloc 힙) — B=정확 · A=크기 추정 ──\n"
"  ※ 여기서 '힙'은 프로세스의 동적 할당 영역(malloc)이다. 테이블 데이터가 저장되는\n"
"     스토리지의 '힙 파일'(HFID)과는 다른 것이며, 그쪽은 cub_volmap 이 다룬다.\n"
"  [B] 항목        엔진이 직접 보고하는 값 → 정확. plan·결과·필터 캐시·락·세션·카탈로그·연결\n"
"  lock table (tran)  lk_Gl.tran_lock_table — 트랜잭션별 락 보유 슬롯이다.\n"
"                  num_trans x 152B 로, 접속 상한에 비례해 미리 잡힌다.\n"
"                  ※ cubrid lockdb 의 \"Object Lock Table\" 과는 다른 구조다.\n"
"                    저쪽은 잠긴 객체의 LK_ENTRY/LK_RES 풀(예: 1000개 242K)이고\n"
"                    이쪽은 트랜잭션 슬롯(예: 101개 15K)이라 값이 다른 것이 정상이다.\n"
"  [A] 항목        glibc 청크 분포 추정. 정렬·해시 작업버퍼 등 B가 못 잡는 것\n"
"  [A] overcount   A 합계가 상주 초과(free 청크·비상주 포함) — 정상, 상한으로 읽을 것\n"
"\n"
"── ④' 버퍼풀 상태 — 페이지 슬롯 = 버퍼 제어블록(BCB, Buffer Control Block) (◌) ──\n"
"  pgbuf_Pool.BCB_table 배열을 방법 B 와 같은 경로로 통째 읽어 어떤 페이지가 버퍼에\n"
"  올라와 있고(resident) 몇 장이 아직 볼륨에 안 내려갔는지(dirty) 센다. 래치 없는 스냅샷.\n"
"  게이지: 전경=상주율, 배경(청색)=dirty 비율.  zones: hot/warm/cold = LRU 3구역,\n"
"  void = 적재/축출 전이 중, free = 빈 BCB.  victim 은 cold 에서 뽑고 dirty 는 건너뛴다.\n"
"  log: append = 로그 꼬리, flushed(nxio) = 디스크에 내려간 경계 → 차이 = 아직 fsync 안 된 로그.\n"
"  두 가지 '뒤처짐' 을 구분한다 — 둘 다 로그 꼬리(append) 기준이지만 대상이 다르다:\n"
"    fsync 지연 = append − flushed(nxio)     로그 자체가 디스크에 안 내려간 거리.\n"
"                 커밋 응답이 직접 느려지므로 더 급한 신호다.\n"
"    flush 지연 = append − oldest dirty LSA  데이터 페이지의 볼륨 반영이 뒤처진 거리\n"
"                 = 장애 시 redo 해야 할 길이. 정상에서도 0 보다 크며 크기보다 추세를 본다.\n"
"                 dirty 개수와 다르다 — 개수가 적어도 오래된 한 장이 남으면 이 값은 크다.  자기검증: VPID 이상치 1% · 헤더 읽기실패 50% · **재검 후** 잔여 불일치 5%(50% 초과면 비활성, 그 사이는 검증분만 집계).\n"
"  --bcb-dump F : 스냅샷을 파일로 → cub_volmap --bufmap F 로 볼륨 지도 위에 버퍼 상주/dirty 를 얹는다.\n"
"\f\n"
"── ⓐ 용량(capacity) — 증설·축소·개선을 무엇으로 판단하나 ──\n"
"  숲 관점의 축이다: 수요(지금 얼마 쓰나) · 상한(이 환경이 얼마까지 되나) · 헤드룸(남은 여유).\n"
"  수요   dev IOPS = 장치 전체, this server IOPS = 이 인스턴스 귀속(/proc/<pid>/io).\n"
"  지연   await = 요청당 대기(ms). IOPS 는 낮은데 await 가 크면 'IOPS 증설'이 아니라\n"
"         '더 빠른 스토리지'가 답이다. queue = 평균 큐 깊이.\n"
"  상한   포화(사용률 90%% 이상) 또는 큐가 쌓인 순간의 IOPS 를 그 환경의 실효 상한으로 기억한다.\n"
"         **하나의 숫자로 기억하지 않는다.** 같은 장치라도 4KB 랜덤과 128KB 순차는 IOPS 상한이\n"
"         자릿수로 다르므로, I/O 프로파일 = 평균 요청 크기(<8KB / 8-64KB / >64KB) x 읽기 비중\n"
"         (write-heavy <30%% / mixed / read-heavy >70%%) 9칸으로 나눠 칸마다 따로 기억하고,\n"
"         지금 프로파일과 **같은 칸**의 상한하고만 비교한다. 다른 칸 값을 빌려 쓰면\n"
"         (128KB 로 잰 상한을 4KB 워크로드에 적용) '여유 있다' 는 거짓 판정이 나온다.\n"
"         --capacity-state FILE 로 보존하면 재실행해도 칸별 상한을 이어서 판정한다.\n"
"         그 칸이 한 번도 포화하지 않았으면 '이 칸은 미관측'으로 표기한다(추정을 사실로 위장하지 않는다).\n"
"  버퍼   히트율의 출처가 두 가지다.\n"
"         perfmon   엔진 카운터가 **이미 돌고 있을 때**(statdump·csql ;histo 등 watcher 존재)\n"
"                   그 값을 읽는다. 우리가 watcher 를 만들지는 않는다(무침습 절대 조건).\n"
"         turnover  카운터가 정지면 BCB 스냅샷 두 장의 VPID 집합 차이로 회전율을 낸다.\n"
"                   적재 pg/s = 미스, 재적재%% = 빠졌다 다시 들어온 비율(작업집합>버퍼 신호).\n"
"  판정   ⚠ 용량 한계 = 헤드룸 80%%↑ **그리고** 장치가 실제로 밀릴 때(await 20ms↑ 또는 큐 10↑).\n"
"         헤드룸만 높고 적체가 없으면 '상한 근접 — 한계 단정 불가'로만 말하고 증설을 권고하지 않는다.\n"
"         상한은 과거 다른 순간의 관측치라 워크로드가 가벼워지기만 해도 80%%를 넘길 수 있기 때문이다.\n"
"         · ⚠ 지연 병목(await 20ms↑ & IOPS 낮음)\n"
"         ⚠ 버퍼 부족(적재 많고 재적재 30%%↑) · ⚠ 스캔(적재 많고 재적재 없음 — 증설 무효)\n"
"         · 과다 할당(버퍼 60%% 미사용 & 적재 거의 없음 — 축소 가능) · · 여유\n"
"  비용   취합 예산 0.5초. 넘으면 그 프레임 값을 버리고 '취합 생략'을 화면에 남긴다.\n"
"         실측 0.2~0.9ms(예산의 0.2%% 미만). 대상 프로세스 쓰기 0, 접속·락 0.\n"
"\n"
"── ⑤ 메모리 추이 박스의 load(OS) 겹침 ──\n"
"  분홍=서버 전체, 적색=동적 힙, 노랑=load(OS, 1분). load 는 단위가 달라\n"
"  자체 최대 기준으로 모양만 겹치고, 실제 값·범위는 우측 범례에 있다.\n"
"  읽는 법: 메모리가 늘 때 load 도 같이 오르면 부하 기인 —\n"
"           load 는 평탄한데 메모리만 계속 오르면 누수를 의심할 것.\n"
"\f\n"
"── ⑥ I/O 박스 — iowait · D스레드 · 항목 ──\n"
"  wa  iowait      ※ 'CPU 가 I/O 를 기다리며 바빴다' 가 아니다.\n"
"                  CPU 가 놀고 있었고(idle) 동시에 그 CPU 에 디스크 대기 태스크가\n"
"                  있었던 시간의 비율 — 즉 idle 의 하위 분류다. 함정 둘:\n"
"                    · 코어 수로 나뉜다. 64코어에서 한 프로세스가 디스크를 완전히\n"
"                      포화시켜도 wa 는 1~2% 로밖에 안 보인다\n"
"                    · CPU 가 바쁘면 idle 이 없어 wa 도 0 이 된다 → I/O 병목이 가려진다\n"
"                  그래서 wa 단독으로 판단하지 말고 아래 조합으로 읽을 것:\n"
"                    wa 낮음 + dev util 높음  CPU 가 바빠 wa 가 가려진 상태. 병목 가능성 있음\n"
"                    wa 높음 + dev util 높음  디스크 병목이 확실 — data_buffer 부족을 의심\n"
"                    wa 높음 + dev util 낮음  디스크가 아닌 대기이거나 측정 구간이 짧다\n"
"                    majflt 증가             버퍼에 없어 디스크에서 읽은 것 — 증설의 직접 근거\n"
"                  dev util 과 majflt 는 코어 수에 희석되지 않는 물리량이라 더 믿을 만하다.\n"
"                  ※ I/O 박스가 이 조합을 대신 판정해 한 줄로 알려준다(아래 참조).\n"
"  D스레드 n/m     iowait 행 오른쪽. cub_server 스레드 m 개 중 n 개가 D 상태\n"
"                  (디스크 응답을 기다리는 중단 불가 대기)라는 뜻이다.\n"
"                  wa 와 달리 코어 수로 희석되지 않아, 대기가 있으면 반드시 잡힌다.\n"
"                  0 이 아니면 그 순간 실제로 디스크에 막혀 있다는 직접 증거다.\n"
"  cache absorb    요청 읽기 중 디스크로 안 내려간 비율. 0% 면 캐시가 무효\n"
"  dev util        장치 포화도 — I/O 가 많은지의 1차 판정. 100%면 포화\n"
"  blkio wait      이 서버가 디스크를 기다린 시간의 비율(delayacct_blkio_ticks).\n"
"                  전 스레드 합이라 한 코어(100%%)를 넘을 수 있다 — CPU 사용률과\n"
"                  같은 규약이다. cub_server 는 워커가 I/O 를 하므로 반드시 스레드를\n"
"                  모두 더해야 한다(메인 스레드만 보면 늘 0 이다).\n"
"                  pidstat -d 의 iodelay 와 같은 출처이며, 그쪽은 기본이 메인 스레드\n"
"                  기준이라(-t 로 스레드별) 값이 다르게 보일 수 있다.\n"
"                  비동기 대기는 안 잡히므로 0 을 '문제 없음'으로 단정하지 말 것.\n"
"\f\n"
"── ⑥ I/O 판정문 (I/O 박스 맨 아래) ──\n"
"  wa·장치 사용률·인스턴스 신호 세 가지를 조합해 결론을 한 줄로 낸다. 조건이\n"
"  풀려도 지워지지 않고 시각과 함께 최근 2건까지 남는다(지난 것은 회색) —\n"
"  I/O 이상은 순간적이라 지워지면 '봤는데 없어졌다' 가 되기 때문이다.\n"
"\n"
"  판정에 쓰는 세 조건\n"
"    (1) wa 높음    iowait >= 100/코어수 x 0.5 (%%). 코어 수에 맞춰 자동 조정된다 —\n"
"                   64코어면 0.78%%, 32코어면 1.6%%, 4코어면 12.5%% (하한 0.5%%).\n"
"                   고정 3%% 같은 기준은 코어가 많으면 도달하지 않아 병목을 놓친다.\n"
"    (2) 장치 바쁨  최다사용 장치의 사용률 >= 80%%. 장치를 식별했을 때만 성립한다.\n"
"    (3) 내 신호    이 인스턴스가 실제로 I/O 중이라는 증거. 넷 중 하나면 성립:\n"
"                   majflt >= 1/s · D스레드 > 0 · blkio >= 1%% · 이 서버 io >= 1MB/s\n"
"                   ※ 컨테이너(overlay)에서는 DB 파일의 백킹 장치를 diskstats 에서\n"
"                     못 찾아 (2)가 인스턴스 부하와 이어지지 않는다. 그래서 (3)에\n"
"                     이 서버의 실제 읽기/쓰기량을 반드시 포함한다.\n"
"\n"
"  나올 수 있는 경우 — 세 조건의 8조합이 판정 5종으로 모인다\n"
"   (1)(2)(3)  판정\n"
"    O  O  O   ⚠ 디스크 병목 — 셋 다 성립. 가장 확실한 신호다.\n"
"              data_buffer 를 늘려 디스크 읽기를 줄이는 것이 1차 대응이다.\n"
"    X  O  O   ⚠ wa 가 가려짐 — 장치는 바쁜데 wa 가 낮다. CPU 가 바빠 idle 이\n"
"              없으면 wa 도 0 이 된다(wa 는 idle 의 하위 분류). 병목일 수 있다.\n"
"    O  X  O   · 비디스크 대기 — wa 는 있는데 장치는 한가. 네트워크·NFS 대기이거나\n"
"    O  X  X     측정 구간이 짧아 장치 통계가 아직 안 잡힌 경우다.\n"
"    X  O  X   · 다른 프로세스의 부하 — 장치는 바쁘지만 이 인스턴스는 조용하다.\n"
"    O  O  X     CUBRID 문제가 아니므로 같은 호스트의 다른 프로세스를 볼 것.\n"
"    X  X  O   (표시 없음) — 이 서버가 I/O 를 하지만 wa 도 장치도 한가하다.\n"
"    X  X  X   (표시 없음) — 유휴. 정상 상태다.\n"
"\n"
"  읽는 순서\n"
"    1) ⚠(적색·주황)이면 그 줄만 읽어도 결론이 난다.\n"
"    2) ·(회색)이면 이 인스턴스의 문제가 아니거나 판단하기 이른 상태다.\n"
"    3) 아무것도 없으면 I/O 는 정상이다 — 느리다면 CPU(OS 박스)나 잠금을 볼 것.\n"
"\f\n"
"── 대시보드 · 키 ──\n"
"\n"
"    키            동작\n"
"    ───────────   ─────────────────────────────────────────────────────────\n"
"    1  2  3       ③ 보기 전환   1 할당·상주   2 핫(최근 접근)   3 설정 초과\n"
"    r             ④ [A] 청크 추정을 지금 1회 다시 측정\n"
"    a             ④ [A] 자동 측정 켬/끔 (주기는 측정 시간에 맞춰 자동)\n"
"    <  >          인스턴스(DB) 이동 — 여러 cub_server 가 떠 있을 때\n"
"    p             시계열 화면으로 (그동안 쌓인 히스토리가 바로 그려진다)\n"
"    space         일시정지 / 재개\n"
"    l             한글 / 영문 라벨 전환\n"
"    h             이 도움말        q   종료\n"
"\n"
"  ※ 한글 입력 상태로 눌러도 된다 — ㅂ=q  ㅗ=h  ㅔ=p  ㅣ=l (자판 위치로 되돌림)\n"
"\n"
"── 환경변수 CUBRID (설정 축이 살아 있으려면) ──\n"
"  CUBRID=<설치경로>  설정 시 시작할 때 한 번 '<경로>/bin/cubrid paramdump <db>' 를 실행해\n"
"  서버가 실제로 적용 중인 파라미터를 읽는다(셸을 거치지 않고 execvp, 최대 5초).\n"
"  없으면 <경로>/conf/cubrid.conf 를 직접 파싱하고, 그것도 없으면 아래가 모두 빠진다:\n"
"    · data_buffer/log_buffer 의 '설정 대비 %' 판정 — 설정값이 없어 초과 여부를 알 수 없다\n"
"    · 파라미터 귀속률 — 설정으로 설명되는 몫을 셀 수 없어 표시되지 않는다\n"
"    · DB 페이지 크기 — data_buffer_size/pages 로 역산한다(실패 시 16KB 로 가정)\n"
"    · 방법 B 자기검증 — num_buffers 와 data_buffer_pages 대조가 불가해 힙 B 가 꺼진다\n"
"    · 렌즈2(설정초과)·하단 '메모리 파라미터' 표 전체\n"
"  즉 RSS/PSS 같은 실측은 그대로 나오지만, '왜 이만큼인가' 를 설명하는 축이 사라진다.\n"
"\n"
"── 여러 데이터베이스(인스턴스) ──\n"
"  cub_top <db명>     그 DB 를 본다.  생략하면 가동 중인 것 중 알파벳순 첫 번째\n"
"  라이브에서 < > 로 이동. 상세 추적은 화면에 보이는 DB 하나뿐이고,\n"
"  나머지는 총량만 집계한다(clear_refs 를 전부에 돌리면 대상 서버가 느려지기 때문).\n"
"  terse 는 instance.<db>.* 로 모든 인스턴스를 내보내며 tracked=1 이 현재 추적 대상이다.\n"
"\n"
"  자세한 한계는 docs/limitations.md, 이식성은 docs/portability.md 참조.");
}
static void help_keys_plot(void){
    hout(
"── 시계열(-p) · 이 화면이 무엇을 보여주나 ──\n"
"\n"
"  대시보드가 \"지금 값\"이라면 이 화면은 \"그 값이 어떻게 움직였나\"다.\n"
"  한 프레임의 숫자로는 알 수 없는 세 가지를 여기서 얻는다:\n"
"\n"
"    1) 추세   지금 값이 원래 그런지, 방금 올라간 것인지\n"
"    2) 상관   메모리가 늘 때 load 도 같이 올랐나(부하 기인) 혼자 올랐나(누수)\n"
"    3) 순서   장치가 먼저 포화됐나, 버퍼 적재가 먼저 늘었나 — 원인과 결과의 방향\n"
"\n"
"  ★ 패널의 범위가 다르다 — 1번은 호스트 전체, 2~4번은 지금 보는 인스턴스다.\n"
"    머신 차원의 문제인지(1번) 이 DB 의 문제인지(2~4번)를 한 화면에서 가른다.\n"
"\n"
"    맨 윗줄   db · 관측창(샘플 수·간격) · 인스턴스 위치 · 현재 시각\n"
"    1 호스트 리소스   CPU 코어·포화·RAM·장치·load          [호스트 전체]  + 판정문\n"
"    2 인스턴스 메모리 왜·어디서 느나 (할당 vs 상주, load)  [이 인스턴스] + 판정문\n"
"    3 올리는 쪽       볼륨→버퍼, 버퍼가 일하나             [이 인스턴스] + 판정문\n"
"    4 내리는 쪽       버퍼→볼륨, 쓰기가 밀리나             [이 인스턴스] + 판정문\n"
"    맨 아랫줄 시간축 눈금 · 키 안내\n"
"\n"
"  공통 규약\n"
"    · 관측창은 자동 확장된다 — 시작 직후 초 단위, 오래 두면 분 단위(제목에 표기).\n"
"    · 범례는 \"이름 현재값 (최소~최대)\" 다. 모양은 그래프, 숫자는 범례에서 읽는다.\n"
"    · 3·4 패널의 회색 겹침선은 호스트 값이다(장치%%, await) — 주 계열은 이 인스턴스,\n"
"      겹침은 환경. 둘을 대조해 \"장치는 포화인데 내 인스턴스는 조용\"(남의 부하)을 읽는다.\n"
"    · 1·3·4 패널은 계열마다 단위가 달라 계열별로 정규화한다(높이끼리 비교 금지).\n"
"      2 패널은 공통 바이트 축이라 높이 비교가 유효하다.\n"
"    · 각 패널 아래 한 줄 판정문:  ⚠ = 조치 신호   · = 사실 기록\n"
"    · 표본 2개 미만이면 \"표본 수집 중…\" 으로 판정을 보류한다(추측하지 않는다).\n"
"\f\n"
"── 1 호스트 리소스 [호스트 전체] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    사용 코어 (하늘)      코어         호스트에서 실제 돌고 있는 코어 수(전체 프로세스)\n"
"    포화 코어 (적색)      개           사용률 95%% 이상 코어 수. 물리코어에 근접하면 CPU 가 제약\n"
"    호스트 RAM 사용 (자주)바이트       MemTotal-MemAvailable. 92%% 넘으면 여유가 적다\n"
"    CUBRID 총합 (분홍)    바이트       CUBRID 프로세스 전체 PSS 합 — RAM 중 우리 몫\n"
"    장치 사용률 (주황)    %%            최다사용 장치 포화도(호스트 전역). 80%%↑ 면 스토리지가 제약\n"
"    장치 IOPS (노랑)      회/s         호스트 장치 전체 IOPS. 포화 시점의 값이 그 환경의 실효 상한\n"
"    load (노랑 겹침)      태스크       실행 대기 포함. 코어 수를 넘으면 큐가 쌓인다\n"
"\n"
"  읽는 법 — \"머신이 문제냐, 이 DB 가 문제냐\" 를 먼저 가른다.\n"
"    포화 코어 있음        호스트 CPU 가 제약(판정문: ⚠ 포화 코어 N/N — 호스트 CPU 가 제약)\n"
"    load > 코어 수        실행 대기 큐 — 호스트 전체 값이라 CUBRID 만의 문제로 단정 불가\n"
"    장치 80%↑             호스트 스토리지가 제약 → 이 인스턴스 몫은 3·4번 패널에서 확인\n"
"    호스트 RAM 92%↑       여유 부족. CUBRID 총합과 견주어 우리 몫인지 남의 몫인지 본다\n"
"    CUBRID 총합이 낮은데   같은 호스트의 다른 프로세스가 원인 — CUBRID 를 건드릴 일이 아니다\n"
"    RAM·CPU 가 높다\n"
"\f\n"
"── 2 인스턴스 메모리 [이 인스턴스] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    server(PSS) (분홍)    바이트       이 인스턴스 상주(PSS). 우상향이면 무엇이든 늘고 있다\n"
"    data_buffer (녹색)    바이트       페이지 버퍼 몫. 설정대로면 평탄하다\n"
"    log_buffer (노랑)     바이트       로그 버퍼 몫. 선할당이라 거의 평탄\n"
"    dynamic-heap (적색)   바이트       malloc 힙(정렬·캐시·세션). server 와 같이 오르면 원인이 여기\n"
"    할당(VmSize) (하늘)   바이트       예약. 상주와의 간격 = 잡아뒀지만 안 쓰는 몫\n"
"    load (노랑 겹침)      태스크       호스트 load — 증가가 부하 기인인지 가르는 기준선\n""    heapB 합계 (연녹)     바이트       동적 메모리 중 엔진이 직접 보고하는 몫(정확). 계단형이 정상 —\n"
"                                       conn/lock/session 은 max_clients 비례 선할당이라 고정이고,\n"
"                                       plan·결과·카탈로그 캐시만 실사용에 따라 오른다\n"
"                                       (실측: 유휴 154KB → 질의 6세션 412KB)\n"
"\n"
"  ※ A(크기 추정)는 이 그래프에 없다 — r/a 로만 재고 수백 ms 가 걸려 표본이 띄엄띄엄하다.\n"
"    점 몇 개를 선으로 이으면 없는 추세가 생기므로 넣지 않는다. A 가 필요하면 p 로 대시보드에서 본다.\n"
"    dynamic-heap 은 오르는데 heapB 가 평탄하면 = B 가 못 잡는 몫(A 영역)이 늘고 있다는 신호다.\n"
"\n"
"  읽는 법 — 이 패널의 목적은 \"증가가 부하 때문인가\" 를 가르는 것이다.\n"
"    메모리↑ + load↑      부하 기인(접속·질의 증가로 작업 메모리·캐시가 커진 것)\n"
"                         판정문: ⚠ 서버 +N (구간) · load 도 함께 상승 → 부하 기인 증가\n"
"    메모리↑ + load 평탄   부하와 무관한 증가 = 누수 의심\n"
"                         판정문: ⚠ … load 평탄 → 동적 메모리 상세(B/A)에서 어느 항목인지 확인\n"
"                         → p 로 대시보드에 가서 ④ [B]/[A] 로 항목을 특정한다\n"
"    dynamic-heap 만↑      정렬·캐시·세션 쪽. data_buffer 는 설정이라 이렇게 안 움직인다\n"
"    할당만↑ 상주 평탄     예약만 늘었다 — 물리 메모리는 아직 안 쓴다(문제 아님)\n"
"    메모리↓               작업 메모리·캐시 반환(정상 회수)\n"
"\n"
"  제목 오른쪽: 시간당 증가율(+N/h)과 RAM 대비 점유율. 창이 짧으면 순간 변동이 과대 반영된다.\n"
"\f\n"
"── 3 올리는 쪽 (볼륨 → 버퍼) [이 인스턴스] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    디스크 읽기 (적색)    B/s          볼륨에서 실제로 읽어 올린 양. 버퍼가 일하면 낮다\n"
"    버퍼 상주%% (녹색)     %%            data_buffer 중 페이지가 올라와 있는 비율\n"
"    cold 존%% (주황)       %%            LRU cold 비중. 높으면 재사용 없이 지나가는 페이지가 많다\n"
"    data_buffer 핫%% (하늘)%%            상주 페이지 중 최근 접근된 비율(--no-hot 이면 없음)\n"
"    적재 pg/s (노랑)      pg/s         버퍼에 새로 올린 페이지 수 = 미스율의 직접 지표\n""    장치%%(호스트) (회색)  %%            겹침선. 호스트 전역 장치 포화도(이 인스턴스 값 아님)\n"
"\n"
"  패널 아래 막대 2행 — 선으로는 값이 작아 바닥에 붙어 안 보이던 지표를 따로 깐다\n"
"  (주식 차트의 거래량 자리. 막대는 자기 최대 기준, 백분율은 100 기준):\n"
"    핫%%    상주 페이지 중 최근 접근된 비율\n"
"    흡수%%  버퍼가 미스해도 OS 페이지 캐시가 받아준 비율 — 높으면 디스크로 안 내려간다\n"
"           (실측 호스트 캐시 64.7GB vs data_buffer 512MB)\n"
"    OS 캐시 흡수%% (연노랑) %%           버퍼가 미스해도 OS 페이지 캐시가 받아준 비율.\n"
"\n"
"  읽는 법 — 버퍼 증설이 효과가 있을 상황인지 가른다.\n"
"    적재↑ + 재적재 30%%↑   작업집합 > 버퍼 → 증설이 듣는다 (판정문: ⚠ 버퍼 부족 — 재적재 N%%)\n"
"                          근거는 줌(3) 보조막대의 '재적재' 막대에서 눈으로 확인한다\n"
"    적재↑ + 재적재 없음   한 번 읽고 버리는 스캔 → 키워도 읽기가 줄지 않는다\n"
"                          판정문: ⚠ 재사용 없는 스캔\n"
"    상주 100% + 읽기 0    버퍼에서 다 서비스되는 정상 (판정문: · 버퍼에서 서비스 중)\n"
"    상주 60% 미만 + 적재 0 과다 할당 — 축소 가능 (판정문: · 과다 할당)\n"
"    상주이 오르는 중       버퍼 채우는 중 — 히트율 판단 보류(기동 직후)\n"
"\n"
"  ※ 버퍼풀 상태(BCB) 읽기가 꺼진 환경에서는 상주%·cold%·적재가 없고, 디스크 읽기와\n"
"    캐시 흡수율만으로 판정한다(판정문에 \"버퍼풀 상태 읽기 비활성\" 이 붙는다).\n"
"\f\n"
"── 4 내리는 쪽 (버퍼 → 볼륨) [이 인스턴스] ──\n"
"\n"
"    계열(범례 색)         단위         무엇을 뜻하나 · 어떻게 읽나\n"
"    ───────────────────   ──────────   ─────────────────────────────────────────────────\n"
"    dirty 페이지 (분홍)   pg           아직 볼륨에 안 내려간 버퍼 페이지 수\n"
"    flush 지연 (주황)     로그pg       가장 오래된 dirty 가 로그 꼬리에서 뒤처진 거리\n"
"                                       = 장애 시 redo 해야 할 길이. dirty 페이지(개수)와 다르다 —\n"
"                                       개수가 적어도 오래된 한 장이 안 내려가면 이 값은 크다\n"
"                                       ※ 정상에서도 0 보다 크다. 크기보다 계속 늘어나는지를 볼 것\n"
"    디스크 쓰기 (하늘)    B/s          실제로 볼륨에 내려간 양\n"
"    D스레드 (적색)        개           디스크 응답을 기다리는 스레드 수. 코어 수에 희석되지 않아\n"
"                                       0 이 아니면 그 순간 실제로 막혀 있다는 직접 증거\n"
"    await ms(호스트) (회색) ms           겹침선. 호스트 장치의 요청당 평균 대기(이 인스턴스 값 아님)\n"
"\n"
"  읽는 법 — 쓰기 경로가 따라오지 못하는지, 커밋이 느려질 위험이 있는지.\n"
"    쓰기↑ 인데 1번 장치도↑   쓰기 병목 (판정문: ⚠ 장치 N% · 쓰기 N/s — 쓰기 병목)\n"
"    flush 지연↑ 계속        체크포인트가 밀린다 → 장애 복구 시간이 길어진다\n"
"                            판정문: ⚠ 로그 fsync 지연 N 로그 페이지 — 커밋 지연 위험\n"
"    dirty↑ 인데 쓰기 평탄   내려보내지 못하고 쌓이는 중 — 1번의 장치·이 패널 await 를 함께\n"
"    await↑(겹침) 인데       스토리지 자체가 느리다. 증설이 아니라 교체가 답이다\n"
"    내 쓰기는 평탄\n"
"    D스레드 > 0 지속        이 인스턴스가 실제로 디스크에 막혀 있다 — wa 가 0 이어도 확실한 증거\n""\n"
"  flush 지연 과 fsync 지연 — 둘 다 \"로그 꼬리(append)에서 얼마나 뒤처졌나\" 이지만 대상이 다르다.\n"
"\n"
"    이름          계산                      무엇이 뒤처진 것인가          어디에 나오나\n"
"    ───────────   ───────────────────────   ──────────────────────────   ──────────────────\n"
"    flush 지연    append − oldest_dirty     데이터 페이지의 볼륨 반영     이 패널의 주황 계열\n"
"    fsync 지연    append − nxio(flushed)    로그 자체의 디스크 기록      이 패널 판정문·트리\n"
"\n"
"    · flush 지연만 크다   데이터 반영이 밀렸다 → 체크포인트 주기·flush 처리량을 본다.\n"
"                          장애 시 redo 가 길어질 뿐, 커밋 응답은 정상일 수 있다.\n"
"    · fsync 지연이 있다   로그가 디스크에 안 내려갔다 → 커밋 응답이 직접 느려진다(더 급한 신호).\n"
"                          판정문: ⚠ 로그 fsync 지연 N 로그 페이지 — 커밋 지연 위험\n"
"    · 둘 다 크다          쓰기 경로 전체가 밀린 상태 — 1번의 장치·await 겹침선을 함께 볼 것.\n"
"\n"
"  1번(호스트)과 세로로 함께 읽는 것이 이 화면의 값이다 — 예: 1번 장치 포화가 먼저 오르고\n"
"  뒤이어 3번 적재가 오르며 2번 메모리가 평탄하면, 원인은 쓰기·읽기 경로이고 메모리는 결과가 아니다.\n"
"  1번 장치는 바쁜데 3·4번이 조용하면 다른 프로세스의 부하다.\n"
"\f\n"
"── 시계열 · 키 ──\n"
"\n"
"    키            동작\n"
"    ───────────   ─────────────────────────────────────────────────────────\n"
"    2 3 4         그 패널을 확대\n"
"                    화면 = [1 호스트 요약] + [확대한 패널] + [원인 지표 막대]\n"
"                    확대는 세로를 키우는 것만이 아니다 — 아래에 그 지표의 *원인이 되는\n"
"                    양*을 같은 시간축 막대로 깔아 원인·결과를 한 번에 읽게 한다\n"
"                    (주식 차트의 가격+거래량 구조. 막대는 모양, 숫자는 우측 값)\n"
"                      2 확대 → heapB · 마이너 폴트\n"
"                      3 확대 → 적재 pg/s · OS 캐시 흡수%% · 메이저 폴트\n"
"                      4 확대 → 디스크 쓰기 · D스레드 · blkio%%\n"
"    1             기본 화면(4패널)으로 복귀. 1번은 확대 대상이 아니다\n"
"                    (줌에서도 1번은 항상 위에 남으므로 따로 크게 볼 이유가 없다)\n"
"                    확대한 키를 다시 눌러도 복귀한다\n"
"    <  >          인스턴스(DB) 이동 — 2~4번이 그 DB 로 바뀌고 히스토리도 새로 쌓는다\n"
"    p             대시보드 화면으로 (지금 값의 분해를 보러)\n"
"    space         일시정지 / 재개 — 멈춰 두고 구간을 비교할 때\n"
"    l             한글 / 영문 라벨 전환\n"
"    h             이 도움말        q   종료\n"
"\n"
"  ※ 한글 입력 상태로 눌러도 된다 — ㅂ=q  ㅗ=h  ㅔ=p  ㅣ=l (자판 위치로 되돌림)\n"
"  ※ 히스토리는 대시보드를 보는 동안에도 계속 쌓인다 — p 로 오면 그동안의 창이 바로 그려진다.\n"
"  ※ 대시보드 ⑤ \"메모리 추이\" 상자는 이 2번 패널의 축소판이다.\n"
"\f\n"
"── 환경변수 CUBRID (설정 축이 살아 있으려면) ──\n"
"  CUBRID=<설치경로>  설정 시 시작할 때 한 번 '<경로>/bin/cubrid paramdump <db>' 를 실행해\n"
"  서버가 실제로 적용 중인 파라미터를 읽는다(셸을 거치지 않고 execvp, 최대 5초).\n"
"  없으면 <경로>/conf/cubrid.conf 를 직접 파싱하고, 그것도 없으면 아래가 모두 빠진다:\n"
"    · data_buffer/log_buffer 의 '설정 대비 %' 판정 — 설정값이 없어 초과 여부를 알 수 없다\n"
"    · 파라미터 귀속률 — 설정으로 설명되는 몫을 셀 수 없어 표시되지 않는다\n"
"    · DB 페이지 크기 — data_buffer_size/pages 로 역산한다(실패 시 16KB 로 가정)\n"
"    · 방법 B 자기검증 — num_buffers 와 data_buffer_pages 대조가 불가해 힙 B 가 꺼진다\n"
"    · 렌즈2(설정초과)·하단 '메모리 파라미터' 표 전체\n"
"  즉 RSS/PSS 같은 실측은 그대로 나오지만, '왜 이만큼인가' 를 설명하는 축이 사라진다.\n"
"\n"
"── 여러 데이터베이스(인스턴스) ──\n"
"  cub_top <db명>     그 DB 를 본다.  생략하면 가동 중인 것 중 알파벳순 첫 번째\n"
"  라이브에서 < > 로 이동. 상세 추적은 화면에 보이는 DB 하나뿐이고,\n"
"  나머지는 총량만 집계한다(clear_refs 를 전부에 돌리면 대상 서버가 느려지기 때문).\n"
"  terse 는 instance.<db>.* 로 모든 인스턴스를 내보내며 tracked=1 이 현재 추적 대상이다.\n"
"\n"
"  자세한 한계는 docs/limitations.md, 이식성은 docs/portability.md 참조.");
}
/* Help entry point.
   mode 0 = batch/CLI (-h): name, usage, options plus the dashboard description.
   mode 1 = live dashboard, 2 = live time series: that mode's help only. */
static void usage_modal(int mode){
    if(mode==1){ help_keys_dash(); return; }
    if(mode==2){ help_keys_plot(); return; }
    usage();
}

/* A numeric option value: the whole string must be a number within [lo,hi]. */
static int opt_num(const char *opt,const char *s,double lo,double hi,double *out){
    char *end; double v=strtod(s,&end);
    if(end==s || *end || !(v>=lo && v<=hi)){
        fprintf(stderr,"cub_top: %s 의 값 '%s' 이(가) 올바르지 않습니다 (범위 %g ~ %g)\n",opt,s,lo,hi);
        return 0;
    }
    *out=v; return 1;
}

int main(int argc,char **argv){
    g_argc=argc; g_argv=argv;
    /* Hot measurement is off by default: clear_refs walks the target's PTEs at a cost
       proportional to resident size (about 30ms/GB), after which the target takes minor
       faults re-setting the accessed bits.  Staying non-invasive requires the default
       to be off; --hot turns it on explicitly. */
    int terse=0,with_a=0,no_hot=1,live=0,view0=0,dump_hist=0,force_ko=0; double interval=0.5;
    const char *want_db=NULL;         /* The positional argument is the db to view; without it, the first alphabetically */
    /* Options that take a value.  The tests below are written "&&i+1<argc", which would
       let an option without its value match nothing (--replay with no path would sample
       live), so a missing value is rejected here first. */
    static const char *const NEEDVAL[]={"--record","--replay","--replay-speed","--offsets",
        "--interval","--bcb-dump","--capacity-state","--hot-limit-gb","--hot-every",NULL};
    for(int i=1;i<argc;i++){
        for(int n=0;NEEDVAL[n];n++)
            if(!strcmp(argv[i],NEEDVAL[n])&&i+1>=argc){
                fprintf(stderr,"cub_top: %s 에 값이 필요합니다\n",argv[i]);
                return 2;
            }
    }
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-t")) terse=1;
        else if(!strcmp(argv[i],"--json")){ terse=1; g_json=1; }
        else if(!strcmp(argv[i],"-b")||!strcmp(argv[i],"--live")) live=1;
        else if(!strcmp(argv[i],"-p")||!strcmp(argv[i],"--plot")){ live=1; view0=1; }
        else if(!strcmp(argv[i],"--record")&&i+1<argc) g_rec_path=argv[++i];
        else if(!strcmp(argv[i],"--replay")&&i+1<argc) g_rep_path=argv[++i];
        else if(!strcmp(argv[i],"--replay-speed")&&i+1<argc){ if(!opt_num(argv[i],argv[i+1],0,1e6,&g_rep_speed)) return 2; i++; }
        else if(!strcmp(argv[i],"--dump-hist")) dump_hist=1;
        else if(!strcmp(argv[i],"--heap")) with_a=1;
        else if(!strcmp(argv[i],"--no-hot")) no_hot=1;      /* Backward compatibility: the default is already off */
        else if(!strcmp(argv[i],"--hot")) no_hot=0;          /* Hot measurement requested explicitly */
        else if(!strcmp(argv[i],"--hot-limit-gb") && i+1<argc){ if(!opt_num(argv[i],argv[i+1],0,1e6,&g_hot_limit_gb)) return 2; i++; }
        else if(!strcmp(argv[i],"--hot-every") && i+1<argc){ if(!opt_num(argv[i],argv[i+1],0,1e6,&g_hot_every)) return 2; i++; }
        else if(!strcmp(argv[i],"--ascii")) g_ascii=1;
        else if(!strcmp(argv[i],"--ko")) force_ko=1;
        else if(!strcmp(argv[i],"--offsets") && i+1<argc)
            snprintf(g_offs_file,sizeof g_offs_file,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--interval") && i+1<argc){ if(!opt_num(argv[i],argv[i+1],0.05,3600,&interval)) return 2; i++; }
        else if(!strcmp(argv[i],"--bcb-dump") && i+1<argc) snprintf(g_bcb_dump,sizeof g_bcb_dump,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--capacity-state") && i+1<argc) snprintf(g_cap_state,sizeof g_cap_state,"%s",argv[++i]);
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")||!strcmp(argv[i],"/h")
                ||!strcmp(argv[i],"-?")||!strcmp(argv[i],"/?")){
            /* Dumping 172 lines at once scrolls the first screen - the most important section -
               out of view, so the same pager as live h is used.
               A pipe or redirect dumps plainly, keeping grep and friends working. */
            if(isatty(STDOUT_FILENO)&&isatty(STDIN_FILENO)){
                g_hn=0; g_hcap=1; usage(); g_hcap=0;    /* -h is the CLI context: no mode keys */
                tui_enter(); help_pager(g_hln,g_hn); tui_leave();
            } else usage();
            return 0; }
        else if(argv[i][0]!='-'&&argv[i][0]!='/'){
            /* One database per run */
            if(want_db){ fprintf(stderr,"cub_top: 데이터베이스는 하나만 지정합니다 ('%s' 와 '%s')\n",want_db,argv[i]);
                         return 2; }
            want_db=argv[i]; }
        else {
            /* Unknown options are rejected so a typo (--jsn) cannot pass for a valid run. */
            fprintf(stderr,"cub_top: 알 수 없는 옵션 '%s' (-h 로 목록 확인)\n",argv[i]);
            return 2;
        }
    }
    /* --record captures live frames only: elsewhere it would leave an empty file, and
       with --replay on the same path it would truncate the recording being read. */
    if(g_rec_path && g_rep_path){
        fprintf(stderr,"cub_top: --record 와 --replay 는 함께 쓸 수 없습니다 (녹화 파일 보호).\n");
        return 2;
    }
    if(g_rec_path && !live){
        fprintf(stderr,"cub_top: --record 는 라이브 모드(-b 또는 -p)에서만 기록합니다.\n");
        return 2;
    }
    /* Open the recording before the first frame, so a path that cannot be written
       fails the run instead of a capture that reports success with nothing written. */
    if(g_rec_path){
        FILE *rf=fopen(g_rec_path,"we");
        if(!rf){ fprintf(stderr,"cub_top: --record %s: %s\n",g_rec_path,strerror(errno)); return 2; }
        RPR.f=rf;
    }
    /* Plots write ANSI to stdout and would break a terse parser, so the combination is refused. */
    if(view0 && terse){
        fprintf(stderr,"cub_top: -p 와 -t 는 함께 쓸 수 없습니다 (플롯은 ANSI 출력).\n");
        return 2;
    }
    auto_lang(force_ko);   /* Detect whether Hangul can be displayed; explicit options win. */
    /* Replay draws only from the recording, so it must run before enum_instances() and
       paramdump touch /proc or the server - a replay works on a host with no CUBRID. */
    if(g_rep_path) return rp_run(g_rep_path,view0,no_hot,terse,with_a,g_json);
    static proc_t P,P0;   /* About 1MB twice including region_t[4096]: static rather than stack */
    enum_instances();
    if(want_db){
        int k=inst_find(want_db);
        if(k<0){
            fprintf(stderr,"cub_top: '%s' 인스턴스가 없습니다.\n",want_db);
            if(g_ninst>0){ fprintf(stderr,"  가동 중:");
                for(int i=0;i<g_ninst;i++) fprintf(stderr,"%s %s",i?",":"",g_inst[i].db);
                fprintf(stderr,"\n"); }
            else fprintf(stderr,"  가동 중인 cub_server 가 없습니다.\n");
            return 2;
        }
        g_cur=k;
    } else g_cur=0;                     /* First alphabetically */
    if(g_ninst>0){ P.pid=g_inst[g_cur].pid;
                   snprintf(P.db,sizeof P.db,"%s",g_inst[g_cur].db);
                   snprintf(g_cur_db,sizeof g_cur_db,"%s",g_inst[g_cur].db); }
    else { P.pid=-1; snprintf(P.db,sizeof P.db,"-"); g_cur_db[0]=0; }
    const char *cub=getenv("CUBRID");
    FILE *pd=NULL;
    if(P.pid<0){
        /* Server not running: fill in what is observable rather than stopping */
        memset(&CAP,0,sizeof CAP);
        if(cub && *cub) params_from_conf(cub,P.db);
        if(g_pagesize<=0) g_pagesize=16384;
        snprintf(CAP.note,sizeof CAP.note,g_ascii
                 ?"cub_server not running - only OS, broker and device are observed"
                 :"cub_server 미기동 — OS·브로커·장치만 관측");
    } else {
        cap_probe(P.pid);
        if(cub && *cub) pd=paramdump_start(cub,P.db);   /* The child (about 0.3s) overlaps the sample window */
        else g_pagesize=16384;
    }

    g_interval=interval;            /* Used to display the hot window */

    static iod_t IO;
    static mem_t M; static tier_t T;
    /* Live always measures method A once at startup: even with automatic mode, A must not
       be empty on the first screen (--heap gates only the one-shot output). */
    if(live) with_a=1;
    double m_a0=now_ms();
    double work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,no_hot,with_a,&pd);
    if(with_a){ g_a_dur=(now_ms()-m_a0)/1000.0-interval; if(g_a_dur<0) g_a_dur=0;
                g_a_when=time(NULL); }

    if(live){
        /* ---- Live dashboard ---- */
        int lens=1,paused=0,a_pending=0,help=0,view=view0; double a_at=now_ms();   /* When the startup measurement finished */
        tui_enter();
        while(!g_stop){
            if(help){
                /* The help does not fit one screen, so it is collected as lines and handed to the pager,
                   with the current mode's key section first (2 time series, 1 dashboard). */
                g_hn=0; g_hcap=1; usage_modal(view?2:1); g_hcap=0;
                help_pager(g_hln,g_hn);
                help=0;
                continue;
            }
            /* History accumulates every frame regardless of view, so pressing p draws the
               window collected so far.  Not while paused, though: the pause loop
               returns here every 150ms, so without this guard the series would
               keep scrolling - faster than when running. */
            if(!paused) hist_push(now_ms()/1000.0, &P, &M, &T, &IO, !no_hot);
            if(g_rec_path && !paused){
                char rts[32]; time_t rt=time(NULL); struct tm tmv;
                localtime_r(&rt,&tmv); strftime(rts,sizeof rts,"%Y-%m-%d %H:%M:%S",&tmv);
                rp_record(&P,&M,&T,&IO,with_a,rts,(long)rt);
            }
            if(view) render_plot(&P,&M,&T,&IO,paused,!no_hot);
            else     render_dash(&P,&M,&T,&IO,lens,paused,work_ms,
                                 a_at>=0?(now_ms()-a_at)/1000.0:-1,with_a);
            /* Key handling */
            int k;
            while((k=tui_key())>=0){
                if(k=='q') { g_stop=1; break; }
                else if(k>='1'&&k<='4'){
                    /* Plot zoom: panel 1 (host) is not a zoom target, since it stays on top regardless.
                       So 1 restores the four-panel view and 2-4 zoom that panel (the
                       same key again restores). */
                    if(view) g_plot_zoom = (k=='1' || g_plot_zoom==k-'0') ? 0 : k-'0';
                    else if(k<='3') lens=k-'0';
                }
                else if(k==' ') paused=!paused;
                else if(k=='r'){ with_a=1; a_pending=1; }            /* Reanalyse once */
                else if(k=='a'){ g_a_auto=!g_a_auto;                  /* Toggle automatic mode */
                                 if(g_a_auto){ with_a=1; a_pending=1; } }
                else if(k=='l') g_ascii=!g_ascii;   /* Switch labels between Korean and English, for a tty without Hangul support */
                else if(k=='p'||k=='g') view=!view;   /* Dashboard and time-series plots */
                else if((k=='<'||k==','||k=='>'||k=='.') && g_ninst>1){
                    /* Switch instance.  The previous target's classification would make method_a
                       exclude the wrong region and double-count, so the globals are
                       cleared and tracking starts over. */
                    g_cur = (k=='<'||k==',') ? (g_cur+g_ninst-1)%g_ninst
                                             : (g_cur+1)%g_ninst;
                    P.pid=g_inst[g_cur].pid;
                    snprintf(P.db,sizeof P.db,"%s",g_inst[g_cur].db);
                    snprintf(g_cur_db,sizeof g_cur_db,"%s",g_inst[g_cur].db);
                    reset_instance_state();
                    cap_probe(P.pid);                         /* Capabilities belong to the target, not the last one */
                    memset(&P0,0,sizeof P0); P0.pid=P.pid;   /* A fresh delta baseline too */
                    HN=0;                                     /* History is per database; mixing them falsifies it */
                    with_a=1; a_pending=1;                    /* Run method A once immediately for the new target */
                    g_pagesize=0;                             /* Configuration follows the new database too */
                    if(pd){ paramdump_finish(pd); pd=NULL; }
                    if(cub&&*cub) pd=paramdump_start(cub,P.db);
                }
                else if(k=='h') help=1;
            }
            if(g_stop||help) continue;
            if(paused){ usleep(150000); continue; }
            /* Automatic mode: reschedule once the adaptive period has passed since the last completion */
            if(g_a_auto && !a_pending && a_at>=0 && (now_ms()-a_at)/1000.0>=a_period())
                a_pending=1;
            /* After a restart, remeasure A once even outside automatic mode: a heap
               decomposition taken from the dead server must not appear to belong to
               the new one. */
            if(g_a_redo){ g_a_redo=0; with_a=1; a_pending=1; }
            {   double m0=now_ms();
                int wa = with_a && a_pending;                 /* A runs only on scheduled frames */
                work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,no_hot,wa?1:0,&pd);
                if(a_pending){ g_a_dur=(now_ms()-m0)/1000.0-interval;
                               g_a_when=time(NULL);
                               if(g_a_dur<0) g_a_dur=0;
                               a_at=now_ms(); a_pending=0; } }
        }
        tui_leave();
        if(g_mem_fd>=0) close(g_mem_fd);
        return 0;
    }

    /* Time */
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv);
    char ts[32]; strftime(ts,sizeof ts,"%Y-%m-%d %H:%M:%S",&tmv);
    /* For cross-checking: build one history sample by the same path the screen uses and
       write it to stderr.  tools/check-hist.sh compares it with the terse keys, so
       plots, dashboard and terse are checked against one another. */
    if(dump_hist){ hist_push((double)now_ms()/1000.0,&P,&M,&T,&IO,!no_hot); hist_dump_last(); }
    /* --json captures the terse output to a temporary file and converts it, so the
       content is never produced twice.  stdout is redirected at fd level, which does
       not rely on the stdio implementation. */
    FILE *jf=NULL; int jsave=-1;
    if(terse && g_json){
        /* A consumer expecting JSON must not get key=value or nothing with exit 0. */
        fflush(stdout);
        jf=tmpfile();
        if(jf) jsave=dup(1);
        if(!jf || jsave<0 || dup2(fileno(jf),1)<0){
            fprintf(stderr,"cub_top: --json 임시 출력 준비 실패: %s\n",strerror(errno));
            return 1;
        }
    }
    if(terse){ char ou[256]; opts_used_str(argc,argv,ou,sizeof ou);
               printf("opts.used=\"%s\"\n",ou); }
    else    { print_opts_line(argc,argv); putchar('\n'); }   /* Blank line between the options and the body */
    if(terse){
        render_terse(&P,&M,&T,with_a,IO.rb,IO.wb,IO.ri,IO.wi,
                     IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,ts,(long)now,
                     IO.rchar,IO.minflt,IO.majflt,IO.cpu_pct,IO.ncpu,
                     IO.d_thr,IO.n_thr);
        /* This block must stay inside the terse branch: outside it, the else below binds to
           if(jf) and plain -t prints the tree as well. */
        if(jf){ fflush(stdout);
                if(dup2(jsave,1)<0){ fprintf(stderr,"cub_top: --json 출력 복원 실패: %s\n",strerror(errno)); return 1; }
                close(jsave);
                json_from_terse(jf,stdout); fclose(jf);
                if(fflush(stdout)!=0 || ferror(stdout)){ fprintf(stderr,"cub_top: --json 출력 쓰기 실패: %s\n",strerror(errno)); return 1; } }
    }
    else {
        /* The tree prints every instance in turn; one per invocation would mean running the
           command repeatedly to see the other databases.  A named DB prints only that
           one.  Each instance is collected afresh at its own instant. */
        int only = want_db ? g_cur : -1;
        for(int ii=0; ii<(g_ninst>0?g_ninst:1); ii++){
            if(only>=0 && ii!=only) continue;
            if(g_ninst>0 && ii!=g_cur){        /* Switch target, the same procedure as live < and > */
                g_cur=ii;
                P.pid=g_inst[ii].pid;
                snprintf(P.db,sizeof P.db,"%s",g_inst[ii].db);
                snprintf(g_cur_db,sizeof g_cur_db,"%s",g_inst[ii].db);
                reset_instance_state();
                cap_probe(P.pid);
                memset(&P0,0,sizeof P0); P0.pid=P.pid;
                g_pagesize=0;
                if(pd){ paramdump_finish(pd); pd=NULL; }
                if(cub && *cub) pd=paramdump_start(cub,P.db);
                work_ms=sample_once(&P,&P0,&M,&T,&IO,interval,no_hot,with_a,&pd);
            }
            if(ii>0 || only<0) printf("[@%s]\n",P.db[0]?P.db:"-");
            render_tree(&P,&M,&T,with_a,IO.rb,IO.wb,IO.ri,IO.wi,
                        IO.absorb_ok?IO.absorb:-1.0,IO.blkpct,IO.devnm,IO.devutil,IO.devr,ts,
                        IO.rchar,work_ms,IO.cpu_pct,IO.ncpu);
            if(g_ninst>1 && ii<g_ninst-1){
                putchar('\n');
                for(int c=0;c<70;c++) putchar('-');
                putchar('\n');
            }
        }
    }
    if(g_mem_fd>=0) close(g_mem_fd);
    return 0;
}
