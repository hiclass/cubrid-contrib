#!/bin/bash
# t31: all_tdes and its count are read again every window: an array the
# engine reallocated (restart recovery), grew in place or freed (shutdown)
# is followed, never read where it was (no server; the test reads itself).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_tran\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -o "$T/t31" "$HERE/t31_tran_table_moves.c" "${SRCS[@]}" -lm \
  || { echo "t31 FAIL: build"; exit 1; }
LANG=C timeout 60 "$T/t31" || exit 1
# every window asks: the frame capture calls it before it takes the count
awk '/^view_capture_idents/ { f = 1 } f && /tran_table_recheck \(\)/ { r = NR } f && /view.ntran = / { n = NR; exit }
     END { exit !(r && n && r < n) }' "$HERE/../src/ctv_frame.c" \
  || { echo "t31 FAIL: view_capture_idents does not recheck the table before it takes the count"; exit 1; }
