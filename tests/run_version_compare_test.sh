#!/bin/bash
# Update-check version ordering (v2.5.0 audit A5.4).
#
#   tests/run_version_compare_test.sh    build + run the assertions
set -e
cd "$(dirname "$0")/.."
cmake --build build --target version_compare_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null
./build/version_compare_test
