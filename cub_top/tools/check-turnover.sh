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
# Turnover and refill from synthetic BCB snapshots, through cap_turnover() itself.
# A scan that never rereads must give 0% refill however long it runs, and a large
# buffer must not lose replacements: pages are compared by (volid, pageid), not a hash.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-turnover.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/t.c" <<C
#define main cub_top_main
#include "$SRC"
#undef main
/* Slot i belongs to block i/q of nblk blocks; block f%nblk is refilled at frame f with
   the next pages of a sequence of length ws (0 = endless, never reread). */
static int nb_,nblk_; static long ws_;
static long last_fill(int f,int blk){ return f-((f%nblk_-blk+nblk_)%nblk_); }
static int page_of(int f,int i){ int q=nb_/nblk_; f+=nblk_;
    if(i>=q*nblk_) return i;
    long p=last_fill(f,i/q)*q + i%q; return (int)(ws_? p%ws_ : p); }
static int run(const char*name,int nb,int nblk,long ws,int frames,double want_t,double want_r){
    nb_=nb; nblk_=nblk; ws_=ws; g_inst_gen++;
    if(PG.reccap<nb){ PG.rec=realloc(PG.rec,sizeof(pgrec_t)*(size_t)nb); PG.reccap=nb; }
    double ts=0,rs=0; int k=0;
    for(int f=0;f<frames;f++){
        PG.ok=1; PG.nbuf=nb; PG.nrec=nb;
        for(int i=0;i<nb;i++){ memset(&PG.rec[i],0,sizeof PG.rec[i]); PG.rec[i].volid=(int16_t)(i&3); PG.rec[i].pageid=page_of(f,i); }
        cap_turnover(1000.0+f);
        if(f>=frames/2){ ts+=CAP2.turnover_pct; rs+=CAP2.readmit_pct; k++; }
    }
    double t=ts/k, r=rs/k; int ok=fabs(t-want_t)<0.5 && fabs(r-want_r)<0.5;
    printf("  %-4s %-40s turnover %6.2f%% (want %5.1f)  refill %6.2f%% (want %5.1f)\n",
           ok?"ok":"FAIL",name,t,want_t,r,want_r);
    return ok;
}
int main(void){
    int ok=1, nb=32768;
    ok&=run("endless scan, 1/4 of the buffer per frame",nb,4,0,40,25.0,0.0);
    ok&=run("working set 1.5x the buffer",nb,4,nb+nb/2,40,25.0,100.0);
    ok&=run("working set 3x the buffer (outside window)",nb,4,3L*nb,60,25.0,0.0);
    ok&=run("262144 BCBs, 1% new pages per frame",262144,100,0,20,1.0,0.0);
    return ok?0:1;
}
C
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
