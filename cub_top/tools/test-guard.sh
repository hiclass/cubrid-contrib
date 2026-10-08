#!/bin/sh
#
# Copyright 2016 CUBRID Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# Negative test: with an offset deliberately corrupted, do the gates actually fire?
# An L2 alignment violation is caught at runtime (method B disabled); an aligned but wrong value by L3.
# The C implementation only.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
# Source location: src/.
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-guard.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
fail=0
say(){ printf "  %-34s %s\n" "$1" "$2"; }

# ---- Negative 1: alignment violation (12->11); the builtin table must fail the gate.
#    A runtime DWARF fallback can rescue that slot, so what is tested is not
#    "B disabled" but "not adopted as table-11.5":
#      heapB.table=dwarf-runtime     gate fired, DWARF rescued it (PASS)
#      heapB.table=offsets-embedded  gate fired, an embedded table rescued it (PASS)
#      heapB.enabled=0               gate fired, no fallback available (PASS)
#      heapB.table=table-11.5        the corrupted table was adopted (FAIL)
sed 's/#define OFF_LF_ALLOCCNT     12/#define OFF_LF_ALLOCCNT     11/' "$SRC" > "$TMP/bad1.c"
if gcc -O2 -std=gnu99 -I "$(dirname "$SRC")" -o "$TMP/bad1" "$TMP/bad1.c" 2>/dev/null; then
  raw=$("$TMP/bad1" -d 2>/dev/null)
  tbl=$(printf '%s' "$raw" | grep -o 'heapB.table="[^"]*"')
  en=$(printf '%s' "$raw" | grep -o 'heapB.enabled=[01]')
  if printf '%s' "$raw" | grep -q '^server.up=0'; then
    say "C misalignment -> table rejected" "SKIP(cub_server not running)"
  elif [ "$tbl" = 'heapB.table="table-11.5"' ]; then
    say "C misalignment -> table rejected" "FAIL(corrupted table adopted)"; fail=1
  elif [ "$tbl" = 'heapB.table="dwarf-runtime"' ] \
    || [ "$tbl" = 'heapB.table="offsets-embedded"' ] \
    || [ "$en" = "heapB.enabled=0" ]; then
    say "C misalignment -> table rejected" "PASS($tbl)"
  else
    say "C misalignment -> table rejected" "FAIL(undecidable: $tbl $en)"; fail=1
  fi

  # ---- Negative 1b: with the DWARF cache corrupted too, the fallback also fails -> B disabled ----
  # Use the lib cub_top actually read (heapB.lib): after a rebuild the server's mapped old inode
  # is read via /proc/<pid>/map_files/, so the disk path's size/mtime would not match the cache key.
  SO=$(printf '%s' "$raw" | grep -m1 -oE 'heapB\.lib="[^"]+"' | sed 's/heapB.lib="//;s/"$//')
  [ -n "$SO" ] && [ "$SO" != "-" ] || SO=$(grep -m1 -oE '/[^ ]*libcubrid\.so\.[0-9.]+' /proc/$(pgrep -f 'cub_server ' | head -1)/maps 2>/dev/null)
  if [ -n "$SO" ] && stat -L "$SO" >/dev/null 2>&1; then
    SZ=$(stat -L -c%s "$SO"); MT=$(stat -L -c%Y "$SO")
    CACHE="/var/tmp/cub_top.dw-$(id -u)-$SZ-$MT.tbl"   # the name cub_top reads (uid-scoped)
    cp -f "$CACHE" "$TMP/cache.bak" 2>/dev/null
    printf 'OFF_PGBUF_NBUF 4\n' > "$CACHE"
    raw=$("$TMP/bad1" -d 2>/dev/null)
    en=$(printf '%s' "$raw" | grep -o 'heapB.enabled=[01]')
    tbl2=$(printf '%s' "$raw" | grep -o 'heapB.table="[^"]*"')
    # With the cache corrupted (so the DWARF candidate fails too), the result must be
    #   an embedded table (a correct table, not the corrupted one),
    #   a downgrade to probe-runtime (count-only), or B off.
    # What is tested is that the CORRUPTED table invents no byte values - a rescue by a
    # table that is known good is the fallback working, not the gate leaking.  So when
    # an embedded table takes over, the items sized by the table (num x struct size)
    # are compared against the healthy build.  Not the total: it includes the plan
    # cache's live usage, which moves between the two runs on any active server.
    sized() { printf '%s' "$1" | grep -oE 'heapB\.(lock_table_tran|conn_entries_css|vacuum_data)_bytes=[0-9]+' | tr '\n' ' '; }
    tot=$(printf '%s' "$raw" | grep -o 'heapB.total_bytes=[0-9]*')
    if [ "$tbl2" = 'heapB.table="probe-runtime"' ] && [ "$tot" = "heapB.total_bytes=0" ]; then
        say "corrupted cache -> probe downgrade" "PASS(count-only)"
    elif [ "$en" = "heapB.enabled=0" ]; then
        say "corrupted cache -> probe downgrade" "PASS(B OFF)"
    elif [ "$tbl2" = 'heapB.table="offsets-embedded"' ]; then
        good=$(sized "$("$ROOT/cub_top" --heap -d 2>/dev/null)"); mine=$(sized "$raw")
        if [ -n "$good" ] && [ "$mine" = "$good" ]; then
            say "corrupted cache -> embedded rescue" "PASS(sized items match the healthy build)"
        else
            say "corrupted cache -> embedded rescue" "FAIL($mine vs healthy $good)"; fail=1
        fi
    else
        say "corrupted cache -> probe downgrade" "FAIL($tbl2 $tot)"; fail=1
    fi
    if [ -f "$TMP/cache.bak" ]; then mv -f "$TMP/cache.bak" "$CACHE"; else rm -f "$CACHE"; fi
  else
    say "corrupted cache -> fallback also fails" "SKIP(libcubrid path unavailable)"
  fi
else
    say "C misalignment build" "FAIL(compile failed - the gate was not exercised)"; fail=1
fi


# ---- Negative 2: aligned but wrong offset (12->16); the L3 comparison must catch it ----
sed 's/^#define OFF_LF_ALLOCCNT        12/#define OFF_LF_ALLOCCNT        16/' "$DIR/rules-11.5.h" > "$TMP/gold_bad.h"
if "$DIR/check-offsets.sh" "$TMP/gold_bad.h" >/dev/null 2>&1
then say "L3 detects a wrong table" "FAIL(let it through)"; fail=1
else say "L3 detects a wrong table" "PASS"; fi


[ $fail -eq 0 ] && echo "→ PASS" || { echo "→ FAIL"; exit 1; }
