#!/bin/bash
# t24: hand-made minimal records on reader paths the fixtures do not cover
# (tests/inputs/*.rec) replay in every view, to their last frame (no server).
# The lines have the shapes the tool writes; the values are not from a run.
# Under tests/sanitize.sh this is where undefined behaviour on those paths
# shows.
#   idle-no-identity.rec    an idle server: no client, so no identity line
#   steady-identities.rec   identities that do not change: delta frames
#                           with nothing updated and nothing removed
#   all-line-types.rec      the example record of the specification: every
#                           line type, a delta frame that updates and removes
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
n=0
for rec in "$HERE"/inputs/*.rec; do
  frames=$(grep -c '^F ' "$rec")
  for view in "" "--json" "--detail" "--trans" "--narrow" "--wide"; do
    out=$(LANG=C timeout 20 "$TOOL" --replay "$rec" --replay-speed 0 $view 2>&1); rc=$?
    bad=$(echo "$out" | grep -m1 -e 'runtime error' -e 'Sanitizer' -e 'malformed')
    if [ -n "$bad" ] || [ $rc -ne 0 ]; then
      echo "t24 FAIL: $(basename "$rec") $view: exit $rc: $bad"
      exit 1
    fi
    # one JSON document a frame; the other views head each frame with "--- "
    if [ "$view" = "--json" ]; then mark='^{'; else mark='^--- '; fi
    got=$(echo "$out" | grep -c -e "$mark")
    if [ "$got" != "$frames" ]; then
      echo "t24 FAIL: $(basename "$rec") ${view:-(scroll)}: $got frames shown, $frames in the record"
      exit 1
    fi
    n=$((n + 1))
  done
done
echo "t24 PASS: $n replays of $(ls "$HERE"/inputs/*.rec | wc -l) records on paths the fixtures do not cover"
