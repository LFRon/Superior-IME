#!/usr/bin/env bash
# Runs the desktop overlay against an agent (or tools/scene-sim).
#
# The platform plugin is chosen by the binary itself (PlatformChoice.cpp): on a
# Wayland session it switches to XWayland so the overlay stays override-redirect
# (always on top, never takes focus). Set SUPERIOR_UI_PLATFORM to override.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${SUPERIOR_PRESET:-linux-ui}"
binary="$root/build/$preset/frontend/desktop/superior_ui"

if [[ ! -x "$binary" ]]; then
    echo "run-ui: '$binary' not found; run scripts/configure.sh $preset && scripts/build.sh $preset" >&2
    exit 1
fi

if [[ -z "${DISPLAY:-}" && -z "${SUPERIOR_UI_PLATFORM:-}" ]]; then
    echo "run-ui: DISPLAY is not set; set SUPERIOR_UI_PLATFORM to pick a plugin" >&2
    exit 1
fi

exec "$binary" "$@"
