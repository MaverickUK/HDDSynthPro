#!/bin/sh
# Build and run the host-side unit tests (macOS: clang++, Linux: g++).
# Usage: test/run_native_tests.sh
#        SANITIZE=thread test/run_native_tests.sh   # ThreadSanitizer, for cross-core code
set -e
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
mkdir -p .pio/native-tests
SANITIZE="${SANITIZE:-address,undefined}"
"$CXX" -std=c++17 -O2 -g -Wall -Wextra -pthread -fsanitize="$SANITIZE" \
    -Isrc test/native/test_main.cpp test/native/test_ata_device.cpp \
    src/trace_format.cpp src/identify.cpp src/ata_device.cpp \
    -o .pio/native-tests/run_tests
.pio/native-tests/run_tests "$@"
