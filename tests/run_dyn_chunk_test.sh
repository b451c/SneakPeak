#!/bin/bash
# Chunked-detector equivalence check runner (v2.5 8f).
#
#   tests/run_dyn_chunk_test.sh    build + run the exact-compare assertions
#
# Machine-independent (memcmp on doubles, no baselines), deterministic.
set -e
cd "$(dirname "$0")/.."

cmake --build build --target dyn_chunk_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/dyn_chunk_test
