#!/usr/bin/env bash
# Installs the Fcitx5 pieces into the system directories.
#
# Three different things are installed:
#   1. superioragent  - Category=Module observer addon (not an input method)
#   2. superiorpinyin   - Category=InputMethod engine addon (bundled librime)
#   3. super-ime.conf - the only input method entry, bound to superiorpinyin;
#                       the file name is the unique name ("super-ime")
#
# Requires root. Nothing is restarted automatically.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${SUPERIOR_PRESET:-linux-system}"
dry_run=0

for argument in "$@"; do
    case "$argument" in
        --dry-run) dry_run=1 ;;
        -h|--help)
            echo "usage: scripts/install-addon-system.sh [--dry-run]"
            exit 0
            ;;
        *)
            echo "install-addon-system: unknown argument '$argument'" >&2
            exit 2
            ;;
    esac
done

build="$root/build/$preset"

so="$(find "$build" -name 'libsuperioragent.so' -print -quit 2>/dev/null || true)"
addon_conf="$(find "$build" -name 'superioragent.conf' -print -quit 2>/dev/null || true)"
rime_so="$(find "$build" -name 'libsuperiorpinyin.so' -print -quit 2>/dev/null || true)"
rime_addon_conf="$(find "$build" -name 'superiorpinyin.conf' -print -quit 2>/dev/null || true)"
entry_conf="$(find "$build" -name 'super-ime.conf' -print -quit 2>/dev/null || true)"
icon="$root/backend/platforms/fcitx5/data/icons/org.superior.ime.svg"

if [[ -z "$so" || -z "$addon_conf" || -z "$rime_so" || -z "$rime_addon_conf" ||
      -z "$entry_conf" || ! -f "$icon" ]]; then
    echo "install-addon-system: build the fcitx5 target first:" >&2
    echo "  scripts/configure.sh $preset" >&2
    echo "  scripts/build.sh $preset" >&2
    exit 1
fi

multiarch="$(gcc -print-multiarch 2>/dev/null || true)"
addon_dir="${SUPERIOR_FCITX5_ADDON_DIR:-/usr/lib/${multiarch:-$(uname -m)-linux-gnu}/fcitx5}"
data_dir="${SUPERIOR_FCITX5_DATA_DIR:-/usr/share/fcitx5/addon}"
inputmethod_dir="${SUPERIOR_FCITX5_INPUTMETHOD_DIR:-/usr/share/fcitx5/inputmethod}"
icon_dir="${SUPERIOR_ICON_DIR:-/usr/share/icons/hicolor/scalable/apps}"

echo "install-addon-system:"
echo "  $so -> $addon_dir/libsuperioragent.so"
echo "  $addon_conf -> $data_dir/superioragent.conf"
echo "  $rime_so -> $addon_dir/libsuperiorpinyin.so"
echo "  $rime_addon_conf -> $data_dir/superiorpinyin.conf"
echo "  $entry_conf -> $inputmethod_dir/super-ime.conf"
echo "  $icon -> $icon_dir/org.superior.ime.svg"

if (( dry_run )); then
    echo "dry-run: nothing written"
    exit 0
fi

sudo install -Dm755 "$so" "$addon_dir/libsuperioragent.so"
sudo install -Dm644 "$addon_conf" "$data_dir/superioragent.conf"
sudo install -Dm755 "$rime_so" "$addon_dir/libsuperiorpinyin.so"
sudo install -Dm644 "$rime_addon_conf" "$data_dir/superiorpinyin.conf"
sudo install -Dm644 "$entry_conf" "$inputmethod_dir/super-ime.conf"
sudo install -Dm644 "$icon" "$icon_dir/org.superior.ime.svg"

cat <<'MSG'

Restart Fcitx5 once so the new input method entry is discovered:
  fcitx5-remote -e && fcitx5 -d

Then add "先！进！输入法" in Fcitx5 configuration, or switch directly:
  fcitx5-remote -s super-ime

Note: do NOT use the D-Bus method Controller1.ResetIMList for this. It rebuilds
the input method group from defaults and drops the user's own entries.

The unique name is "super-ime" (derived from the file name). The entry is bound
to the bundled engine (Addon=superiorpinyin); there is no libime entry and no
second input method to choose from. The 万象拼音 schema ships in data/wanxiang
and is not deployed by this script (it writes to the user's data directory):

  scripts/fetch-wanxiang-schema.sh
MSG
