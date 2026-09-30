#!/bin/sh
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

g++ -x c++ -std=gnu++17 -O2 -Wall -DNDEBUG -DVOLMAP_STANDALONE $INC \
    "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
    -DVOLMAP_NO_DLOPEN -static-libstdc++ -static-libgcc -static -o "$OUT" 2>&1 | grep -vE "^In file|warning:" || true
[ -x "$OUT" ] || { echo "full-static unavailable — building with static libstdc++/libgcc (glibc dynamic)"; \
  g++ -x c++ -std=gnu++17 -O2 -Wall -DNDEBUG -DVOLMAP_STANDALONE -DVOLMAP_NO_DLOPEN $INC \
    "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
    -static-libstdc++ -static-libgcc -o "$OUT"; }
echo "built: $OUT"; file "$OUT" | cut -c1-100; ldd "$OUT" 2>&1 | head -3

# dyn variant: glibc dynamic — dlopen of libcubridcs.so works here, enabling the
# live-server overlay (pass 2) when a CUBRID installation is present at runtime
g++ -x c++ -std=gnu++17 -O2 -Wall -DNDEBUG -DVOLMAP_STANDALONE $INC \
    "$(dirname "$0")/../src/volmap.c" "$(dirname "$0")/../src/volmap_standalone.cpp" \
    -static-libstdc++ -static-libgcc -ldl -o "$OUT-dyn"
echo "built: $OUT-dyn (glibc dynamic, runtime-dlopen overlay capable)"
