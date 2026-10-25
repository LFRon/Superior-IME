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
# The pinyin engine is optional: a build with SUPERIOR_ENABLE_FCITX5 always
# produces it, but an older build tree may not, and the agent must still install.
pinyin_so="$(find "$build" -name 'libsuperiorpinyin.so' -print -quit 2>/dev/null || true)"
pinyin_addon_conf="$(find "$build" -name 'superiorpinyin.conf' -print -quit 2>/dev/null || true)"
ui="$build/frontend/desktop/superior_ui"
settings="$build/frontend/settings/superior_settings"
config_cli="$build/tools/superior-config/superior_config_cli"
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
[[ -n "$pinyin_so" ]] && echo "  $pinyin_so -> $lib_dir/libsuperiorpinyin.so"
[[ -n "$pinyin_addon_conf" ]] && echo "  $pinyin_addon_conf -> $addon_dir/superiorpinyin.conf"
echo "  $ui            -> $bin_dir/superior_ui"
[[ -x "$settings" ]] && echo "  $settings -> $bin_dir/superior_settings"
[[ -x "$config_cli" ]] && echo "  $config_cli -> $bin_dir/superior-config"
echo "  wrappers       -> $wrapper_dir/{superior_ui,superior_settings,superior-config}"
echo "  skins          -> $skin_dir"
echo "  env            -> $env_dir/90-superior-ime.conf"

if (( dry_run )); then
    echo "dry-run: nothing written"
    exit 0
fi

install -Dm755 "$so" "$lib_dir/libsuperioragent.so"
install -Dm644 "$addon_conf" "$addon_dir/superioragent.conf"
install -Dm644 "$entry_conf" "$im_dir/super-ime.conf"
if [[ -n "$pinyin_so" && -n "$pinyin_addon_conf" ]]; then
    install -Dm755 "$pinyin_so" "$lib_dir/libsuperiorpinyin.so"
    install -Dm644 "$pinyin_addon_conf" "$addon_dir/superiorpinyin.conf"
    # The addon was renamed with the capability; a leftover copy would make
    # Fcitx5 load two engines that provide the same input method.
    rm -f "$lib_dir/libsuperiorrime.so" "$addon_dir/superiorrime.conf"
else
    echo "install-user: no Rime engine in this build; text input will not work" >&2
fi
install -Dm644 backend/platforms/fcitx5/data/icons/org.superior.ime.svg \
    "$icon_dir/org.superior.ime.svg"
install -Dm755 "$ui" "$bin_dir/superior_ui"
if [[ -x "$settings" ]]; then
    install -Dm755 "$settings" "$bin_dir/superior_settings"
fi
if [[ -x "$config_cli" ]]; then
    install -Dm755 "$config_cli" "$bin_dir/superior-config"
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
[[ -x "$config_cli" ]] && write_wrapper superior-config "$bin_dir/superior-config"

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

The input method needs its schema data (26 MB download, compiled locally):
  scripts/fetch-pinyin-data.sh
Without it the entry still appears, but typing has no candidates and the log
says why. There is only one entry: 「先！进！输入法」 (super-ime).

Uninstall everything this script created:
  scripts/uninstall-user.sh
MSG
