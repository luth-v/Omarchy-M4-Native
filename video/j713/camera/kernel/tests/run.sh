#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/.build
python3 -I tests/extract_restart.py
python3 -I tests/extract_session.py
for test in firmware_test wire_test arena_sessions restart_test lifecycle_test sequence_test fps_test print_test pacing_test; do
  gcc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer -fno-pie -no-pie \
    -fsanitize=address,undefined "tests/$test.c" -o "tests/.build/$test"
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "tests/.build/$test"
done
python3 -I tests/test_sequence.py
