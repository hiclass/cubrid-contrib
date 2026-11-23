#!/bin/bash
# fuzz_full.sh: the long run of tests/fuzz_replay.py on an ASan + UBSan build
# of the tool: 3 seeds x 60000 mutated records, the interactive screen on a
# pty included (no server; about 20 minutes with its 8 jobs).  Run it before a
# push and with every change of the record reader (src/ctv_rec.c) or of what
# a view does with a record's values.  An input it finds goes to
# tests/inputs/damaged/ with a MANIFEST row.
# Exit 0: nothing new; 1: findings (their reproducers' directory is
# printed); 2: not run (the sanitizer build failed: see tests/sanitize.sh).
#   usage: tests/fuzz_full.sh [SEED...]      default seeds 1 2 3
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
SAN="-g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources)
if ! gcc -O1 $SAN -I"$HERE/../src" -o "$T/cub_ctv" "${SRCS[@]}" -lm 2>"$T/build.log"; then
  tail -5 "$T/build.log"
  echo "fuzz_full NOT RUN: no sanitizer build (tests/sanitize.sh tells a missing runtime from a build error)"
  exit 2
fi
[ $# -gt 0 ] || set -- 1 2 3
rc=0
for seed in "$@"; do
  python3 "$HERE/fuzz_replay.py" --tool "$T/cub_ctv" \
    --seeds "$HERE/fixtures" "$HERE/inputs" --fixed "$HERE/inputs/damaged" \
    --n 60000 --seed "$seed" --jobs 8 --pty-share 0.15 || rc=1
done
[ $rc -eq 0 ] && echo "fuzz_full PASS: seeds $*"
exit $rc
