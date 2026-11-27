#!/bin/bash
# t32: --trans -i goes on while the transaction table is away (a restart
# rebuilding it, an attach during a boot) and ends when the server ends
# (no server; cases in the .c file).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
W=-Wl,--wrap=mono_s,--wrap=sleep_until,--wrap=term_stop_requested,--wrap=view_capture_trans
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_live\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -o "$T/t32" "$HERE/t32_trans_away.c" "${SRCS[@]}" -lm $W \
  || { echo "t32 FAIL: build"; exit 1; }
LANG=C timeout 60 "$T/t32" || exit 1
# a record with a window without the table replays as the live run went:
# that window says so and the replay goes on
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
awk '$1 == "F" { f++; if (f == 3) $6 = 0 } { print }' "$HERE/fixtures/demodb-live.rec" > "$T/away.rec"
out=$(LANG=C timeout 30 "$TOOL" --replay "$T/away.rec" --trans --replay-speed 0 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$(echo "$out" | grep -c '^--- ')" = 9 ] && [ "$(echo "$out" | grep -c 'transaction table unavailable')" = 1 ] \
  || { echo "t32 FAIL: replay of a window without the table (exit $rc, $(echo "$out" | grep -c '^--- ') windows)"; exit 1; }
echo "t32 PASS: the replay of such a record goes on past that window too"
