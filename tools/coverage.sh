#!/usr/bin/env bash
#
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# coverage.sh — build with gcov instrumentation, run the tests, run the gate.
#
#   tools/coverage.sh                 # gate against tools/coverage-baseline.json
#   tools/coverage.sh --strict        # require 100% for every file
#   tools/coverage.sh --list-uncovered
#
# Always removes old .gcda/.gcno files first: after a rename or a deleted source
# the old notes belong to files that no longer exist, and gcov would report them
# as dead files (or as bogus zeros).  The rebuild is therefore always clean.

set -euo pipefail

cd "$(dirname "$0")/.."

build_dir="build-cov"
target_debug="-DCMAKE_BUILD_TYPE=Debug"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    cmake -B "$build_dir" $target_debug -DBUILD_TESTING=ON -DENABLE_COVERAGE=ON
fi

# Orphaned notes of moved/removed sources would be reported as uncovered files.
find "$build_dir" -name '*.gcda' -delete
find "$build_dir" -name '*.gcno' -delete

cmake --build "$build_dir" -j"$(nproc)" --clean-first

find "$build_dir" -name '*.gcda' -delete

# The gate runs below, after the report is generated, so a regression always
# produces a readable list instead of aborting in the middle of ctest.
ctest --test-dir "$build_dir" --output-on-failure -E coverage_gate

# Ratchet against the committed baseline unless strict mode was requested.
baseline_args=(--baseline tools/coverage-baseline.json)
for arg in "$@"; do
    if [ "$arg" = "--strict" ]; then
        baseline_args=()
    fi
done

exec python3 tools/check_coverage.py --build-dir "$build_dir" "${baseline_args[@]}" "$@"
