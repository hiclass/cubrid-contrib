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
# What volmap says about the processes beside a database comes from /proc alone, and
# it must say the right thing:
#   - a running server is "running" with its pid, registered with cub_master;
#   - a database with no server is "not_running";
#   - a backupdb on the database is reported while it runs (batch head, JSON
#     maintenance, and the interactive bar with the scan paused) and not after;
#   - a TDE note never makes --check exit 2 on its own (checked on the JSON shape).
# Needs: a running database RUNDB (default demodb, backed up with backupdb -C into a
# temporary directory - a read of the database), and a database with no server STOPDB
# (a name or a vinf path; default: none, that check is skipped).
#
#   RUNDB=demodb STOPDB=/path/to/x_vinf sh tools/check-procstate.sh
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
BIN=${VOLMAP:-$ROOT/cub_volmap}
RUNDB=${RUNDB:-demodb}
TMP=$(mktemp -d /tmp/volmap-proc.XXXXXX)
rc=0
cleanup(){ [ -n "$bp" ] && kill $bp 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
[ -x "$BIN" ] || { echo "→ FAIL (no binary: $BIN)"; exit 1; }
command -v cubrid >/dev/null || { echo "→ SKIP (no cubrid in PATH)"; exit 0; }
j(){ python3 -c "import sys,json; d=json.load(sys.stdin); print($1)"; }
# 1. the running server
st=$("$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['server']['state'], d['server']['registered'], d['server']['pid']>0")
if [ "$st" = "running True True" ]; then echo "  ok     $RUNDB: server running, registered with cub_master (from /proc/net/unix)"
else echo "  FAIL   $RUNDB: server state '$st' (want running True True)"; rc=1; fi
# 2. a database without a server
if [ -n "$STOPDB" ]; then
  st=$("$BIN" --format=json "$STOPDB" 2>/dev/null | j "d['server']['state'], d['maintenance']")
  if [ "$st" = "not_running None" ]; then echo "  ok     $STOPDB: no server, no utility"
  else echo "  FAIL   $STOPDB: '$st' (want not_running None)"; rc=1; fi
fi
# 3. backupdb while it runs: batch head, JSON, interactive bar; and gone afterwards
mkdir -p "$TMP/bk"
cubrid backupdb -C -D "$TMP/bk" "$RUNDB" > "$TMP/bk.out" 2>&1 & bp=$!
sleep 1.5
head=$("$BIN" --plain "$RUNDB" 2>/dev/null | grep -c 'maintenance: cubrid backupdb is running')
mt=$("$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['maintenance']['utility'] if d['maintenance'] else None")
cat > "$TMP/bar.py" <<'PY'
import os, sys, pty, time, select, fcntl, termios, struct, signal, re
binp, db = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"; os.execv(binp, [binp, "-i", db])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 50, 200, 0, 0))
buf = b""; t0 = time.time()
while time.time() - t0 < 3:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try: buf += os.read(fd, 65536)
        except OSError: break
txt = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", buf).decode("utf-8", "replace")
print("paused" if "backupdb running" in txt and "paused" in txt else "no-bar")
os.write(fd, b"q"); time.sleep(0.5)
try: os.waitpid(pid, 0)
except ChildProcessError: pass
try: os.kill(pid, signal.SIGKILL)
except OSError: pass
PY
bar=$(timeout 20 python3 "$TMP/bar.py" "$BIN" "$RUNDB" 2>/dev/null)
wait $bp; bprc=$?; bp=""
after=$("$BIN" --format=json "$RUNDB" 2>/dev/null | j "d['maintenance']")
if [ "$head" = 1 ] && [ "$mt" = backupdb ] && [ "$bar" = paused ] && [ "$after" = None ]; then
  echo "  ok     backupdb on $RUNDB seen while it ran (head, JSON, -i bar paused) and not after (backup rc $bprc)"
else echo "  FAIL   backupdb detection: head=$head json=$mt bar=$bar after=$after (backup rc $bprc)"; rc=1; fi
# 4. notes are not findings: the JSON has both arrays and notes carry no "finding" key
nt=$("$BIN" --format=json --check "$RUNDB" 2>/dev/null | j "isinstance(d['notes'], list) and all('finding' not in n for n in d['notes'])")
if [ "$nt" = True ]; then echo "  ok     JSON notes[] is separate from findings[]"
else echo "  FAIL   JSON notes: $nt"; rc=1; fi
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
