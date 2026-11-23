#!/bin/bash
# t24: records the tool writes in ordinary runs that the fixtures do not
# cover (tests/inputs/*.rec) replay in every view, to their last frame (no
# server).  Under tests/sanitize.sh this is where undefined behaviour on
# those paths shows.
#   idle-no-identity.rec    an idle server: no client, so no identity line
#   steady-identities.rec   identities that do not change: delta frames
#                           with nothing updated and nothing removed
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
n=0
for rec in "$HERE"/inputs/*.rec; do
  frames=$(grep -c '^F ' "$rec")
  for view in "" "--json" "--detail" "--trans" "--narrow" "--wide"; do
    out=$(LANG=C timeout 20 "$TOOL" --replay "$rec" --replay-speed 0 $view 2>&1); rc=$?
    bad=$(echo "$out" | grep -m1 -e 'runtime error' -e 'Sanitizer' -e 'malformed')
    # --trans ends 1 when the record holds no transaction table
    if [ -n "$bad" ] || { [ $rc -ne 0 ] && ! { [ "$view" = "--trans" ] && [ $rc -eq 1 ]; }; }; then
      echo "t24 FAIL: $(basename "$rec") $view: exit $rc: $bad"
      exit 1
    fi
    if [ "$view" = "--json" ] && [ "$(echo "$out" | grep -c '^{')" != "$frames" ]; then
      echo "t24 FAIL: $(basename "$rec") --json: $(echo "$out" | grep -c '^{') documents, $frames frames"
      exit 1
    fi
    n=$((n + 1))
  done
done
echo "t24 PASS: $n replays of $(ls "$HERE"/inputs/*.rec | wc -l) ordinary records the fixtures do not cover"
