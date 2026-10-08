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
# Regression: I/O verdict branches.
# Showing a verdict on screen needs a real disk bottleneck, which is hard to reproduce,
# so a table-based test duplicating the logic covers every branch.
# When cub_top.c's verdict expressions change, update decide() in this file too.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/verdict-test.c"
BIN="$(mktemp -u /tmp/verdict-test.XXXXXX)"
trap 'rm -f "$BIN"' EXIT
if ! gcc -O2 -Wall -Wextra -o "$BIN" "$SRC" 2>/dev/null; then
    echo "-> FAIL (compile failed)"; exit 1
fi
echo "verdict branches (wa, device and instance signal combinations)"
"$BIN" || { echo "→ FAIL"; exit 1; }
# Keep the table in step with the source: confirm the threshold expression is unchanged
CS="$ROOT/src/cub_top.c"
if ! grep -q 'wa_hi *= *(IO->ncpu>0 *? *100.0/IO->ncpu *: *100.0)\*0.5' "$CS"; then
    echo "-> FAIL (the source wa threshold changed - update decide() in verdict-test.c)"; exit 1
fi
echo "→ PASS"
