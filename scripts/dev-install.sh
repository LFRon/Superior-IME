#!/usr/bin/env bash
# Installs skins into the per-user data directory.
#
# The Fcitx5 addon is not built yet (M2/M3), so this script only stages skins.
# It never edits the user's Fcitx5 profile.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${SUPERIOR_PRESET:-linux-debug}"
build_dir="build/${preset}"
lint="$build_dir/tools/skin-lint/skin_lint"

if [[ ! -x "$lint" ]]; then
    echo "dev-install: '$lint' not found; run scripts/build.sh $preset first" >&2
    exit 1
fi

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
destination="$data_home/superior-ime/skins"

echo "dev-install: validating skins"
for package in skins/*/; do
    [[ -f "${package}skin.json" ]] || continue
    "$lint" "$package" --strict
done

binary="$build_dir/frontend/desktop/superior_ui"
if [[ -x "$binary" ]]; then
    install -Dm755 "$binary" "${HOME}/.local/bin/superior-ui"
    echo "dev-install: installed ~/.local/bin/superior-ui"
else
    echo "dev-install: UI binary not built (configure with SUPERIOR_ENABLE_QT=ON)" >&2
fi

echo "dev-install: staging skins into $destination"
mkdir -p "$destination"
for package in skins/*/; do
    [[ -f "${package}skin.json" ]] || continue
    name="$(basename "$package")"
    rm -rf "${destination:?}/$name"
    cp -a "$package" "$destination/$name"
    echo "  $name"
done

cat <<EOF

Skins installed in:
  $destination

The Fcitx5 agent is installed system-wide (it must live in Fcitx5's addon
directory):

  sudo scripts/install-addon-system.sh
  fcitx5-remote -e && fcitx5 -d

The agent is a passive observer: it does not register an input method and does
not change the client's preedit path.

This project never modifies ~/.config/fcitx5/profile on your behalf.
EOF
