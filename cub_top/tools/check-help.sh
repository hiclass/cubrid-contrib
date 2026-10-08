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
# Regression: help completeness.
# hout()'s fixed buffer (HOUT_MAX) and the collection array (HELPMAX) truncate silently,
# so the help is checked to arrive whole.
# The last line, the required sections and UTF-8 integrity are checked to prevent that.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/cub_top.c"
BIN="$(mktemp -u /tmp/cub_top-help.XXXXXX)"
trap 'rm -f "$BIN"' EXIT
gcc -O2 -std=gnu99 -o "$BIN" "$SRC" 2>/dev/null || { echo "-> FAIL (compile failed)"; exit 1; }

ERR="$("$BIN" -h 2>&1 >/dev/null)"
OUT="$("$BIN" -h 2>/dev/null)"
N=$(printf '%s\n' "$OUT" | wc -l)
bad=0
echo "help: $N lines"

# 1) A hout/HELPMAX truncation warning fails immediately
if [ -n "$ERR" ]; then echo "  truncation warning: $ERR"; bad=1
else echo "  ok  no truncation warning"; fi
if printf '%s' "$OUT" | grep -q 'HELPMAX 에서 잘렸다'; then
    echo "  FAIL HELPMAX exceeded"; bad=1; fi

# 2) The last line being present proves the block was emitted in full
if printf '%s' "$OUT" | tail -1 | grep -q 'portability.md'; then
    echo "  ok  last line reached"
else echo "  FAIL last line missing - the help was cut short"; bad=1; fi

# 3) Each section must survive (an overflow drops them from the end)
#    -h (CLI) carries only the concept and option sections; the mode-key sections must be absent.
for sec in "환경변수 CUBRID" "여러 데이터베이스" "두 가지 라이브 화면" \
           "대시보드 화면 구성" "CPU 와 load" "wa  iowait" "D스레드 n/m" "I/O 판정문" \
           "파라미터 귀속률" "프로세스 박스가 PSS 인 이유" "관측 등급" "각 항목이 알려주는 것"; do
    if printf '%s' "$OUT" | grep -qF "$sec"; then :; else
        echo "  FAIL missing: $sec"; bad=1; fi
done
[ $bad -eq 0 ] && echo "  ok  all 12 required sections present"
# -h is the CLI context, so no mode-key section may appear
for kx in "대시보드 키 (지금 이 화면)" "시계열 키 (지금 이 화면)"; do
    if printf '%s' "$OUT" | grep -qF "$kx"; then
        echo "  FAIL a mode-key section leaked into -h: $kx"; bad=1; fi
done
[ $bad -eq 0 ] && echo "  ok  no mode-key section in -h (CLI context)"

# 4) UTF-8 integrity: truncation cuts mid-sequence and leaves a broken character
if printf '%s' "$OUT" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1; then
    echo "  ok  UTF-8 intact"
else echo "  FAIL UTF-8 broken - cut at a buffer boundary"; bad=1; fi

# 5) Help line count must stay below HELPMAX with margin; overflow truncates the live h pager.
NL=$(printf '%s\n' "$OUT" | wc -l)
HM=$(grep -m1 '#define HELPMAX' "$(dirname "$0")/../src/cub_top.c" | awk '{print $3}')
if [ -n "$HM" ] && [ "$NL" -lt $((HM-20)) ]; then
    echo "  ok  line count has margin ($NL < HELPMAX $HM - 20)"
else echo "  FAIL ${NL} help lines is at or near HELPMAX ${HM:-?} - raise HELPMAX"; bad=1; fi

# 6) The per-mode key help must be defined in the source (the section live h leads with).
#    Live h needs a tty and cannot run here, so only its definition and wiring are checked.
for fn in "help_keys_dash" "help_keys_plot" "usage_modal(view?2:1)"; do
    if grep -qF "$fn" "$SRC"; then :; else
        echo "  FAIL per-mode key help missing: $fn"; bad=1; fi
done
[ $bad -eq 0 ] && echo "  ok  per-mode key help defined and wired"

[ $bad -eq 0 ] && { echo "→ PASS"; exit 0; }
echo "→ FAIL"; exit 1
