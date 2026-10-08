#!/bin/bash
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
# Regression: retention of I/O verdicts.
# A verdict survives its condition clearing, staying up to two rows with its timestamp,
# because an I/O anomaly is brief and would otherwise vanish as soon as it was seen.
# A real bottleneck is hard to reproduce, so the retention logic is duplicated and table-tested.
# When cub_top.c's retention logic changes, update step() in this file too.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/verdict-keep-test.c"
BIN="$(mktemp -u /tmp/verdict-keep.XXXXXX)"
trap 'rm -f "$BIN"' EXIT
gcc -O2 -Wall -Wextra -o "$BIN" "$SRC" 2>/dev/null || { echo "-> FAIL (compile failed)"; exit 1; }
echo "verdict retention (occur, hold, recur, push down, limit)"
"$BIN" || { echo "→ FAIL"; exit 1; }
CS="$ROOT/src/cub_top.c"
# Keep the table in step with the source: confirm the key invariants still exist there
for pat in '#define VDKEEP 2' 'if(g_vdn>0) g_vd[0].live=0;' 'g_vd[0].kind==verdict'; do
    grep -qF "$pat" "$CS" || { echo "-> FAIL ('$pat' has disappeared from the source)"; exit 1; }
done
echo "→ PASS"
