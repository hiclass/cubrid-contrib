#!/bin/bash
# sanitize.sh: every test that needs no server, with the tool and the test
# programs built with AddressSanitizer and UndefinedBehaviorSanitizer; the
# first report stops the program, so the test using it fails.  Exit 0: all
# passed; 1: a test failed; 2: not run (no sanitizer runtime to link).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
SAN="-g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources)
if ! gcc -O1 $SAN -I"$HERE/../src" -o "$T/cub_ctv" "${SRCS[@]}" -lm 2>"$T/build.log"; then
  # the linker's own words only: a source path may hold "libasan" too
  if grep -q -E '(cannot find|unable to find library|library not found for|cannot open) (-l(asan|ubsan)|[^ ]*/lib(asan|ubsan)\.so[^ /]*)( |:|$)' "$T/build.log"; then
    echo "sanitize NOT RUN: the sanitizer runtimes are not installed (libasan, libubsan)"
    exit 2
  fi
  cat "$T/build.log"
  echo "sanitize FAIL: build"
  exit 1
fi
export CUB_CTV_TOOL="$T/cub_ctv" CTV_TEST_CFLAGS="$SAN"
# leaks at exit are not errors here: the tool frees nothing it holds for the run
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
rc=0
n=0
# every t<N>_ test but the ones that need a server: a new test is run
# without being listed here
for t in "$HERE"/t[0-9]*_*.sh "$HERE"/t[0-9]*_*.py; do
  [ -e "$t" ] || continue
  case $(basename "$t") in
    t2_*|t4_*|t7_*|t9_*|t10_*) continue ;;	# need a server
    t21_clock_use.sh) continue ;;	# a source check: nothing runs
  esac
  case $t in
    *.py) out=$(timeout 600 python3 "$t" 2>&1) ;;
    *) out=$(timeout 600 bash "$t" 2>&1) ;;
  esac
  if [ $? -ne 0 ]; then
    echo "sanitize FAIL: $(basename "$t")"
    printf '%s\n' "$out" | grep -e 'runtime error' -e 'AddressSanitizer' -e 'FAIL' | head -5
    rc=1
  fi
  n=$((n + 1))
done
[ $rc -eq 0 ] && echo "sanitize PASS: $n tests with the tool and the test programs under ASan + UBSan"
exit $rc
