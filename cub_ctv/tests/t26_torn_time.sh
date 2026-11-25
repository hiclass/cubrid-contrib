#!/bin/bash
# t26: a statistics block whose time sum was read between the engine's
# stores (tv_usec >= 1e6) is read again; whole blocks are not (no server).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v '/cub_ctv\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -Wl,--wrap=mem_gather_ok,--wrap=mem_read \
  -o "$T/t26" "$HERE/t26_torn_time.c" "${SRCS[@]}" -lm || { echo "t26 FAIL: build"; exit 1; }
LANG=C timeout 60 "$T/t26" || { echo "t26 FAIL: exit $?"; exit 1; }
