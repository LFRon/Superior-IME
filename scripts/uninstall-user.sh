#!/usr/bin/env bash
# Removes everything scripts/install-user.sh created.
#
# Only paths owned by this project are touched; Fcitx5's profile, classicui
# theme and other addons are never modified.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dry_run=0

for argument in "$@"; do
    case "$argument" in
        --dry-run) dry_run=1 ;;
        -h|--help) echo "usage: scripts/uninstall-user.sh [--dry-run]"; exit 0 ;;
        *) echo "uninstall-user: unknown argument '$argument'" >&2; exit 2 ;;
    esac
done

files=(
    "$HOME/.local/lib/fcitx5/libsuperioragent.so"
    "$HOME/.local/share/fcitx5/addon/superioragent.conf"
    "$HOME/.local/share/fcitx5/inputmethod/super-ime.conf"
    "$HOME/.local/share/icons/hicolor/scalable/apps/org.superior.ime.svg"
    # Wrappers only; the binaries themselves live in the tree below.
    "$HOME/.local/bin/superior_ui"
    "$HOME/.local/bin/superior_settings"
    "$HOME/.local/share/applications/superior-settings.desktop"
    "$HOME/.config/environment.d/90-superior-ime.conf"
)
trees=(
    # Binaries + skins.
    "$HOME/.local/share/superior-ime"
)

echo "uninstall-user:"
for path in "${files[@]}"; do
    if [[ -e "$path" ]]; then
        echo "  remove $path"
        (( dry_run )) || rm -f "$path"
    fi
done
for path in "${trees[@]}"; do
    if [[ -d "$path" ]]; then
        echo "  remove $path/"
        (( dry_run )) || rm -rf "$path"
    fi
done

cat <<'MSG'

Done. The settings file is kept on purpose:
  ~/.config/superior-ime/settings.json
Remove it manually if you also want to drop your preferences.

Restart Fcitx5 so the addon disappears:
  fcitx5-remote -e && fcitx5 -d
MSG
