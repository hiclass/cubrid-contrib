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
# Files cub_top writes (--record, --bcb-dump) never follow a symlink and never empty a file
# unasked: a recording is created new (0600), an existing one is replaced only with
# --record-overwrite and only when it is this account's regular file.  Run as root,
# following a planted link would truncate whatever it points at.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR"); BIN="$ROOT/cub_top"
TMP=${TMPDIR:-/tmp}/cub_top-outfile.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
bad=0
ok(){ echo "  ok   $1"; }; no(){ echo "  FAIL $1"; bad=1; }
rec(){ # args...: a short live run that records; prints the exit code
    "$BIN" -b --interval 0.2 "$@" </dev/null >/dev/null 2>"$TMP/err" & p=$!
    i=0; while kill -0 $p 2>/dev/null && [ $i -lt 15 ]; do sleep 0.1; i=$((i+1)); done
    kill $p 2>/dev/null; wait $p 2>/dev/null; echo $?
}
victim="$TMP/victim"; printf 'precious\n' > "$victim"

rec --record "$TMP/new.jsonl" >/dev/null
m=$(stat -c %a "$TMP/new.jsonl" 2>/dev/null)
if [ -s "$TMP/new.jsonl" ] && [ "$m" = 600 ]; then ok "new recording created, mode $m"; else no "new recording: mode '$m', size $(stat -c %s "$TMP/new.jsonl" 2>/dev/null)"; fi

printf 'keep\n' > "$TMP/old.jsonl"
rc=$(rec --record "$TMP/old.jsonl")
if [ "$rc" = 2 ] && [ "$(cat "$TMP/old.jsonl")" = keep ]; then ok "existing recording refused, kept ($(head -1 "$TMP/err" | cut -c1-70))"
else no "existing recording: rc $rc, content '$(head -c 20 "$TMP/old.jsonl")'"; fi

rec --record "$TMP/old.jsonl" --record-overwrite >/dev/null
if grep -q '^{' "$TMP/old.jsonl"; then ok "--record-overwrite replaces this account's file"; else no "--record-overwrite did not replace"; fi

ln -s "$victim" "$TMP/link.jsonl"
for o in "" --record-overwrite; do
    rc=$(rec --record "$TMP/link.jsonl" $o)
    if [ "$rc" = 2 ] && [ "$(cat "$victim")" = precious ]; then ok "symlink not followed ${o:-(no overwrite)}: $(head -1 "$TMP/err" | cut -c1-60)"
    else no "symlink ${o:-(no overwrite)}: rc $rc, victim '$(head -c 20 "$victim")'"; fi
done

if [ "$(id -u)" = 0 ] && id hiclass >/dev/null 2>&1; then
    printf 'theirs\n' > "$TMP/theirs.jsonl"; chown hiclass "$TMP/theirs.jsonl"
    rc=$(rec --record "$TMP/theirs.jsonl" --record-overwrite)
    if [ "$rc" = 2 ] && [ "$(cat "$TMP/theirs.jsonl")" = theirs ]; then ok "another account's file not replaced"
    else no "another account's file: rc $rc, content '$(head -c 20 "$TMP/theirs.jsonl")'"; fi
else echo "  SKIP another account's file (needs root and a second account)"; fi

if "$BIN" -d 2>/dev/null | grep -q '^pgbuf.enabled=1'; then
    ln -s "$victim" "$TMP/dump.bin"
    "$BIN" --bcb-dump "$TMP/dump.bin" >/dev/null 2>&1
    if [ "$(cat "$victim")" = precious ] && [ -f "$TMP/dump.bin" ] && [ ! -L "$TMP/dump.bin" ]; then ok "--bcb-dump replaces a symlink by a file, the link target untouched"
    else no "--bcb-dump through a symlink: victim '$(head -c 20 "$victim")'"; fi
    left=$(ls "$TMP" | grep -c '\.tmp$')
    if [ "$left" = 0 ]; then ok "no temporary left behind"; else no "$left temporary file(s) left"; fi
else echo "  SKIP --bcb-dump (no buffer pool readable)"; fi

if [ $bad = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
