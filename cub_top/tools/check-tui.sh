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
# The live screen and its terminal.
#  - an OSC colour reply arriving after the query gave up (slow SSH) is swallowed, not read
#    as keys (r, g, a, 1 in "rgb:..." would act); Esc then ']' typed by hand still are keys.
#    Also when it arrives split (TCP): ESC alone and the rest 60 ms later, the reply cut
#    after "rgb:1c1c" and the rest 200 ms later, a cut right after ESC ] - and no read
#    holds the frame longer than the 50 ms gap it already allowed
#  - Ctrl-Z gives the terminal back (cooked mode, cursor, colours) and fg takes it again
#    with a clean screen; SIGHUP/SIGQUIT leave the normal way, abort() restores then aborts
#  - a terminal under 60 columns or 10 rows is told so instead of drawn with wrapping
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"; REC="$ROOT/examples/10-recording.jsonl"
TMP=$(mktemp -d /tmp/cub_top-tui.XXXXXX); trap 'rm -rf "$TMP"' EXIT
bad=0; ok(){ echo "  ok   $1"; }; no(){ echo "  FAIL $1"; bad=1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static int keys(const char *in,int *out,int max){
    int p[2]; if(pipe(p)) return -1; if(write(p[1],in,strlen(in))<0){} close(p[1]); dup2(p[0],0); close(p[0]);
    g_kqn=0; int n=0;
    for(int i=0;i<40 && n<max;i++){ int k=tui_key(); if(k==-1 && g_kqn==0){ unsigned char c; if(read(0,&c,0)<0){} }
        if(k>=0) out[n++]=k; else if(k==-1){ struct pollfd pf={0,POLLIN,0}; if(poll(&pf,1,0)<=0 && g_kqn==0) break; } }
    return n;
}
/* Split arrival: part a, then part b gap_ms later, read the way the live loop does
   (every 10 ms, never blocking on the pipe); the longest single tui_key call is kept */
static double g_longest;
static int keys2(const char *a,int gap_ms,const char *b,int *out,int max){
    int p[2]; if(pipe(p)) return -1; dup2(p[0],0); close(p[0]); fcntl(0,F_SETFL,O_NONBLOCK);
    g_kqn=0; g_esc_lone_at=-1; g_osc_drop_until=-1; g_longest=0; int n=0;
    if(write(p[1],a,strlen(a))<0){}
    for(int ph=0;ph<2;ph++){
        double end=now_ms()+(ph?500:gap_ms);
        while(now_ms()<end){
            for(;;){ double t=now_ms(); int k=tui_key(); t=now_ms()-t; if(t>g_longest) g_longest=t;
                     if(k<0) break; if(n<max) out[n++]=k; }
            usleep(10000);
        }
        if(!ph){ if(write(p[1],b,strlen(b))<0){} close(p[1]); }
    }
    return n;
}
static int same(const int *k,int n,const int *w,int wn){ if(n!=wn) return 0; for(int i=0;i<n;i++) if(k[i]!=w[i]) return 0; return 1; }
static void show(char *o,size_t on,const int *k,int n){ size_t m=0; o[0]=0;
    for(int i=0;i<n && m+8<on;i++) m+=snprintf(o+m,on-m,k[i]==0x1b?"Esc ":k[i]==7?"BEL ":"%c ",k[i]); }
int main(void){
    int bad=0, k[64], n;
    { struct { const char *what,*a; int gap; const char *b; int w[4], wn; } T[]={
        {"reply cut after ESC ] 1 (100 ms)",        "\033]1",100,"1;rgb:1c1c/1c1c/1c1c\007q",{0x1b,'q'},2},
        {"Esc ] typed by hand, x after 400 ms",     "\033]",400,"x",{0x1b,']','x'},3},
        {"reply split after ESC (60 ms)",           "\033",60,"]11;rgb:1c1c/1c1c/1c1c\007q",{0x1b,'q'},2},
        {"reply cut after rgb:1c1c (200 ms)",       "\033]11;rgb:1c1c",200,"/1c1c/1c1c\007q",{'q'},1},
        {"reply cut before ST's backslash (150 ms)","\033]10;rgb:d8d8/d8d8/d8d8\033",150,"\\g",{'g'},1},
        {"reply cut right after ESC ] (100 ms)",    "\033]",100,"11;rgb:1c1c/1c1c/1c1c\007q",{0x1b,'q'},2},
        {"cut reply whose rest never comes, q after 400 ms","\033]11;rgb:1c",400,"q",{'q'},1},
        {"Esc then ] x typed by hand (60 ms)",      "\033",60,"]x",{0x1b,']','x'},3},
        {"Esc then ] 1 typed by hand (400 ms)",     "\033",400,"]1",{0x1b,']','1'},3},
      };
      for(size_t i=0;i<sizeof T/sizeof T[0];i++){
        n=keys2(T[i].a,T[i].gap,T[i].b,k,64); char got[128]; show(got,sizeof got,k,n);
        int ok = same(k,n,T[i].w,T[i].wn) && g_longest<=60;
        printf("  %s   %s: keys [%s], longest read %.0f ms\n",ok?"ok  ":"FAIL",T[i].what,got,g_longest); bad|=!ok;
      }
    }
    n=keys("\033]11;rgb:1c1c/1c1c/1c1c\007q",k,64);
    int ok = n==1 && k[0]=='q';
    printf("  %s   a late colour reply then q: keys seen %d (want only q)\n",ok?"ok  ":"FAIL",n); bad|=!ok;
    n=keys("\033]10;rgb:d8d8/d8d8/d8d8\033\\g",k,64);
    ok = n==1 && k[0]=='g';
    printf("  %s   a reply ended by ESC \\\\ (ST) then g: keys seen %d (want only g)\n",ok?"ok  ":"FAIL",n); bad|=!ok;
    n=keys("\033]x",k,64);
    ok = n==3 && k[0]==0x1b && k[1]==']' && k[2]=='x';
    printf("  %s   Esc ] x typed by hand: still three keys (%d)\n",ok?"ok  ":"FAIL",n); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
"$TMP/t" || bad=1
command -v script >/dev/null && [ -s "$REC" ] || { echo "  SKIP pty checks (no script or recording)"; [ $bad = 0 ] && echo "→ PASS" || { echo "→ FAIL"; exit 1; }; exit 0; }
cp "$ROOT/cub_top" "$TMP/cub_top_tui"; BIN="$TMP/cub_top_tui"
# Small terminals
for sz in "50 30" "120 6"; do set -- $sz
    timeout -s INT 3 script -qc "stty cols $1 rows $2; '$BIN' --ascii --replay '$REC' -b --replay-speed 1" /dev/null > "$TMP/small" 2>&1
    if grep -q "needs at least" "$TMP/small" && ! grep -q "OS context" "$TMP/small"; then ok "${1}x$2: told the terminal is too small, nothing drawn"
    else no "${1}x$2: drawn anyway"; fi
done
# Ctrl-Z / fg, SIGHUP, abort: run under a pty and look at the tty modes from outside
run(){ # sig...: start under a pty, send each signal in turn, report the tty modes after each
    script -qc "stty cols 140 rows 50; '$BIN' --ascii --replay '$REC' -b --replay-speed 0.05" "$TMP/typescript" >/dev/null 2>&1 &
    sp=$!; i=0; while [ $i -lt 30 ]; do pid=$(pgrep -n -f "^$BIN "); [ -n "$pid" ] && break; sleep 0.1; i=$((i+1)); done
    sleep 1; tty=$(readlink /proc/$pid/fd/0)
    for s in "$@"; do kill -$s $pid 2>/dev/null; sleep 0.6
        printf '%s:%s ' "$s" "$(stty -F "$tty" -a 2>/dev/null | grep -o -- '-\?icanon' | head -1)"; done
    kill -TERM $pid 2>/dev/null; sleep 0.3; kill $sp 2>/dev/null; wait $sp 2>/dev/null
}
r=$(run TSTP CONT)
case "$r" in "TSTP:icanon CONT:-icanon "*) ok "Ctrl-Z: cooked mode back while stopped, raw again after fg ($r)";; *) no "Ctrl-Z/fg tty modes: $r";; esac
n=$(grep -ao "$(printf '\033\[2J')" "$TMP/typescript" | wc -l); [ "$n" -ge 2 ] && ok "fg clears the screen for the next frame" || no "no clear after fg ($n)"
ends(){ # sig: the process leaves and restores the terminal (cursor shown again) after it
    script -qc "stty cols 140 rows 50; '$BIN' --ascii --replay '$REC' -b --replay-speed 0.05" "$TMP/ts2" >/dev/null 2>&1 &
    sp=$!; i=0; while [ $i -lt 30 ]; do pid=$(pgrep -n -f "^$BIN "); [ -n "$pid" ] && break; sleep 0.1; i=$((i+1)); done
    sleep 1; n0=$(grep -ao "$(printf '\033\[?25h')" "$TMP/ts2" | wc -l)
    kill -$1 $pid 2>/dev/null; i=0; while kill -0 $pid 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
    gone=1; kill -0 $pid 2>/dev/null && gone=0; sleep 0.2
    n1=$(grep -ao "$(printf '\033\[?25h')" "$TMP/ts2" | wc -l); kill $sp 2>/dev/null; wait $sp 2>/dev/null
    [ $gone = 1 ] && [ "$n1" -gt "$n0" ]
}
ends HUP && ok "SIGHUP: leaves the normal way, terminal restored" || no "SIGHUP: not left or not restored"
ends QUIT && ok "SIGQUIT: leaves the normal way, terminal restored" || no "SIGQUIT: not left or not restored"
ends ABRT && ok "abort: terminal restored, then the default action" || no "abort: not restored"
if [ $bad = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
