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
# Remembered ceilings are per device, in memory and in the --capacity-state file.
# Two devices alternate as the busiest; each must be judged against its own ceiling.
# Drives cap_collect() itself with synthetic diskstats.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-ceiling.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/t.c" <<C
#define main cub_top_main
#include "$SRC"
#undef main
/* One 1-second frame on device nm: iops of 16KB reads at utilization util */
static void frame(const char*nm,double iops,double util){
    static proc_t P; static iod_t IO; dstat_t d0,d1;
    memset(&d0,0,sizeof d0); memset(&IO,0,sizeof IO);
    snprintf(d0.name,sizeof d0.name,"%s",nm); d1=d0;
    d1.rios=(unsigned long)iops; d1.rsect=(unsigned long)(iops*32); d1.rticks=(unsigned long)(iops*0.5); d1.tiq=500;
    snprintf(IO.devnm,sizeof IO.devnm,"%s",nm); IO.devutil=util;
    cap_collect(&P,&IO,&d0,1,&d1,1,1.0);
}
static int expect(const char*what,double ceil,double used){
    double c=CAP2.ceil_valid?CAP2.ceil_iops:-1; int ok=fabs(c-ceil)<0.5 && fabs(CAP2.headroom_pct-used)<0.05;
    printf("  %-4s %-44s ceiling %6.0f (want %6.0f)  used %5.1f%% (want %5.1f)\n",ok?"ok":"FAIL",what,c,ceil,CAP2.headroom_pct,used);
    return ok;
}
int main(int argc,char**argv){
    int ok=1;
    snprintf(g_cap_state,sizeof g_cap_state,"%s",argv[1]);
    if(argc>2){     /* second process: ceilings come back from the state file */
        frame("sda",500,20);     ok&=expect("restart: sda 500 IOPS",1000,50.0);
        frame("nvme0n1",2000,20);ok&=expect("restart: nvme0n1 2000 IOPS",10000,20.0);
        return ok?0:1;
    }
    frame("nvme0n1",10000,95); ok&=expect("nvme0n1 saturated at 10000 IOPS",10000,100.0);
    frame("sda",1000,95);      ok&=expect("sda saturated at 1000 IOPS",1000,100.0);
    frame("nvme0n1",5000,40);  ok&=expect("nvme0n1 at 5000 IOPS",10000,50.0);
    frame("sda",500,20);       ok&=expect("sda at 500 IOPS",1000,50.0);
    return ok?0:1;
}
C
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
bad=0
"$TMP/t" "$TMP/state" || bad=1
"$TMP/t" "$TMP/state" restart || bad=1
[ $bad -eq 0 ] && { echo "→ PASS"; exit 0; }
echo "→ FAIL"; exit 1
