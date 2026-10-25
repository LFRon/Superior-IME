#!/usr/bin/env bash
# Runs the test suite for one preset. Defaults to linux-debug.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${1:-linux-debug}"
build_dir="build/${preset}"

if [[ ! -f "$build_dir/CTestTestfile.cmake" ]]; then
    echo "test: '$build_dir' is not configured; running scripts/build.sh $preset" >&2
    "$root/scripts/build.sh" "$preset"
fi

echo "test: preset=${preset}"
ctest --test-dir "$build_dir" --output-on-failure
