#!/usr/bin/env bash
# Configures one CMake preset. Defaults to linux-debug.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${1:-linux-debug}"

if ! command -v cmake >/dev/null 2>&1; then
    echo "configure: cmake not found; run scripts/bootstrap.sh" >&2
    exit 1
fi

if ! cmake --list-presets >/dev/null 2>&1; then
    echo "configure: CMakePresets.json is not readable" >&2
    exit 1
fi

echo "configure: preset=${preset}"
cmake --preset "$preset"
