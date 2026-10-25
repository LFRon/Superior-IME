#!/usr/bin/env bash
# System-wide install into /usr.
#
# With CMAKE_INSTALL_PREFIX=/usr, GNUInstallDirs selects the Debian multiarch
# libdir automatically, so the addon lands exactly where Fcitx5 looks at
# startup. No environment variable is required afterwards.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="linux-system"
dry_run=0
skip_build=0

for argument in "$@"; do
    case "$argument" in
        --dry-run) dry_run=1 ;;
        --no-build) skip_build=1 ;;
        -h|--help)
            echo "usage: scripts/install-system.sh [--dry-run] [--no-build]"
            exit 0
            ;;
        *)
            echo "install-system: unknown argument '$argument'" >&2
            exit 2
            ;;
    esac
done

build="$root/build/$preset"

if (( ! skip_build )); then
    cmake --preset "$preset"
    cmake --build --preset "$preset" --parallel
    ctest --test-dir "$build" --output-on-failure
fi

if [[ ! -f "$build/CMakeCache.txt" ]]; then
    echo "install-system: '$build' is not configured" >&2
    exit 1
fi

prefix="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$build/CMakeCache.txt")"
libdir="$(sed -n 's/^CMAKE_INSTALL_LIBDIR:PATH=//p' "$build/CMakeCache.txt")"

echo "install-system:"
echo "  prefix : $prefix"
echo "  libdir : $libdir"
echo "  addon  : $prefix/$libdir/fcitx5/libsuperioragent.so"
echo "  config : $prefix/share/fcitx5/addon/superioragent.conf"
echo "  binary : $prefix/bin/superior_ui"

if (( dry_run )); then
    echo "dry-run: no files written"
    exit 0
fi

sudo cmake --install "$build"

cat <<'MSG'

Installed into /usr. Fcitx5 finds the addon through its compiled-in directory,
so no FCITX_ADDON_DIRS is needed and it will be loaded on every login.

Restart Fcitx5 once to load the new addon now:
  fcitx5-remote -e && fcitx5 -d

Verify:
  fcitx5-diagnose | grep -i superior
MSG
