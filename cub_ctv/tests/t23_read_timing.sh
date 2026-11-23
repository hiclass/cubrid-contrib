#!/bin/bash
# t23: procfs readings of a holder fall anywhere in its holds with the same
# chance (no server).  The cases are listed in the .c file.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
W=-Wl,--wrap=mono_s,--wrap=thr_index_of_tid_mem,--wrap=thr_tid_of,--wrap=os_task_comm,--wrap=os_task_state,--wrap=cs_owner_now
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -o "$T/t23" "$HERE/t23_read_timing.c" "${SRCS[@]}" -lm $W \
  || { echo "t23 FAIL: build"; exit 1; }
LANG=C "$T/t23"
