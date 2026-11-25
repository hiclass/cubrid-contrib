#!/bin/bash
# t13: verdict figures of the live fixture's frames vs golden (no server).
# Regenerate the golden with --update.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
G=$HERE/fixtures/demodb-live.rec.verdict.golden
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v '/cub_ctv\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -o "$T/t13" "$HERE/t13_verdict.c" "${SRCS[@]}" \
  || { echo "t13 FAIL: build"; exit 1; }
LANG=C "$T/t13" "$HERE/fixtures/demodb-live.rec" > "$T/out" || { echo "t13 FAIL: run"; exit 1; }
if [ "${1:-}" = "--update" ]; then cp "$T/out" "$G"; echo "t13 golden updated"; exit 0; fi
cmp -s "$T/out" "$G" || { diff "$T/out" "$G" | head; echo "t13 FAIL: verdict figures differ"; exit 1; }
# window growth of the first frame's u32 counters, by case.  awk sets the
# rows of that frame (row N = CS index; C tokens: $9 has_stats, $10 nenter,
# $11 nwait, $13 total_ms, $20/$21/$23 their window-start values; F token
# $5 = dt); then "NAME:enter:wait" gives the growth wanted of a CS.
check () {
  local name=$1 prog=$2; shift 2
  awk "\$1 == \"F\" { f++ } $prog { print }" \
    "$HERE/fixtures/demodb-live.rec" > "$T/c.rec"
  "${CUB_CTV_TOOL:-$HERE/../cub_ctv}" --replay "$T/c.rec" --json 2>/dev/null | head -1 | python3 -c '
import json, sys
d = json.loads(sys.stdin.readline())
for w in sys.argv[1:]:
    n, de, dw = w.split(":")
    r = [c for c in d["cs"] if c["name"] == n][0]
    for got, want in ((r["enter_s"] * d["dt"], float(de)), (r["wait_s"] * d["dt"], float(dw))):
        if abs(got - want) > 0.01 * max(want, 1) + 1:
            sys.exit("%s: growth %.0f, wanted %.0f" % (n, got, want))' "$@" \
    || { echo "t13 FAIL: counter growth ($name)"; exit 1; }
}
row () { echo "f == 1 && \$1 == \"C\" && \$2 == \"$1\" { $2 }"; }
LOG=1; CLS=2; DISK=5; HEAP=7
# the frame as recorded
check "plain growth" "" LOG:1243:362 HEAP_CHNGUESS:3070:8 DISK_CHECK:1768:0
# a counter of one row fell far (a reset of the statistics, or a wrap: not
# told apart): the frame is rebased, no row has a growth
NONE="LOG:0:0 HEAP_CHNGUESS:0:0 DISK_CHECK:0:0"
check "reset" "$(row $LOG '$10 = 1200; $11 = 300; $13 = 500.0')" $NONE
check "reset, a CS that never waited" \
  "$(row $LOG '$23 = 0.0; $21 = 0; $10 = 1200; $11 = 0; $13 = 0.0')" $NONE
check "reset, time grew past the old value" \
  "$(row $LOG '$23 = 100.0; $10 = 1200; $11 = 300; $13 = 500.0')" $NONE
check "u32 wrap of the enters" \
  "$(row $LOG '$20 = 4294967000; $10 = 500; $13 = 999999.0')" $NONE
check "enters grew, waits fell by 4097" "$(row $LOG '$11 = $21 - 4097')" $NONE
check "u32 wrap of the waits" "$(row $LOG '$21 = 4294967200; $11 = 100')" $NONE
check "the first CS alone fell" "$(row 0 '$20 = 10000; $10 = 5')" $NONE
check "the last CS alone fell" "$(row 17 '$10 = 5')" $NONE
# CS_SLIP_MAX (4096): nenter that fell by no more lost updates of the
# engine's unlocked nenter++; that counter grew by none, nothing else changes
check "enters fell by 1" "$(row $LOG '$10 = $20 - 1')" LOG:0:362 HEAP_CHNGUESS:3070:8
check "enters fell by 4096" "$(row $LOG '$10 = $20 - 4096')" LOG:0:362 HEAP_CHNGUESS:3070:8
check "enters fell by 4097: rebased" "$(row $LOG '$10 = $20 - 4097')" $NONE
check "waits fell by 4096" "$(row $LOG '$11 = $21 - 4096')" LOG:1243:0 HEAP_CHNGUESS:3070:8
# a row whose statistics were not read is all zero: it rebases nothing
check "an unread row" "$(row $CLS '$9 = 0; $10 = 0; $11 = 0; $13 = 0.0')" \
  LOG:1243:362 LOCATOR_CLASSNAME_TABLE:0:0 HEAP_CHNGUESS:3070:8
# the time sum read a little low: the counters are not touched
check "torn total_ms" "$(row $LOG '$13 = 528854.0')" LOG:1243:362
# the scrolling rows of a rebased frame (enter/s, wait/s)
awk "\$1 == \"F\" { f++ } $(row $LOG '$10 = 1200; $11 = 300; $13 = 500.0') { print }" \
  "$HERE/fixtures/demodb-live.rec" > "$T/c.rec"
LANG=C "${CUB_CTV_TOOL:-$HERE/../cub_ctv}" --replay "$T/c.rec" 2>/dev/null \
  | awk '$2 == "HEAP_CHNGUESS" { print $6, $7; exit }' | grep -qx '0\.0 0\.0' \
  || { echo "t13 FAIL: scrolling row in a rebased frame"; exit 1; }
# the verdict line of the first frame (dt 0.5003 s)
top () {
  awk "\$1 == \"F\" { f++ } $2 { print }" "$HERE/fixtures/demodb-live.rec" > "$T/c.rec"
  LANG=C "$T/t13" "$T/c.rec" | grep -q "^1 top=$3 " || { echo "t13 FAIL: top CS ($1)"; exit 1; }
}
# a time sum that fell: no time lost, whatever the fall
top "time fell by 300 ms" "$(row $LOG '$13 = $23 - 300')" "LOCATOR_CLASSNAME_TABLE sev=2 lost=0.092"
top "time fell by 2 s" "$(row $LOG '$13 = $23 - 2000')" "LOCATOR_CLASSNAME_TABLE sev=2 lost=0.092"
# lost updates of the enters leave the row's time and the other rows alone
top "enters fell by 1" "$(row $LOG '$10 = $20 - 1') $(row $HEAP '$13 = 1400.0')" "LOG sev=3 lost=0.604"
# a rebased frame: no time lost on any row; what the samples saw still ranks
top "rebased frame" "$(row $LOG '$10 = 1200; $11 = 300; $13 = 500.0')" "LOG sev=2 lost=0.000"
top "rebased by another row" "$(row $HEAP '$10 = 40')" "LOG sev=2 lost=0.000"
echo "t13 PASS: verdict figures of $(wc -l < "$G") frames match the golden; rebased frame (reset, wrap), lost updates, torn time"
