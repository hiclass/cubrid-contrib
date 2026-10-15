#!/bin/sh
#
# Copyright 2016 CUBRID Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# The capacity verdict judges the disk under the DB's own files, not the host's busiest
# disk.  Run as the install account (no root): the target's open <db>, <db>_* files, or its
# databases.txt entry when the fds are another account's; a plain block mount through st_dev,
# overlay through the
# upper filesystem's fsid; partitions and dm/md down to the physical disks; a DB on no
# block device (tmpfs) gives no device verdict and says why.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d /tmp/cub_top-dbdisk.XXXXXX); chmod 755 "$TMP"; trap 'rm -rf "$TMP" "$SHM" "$BLK"' EXIT
SHM=$(mktemp -d /dev/shm/cub_top-dbdisk.XXXXXX 2>/dev/null)
# A plain block-device mount (not /, not overlay) to place files on, and the disk it should give
BLK=""; BLKDISK=""
while read -r _ _ mm _ mp rest; do
    src=${rest#* - }; set -- $src; fs=$1; dv=$2
    case "$dv" in /dev/*) ;; *) continue;; esac
    [ "$mp" = / ] && continue; [ -e "/sys/dev/block/$mm" ] || continue
    sp=$(readlink -f "/sys/dev/block/$mm"); [ -e "$sp/partition" ] && sp=$(dirname "$sp")
    [ -n "$(ls "$sp/slaves" 2>/dev/null)" ] && continue
    BLK=$(mktemp -d "$mp/cub_top-dbdisk.XXXXXX" 2>/dev/null) || { BLK=""; continue; }
    chmod 777 "$BLK"; BLKDISK=$(basename "$sp"); break
done < /proc/self/mountinfo
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static pid_t holder(const char *dir,const char *a,const char *b){   /* a server stand-in holding files open */
    pid_t c=fork(); if(c) { usleep(100000); return c; }
    char p[512]; if(a){ snprintf(p,sizeof p,"%s/%s",dir,a); open(p,O_RDWR|O_CREAT,0600); }
    if(b){ snprintf(p,sizeof p,"%s/%s",dir,b); open(p,O_RDWR|O_CREAT,0600); }
    pause(); _exit(0);
}
static int is_leaf(const char *nm){             /* a whole disk with no slaves, present in diskstats */
    char p[256]; struct stat st; DIR *d;
    snprintf(p,sizeof p,"/sys/class/block/%s/partition",nm); if(stat(p,&st)==0) return 0;
    snprintf(p,sizeof p,"/sys/class/block/%s/slaves",nm);
    if((d=opendir(p))){ struct dirent *e; int n=0; while((e=readdir(d))) if(e->d_name[0]!='.') n++; closedir(d); if(n) return 0; }
    dstat_t ds[64]; int nd=parse_diskstats(ds,64);
    for(int i=0;i<nd;i++) if(!strcmp(ds[i].name,nm)) return 1;
    return 0;
}
int main(int c,char**v){
    const char *tmp=v[1], *shm=v[2]; int bad=0, ok; char dk[8][32], why[160]; pid_t h[4]; int n;
    h[0]=holder(tmp,"tdb","tdb_lgat");
    n=db_disks(h[0],"tdb",dk,8,why,sizeof why);
    ok = n>0 && is_leaf(dk[0]);
    printf("  %s   own files: %d disk(s) %s via %s (whole disk, in diskstats)%s%s\n",ok?"ok  ":"FAIL",n,n?dk[0]:"-",
           g_dk_via==1?"st_dev":g_dk_via==2?"mount":g_dk_via==3?"statfs fsid (overlay)":"-",n?"":" - ",why); bad|=!ok;
    if(shm[0]){ h[1]=holder(shm,"tdb2","tdb2_lgat");
        n=db_disks(h[1],"tdb2",dk,8,why,sizeof why);
        ok = n==0 && why[0];
        printf("  %s   files on tmpfs: %d disk(s), reason \"%s\"\n",ok?"ok  ":"FAIL",n,why); bad|=!ok; kill(h[1],SIGKILL); }
    else printf("  SKIP   files on tmpfs (no /dev/shm)\n");
    if(v[3] && v[3][0]){ h[3]=holder(v[3],"tdb5","tdb5_lgat");
        n=db_disks(h[3],"tdb5",dk,8,why,sizeof why);
        ok = n==1 && !strcmp(dk[0],v[4]) && g_dk_via==1;
        printf("  %s   plain block mount %s: %s via %s (want %s via st_dev)\n",ok?"ok  ":"FAIL",v[3],n?dk[0]:"-",
               g_dk_via==1?"st_dev":g_dk_via==2?"mount":g_dk_via==3?"statfs fsid":"-",v[4]); bad|=!ok; kill(h[3],SIGKILL); }
    else printf("  SKIP   plain block mount (none writable)\n");
    h[2]=holder(tmp,"tdb3x_lgat",NULL);              /* a name merely starting with the db name */
    unsetenv("CUBRID_DATABASES");
    n=db_disks(h[2],"tdb3",dk,8,why,sizeof why);
    ok = n==0 && why[0];
    printf("  %s   another db's file (tdb3x_lgat) not taken for tdb3: %d disk(s), \"%s\"\n",ok?"ok  ":"FAIL",n,why); bad|=!ok;
    /* fds not readable (another account's server): databases.txt gives the volume path */
    { char p[512]; snprintf(p,sizeof p,"%s/databases.txt",tmp); FILE *f=fopen(p,"w");
      fprintf(f,"#db-name\tvol-path\tdb-host\tlog-path\tlob-base-path\ntdb4\t%s\tlocalhost\t%s\tfile:%s/lob\n",tmp,tmp,tmp); fclose(f);
      setenv("CUBRID_DATABASES",tmp,1); }
    n=db_disks(1,"tdb4",dk,8,why,sizeof why);
    char ref[8][32]; int nr=db_disks(h[0],"tdb",ref,8,why,sizeof why);
    ok = n>0 && nr>0 && !strcmp(dk[0],ref[0]);
    printf("  %s   fds unreadable: databases.txt entry gives %s (same disk as the open files: %s)\n",ok?"ok  ":"FAIL",n?dk[0]:"-",nr?ref[0]:"-"); bad|=!ok;
    /* The verdict's device is the DB disk even when another disk is far busier */
    if(nr>0){
        static proc_t P; static iod_t IO; dstat_t d0[2], d1[2];
        P.pid=h[0]; snprintf(P.db,sizeof P.db,"tdb");
        memset(d0,0,sizeof d0); memset(d1,0,sizeof d1);
        snprintf(d0[0].name,32,"zzbusy"); snprintf(d1[0].name,32,"zzbusy"); d1[0].io_ms=990; d1[0].rios=5000; d1[0].tiq=9000;
        snprintf(d0[1].name,32,"%s",ref[0]); snprintf(d1[1].name,32,"%s",ref[0]); d1[1].io_ms=100; d1[1].rios=50; d1[1].rticks=100; d1[1].tiq=100;
        snprintf(IO.devnm,sizeof IO.devnm,"zzbusy");
        cap_collect(&P,&IO,d0,2,d1,2,1.0);
        ok = !strcmp(CAP2.dev,ref[0]) && CAP2.dev_iops==50 && CAP2.qdepth<1;
        printf("  %s   host's busiest is zzbusy (5000 IOPS, q9): judged %s at %.0f IOPS q%.1f\n",ok?"ok  ":"FAIL",CAP2.dev,CAP2.dev_iops,CAP2.qdepth); bad|=!ok;
    }
    /* No DB disk: no storage sentence built from some other disk */
    { const char *col; char o[512];
      memset(&CAP2,0,sizeof CAP2); CAP2.headroom_pct=-1; CAP2.await_ms=CAP2.qdepth=-1; CAP2.miss_pps=-1; CAP2.readmit_pct=-1; CAP2.hit_pct=-1;
      snprintf(CAP2.dev_why,sizeof CAP2.dev_why,"test reason");
      vd_capacity(o,sizeof o,&col);
      ok = strstr(o,"test reason") && !strstr(o,"IOPS");
      printf("  %s   no DB disk: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok; }
    kill(h[0],SIGKILL); kill(h[2],SIGKILL);
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
chmod 755 "$TMP/t"; [ -n "$SHM" ] && chmod 777 "$SHM"
RUN=""; if [ "$(id -u)" = 0 ] && id hiclass >/dev/null 2>&1; then RUN="runuser -u hiclass --"; fi
chmod 777 "$TMP"
if $RUN "$TMP/t" "$TMP" "${SHM:-}" "$BLK" "$BLKDISK"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
