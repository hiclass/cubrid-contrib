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
# The re-admit rate is the one signal separating "buffer too small" from "scan without
# reuse".  When it was not measured (-1) it must stay -1 in the history and the verdict
# must not pick either conclusion; a 0 there read as a scan.  A large buffer scanned on
# request is scanned every frame, so every frame after the first has a measured rate.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-readmit.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static int verdict(double readmit,double cold,const char *want_has,const char *want_not){
    static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
    AG.ok=1; AG.nbuf=1000; AG.z3=(long)(cold*10); AG.z1=1000-AG.z3; AG.z2=0; AG.dirty=0; PG.ok=0;
    IO.rb=20.0*1024*1024; CAP2.readmit_pct=readmit; CAP2.miss_pps=1280; g_pagesize=16384;   /* 20MB/s admitted */
    hist_push(1,&P,&M,&T,&IO);
    char o[400]; const char*c=VD_INFO; vd_buf(o,sizeof o,&c);
    int ok = HS[HN-1].cap_readmit==readmit && (!want_has || strstr(o,want_has)) && (!want_not || !strstr(o,want_not));
    printf("  %s   readmit %5.0f cold %.0f%% -> hist %.0f, \"%s\"\n",ok?"ok  ":"FAIL",readmit,cold,HS[HN-1].cap_readmit,o);
    return ok?0:1;
}
/* Exact LRU of B slots under uniform random access over W pages, with a second LRU of 2B fed
   the same accesses; the frame's resident set goes through cap_turnover. */
static int *pv1,*nx1,*in1,h1,t1,c1, *pv2,*nx2,*in2,h2,t2,c2;
static int lru(int p,int B,int*pv,int*nx,int*in,int*h,int*t,int*c){
    if(in[p]){ if(pv[p]>=0) nx[pv[p]]=nx[p]; else *h=nx[p]; if(nx[p]>=0) pv[nx[p]]=pv[p]; else *t=pv[p];
               pv[p]=-1; nx[p]=*h; if(*h>=0) pv[*h]=p; *h=p; if(*t<0) *t=p; return 1; }
    if(*c>=B){ int v=*t; if(pv[v]>=0) nx[pv[v]]=-1; else *h=-1; *t=pv[v]; in[v]=0; (*c)--; }
    pv[p]=-1; nx[p]=*h; if(*h>=0) pv[*h]=p; *h=p; if(*t<0) *t=p; in[p]=1; (*c)++; return 0;
}
static int lru_case(int B,double mult){
    int W=(int)(B*mult); srand(7);
    pv1=calloc(W,4); nx1=calloc(W,4); in1=calloc(W,4); pv2=calloc(W,4); nx2=calloc(W,4); in2=calloc(W,4);
    h1=t1=h2=t2=-1; c1=c2=0;
    static pgrec_t rec[1<<14]; reset_instance_state(); PG.rec=rec; PG.reccap=1<<14;
    long hit1=0,hit2=0,acc=0; double rs=0; int k=0;
    for(int f=0;f<400;f++){
        for(int a=0;a<B/4;a++){ int p=rand()%W;
            int g1=lru(p,B,pv1,nx1,in1,&h1,&t1,&c1), g2=lru(p,2*B,pv2,nx2,in2,&h2,&t2,&c2);
            if(f>=100){ hit1+=g1; hit2+=g2; acc++; } }
        PG.nrec=0; for(int p=h1;p>=0;p=nx1[p]){ pgrec_t*r=&rec[PG.nrec++]; memset(r,0,sizeof *r); r->pageid=p; }
        PG.ok=1; PG.nbuf=B; PG.degraded=0; cap_turnover(100.0+f*0.5);
        if(f>=100 && CAP2.readmit_pct>=0){ rs+=CAP2.readmit_pct; k++; }
    }
    double m1=1.0-(double)hit1/acc, m2=1.0-(double)hit2/acc, cut=100.0*(m1-m2)/m1, r=rs/k;
    int ok = r<=cut+1.0 && r>=cut-6.0;      /* a lower bound, close to the doubled buffer's cut */
    printf("  %s   W=%4.1fB: re-admit %5.1f%%, misses a doubled buffer removes %5.1f%%\n",ok?"ok  ":"FAIL",mult,r,cut);
    free(pv1); free(nx1); free(in1); free(pv2); free(nx2); free(in2);
    return ok?0:1;
}
/* Fake pool of NB BCBs (the review's 300,000 - above PG_LARGE).  The first 1000 BCBs
   swap between page sets A and B each frame, so from the third frame every admitted
   page was evicted two frames before: re-admit 100%. */
#define NB 300000
static unsigned char pool_[4096], *bcbs_, *iopg_;
static void put(int i,int pageid){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0; uint32_t f=0x30000u; uint64_t ol=~(uint64_t)0;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2); memcpy(b+g_bt->pg.flags,&f,4); memcpy(b+g_bt->pg.olsa,&ol,8);
    unsigned char*io=iopg_+(size_t)i*64; uint64_t ip=(uint64_t)(uintptr_t)io; memcpy(b+g_bt->pg.iopg,&ip,8);
    unsigned char*h=io+g_bt->pg.iobuf_iopg; uint64_t l=5; memcpy(h,&l,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
int main(void){
    int bad=0;
    printf("1. verdict on panel 3\n");
    bad|=verdict(-1,85,"재적재율 미측정","재사용 없는");
    bad|=verdict(-1,85,NULL,"⚠ 버퍼 부족");
    bad|=verdict(50,85,"버퍼 부족",NULL);
    bad|=verdict(5,85,"재사용 거리가 버퍼 밖","무효");
    bad|=verdict(20,85,"버퍼 2배면","무효");
    { /* the OS page cache absorbs a short buffer: no disk reads, the buffer still misses */
      static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
      IO.rb=0; CAP2.miss_pps=3840; CAP2.readmit_pct=85; g_pagesize=16384;
      hist_push(1,&P,&M,&T,&IO);
      char o[400]; const char*c=VD_INFO; vd_buf(o,sizeof o,&c);
      int ok = c==VD_WARN && strstr(o,"버퍼 부족") && strstr(o,"OS 페이지 캐시");
      printf("  %s   disk 0, 3840 pages/s admitted, re-admit 85%%: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
      CAP2.miss_pps=-1; hist_push(2,&P,&M,&T,&IO); c=VD_INFO; vd_buf(o,sizeof o,&c);
      ok = c!=VD_OK && strstr(o,"적재 미측정");
      printf("  %s   admissions not measured: not green, \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
      ok = HS[HN-1].cap_miss==-1;
      printf("  %s   history keeps admissions unmeasured (%.0f)\n",ok?"ok  ":"FAIL",HS[HN-1].cap_miss); bad|=!ok; }
    { /* capacity verdict: 300 pages/s admitted, re-admit 7% (a working set ~15x the buffer) */
      memset(&CAP2,0,sizeof CAP2); CAP2.await_ms=1; CAP2.dev_iops=100; CAP2.qdepth=-1; CAP2.headroom_pct=-1;
      CAP2.miss_pps=300; CAP2.readmit_pct=7; CAP2.turnover_pct=5; CAP2.hit_pct=-1; snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"-");
      PG.ok=1; PG.nbuf=1000; PG.resident=1000;
      char o[400]; const char*c=VD_INFO; vd_capacity(o,sizeof o,&c);
      int ok=!strstr(o,"무효") && strstr(o,"재사용 거리가 버퍼 밖");
      printf("  %s   capacity, re-admit 7%%: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok; }
    printf("2. large buffer (%d BCBs) scanned on request: re-admit every frame\n",NB);
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb); iopg_=calloc(NB,64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++) put(i,1000000+i);
    g_pg_detail=1;
    char seq[200]=""; int n=0, bad2=0;
    for(int f=0;f<6;f++){
        for(int i=0;i<1000;i++) put(i,(f%2?2000000:3000000)+i);
        pgbuf_scan(&P);
        cap_turnover(100.0+f*0.5);
        n+=snprintf(seq+n,sizeof seq-(size_t)n,"%s%.0f",f?",":"",CAP2.readmit_pct);
        if(!PG.ok) { printf("  FAIL frame %d not scanned: %s\n",f,PG.why); bad2=1; }
        if(f>=1 && CAP2.readmit_pct<0) bad2=1;
        if(f>=2 && CAP2.readmit_pct!=100) bad2=1;
    }
    printf("  %s   re-admit by frame: %s (want -1 then measured every frame, 100 from the third)\n",bad2?"FAIL":"ok  ",seq);
    bad|=bad2;
    g_pg_detail=0; pgbuf_scan(&P); cap_turnover(104.0);
    printf("  %s   not requested: scanned=%d re-admit=%.0f (want 0, -1)\n",(!PG.ok&&CAP2.readmit_pct<0)?"ok  ":"FAIL",PG.ok,CAP2.readmit_pct);
    if(PG.ok||CAP2.readmit_pct>=0) bad=1;
    printf("3. re-admit estimates what doubling the buffer saves (exact LRU, uniform random)\n");
    { double m[]={4,6,10,15}; for(int i=0;i<4;i++) bad|=lru_case(4096,m[i]); }
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
