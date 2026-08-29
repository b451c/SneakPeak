#!/bin/bash
# DynamicsParams string round-trip + range clamp (audit A7.5).
#
#   tests/run_dyn_params_test.sh    build + run the assertions (machine-independent)
set -e
cd "$(dirname "$0")/.."

cmake --build build --target dyn_params_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/dyn_params_test
