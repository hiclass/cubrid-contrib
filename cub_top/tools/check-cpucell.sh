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
# Regression: cell widths in the OS box's CPU row.
# As values grow (us/sy at 100%) the labels must drop so the cell does not overflow;
# an overflow shifts the pipe and misaligns the Memory row below.  Numbers win over labels.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(mktemp -u /tmp/cpucell.XXXXXX)"
trap 'rm -f "$BIN"' EXIT
gcc -O2 -Wall -Wextra -o "$BIN" "$ROOT/tools/cpucell-test.c" 2>/dev/null \
  || { echo "-> FAIL (compile failed)"; exit 1; }
echo "CPU row cell widths (label-dropping rule)"
"$BIN" || { echo "→ FAIL"; exit 1; }
CS="$ROOT/src/cub_top.c"
grep -qF 'CPUCELL(c4,"us"' "$CS" || { echo "-> FAIL (CPUCELL missing from the source)"; exit 1; }
echo "→ PASS"
