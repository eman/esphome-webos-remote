#!/bin/sh
# Build and run the ZipIR decoder host test. Needs a C compiler (cc) and python3; no ESPHome.
#   tests/zipir/run.sh                     # check vectors.h is current, build with sanitizers if available, run
#   EXTRA_CFLAGS=-DTEST_KNOWN_BUGS tests/zipir/run.sh   # also run the cases marked as known decoder issues
set -eu
cd "$(dirname "$0")"
CC="${CC:-cc}"
SRC="test_zipir.c ../../components/lg_ir_remote/lgir_zipir.c"
CFLAGS="-std=c99 -Wall -Wextra -O1 -g -I../../components/lg_ir_remote ${EXTRA_CFLAGS:-}"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

python3 -I make_vectors.py --check
trap 'rm -rf test_zipir.dSYM' EXIT  # macOS debug-symbol bundle from -g

# shellcheck disable=SC2086
if "$CC" $CFLAGS $SAN -o test_zipir $SRC 2>/dev/null; then
  echo "built with AddressSanitizer and UBSan"
else
  echo "sanitizers not available; building without them"
  "$CC" $CFLAGS -o test_zipir $SRC
fi
./test_zipir
