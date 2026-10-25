#!/usr/bin/env bash
# Installs Superior-IME into the per-user prefix (~/.local).
#
# Everything this script writes is listed by scripts/uninstall-user.sh, so the
# installation can be removed completely without touching Fcitx5's config.
#
# One caveat is unavoidable: Fcitx5 only scans its compiled-in addon directory
# plus FCITX_ADDON_DIRS (there is no per-user addon directory in Fcitx5 5.x).
# This script therefore writes ~/.config/environment.d/90-superior-ime.conf and
# prints how to apply it to the running session.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

preset="${SUPERIOR_PRESET:-linux-ui}"
build="$root/build/$preset"
dry_run=0

for argument in "$@"; do
    case "$argument" in
        --dry-run) dry_run=1 ;;
        -h|--help)
            echo "usage: scripts/install-user.sh [--dry-run]"
            exit 0
            ;;
        *) echo "install-user: unknown argument '$argument'" >&2; exit 2 ;;
    esac
done

so="$(find "$build" -name 'libsuperioragent.so' -print -quit 2>/dev/null || true)"
addon_conf="$(find "$build" -name 'superioragent.conf' -print -quit 2>/dev/null || true)"
entry_conf="$(find "$build" -name 'super-ime.conf' -print -quit 2>/dev/null || true)"
ui="$build/frontend/desktop/superior_ui"
settings="$build/frontend/settings/superior_settings"
staged_skins="$build/stage-skins"

if [[ -z "$so" || -z "$addon_conf" || -z "$entry_conf" || ! -x "$ui" ]]; then
    echo "install-user: build first: scripts/configure.sh $preset && scripts/build.sh $preset" >&2
    exit 1
fi

# Skins come from the source tree so the install never depends on a system copy.
rm -rf "$staged_skins"
mkdir -p "$staged_skins"
for package in skins/*/; do
    [[ -f "${package}skin.json" ]] || continue
    cp -a "$package" "$staged_skins/$(basename "$package")"
done

# Large binaries live under share/, not in bin/: "bin" is on PATH and is the
# first thing people (and backups) look at, and a 13 MB UI there is noise. The
# only things left in ~/.local/bin are two tiny wrappers.
app_root="$HOME/.local/share/superior-ime"
bin_dir="$app_root/bin"
wrapper_dir="$HOME/.local/bin"
lib_dir="$HOME/.local/lib/fcitx5"
addon_dir="$HOME/.local/share/fcitx5/addon"
im_dir="$HOME/.local/share/fcitx5/inputmethod"
icon_dir="$HOME/.local/share/icons/hicolor/scalable/apps"
app_dir="$HOME/.local/share/applications"
skin_dir="$app_root/skins"
env_dir="$HOME/.config/environment.d"

system_addon_dir="/usr/lib/$(gcc -print-multiarch 2>/dev/null || echo "${HOSTTYPE:-x86_64}-linux-gnu")/fcitx5"

echo "install-user:"
echo "  $so            -> $lib_dir/libsuperioragent.so"
echo "  $addon_conf    -> $addon_dir/superioragent.conf"
echo "  $entry_conf    -> $im_dir/super-ime.conf"
echo "  $ui            -> $bin_dir/superior_ui"
[[ -x "$settings" ]] && echo "  $settings -> $bin_dir/superior_settings"
echo "  wrappers       -> $wrapper_dir/{superior_ui,superior_settings}"
echo "  skins          -> $skin_dir"
echo "  env            -> $env_dir/90-superior-ime.conf"

if (( dry_run )); then
    echo "dry-run: nothing written"
    exit 0
fi

install -Dm755 "$so" "$lib_dir/libsuperioragent.so"
install -Dm644 "$addon_conf" "$addon_dir/superioragent.conf"
install -Dm644 "$entry_conf" "$im_dir/super-ime.conf"
install -Dm644 backend/platforms/fcitx5/data/icons/org.superior.ime.svg \
    "$icon_dir/org.superior.ime.svg"
install -Dm755 "$ui" "$bin_dir/superior_ui"
if [[ -x "$settings" ]]; then
    install -Dm755 "$settings" "$bin_dir/superior_settings"
fi

# Wrappers keep the commands on PATH (and keep the desktop entry stable) while
# the real executables stay out of ~/.local/bin.
mkdir -p "$wrapper_dir"
write_wrapper() {
    local name="$1"
    local target="$2"
    local temporary
    # Write then rename: the overlay may still be running from the previous
    # install, and truncating a running executable fails with ETXTBSY. A rename
    # swaps the inode instead, so the old process keeps working.
    temporary="$(mktemp "$wrapper_dir/.$name.XXXXXX")"
    cat > "$temporary" <<WRAPPEREOF
#!/usr/bin/env sh
# Wrapper installed by Superior-IME (scripts/install-user.sh).
# The real binary lives in ~/.local/share/superior-ime/bin to keep ~/.local/bin small.
exec "$target" "\$@"
WRAPPEREOF
    chmod 0755 "$temporary"
    mv -f "$temporary" "$wrapper_dir/$name"
}
write_wrapper superior_ui "$bin_dir/superior_ui"
[[ -x "$settings" ]] && write_wrapper superior_settings "$bin_dir/superior_settings"

# Exec uses the absolute path: a GUI session does not always have
# ~/.local/bin on PATH.
mkdir -p "$app_dir"
sed "s|^Exec=.*|Exec=$bin_dir/superior_settings|" \
    frontend/settings/superior-settings.desktop.in \
    > "$app_dir/superior-settings.desktop"
chmod 0644 "$app_dir/superior-settings.desktop"

rm -rf "$skin_dir"
mkdir -p "$skin_dir"
cp -a "$staged_skins/." "$skin_dir/"
rm -rf "$staged_skins"

# A base word list makes the difference between "works" and "feels like a real
# IME" for anything outside everyday vocabulary. It is fetched, not bundled: 雾凇
# is ~46 MB of source and updated weekly, so a snapshot in the repository would
# either bloat history or rot. On a machine without network (or without curl) the
# install still succeeds — the engine just keeps using its own dictionary.
dict_dir="${XDG_DATA_HOME:-$HOME/.local/share}/fcitx5/pinyin/dictionaries"
if [[ ! -f "$dict_dir/rime-ice.dict" ]]; then
    if command -v curl >/dev/null 2>&1 && command -v libime_pinyindict >/dev/null 2>&1; then
        echo "install-user: fetching the 雾凇 (rime-ice) word list …"
        if ! SUPERIOR_PRESET="$preset" "$root/scripts/fetch-dictionary.sh" rime-ice \
                --minimal; then
            echo "install-user: word list not installed; run scripts/fetch-dictionary.sh rime-ice later" >&2
        fi
    else
        echo "install-user: skipping the word list (needs curl and libime_pinyindict)"
        echo "  install them, then run: scripts/fetch-dictionary.sh rime-ice"
    fi
fi

# Fcitx5 replaces the compiled-in addon directory when this variable is set, so
# the system directory must stay in the list or every stock addon disappears.
mkdir -p "$env_dir"
cat > "$env_dir/90-superior-ime.conf" <<ENVEOF
# Added by Superior-IME (scripts/install-user.sh).
# FCITX_ADDON_DIRS replaces Fcitx5's compiled-in directory: keep both entries.
FCITX_ADDON_DIRS=$lib_dir:$system_addon_dir
ENVEOF

cat <<MSG

Installed into \$HOME/.local.

Apply the addon directory to the current session, then restart Fcitx5:
  systemctl --user set-environment FCITX_ADDON_DIRS="$lib_dir:$system_addon_dir"
  fcitx5-remote -e && fcitx5 -d

If you log out and back in instead, ~/.config/environment.d/90-superior-ime.conf
is applied automatically.

The overlay starts itself when the agent loads; you can also run it by hand:
  ~/.local/bin/superior_ui            # wrapper -> ~/.local/share/superior-ime/bin/superior_ui

Uninstall everything this script created:
  scripts/uninstall-user.sh
MSG
