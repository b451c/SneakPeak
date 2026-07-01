#!/bin/bash
# Envelope-diff regression harness runner (v2.3.0 INC-0).
#
#   tests/run_dyn_regression.sh          check current engine against baseline
#   tests/run_dyn_regression.sh record   (re)record the baseline — ONLY when a
#                                        behavior change is intended, in its
#                                        own commit with a rationale
#
# Baselines are machine-local (%.17g round-trip doubles): record and check on
# the same machine/compiler. Not wired into CI on purpose.
set -e
cd "$(dirname "$0")/.."

cmake --build build --target dyn_regression -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

BASELINE=tests/baselines/dyn_regression.txt
CURRENT=build/dyn_regression_current.txt

./build/dyn_regression > "$CURRENT"

if [ "$1" = "record" ]; then
  mkdir -p tests/baselines
  cp "$CURRENT" "$BASELINE"
  echo "Baseline recorded: $BASELINE ($(wc -l < "$BASELINE" | tr -d ' ') lines)"
else
  if [ ! -f "$BASELINE" ]; then
    echo "No baseline at $BASELINE - run: tests/run_dyn_regression.sh record" >&2
    exit 1
  fi
  if diff -u "$BASELINE" "$CURRENT"; then
    echo "REGRESSION HARNESS: GREEN (byte-identical, $(wc -l < "$BASELINE" | tr -d ' ') lines)"
  else
    echo "REGRESSION HARNESS: RED - engine output diverged from baseline" >&2
    exit 1
  fi
fi
