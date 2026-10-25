#!/usr/bin/env bash
# Downloads the Rime schema used by the built-in librime engine and deploys it.
#
# The engine owns its data: everything lands in
#   $XDG_DATA_HOME/superior-ime/rime/{shared,user}
# so a user's ~/.config/fcitx5/rime (used by fcitx5-rime) is never touched.
#
# Default schema: 万象拼音 Pure (wanxiang_pure), pinned to one release so two
# machines get the same dictionary. The release is a data download, not vendored
# source: it is ~26 MB compressed / ~80 MB extracted and changes often.
# Schema data: 万象拼音 by amzxyz, CC-BY-4.0 (https://github.com/amzxyz/rime-wanxiang).
#
# Usage:
#   scripts/fetch-rime-data.sh              download + deploy
#   scripts/fetch-rime-data.sh --no-deploy  download only
#   scripts/fetch-rime-data.sh --gram       also download the 399 MB grammar model
#   scripts/fetch-rime-data.sh --force      re-download even if the stamp matches
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

version="${SUPERIOR_RIME_VERSION:-18.1.7}"
archive="rime-wanxiang-pure.zip"
url="${SUPERIOR_RIME_URL:-https://github.com/amzxyz/rime-wanxiang/releases/download/v${version}/${archive}}"
# Checksum of the pinned release. A changed upstream file must be reviewed, not
# silently accepted: the dictionary decides what every candidate looks like.
sha256="${SUPERIOR_RIME_SHA256:-0025b1ca084bfd7f4d766662050a89b62a9d21b1b8b19c623edc931cb25cf986}"
gram_url="https://github.com/amzxyz/RIME-LMDG/releases/download/LTS/wanxiang-lts-zh-hans.gram"
gram_name="wanxiang-lts-zh-hans.gram"

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
rime_root="$data_home/superior-ime/rime"
shared="$rime_root/shared"
user="$rime_root/user"
stamp="$shared/.superior-rime-version"
cache="$root/build/downloads"

deploy=1
gram=0
force=0
for argument in "$@"; do
    case "$argument" in
        --no-deploy) deploy=0 ;;
        --deploy) deploy=1 ;;
        --gram|--grammar) gram=1 ;;
        --force) force=1 ;;
        -h|--help)
            awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next }
                 NR > 1 { exit }' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *) echo "fetch-rime-data: unknown argument '$argument'" >&2; exit 2 ;;
    esac
done

if ! command -v python3 >/dev/null 2>&1; then
    echo "fetch-rime-data: python3 is required to extract the archive" >&2
    exit 1
fi

mkdir -p "$shared" "$user" "$cache"

# The input method owns Chinese/English switching: its agent implements the
# Sogou-style Shift tap and commits letters itself. librime's ascii_composer
# would toggle a second, independent ascii_mode on the same keys, and the two
# states drift apart: after 英文 -> 中文 the engine stays ASCII, so typing only
# produces Latin letters. All switch keys are noop, which makes the engine
# always compose Chinese.
patch_file="$user/default.custom.yaml"
if [[ -f "$patch_file" ]] && ! grep -q "Written by Superior-IME" "$patch_file"; then
    mv -f "$patch_file" "$patch_file.bak-superior"
    echo "fetch-rime-data: kept the previous default.custom.yaml as .bak-superior"
fi
cat > "$patch_file" <<'PATCH'
# Written by Superior-IME (scripts/fetch-rime-data.sh). Do not copy this file
# into ~/.config/fcitx5/rime: it belongs to the bundled engine only.
#
# Chinese/English switching is implemented by the Superior agent ( Shift tap,
# inline English). If librime also toggled ascii_mode on Shift, the two states
# would drift apart. noop keeps the engine in Chinese mode at all times.
patch:
  ascii_composer/switch_key:
    Caps_Lock: noop
    Shift_L: noop
    Shift_R: noop
    Control_L: noop
    Control_R: noop
PATCH

extract_archive() {
    local keep_grammar="$1"
    python3 - "$cache/$archive" "$shared" "$keep_grammar" <<'PYEXTRACT'
import pathlib
import sys
import zipfile

archive, destination, keep_grammar = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3] == "1"
destination.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(archive) as bundle:
    for name in bundle.namelist():
        parts = pathlib.PurePosixPath(name).parts
        if name.startswith("/") or ".." in parts:
            raise SystemExit(f"unsafe path in archive: {name}")
    bundle.extractall(destination)

    # 万象 ships a `grammar:` block that makes librime look for a 399 MB
    # `wanxiang-lts-zh-hans.gram` on every session. Without the file, octagram
    # logs two errors per window. The grammar is opt-in (--gram), so the block is
    # removed unless the model was requested.
    if not keep_grammar:
        for schema in destination.glob("*.schema.yaml"):
            lines = schema.read_text(encoding="utf-8").splitlines(keepends=True)
            kept, skipping = [], False
            for line in lines:
                if not skipping and line.startswith("grammar:"):
                    skipping = True
                    continue
                if skipping:
                    if line and not line[0].isspace() and line[0] not in "#\n":
                        skipping = False
                    else:
                        continue
                kept.append(line)
            schema.write_text("".join(kept), encoding="utf-8")
PYEXTRACT
}

if [[ $force -eq 0 && -f "$stamp" && "$(cat "$stamp")" == "$version" && -f "$shared/default.yaml" ]]; then
    echo "fetch-rime-data: share data for $version already present"
else
    echo "fetch-rime-data: downloading $url"
    temporary="$cache/$archive.part"
    curl -fL --retry 3 --connect-timeout 20 --progress-bar -o "$temporary" "$url"
    actual="$(sha256sum "$temporary" | cut -d' ' -f1)"
    if [[ "$actual" != "$sha256" ]]; then
        rm -f "$temporary"
        echo "fetch-rime-data: checksum mismatch (expected $sha256, got $actual)" >&2
        echo "fetch-rime-data: refusing to deploy an unexpected dictionary" >&2
        exit 1
    fi
    mv -f "$temporary" "$cache/$archive"

    echo "fetch-rime-data: extracting into $shared"
    extract_archive "$gram"
    printf '%s' "$version" > "$stamp"
fi

if [[ $gram -eq 1 ]]; then
    # The octagram plugin resolves the model relative to the *user* directory,
    # not the shared one (librime logs the exact path it tried).
    if [[ -f "$user/$gram_name" ]]; then
        echo "fetch-rime-data: grammar model already present"
    else
        echo "fetch-rime-data: downloading the grammar model (399 MB)"
        curl -fL --retry 2 --connect-timeout 20 --progress-bar -o "$user/$gram_name.part" "$gram_url"
        mv -f "$user/$gram_name.part" "$user/$gram_name"
    fi
    if [[ -f "$cache/$archive" ]] &&
        (! [[ -f "$shared/wanxiang_pure.schema.yaml" ]] ||
         ! grep -q '^grammar:' "$shared/wanxiang_pure.schema.yaml"); then
        echo "fetch-rime-data: restoring the grammar block from the cached archive"
        extract_archive 1
    fi
    # A schema change needs a rebuild, otherwise the old compiled profile wins.
    deploy=1
fi

if [[ $deploy -eq 0 ]]; then
    echo "fetch-rime-data: data ready; run 'superior-config rime deploy' when you want to compile it"
    exit 0
fi

cli="${SUPERIOR_CONFIG_CLI:-}"
if [[ -z "$cli" && -n "${SUPERIOR_PRESET:-}" ]]; then
    candidate="$root/build/$SUPERIOR_PRESET/tools/superior-config/superior_config_cli"
    [[ -x "$candidate" ]] && cli="$candidate"
fi
if [[ -z "$cli" ]]; then
    # Newest build wins, but never an ASan binary: it is slower, needs its
    # runtime on every call and can fail deployment under a leak checker.
    cli="$(find "$root/build" -type f -name superior_config_cli -perm -u+x \
        -printf '%T@ %p\n' 2>/dev/null | grep -v '/linux-asan/' |
        sort -nr | head -1 | cut -d' ' -f2- || true)"
fi
if [[ -z "$cli" ]]; then
    cli="$(find "$root/build" -type f -name superior_config_cli -perm -u+x \
        -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2- || true)"
fi

if [[ -n "$cli" ]]; then
    echo "fetch-rime-data: deploying with $cli"
    "$cli" rime deploy --shared "$shared" --user "$user"
elif command -v rime_deployer >/dev/null 2>&1; then
    echo "fetch-rime-data: using the system rime_deployer (build the project for the exact engine)"
    rime_deployer --build "$user" "$shared" "$user/build"
else
    echo "fetch-rime-data: no deployer found" >&2
    echo "  build the project: scripts/configure.sh linux-ui && scripts/build.sh linux-ui" >&2
    echo "  then re-run:       scripts/fetch-rime-data.sh" >&2
    exit 1
fi

echo
echo "fetch-rime-data: done"
echo "  data    : $shared"
echo "  compiled: $user/build"
echo "  restart Fcitx5; the only entry is 「先！进！输入法」 (super-ime)."
