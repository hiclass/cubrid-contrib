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
# Regression: Hangul leaking into --ascii mode.
# A new label without its g_ascii branch (or L() table entry) leaks Hangul into
# English mode; this recurred across six plot panels and some 40 tree strings.
#
# Every --ascii output mode is checked, not just the tree.  The machine-readable
# modes matter most: -d and --dump-json are parsed by monitoring, and a Korean value
# there breaks the consumer rather than merely looking wrong.
#
# --heap is included per mode because method A/B reason strings are produced only
# on that path.  The dashboard and plots need a pty and stay out of scope.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/cub_top.c"
BIN="$(mktemp -u /tmp/cub_top-ascii.XXXXXX)"
trap 'rm -f "$BIN"' EXIT
gcc -O2 -std=gnu99 -o "$BIN" "$SRC" 2>/dev/null || { echo "-> FAIL (compile failed)"; exit 1; }

# A [Hangul] character class makes grep die with "Invalid collation character" under
# some locales, and that failure reads as zero matches - a false PASS.
# So the UTF-8 lead bytes 0xEA-0xED of Hangul syllables are matched as bytes.
HANGUL=$'[\xea-\xed][\x80-\xbf][\x80-\xbf]'

if ! "$BIN" --ascii --heap -t 2>/dev/null | grep -q 'cub_server'; then
    echo "-> SKIP (cub_server not running - no tree body)"; exit 0
fi

# Self-test: the matcher must actually find Hangul, or every check below is vacuous.
# Without this, a broken pattern or grep would report a clean PASS forever.
if ! printf 'ED\xea\xb0\x80\n' | LC_ALL=C grep -q "$HANGUL"; then
    echo "-> FAIL (Hangul matcher is broken - the check would pass vacuously)"; exit 1
fi
# ...and it must not fire on pure ASCII.
if printf 'plain ascii only\n' | LC_ALL=C grep -q "$HANGUL"; then
    echo "-> FAIL (Hangul matcher matches ASCII - false positives)"; exit 1
fi

bad=0
check() {   # check <label> <args...>
    lbl=$1; shift
    out=$("$BIN" "$@" 2>/dev/null)
    n=$(printf '%s\n' "$out" | LC_ALL=C grep -c "$HANGUL"); rc=$?
    if [ "$rc" -gt 1 ]; then echo "  FAIL $lbl (grep failed rc=$rc)"; bad=1; return; fi
    if [ "${n:-0}" -ne 0 ]; then
        echo "  FAIL $lbl: $n lines with Hangul"
        printf '%s\n' "$out" | LC_ALL=C grep "$HANGUL" | head -3 | cut -c1-110 | sed 's/^/        /'
        bad=1
    else
        echo "  ok   $lbl"
    fi
}

echo "--ascii output modes (0 Hangul required)"
check "tree -t"            --ascii -t
check "tree -t --heap"     --ascii --heap -t
check "dump -d"            --ascii -d
check "dump -d --heap"     --ascii --heap -d
check "--dump-json"        --ascii --dump-json
check "--dump-json --heap" --ascii --heap --dump-json

# Korean mode must keep its Korean - an over-eager fix that made everything English
# would otherwise pass every check above.
KO=$("$BIN" --heap -d 2>/dev/null | LC_ALL=C grep -c "$HANGUL")
if [ "${KO:-0}" -eq 0 ]; then
    echo "  FAIL Korean mode lost its Korean (-d --heap has none)"; bad=1
else
    echo "  ok   Korean mode still Korean ($KO lines)"
fi

[ $bad -eq 0 ] && { echo "-> PASS"; exit 0; } || { echo "-> FAIL"; exit 1; }
