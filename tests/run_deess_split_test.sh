#!/bin/bash
# Split-band de-ess apply check runner (v2.5.0 row 15).
#
#   tests/run_deess_split_test.sh    build + run the DeEssApplySplit claims
#
# Machine-independent (exact compares + dB tolerances, no baselines).
set -e
cd "$(dirname "$0")/.."

cmake --build build --target deess_split_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/deess_split_test
