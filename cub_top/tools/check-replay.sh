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
last,out=rec[-1],rep[-1]
# Properties of the run, not of the frame: how it was invoked, when, and which file
# the recording host's process mapped.
SKIP={'opts.used','ts','ts_epoch','heapB.lib'}
diff=[]; n=0
for k,v in last.items():
    if k in SKIP:
        continue
    n+=1
    if k not in out: diff.append((k,v,'<missing>')); continue
    w=out[k]
    if isinstance(v,(int,float)) and isinstance(w,(int,float)):
        if abs(float(v)-float(w))>max(1e-6,abs(float(v))*1e-9): diff.append((k,v,w))
    elif v!=w: diff.append((k,v,w))
if diff:
    print(f"  FAIL {len(diff)} key(s) differ after replay:")
    for k,a,b in diff[:8]: print(f"        {k}: recorded={a} replayed={b}")
    sys.exit(1)
print(f"  ok   replay reproduces the recorded frame ({n} keys compared, {len(SKIP)} run properties skipped)")
PY
    else
        echo "  skip round-trip comparison (no python3)"
    fi
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
