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
# -d, --json and --record stay valid and round-trip.  A Korean reason cut by a fixed buffer
# must not leave half a character (invalid UTF-8 in JSON); a value holding " \ or a tab
# (a library path) must come back from a recording as it went in, not split into fake keys.
# A bare token is a JSON number only by JSON's grammar: a database named 0123, 0x1f or 2024
# stays a string (CUBRID allows names starting with a digit).
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"; BIN="$ROOT/cub_top"
TMP=${TMPDIR:-/tmp}/cub_top-escape.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static int valid_utf8(const char *s){
    const unsigned char *u=(const unsigned char*)s;
    while(*u){ int l=*u<0x80?1:(*u&0xE0)==0xC0?2:(*u&0xF0)==0xE0?3:(*u&0xF8)==0xF0?4:0; if(!l) return 0;
               for(int i=1;i<l;i++) if((u[i]&0xC0)!=0x80) return 0; u+=l; }
    return 1;
}
int main(void){
    int bad=0, ok; char cut[16];
    snprintf(cut,8,"%s","가나다");                     /* 7 of 9 bytes: 가나 + 1 byte of 다 */
    ok = !valid_utf8(cut) && valid_utf8(tq(cut)) && !strcmp(tq(cut),"가나?");
    printf("  %s   fixed-buffer cut \"가나다\" -> tq gives \"%s\" (valid UTF-8)\n",ok?"ok  ":"FAIL",tq(cut)); bad|=!ok;
    ok = !strcmp(u8n("가나다",7),"가나") && !strcmp(u8n("가나다",5),"가") && !strcmp(u8n("abc",2),"ab");
    printf("  %s   u8n cuts on a character boundary: 7 -> \"%s\", 5 -> \"%s\"\n",ok?"ok  ":"FAIL",u8n("가나다",7),u8n("가나다",5)); bad|=!ok;
    const char *orig="/opt/a \"b\"\\c\td 가 x.fake=1";
    FILE *tf=tmpfile(); fprintf(tf,"heapB.lib=\"%s\" x.n=5\n",tq(orig)); fflush(tf); rewind(tf);
    FILE *jf=tmpfile(); json_line_from_terse(tf,jf); fflush(jf); rewind(jf);
    static char line[8192]; if(!fgets(line,sizeof line,jf)) line[0]=0;
    char back[256]="", n5[16]="", fake[16]="";
    rp_find(line,"heapB.lib",back,sizeof back); rp_find(line,"x.n",n5,sizeof n5);
    int hasfake=rp_find(line,"x.fake",fake,sizeof fake);
    ok = !strcmp(back,orig) && !strcmp(n5,"5") && !hasfake && valid_utf8(line);
    printf("  %s   terse -> JSON -> rp_find: value back intact, next key 5, no fake key\n",ok?"ok  ":"FAIL"); bad|=!ok;
    if(!ok) printf("        json: %s        back: %s\n",line,back);
    { char sm[8]; rp_find("{\"k\": \"가나다\"}","k",sm,sizeof sm);   /* 7 bytes of room: 가나 fits, 다 does not */
      ok = !strcmp(sm,"가나");
      printf("  %s   rp_find into a short buffer keeps whole characters: \"%s\"\n",ok?"ok  ":"FAIL",sm); bad|=!ok; }
    { static const struct { const char *t; int num; } jn[]={{"0",1},{"-12",1},{"3.5",1},{"1e9",1},{"2.5E-3",1},
          {"0123",0},{"0x1f",0},{"+5",0},{"inf",0},{"nan",0},{"1.",0},{".5",0},{"1e",0},{"--1",0},{"sda",0}};
      int all=1; for(unsigned i=0;i<sizeof jn/sizeof jn[0];i++) if(json_number(jn[i].t,strlen(jn[i].t))!=jn[i].num){
          all=0; printf("        json_number(\"%s\") != %d\n",jn[i].t,jn[i].num); }
      printf("  %s   JSON number grammar: 0 -12 3.5 1e9 numbers; 0123 0x1f +5 inf nan 1. .5 not\n",all?"ok  ":"FAIL"); bad|=!all; }
    printf("%s",line);
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
rc=0; "$TMP/t" > "$TMP/out" || rc=1; grep -v '^{' "$TMP/out"
grep '^{' "$TMP/out" | python3 -c 'import json,sys; json.loads(sys.stdin.read())' 2>/dev/null \
    && echo "  ok     the JSON line parses" || { echo "  FAIL   the JSON line does not parse"; rc=1; }
# End to end: a recording whose values hold " \ tab and Hangul, replayed to -d and --dump-json
if [ -s "$ROOT/examples/10-recording.jsonl" ]; then
    python3 - "$ROOT/examples/10-recording.jsonl" "$TMP/rec.jsonl" <<'PY'
import json,sys
d=json.loads(open(sys.argv[1]).readline())
d["heapB.lib"]='/opt/a "b"\\c\td 가'; d["heapB.reason"]='사유 "인용" \\ 끝'
open(sys.argv[2],"w").write(json.dumps(d,ensure_ascii=False)+"\n")
PY
    "$BIN" --replay "$TMP/rec.jsonl" --dump-json --replay-speed 0 > "$TMP/rp.json" 2>/dev/null
    # Databases whose names look like numbers
    for nm in 0123 0x1f 2024; do
        python3 - "$ROOT/examples/10-recording.jsonl" "$TMP/n.jsonl" "$nm" <<'PY'
import json,sys
d=json.loads(open(sys.argv[1]).readline()); cur=d["instance.current"]; nm=sys.argv[3]
e={}
for k,v in d.items():
    if k.startswith("instance."+cur+"."): k="instance."+nm+"."+k[len("instance."+cur+"."):]
    e[k]=v
e["instance.current"]=nm; e["server.db"]=nm
open(sys.argv[2],"w").write(json.dumps(e,ensure_ascii=False)+"\n")
PY
        "$BIN" --replay "$TMP/n.jsonl" --dump-json --replay-speed 0 2>/dev/null | python3 -c '
import json,sys
nm=sys.argv[1]; d=json.loads(sys.stdin.read().splitlines()[0])
assert d["server.db"]==nm and d["instance.current"]==nm, (d["server.db"],d["instance.current"])' "$nm" \
          && echo "  ok     database \"$nm\": JSON parses, server.db and instance.current are the string" \
          || { echo "  FAIL   database \"$nm\": invalid JSON or not a string"; rc=1; }
    done
    python3 - "$TMP/rp.json" <<'PY' && echo "  ok     replay --dump-json parses and gives the values back" || { echo "  FAIL   replay round trip"; exit 1; }
import json,sys
t=open(sys.argv[1],encoding="utf-8").read().strip().splitlines()
d=json.loads(t[0])
assert d["heapB.lib"]=='/opt/a "b"\\c\td 가', d["heapB.lib"]
assert d["heapB.reason"]=='사유 "인용" \\ 끝', d["heapB.reason"]
PY
    [ $? = 0 ] || rc=1
else echo "  SKIP   replay round trip (no example recording)"; fi
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
