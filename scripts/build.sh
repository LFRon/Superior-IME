#!/usr/bin/env bash
# Builds one CMake preset. Defaults to linux-debug.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${1:-linux-debug}"
build_dir="build/${preset}"

if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "build: '$build_dir' is not configured; running scripts/configure.sh $preset" >&2
    "$root/scripts/configure.sh" "$preset"
fi

echo "build: preset=${preset}"
cmake --build --preset "$preset" --parallel
