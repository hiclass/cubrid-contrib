#!/bin/bash
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
# Regression: a recording must replay to the same values it captured.
#
# The check is mechanical rather than visual: --replay ... -d re-emits the frame
# through the same terse path the recorder used, so a recorded frame and its
# replay must agree key for key.  Values that are true of the run rather than of
# the frame (which options were passed, whether this is a replay) are excluded.
#
# Without this, "replay looks right" would be a judgement; here it is a diff.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/cub_top"
[ -x "$BIN" ] || { echo "SKIP: no binary ($BIN) - run build.sh first"; exit 0; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
REC="$TMP/rec.jsonl"

# A recording needs a live frame; without a server there is nothing to record.
"$BIN" -d 2>/dev/null | grep -q '^server.up=1' || { echo "SKIP: cub_server not running"; exit 0; }

# --record is live-only (-b/-p); a short live run records a few frames without a tty.
"$BIN" -b --record "$REC" >/dev/null 2>&1 &
p=$!; sleep 3; kill -TERM $p 2>/dev/null; wait $p 2>/dev/null
[ -s "$REC" ] || { echo "  FAIL no frames recorded"; echo "-> FAIL"; exit 1; }

N=$(grep -c '^{' "$REC")
echo "recorded $N frame(s)"

bad=0
# 1) every line is one self-contained JSON object
if command -v python3 >/dev/null; then
    python3 - "$REC" <<'PY' || bad=1
import json,sys
n=0
for ln in open(sys.argv[1]):
    ln=ln.strip()
    if not ln: continue
    n+=1
    try: json.loads(ln)
    except Exception as e:
        print(f"  FAIL line {n} is not valid JSON: {e}"); sys.exit(1)
print(f"  ok   {n} line(s) valid JSON")
# Frame times carry the sub-second interval: ts_epoch_ms strictly increasing, and at the
# default 0.5s interval at least one gap below a second (whole seconds would lose it)
ms=[json.loads(l)['ts_epoch_ms'] for l in open(sys.argv[1]) if l.strip()]
if any(b<=a for a,b in zip(ms,ms[1:])):
    print("  FAIL ts_epoch_ms not strictly increasing"); sys.exit(1)
if len(ms)>2 and not any(b-a<1000 for a,b in zip(ms,ms[1:])):
    print("  FAIL no sub-second frame gap recorded"); sys.exit(1)
print(f"  ok   frame times in ms, gaps {[int(b-a) for a,b in zip(ms,ms[1:])][:6]}")
PY
else
    echo "  skip JSON validation (no python3)"
fi

# 2) round trip: replay re-emits what was recorded.  --dump-json keeps quoted strings whole
#    and numbers typed, so text keys (reasons, verdict) compare exactly.
"$BIN" --replay "$REC" --dump-json >"$TMP/out" 2>/dev/null
if [ ! -s "$TMP/out" ]; then
    echo "  FAIL --replay --dump-json produced nothing"; bad=1
else
    if command -v python3 >/dev/null; then
        python3 - "$REC" "$TMP/out" <<'PY' || bad=1
import json,sys
rec=[json.loads(l) for l in open(sys.argv[1]) if l.strip().startswith('{')]
rep=[json.loads(l) for l in open(sys.argv[2]) if l.strip().startswith('{')]
if len(rep)!=len(rec):
    print(f"  FAIL replay emitted {len(rep)} frame(s) for {len(rec)} recorded"); sys.exit(1)
# Every frame, both ways: a key the replay drops, a key it invents (a 0 where the
# recording had no measurement) and a changed value all fail.  Only how the run was
# invoked is a property of the run rather than of the frame.
SKIP={'opts.used'}
diff=[]; n=0
for fi,(a,b) in enumerate(zip(rec,rep)):
    for k in sorted(set(a)|set(b)):
        if k in SKIP: continue
        n+=1
        if k not in b: diff.append((fi,k,a[k],'<missing>')); continue
        if k not in a: diff.append((fi,k,'<absent>',b[k])); continue
        v,w=a[k],b[k]
        if isinstance(v,(int,float)) and isinstance(w,(int,float)):
            if abs(float(v)-float(w))>max(1e-6,abs(float(v))*1e-9): diff.append((fi,k,v,w))
        elif v!=w: diff.append((fi,k,v,w))
if diff:
    print(f"  FAIL {len(diff)} key(s) differ after replay:")
    for fi,k,a,b in diff[:8]: print(f"        frame {fi} {k}: recorded={a} replayed={b}")
    sys.exit(1)
print(f"  ok   replay reproduces every frame ({len(rec)} frames, {n} keys compared both ways, {len(SKIP)} run property skipped)")
PY
    else
        echo "  skip round-trip comparison (no python3)"
    fi
fi

# 2b) a large host: the per-core list of 256 cores (MAXCORE) at 100.0 must survive replay,
#     whatever this host's core count is
if command -v python3 >/dev/null; then
    python3 - "$REC" "$TMP/big.jsonl" <<'PY'
import json,sys
d=json.loads(open(sys.argv[1]).readline()); n=256
d['os.core_pct_n']=n; d['os.core_pct']=','.join(['100.0']*n)
open(sys.argv[2],'w').write(json.dumps(d,ensure_ascii=False)+'\n')
PY
    "$BIN" --replay "$TMP/big.jsonl" --dump-json >"$TMP/big.out" 2>/dev/null
    python3 - "$TMP/big.jsonl" "$TMP/big.out" <<'PY' || bad=1
import json,sys
a=json.loads(open(sys.argv[1]).readline()); b=json.loads(open(sys.argv[2]).readline())
if a['os.core_pct']!=b.get('os.core_pct') or a['os.core_pct_n']!=b.get('os.core_pct_n'):
    print(f"  FAIL 256-core list: replayed n={b.get('os.core_pct_n')} ({len(b.get('os.core_pct',''))}B of {len(a['os.core_pct'])}B)"); sys.exit(1)
print(f"  ok   256-core list replays whole ({len(a['os.core_pct'])}B)")
PY
fi

# 2c) the plot view says it is a replay and when it was recorded (driven in a pty)
if command -v python3 >/dev/null; then
    python3 - "$BIN" "$REC" <<'PY' || bad=1
import os,pty,subprocess,select,time,re,sys,fcntl,termios,struct
binp,rec=sys.argv[1],sys.argv[2]
m,s=pty.openpty(); fcntl.ioctl(s,termios.TIOCSWINSZ,struct.pack('HHHH',60,160,0,0))
pr=subprocess.Popen([binp,'--replay',rec,'-p','--replay-speed','0.2'],stdin=s,stdout=s,
                    stderr=subprocess.DEVNULL,close_fds=True)
os.close(s); out=b''; t=time.time()
while time.time()-t<3 and pr.poll() is None:
    r,_,_=select.select([m],[],[],0.2)
    if r:
        try: out+=os.read(m,1<<16)
        except OSError: break
try: os.write(m,b'q')
except OSError: pass
try: pr.wait(timeout=3)
except Exception: pr.kill()
os.close(m)
txt=re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',out.decode('utf-8','replace'))
mark=re.search(r'(재생|REPLAY) \d+/\d+',txt); when=re.search(r'(기록|recorded) \d{4}-\d\d-\d\d \d\d:\d\d:\d\d',txt)
if not (mark and when):
    print(f"  FAIL plot view lacks the replay mark or recorded time (mark={bool(mark)} time={bool(when)})"); sys.exit(1)
print(f"  ok   plot view shows \"{mark.group(0)}\" and \"{when.group(0)}\"")
PY
fi

# 3) a replay must read no /proc of any process - it is a file, not a measurement
if command -v strace >/dev/null; then
    strace -f -e trace=openat -o "$TMP/st" "$BIN" --replay "$REC" -d >/dev/null 2>&1
    n=$(grep -cE '"/proc/[0-9]' "$TMP/st" 2>/dev/null | head -1)
    [ -n "$n" ] || n=0
    if [ "${n:-0}" -ne 0 ]; then
        echo "  FAIL replay opened /proc/<pid> $n time(s) - it must draw only from the file"; bad=1
    else
        echo "  ok   replay reads no /proc/<pid>"
    fi
else
    echo "  skip /proc isolation check (no strace)"
fi

[ $bad -eq 0 ] && { echo "-> PASS"; exit 0; } || { echo "-> FAIL"; exit 1; }
