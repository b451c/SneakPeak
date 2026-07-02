#!/bin/bash
# True-peak limiter offline harness runner (v2.4.0 INC-L0).
#
#   tests/run_limiter_test.sh          run assertions + diff baseline dump
#   tests/run_limiter_test.sh record   (re)record the baseline — ONLY when a
#                                      behavior change is intended, in its own
#                                      commit with a block-tagged rationale
#
# Assertions (ceiling sweep, passthrough, invariants) are machine-independent.
# The dump baseline is machine-local (%.17g round-trip doubles): record and
# check on the same machine/compiler. Not wired into CI on purpose.
set -e
cd "$(dirname "$0")/.."

cmake --build build --target limiter_test -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null

./build/limiter_test

BASELINE=tests/baselines/limiter_test.txt
CURRENT=build/limiter_test_current.txt

./build/limiter_test dump > "$CURRENT"

if [ "$1" = "record" ]; then
  mkdir -p tests/baselines
  cp "$CURRENT" "$BASELINE"
  echo "Baseline recorded: $BASELINE ($(wc -l < "$BASELINE" | tr -d ' ') lines)"
else
  if [ ! -f "$BASELINE" ]; then
    echo "No baseline at $BASELINE - run: tests/run_limiter_test.sh record" >&2
    exit 1
  fi
  if diff -u "$BASELINE" "$CURRENT"; then
    echo "LIMITER BASELINE: GREEN (byte-identical, $(wc -l < "$BASELINE" | tr -d ' ') lines)"
  else
    echo "LIMITER BASELINE: RED - engine output diverged from baseline" >&2
    exit 1
  fi
fi
