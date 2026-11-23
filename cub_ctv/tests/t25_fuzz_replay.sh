#!/bin/bash
# t25: damaged records are refused or replayed, never a crash, a hang or (in
# tests/sanitize.sh) a sanitizer report that is not a listed open one (no
# server).  The kept inputs of tests/inputs/damaged/MANIFEST first, then
# 12000 mutated records of a fixed seed (tests/fuzz_replay.py; the valid
# records of tests/fixtures and tests/inputs are the seed records).  The run
# is the same every time; tests/fuzz_full.sh is the long one.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
out=$(timeout --foreground 540 python3 "$HERE/fuzz_replay.py" --tool "$TOOL" \
  --seeds "$HERE/fixtures" "$HERE/inputs" --fixed "$HERE/inputs/damaged" \
  --n 12000 --seed 20261123 2>&1); rc=$?
if [ $rc -ne 0 ]; then
  printf '%s\n' "$out" | tail -12
  echo "t25 FAIL: exit $rc"
  exit 1
fi
echo "t25 PASS: ${out#fuzz ok: }"
