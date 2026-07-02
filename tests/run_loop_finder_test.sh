#!/bin/bash
# Loop-finder offline check runner (v2.4.0 INC-A2).
#
#   tests/run_loop_finder_test.sh    build + run the assertions
#
# Machine-independent (threshold assertions, no baselines), deterministic.
set -e
cd "$(dirname "$0")/.."

cmake --build build --target loop_finder_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/loop_finder_test
