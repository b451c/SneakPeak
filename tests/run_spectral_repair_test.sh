#!/bin/bash
# Spectral-repair offline harness runner (v2.3.0 INC-5).
#
#   tests/run_spectral_repair_test.sh    build + run the SNR-assertion tests
#
# Unlike run_dyn_regression.sh there is no baseline to record: assertions are
# SNR-improvement thresholds, deterministic and machine-independent.
set -e
cd "$(dirname "$0")/.."

cmake --build build --target spectral_repair_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/spectral_repair_test
