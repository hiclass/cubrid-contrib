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
    IO.rb=20.0*1024*1024; CAP2.readmit_pct=readmit;
    hist_push(1,&P,&M,&T,&IO);
    char o[400]; const char*c=VD_INFO; vd_buf(o,sizeof o,&c);
    int ok = HS[HN-1].cap_readmit==readmit && (!want_has || strstr(o,want_has)) && (!want_not || !strstr(o,want_not));
    printf("  %s   readmit %5.0f cold %.0f%% -> hist %.0f, \"%s\"\n",ok?"ok  ":"FAIL",readmit,cold,HS[HN-1].cap_readmit,o);
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
    bad|=verdict(5,85,"재사용 없는 스캔",NULL);
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
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
