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
# build_standalone.sh - build the standalone volmap binary, copyable to any server as a single file
set -e
SRC=${SRC:?set SRC=/path/to/cubrid-src (CUBRID source checkout)}
BUILD=${BUILD:-$SRC/build_release}
OUT=${OUT:-$(dirname "$0")/../cub_volmap}

SELF=$(cd "$(dirname "$0")" && pwd)
INC="-I$SRC/src/executables -I$SELF/../src -I$SRC/src/storage -I$SRC/src/base -I$SRC/src/compat \
 -I$SRC/src/transaction -I$SRC/src/object -I$SRC/src/parser -I$SRC/src/query \
 -I$SRC/src/connection -I$SRC/src/communication -I$SRC/src/thread -I$SRC/src/monitor \
 -I$SRC/src/session -I$SRC/src/xasl -I$SRC/src/api -I$SRC/src/sp -I$SRC/src/method \
 -I$BUILD -I$SRC/include -I$SRC/3rdparty/rapidjson/include"

# glibc 2.34 merged libdl/libpthread into libc and re-versioned the symbols that
# moved, so a build there records GLIBC_2.34 and will not start on anything older.
# The old versions are still in the same libc, so on 2.34+ we ask for them
# explicitly (src/volmap_glibc_compat.h) and the result runs on both.
GLIBC_COMPAT=""
if [ "$(uname -m)" = "x86_64" ]; then
  gv=$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | tail -1)
  if [ -n "$gv" ]; then
    gmaj=${gv%%.*}; gmin=${gv##*.}
    if [ "$gmaj" -gt 2 ] 2>/dev/null || { [ "$gmaj" -eq 2 ] && [ "$gmin" -ge 34 ]; } 2>/dev/null; then
      GLIBC_COMPAT="-DVOLMAP_GLIBC_COMPAT"
      echo "glibc $gv - pinning old symbol versions (runs on glibc 2.11+)"
    fi
  fi
fi

# Compile through a log rather than a pipe: a pipeline reports grep's status, so the
# "|| true" that silences grep would hide a compiler failure as well.  The old binary
# is removed first so a stale one cannot satisfy the -x test and have a failed build
# reported as "built".
LOG=$(mktemp "${TMPDIR:-/tmp}/volmap-build.XXXXXX")
trap 'rm -f "$LOG"' EXIT
rm -f "$OUT"
if g++ -x c++ -std=gnu++17 -O2 -Wall -DNDEBUG -DVOLMAP_STANDALONE $INC \
     "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
     -DVOLMAP_NO_DLOPEN -static-libstdc++ -static-libgcc -static -o "$OUT" >"$LOG" 2>&1; then
  :                             # built; warnings stay in the log
else
  echo "full-static unavailable - building with static libstdc++/libgcc (glibc dynamic)"
  rm -f "$OUT"
  if ! g++ -x c++ -std=gnu++17 -O2 -Wall -DNDEBUG -DVOLMAP_STANDALONE -DVOLMAP_NO_DLOPEN $INC \
       "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
       -static-libstdc++ -static-libgcc -o "$OUT" >"$LOG" 2>&1; then
    cat "$LOG" >&2
    echo "build failed: $OUT" >&2
    exit 1
  fi
fi
[ -x "$OUT" ] || { echo "compiler reported success but $OUT is missing" >&2; exit 1; }
echo "built: $OUT"; file "$OUT" | cut -c1-100; ldd "$OUT" 2>&1 | head -3

# dyn variant: glibc dynamic — dlopen of libcubridcs.so works here, enabling the
# live-server overlay (pass 2) when a CUBRID installation is present at runtime
rm -f "$OUT-dyn"
if ! g++ -x c++ -std=gnu++17 -O2 -Wall -DNDEBUG -DVOLMAP_STANDALONE $GLIBC_COMPAT $INC \
     "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
     -static-libstdc++ -static-libgcc -ldl -o "$OUT-dyn" >"$LOG" 2>&1; then
  cat "$LOG" >&2
  echo "build failed: $OUT-dyn" >&2
  exit 1
fi
[ -x "$OUT-dyn" ] || { echo "compiler reported success but $OUT-dyn is missing" >&2; exit 1; }
echo "built: $OUT-dyn (glibc dynamic, runtime-dlopen overlay capable)"
