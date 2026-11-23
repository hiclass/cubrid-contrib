#!/bin/bash
# t22: OS state is kept per (thread, CS), read once a sample per thread
# (one sample of every block), and the verdict's disk/CPU row is the named
# holder's state under the top CS - or none, and not judged from fewer
# readings than OS_MIN_READS (no server).  Checked in the sampler,
# the verdict, the record's O lines (also damaged ones) and the summary
# screen of the replayed record.  The cases are listed in the .c file.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "t22 FAIL: $*"; exit 1; }
W=-Wl,--wrap=mono_s,--wrap=thr_index_of_tid_mem,--wrap=thr_tid_of,--wrap=os_task_comm,--wrap=os_task_state,--wrap=cs_owner_now
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -o "$T/t22" "$HERE/t22_os_per_cs.c" "${SRCS[@]}" -lm $W \
  || fail "build"
LANG=C "$T/t22" "$T/r.rec" "$T/few.rec" "$T/zero.rec" "$T/short.rec" "$T/w26.rec" "$T/w27.rec" "$T/hz50.rec" "$T/hz50ok.rec" "$T/rest.rec" || exit 1
for want in "O 10 1 1010 20 0 20 0 " "O 10 0 1010 79 79 0 0 " "O 11 1 1011 75 0 75 0 " "O 11 0 1011 1 0 1 0 "; do
  grep -q "^$want" "$T/r.rec" || fail "record lacks '$want': $(grep '^O ' "$T/r.rec" | tr '\n' ';')"
done
[ "$(grep -c '^O ' "$T/r.rec")" = 4 ] || fail "record has $(grep -c '^O ' "$T/r.rec") O lines, want 4"
# a damaged O line is refused with its line number
n=$(grep -n '^O 10 0 ' "$T/r.rec" | cut -d: -f1)
bad_o() { # name, sed expression
  sed "${n}$2" "$T/r.rec" > "$T/bad.rec"
  out=$(LANG=C timeout 10 "$TOOL" --replay "$T/bad.rec" --replay-speed 0 2>&1); rc=$?
  [ $rc -ne 0 ] && echo "$out" | grep -q "malformed record file at .*bad.rec:$n" \
    || fail "O line with $1 accepted (exit $rc): $(echo "$out" | tail -1)"
}
bad_o "a CS past the count" 's/^O 10 0 /O 10 2 /'
bad_o "CS -2" 's/^O 10 0 /O 10 -2 /'
bad_o "no CS token (the former 11-token line)" 's/^O 10 0 /O 10 /'
bad_o "a negative count" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 76 79 -3 0 /'
bad_o "more states than readings" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 79 79 5 0 /'
bad_o "negative readings" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 -5 3 -8 0 /'
bad_o "states that add up only by overflow" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 79 2147483647 2147483647 81 /'
bad_o "more readings than the window has samples" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 101 101 0 0 /'
# each bound alone (the window has 100 samples)
bad_o "readings past the window, states within it" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 150 50 50 50 /'
bad_o "R below 0" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 79 -1 80 0 /'
bad_o "D below 0" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 79 80 -1 0 /'
bad_o "other below 0" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 79 80 0 -1 /'
bad_o "R past the window" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 1 2147483647 1 0 /'
bad_o "D past the window" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 1 1 2147483647 0 /'
bad_o "other past the window" 's/^O 10 0 1010 79 79 0 0 /O 10 0 1010 1 1 0 2147483647 /'
bad_o "a negative span" 's/^\(O 10 0 1010 79 79 0 0 .*\) [0-9]*$/\1 -1/'
# tick tokens are not checked (a thread slot may change hands in a window,
# so they can run backwards): any value replays, without overflow
sed "${n}s/^O 10 0 1010 79 79 0 0 [0-9]* [0-9]* /O 10 0 1010 79 79 0 0 -9223372036854775808 9223372036854775807 /" "$T/r.rec" > "$T/ticks.rec"
LANG=C timeout 10 "$TOOL" --replay "$T/ticks.rec" --replay-speed 0 >/dev/null 2>"$T/ticks.err" \
  || fail "extreme tick tokens do not replay: $(tail -1 "$T/ticks.err")"
# a live record's hz is checked like --hz: the screen reckons with it
for hz in 0 -5 10001 2147483647; do
  sed "2s/^H \([0-9]*\) 1 100 /H \1 1 $hz /" "$T/r.rec" > "$T/bad.rec"
  out=$(LANG=C timeout 10 "$TOOL" --replay "$T/bad.rec" --replay-speed 0 2>&1); rc=$?
  [ $rc -ne 0 ] && echo "$out" | grep -q "malformed record file at .*bad.rec:2" \
    || fail "header hz $hz accepted (exit $rc): $(echo "$out" | tail -1)"
done
screen() { # record
python3 - "$TOOL" "$1" <<'PY'
import os, pty, fcntl, termios, struct, time, select, re, sys
tool, rec = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
    os.environ.pop("COLUMNS", None)
    os.execv(tool, ["cub_ctv", "--replay", rec, "--replay-speed", "0", "--plain", "--en"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
buf, t = b"", time.time()
while time.time() - t < 2.0:
    if select.select([fd], [], [], 0.05)[0]:
        try:
            d = os.read(fd, 1 << 16)
        except OSError:
            break
        if not d:
            break
        buf += d
try:
    os.kill(pid, 15)
except ProcessLookupError:
    pass
os.waitpid(pid, 0)
print(re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", buf.decode("utf-8", "replace")).replace("\r", ""))
PY
}
SCREEN=$(screen "$T/r.rec")
screen "$T/ticks.rec" | grep -q "disk/cpu" || fail "extreme tick tokens: no disk/cpu row (the tool stopped?)"
echo "$SCREEN" | grep -q "who  *client session tran 5 " || fail "who row is not tran 5: $SCREEN"
echo "$SCREEN" | grep -q "disk/cpu  *holder thread CPU-bound  *worker-10: 79 reads D 0% R 100%" \
  || fail "disk/cpu row is not thread 10's state under CS 0: $(echo "$SCREEN" | grep -m1 'disk/cpu')"
echo "$SCREEN" | grep -q "worker-11: D" && fail "disk/cpu row shows the thread that held LOG"
MIN=$(sed -n 's/^#define OS_MIN_READS  *\([0-9]*\).*/\1/p' "$HERE/../src/ctv.h")
ZERO=$(screen "$T/zero.rec")
echo "$ZERO" | grep -q "disk/cpu  *holder thread state unknown  *worker-10: 0 reads (under $MIN): D 0 R 0 other 0" \
  || fail "disk/cpu row of a holder never read is missing or hides the count: $(echo "$ZERO" | grep -m1 -e 'disk/cpu' -e 'not shown')"
echo "$ZERO" | grep -q "window too short" && fail "a 2 s window is called too short: $(echo "$ZERO" | grep -m1 'disk/cpu')"
SHORT=$(screen "$T/short.rec")
echo "$SHORT" | grep -q "worker-10: 4 reads (under $MIN): D 0 R 4 other 0; window too short for $MIN reads (20 samples, 27 needed)" \
  || fail "a 20-sample window does not say it is too short: $(echo "$SHORT" | grep -m1 -A1 'disk/cpu')"
W26=$(screen "$T/w26.rec")
echo "$W26" | grep -q "window too short for $MIN reads (26 samples, 27 needed)" \
  || fail "a 26-sample window does not say it is too short: $(echo "$W26" | grep -m1 -A1 'disk/cpu')"
W27=$(screen "$T/w27.rec")
echo "$W27" | grep -q "reads (under $MIN)" || fail "the 27-sample window shows no readings: $(echo "$W27" | grep -m1 -A1 'disk/cpu')"
echo "$W27" | grep -q "window too short" && fail "a 27-sample window (it can hold $MIN readings) is called too short: $(echo "$W27" | grep -m1 'disk/cpu')"
HZ50=$(screen "$T/hz50.rec")
echo "$HZ50" | grep -q "window too short for $MIN reads (16 samples, 17 needed)" \
  || fail "a 16-sample window at 50 Hz is not reckoned by the record's hz: $(echo "$HZ50" | grep -m1 -A1 'disk/cpu')"
HZ50OK=$(screen "$T/hz50ok.rec")
echo "$HZ50OK" | grep -q "reads (under $MIN)" || fail "the 20-sample window at 50 Hz shows no readings: $(echo "$HZ50OK" | grep -m1 -A1 'disk/cpu')"
echo "$HZ50OK" | grep -q "window too short" && fail "20 samples at 50 Hz (17 can hold $MIN readings) are called too short: $(echo "$HZ50OK" | grep -m1 'disk/cpu')"
FEW=$(screen "$T/few.rec")
echo "$FEW" | grep -q "disk/cpu  *holder thread state unknown  *worker-10: $((MIN - 1)) reads (under $MIN): D 0 R $((MIN - 1)) other 0" \
  || fail "disk/cpu row of $((MIN - 1)) readings is judged or hides them: $(echo "$FEW" | grep -m1 'disk/cpu')"
echo "t22 PASS: OS state per (thread, CS); one procfs read a sample per thread, one sample a block, failing reads too; disk/cpu = the named holder under the top CS, or none, not judged under $MIN readings (0 shown as 0); damaged O lines refused"
