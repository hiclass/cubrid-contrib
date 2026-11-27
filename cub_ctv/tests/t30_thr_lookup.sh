#!/bin/bash
# t30: within a window the owner's thread is looked up again whenever the
# thread tables were rebuilt, and an OS-state entry never mixes two
# threads that had the same slot (no server).  Cases in the .c file.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
W=-Wl,--wrap=mono_s,--wrap=thr_index_of_tid_mem,--wrap=thr_tid_of,--wrap=os_task_comm,--wrap=os_task_state,--wrap=cs_owner_now,--wrap=thr_cache_gen
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall ${CTV_TEST_CFLAGS:-} -I"$HERE/../src" -o "$T/t30" "$HERE/t30_thr_lookup.c" "${SRCS[@]}" -lm $W \
  || { echo "t30 FAIL: build"; exit 1; }
LANG=C timeout 60 "$T/t30" || exit 1
# the generation the sampler watches moves whenever the tables are rebuilt
awk '/^thr_sorted_rebuild/ { f = 1 } f && /thr_gen\+\+/ { ok = 1 } f && /^}/ { exit } END { exit !ok }' "$HERE/../src/ctv_thr.c" \
  || { echo "t30 FAIL: thr_sorted_rebuild does not move thr_cache_gen"; exit 1; }
