#!/bin/sh
# Build and run the host-side unit tests (macOS: clang++, Linux: g++).
# Usage: test/run_native_tests.sh
#        SANITIZE=thread test/run_native_tests.sh   # ThreadSanitizer, for cross-core code
set -e
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
mkdir -p .pio/native-tests
python3 scripts/embed_welcome.py  # src/welcome_text.h, from welcome/README.TXT
SANITIZE="${SANITIZE:-address,undefined}"
"$CXX" -std=c++17 -O2 -g -Wall -Wextra -pthread -fsanitize="$SANITIZE" \
    -Isrc test/native/test_main.cpp test/native/test_ata_device.cpp test/native/test_short_test.cpp \
    test/native/test_activity.cpp src/activity.cpp \
    test/native/test_activity_link.cpp src/activity_link.cpp \
    test/native/test_drive_config.cpp src/drive_config.cpp \
    test/native/test_seek_timing.cpp src/seek_timing.cpp src/drive_params.cpp src/drive_timing.cpp src/sentence.cpp \
    test/native/test_sector_check.cpp src/sector_check.cpp \
    test/native/test_media_watch.cpp src/media_watch.cpp \
    test/native/test_welcome_disk.cpp src/welcome_disk.cpp \
    src/trace_format.cpp src/identify.cpp src/ata_device.cpp src/short_test.cpp \
    -o .pio/native-tests/run_tests
"$CXX" -std=c++17 -O2 -Wall -Wextra -Isrc test/tools/classify_trace.cpp src/activity.cpp \
    -o .pio/native-tests/classify_trace
"$CXX" -std=c++17 -O2 -Wall -Wextra -Isrc test/tools/make_welcome_img.cpp src/welcome_disk.cpp src/drive_config.cpp \
    src/seek_timing.cpp src/drive_params.cpp src/drive_timing.cpp src/sentence.cpp \
    -o .pio/native-tests/make_welcome_img
.pio/native-tests/run_tests "$@"
