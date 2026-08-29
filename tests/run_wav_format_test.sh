#!/bin/bash
# WAV format robustness check runner (audit A10).
#
#   tests/run_wav_format_test.sh    build + run the exact byte/integer assertions
#
# Hand-built files in the temp dir, no REAPER, deterministic on every platform.
set -e
cd "$(dirname "$0")/.."

cmake --build build --target wav_format_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/wav_format_test
