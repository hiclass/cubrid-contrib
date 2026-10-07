#!/bin/sh
#
# Copyright 2008 Search Solution Corporation
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
# check-releases.sh - build against every current CUBRID release and report
#                     on-disk structure changes.
#
# Two things can break volmap when a release ships:
#
#   1. A header it includes changes shape.  The build catches that, so this
#      script builds the newest tag of every major.minor line.
#   2. An on-disk structure changes in a file volmap cannot include
#      (disk_manager.c, file_manager.c/h, slotted_page.h are not standalone
#      headers, so src/storage_ondisk_layout.hpp holds a copy).  A build
#      cannot catch that - the copy still compiles and silently reads the
#      wrong offsets.  This script diffs those structures between releases
#      instead.  v11.4 inserting vol_creation into DISK_VOLUME_HEADER is the
#      case this exists for.
#
# A structure that is not found at all counts as a change: renaming it, or moving
# it to a file this script does not fetch, would otherwise skip every comparison
# for it and report a clean run.
#
# Usage:
#   sh tools/check-releases.sh              # structures + build (slow)
#   sh tools/check-releases.sh --structs    # structures only (fast, no compiler)
#   sh tools/check-releases.sh --builds     # builds only
#   sh tools/check-releases.sh --develop    # also compare against upstream develop
#
# --develop reports what the next release will bring, before it ships.  It is
# opt-in because develop moves: a change there is a warning, not a defect.
#
# Exit status: 0 clean, 1 a build failed, 2 a structure changed or went missing,
#              3 both.
set -u

SELF=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-${TMPDIR:-/tmp}/volmap-releases.$$}
API=${API:-https://api.github.com/repos/CUBRID/cubrid/releases?per_page=100}
RAW=${RAW:-https://raw.githubusercontent.com/CUBRID/cubrid}
KEEP=${KEEP:-0}

do_structs=1; do_builds=1; with_develop=0
for a in "$@"; do
  case "$a" in
    --structs)   do_builds=0 ;;
    --builds)    do_structs=0 ;;
    --develop)   with_develop=1 ;;
    -h|--help)   sed -n '18,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)           echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

mkdir -p "$WORK" || exit 1
[ "$KEEP" = "1" ] || trap 'rm -rf "$WORK"' EXIT

# ---- the newest tag of each major.minor line ------------------------------
echo "querying releases"
if ! curl -fsSL "$API" -o "$WORK/releases.json"; then
    echo "cannot reach the releases API - check network access to github.com" >&2
    exit 1
fi

TAGS=$(python3 - "$WORK/releases.json" <<'PY'
import json, re, sys, collections
rel = json.load(open(sys.argv[1]))
newest = collections.OrderedDict()
for r in rel:
    t = r.get("tag_name", "")
    if not re.fullmatch(r"v\d+\.\d+(\.\d+)*", t):
        continue
    p = t.lstrip("v").split(".")
    newest.setdefault(f"{int(p[0]):03d}.{int(p[1]):03d}", t)   # releases come newest first
print(" ".join(newest[k] for k in sorted(newest)))
PY
)
[ -n "$TAGS" ] || { echo "no usable tags in the API response" >&2; exit 1; }
echo "  $(echo "$TAGS" | wc -w) release lines: $TAGS"
# develop sorts after every release, so it becomes the newest in the comparison and
# the local copy is checked against it.  Opt-in: it moves, and a change there is a
# warning about the next release rather than something to fix today.
[ "$with_develop" = "1" ] && TAGS="$TAGS develop"

rc=0

# ---- on-disk structures ---------------------------------------------------
if [ "$do_structs" = "1" ]; then
    echo
    echo "== on-disk structures =="
    for t in $TAGS; do
        mkdir -p "$WORK/$t"
        for f in storage/disk_manager.c storage/file_manager.c \
                 storage/file_manager.h storage/slotted_page.h; do
            curl -fsSL "$RAW/$t/src/$f" -o "$WORK/$t/$(basename "$f")" 2>/dev/null
        done
    done

    python3 - "$WORK" "$SELF/../src/storage_ondisk_layout.hpp" $TAGS <<'PY'
import os, re, sys, difflib

work, local = sys.argv[1], sys.argv[2]
tags = sys.argv[3:]

# Structures volmap copies.  Names are the C tags, not the typedefs.
WANTED = ["disk_volume_header", "file_header", "file_extensible_data",
          "spage_header", "spage_slot", "file_heap_des", "file_btree_des"]

def structs(text):
    """struct bodies, comments and spacing normalised away."""
    out = {}
    for m in re.finditer(r'^struct\s+(\w+)\s*\{(.*?)^\};', text, re.M | re.S):
        name, body = m.group(1), m.group(2)
        if name not in WANTED:
            continue
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        body = re.sub(r'//[^\n]*', '', body)
        out[name] = [re.sub(r'\s+', ' ', l).strip()
                     for l in body.split('\n') if l.strip()]
    return out

def read(d):
    acc = {}
    if not os.path.isdir(d):
        return acc
    for fn in os.listdir(d):
        try:
            acc.update(structs(open(os.path.join(d, fn), encoding='utf-8',
                                    errors='replace').read()))
        except OSError:
            pass
    return acc

per = {t: read(os.path.join(work, t)) for t in tags}
missing = [t for t in tags if not per[t]]
if missing:
    print("  could not read sources for: " + " ".join(missing))

changed = False

# A structure that is not found at all is the regression this script exists for:
# renaming it, or moving it to a file that is not fetched, would otherwise make
# every later comparison a silent skip.
def supported(tag):
    """volmap reads 10.1 and up; 10.0 is the pre-redesign format its header
    self-check rejects, and those structures do not exist there."""
    m = re.match(r'v(\d+)\.(\d+)', tag)
    return m is None or (int(m.group(1)), int(m.group(2))) >= (10, 1)

absent = False
for t in tags:
    if not per[t] or not supported(t):
        continue
    gone = [n for n in WANTED if n not in per[t]]
    if gone:
        absent = True
        print(f"  NOT FOUND in {t}: " + ", ".join(gone))
if absent:
    print("  -> a copied structure was renamed or moved; update WANTED and the"
          " file list above, then re-run")

prev_tag = None
for t in tags:
    if not per[t]:
        continue
    if prev_tag is not None:
        for name in WANTED:
            a, b = per[prev_tag].get(name), per[t].get(name)
            if a is None or b is None or a == b:
                continue
            changed = True
            print(f"  CHANGED {name}: {prev_tag} -> {t}")
            for line in difflib.unified_diff(a, b, lineterm=''):
                if line.startswith(('+', '-')) and not line.startswith(('+++', '---')):
                    print(f"      {line}")
    prev_tag = t

# the copy in this tree against the newest release
newest = [t for t in tags if per[t]]
if newest:
    newest = newest[-1]
    mine = structs(open(local, encoding='utf-8', errors='replace').read())
    gone = [n for n in WANTED if n not in mine]
    if gone:
        absent = True
        print("  NOT FOUND in storage_ondisk_layout.hpp: " + ", ".join(gone))
    for name in WANTED:
        a, b = mine.get(name), per[newest].get(name)
        if a is None or b is None or a == b:
            continue
        changed = True
        print(f"  COPY OUT OF DATE {name}: storage_ondisk_layout.hpp vs {newest}")
        for line in difflib.unified_diff(a, b, lineterm=''):
            if line.startswith(('+', '-')) and not line.startswith(('+++', '---')):
                print(f"      {line}")

print("  no structure changes" if not (changed or absent) else
      "  -> update src/storage_ondisk_layout.hpp and docs/ondisk-format.md")
sys.exit(2 if (changed or absent) else 0)
PY
    # Any non-zero status counts, not just 2: an exception exits 1, and swallowing
    # that would report a clean run on a check that never finished.
    case $? in
      0) ;;
      2) rc=$((rc + 2)) ;;
      *) echo "  the structure check did not finish" >&2; rc=$((rc + 2)) ;;
    esac
fi

# ---- builds ---------------------------------------------------------------
if [ "$do_builds" = "1" ]; then
    echo
    echo "== builds =="
    failed=""
    for t in $TAGS; do
        printf '  %-18s ' "$t"
        if REF="$t" WORK="$WORK/build-$t" OUT="$WORK/cub_volmap-$t" \
           sh "$SELF/build_fetch.sh" >"$WORK/$t.log" 2>&1; then
            echo "OK"
        else
            echo "FAILED  (see $WORK/$t.log)"
            failed="$failed $t"
            KEEP=1; trap - EXIT          # keep the logs for a failure
        fi
    done
    [ -n "$failed" ] && { echo "  failed:$failed"; rc=$((rc + 1)); }
fi

exit $rc
