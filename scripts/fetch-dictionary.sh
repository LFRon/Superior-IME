#!/usr/bin/env bash
# Fetches a large community dictionary and installs it as a libime extra dict.
#
# Why this is not in the repository: 雾凇 (rime-ice) is GPL-3.0 like this project,
# so bundling would be legal, but it is ~46 MB of source / ~10 MB compiled and it
# is updated upstream every week. Shipping a snapshot would either bloat the git
# history or silently rot. This script makes the step one command and keeps the
# data out of version control.
#
# Usage:
#   scripts/fetch-dictionary.sh rime-ice            # base + ext
#   scripts/fetch-dictionary.sh rime-ice --minimal  # base only (smaller, faster)
#   scripts/fetch-dictionary.sh wanxiang-cuoyin     # 异读/错音词表 (205 条)
#   scripts/fetch-dictionary.sh rime-ice --force    # rebuild even if present
#   scripts/fetch-dictionary.sh rime-ice --out DIR  # install somewhere else
#
# The result is a *precompiled* libime binary dictionary: `libime_pinyindict`
# builds a double-array trie once, and Fcitx5 later loads it without parsing any
# text. That is the same precompilation Rime does when it deploys a dictionary,
# and it is why the installed file is 10 MB instead of 46 MB of YAML.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

preset="${1:-}"
shift || true

if [[ -z "$preset" ]]; then
    echo "usage: scripts/fetch-dictionary.sh <rime-ice> [--minimal] [--force] [--out DIR]" >&2
    exit 2
fi

minimal=0
force=0
out_dir=""

while (($# > 0)); do
    case "$1" in
        --minimal) minimal=1 ;;
        --force) force=1 ;;
        --out)
            shift
            out_dir="${1:-}"
            [[ -n "$out_dir" ]] || { echo "fetch-dictionary: --out needs a directory" >&2; exit 2; }
            ;;
        -h|--help)
            echo "usage: scripts/fetch-dictionary.sh rime-ice [--minimal] [--force] [--out DIR]"
            exit 0
            ;;
        *)
            echo "fetch-dictionary: unknown argument '$1'" >&2
            exit 2
            ;;
    esac
    shift
done

# Build the converter first: the CLI is what does the conversion.
preset_build="${SUPERIOR_PRESET:-linux-ui}"
dict_tool="$root/build/$preset_build/tools/dict-tool/superior_dict"
if [[ ! -x "$dict_tool" ]]; then
    echo "fetch-dictionary: build first: scripts/configure.sh $preset_build && scripts/build.sh $preset_build" >&2
    exit 1
fi

# The precompiler comes from libime; without it nothing can be installed.
for tool in libime_pinyindict curl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "fetch-dictionary: '$tool' is missing" >&2
        if [[ "$tool" == libime_pinyindict ]]; then
            echo "  Debian/Ubuntu: sudo apt install libime-bin" >&2
        fi
        exit 1
    fi
done

case "$preset" in
    wanxiang-cuoyin)
        # 万象's 错音错字对照表. Small on purpose: it lists words under the
        # reading people actually use (粳米 as gengmi, 籼米 as shanmi), which
        # libime cannot learn on its own. These readings are legal syllables, so
        # they survive the pinyin validator — unlike Rime's `derive` spellings.
        repo="https://raw.githubusercontent.com/amzxyz/rime-wanxiang/wanxiang/dicts"
        sources=(cuoyin)
        name="wanxiang-cuoyin"
        ;;
    rime-ice)
        repo="https://raw.githubusercontent.com/iDvel/rime-ice/main/cn_dicts"
        sources=(base ext)
        name="rime-ice"
        ;;
    *)
        echo "fetch-dictionary: unknown dictionary '$preset'" >&2
        echo "  available: rime-ice" >&2
        exit 2
        ;;
esac

if (( minimal )); then
    sources=(base)
    name="$name-lite"
fi

if [[ -z "$out_dir" ]]; then
    # Same place fcitx5-pinyin reads extra dictionaries from.
    out_dir="${XDG_DATA_HOME:-$HOME/.local/share}/fcitx5/pinyin/dictionaries"
fi

target="$out_dir/$name.dict"
if [[ -f "$target" && "$force" -eq 0 ]]; then
    echo "fetch-dictionary: $target already exists (use --force to rebuild)"
    exit 0
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/superior-dict-XXXXXX")"
trap 'rm -rf "$work"' EXIT

files=()
for source in "${sources[@]}"; do
    echo "downloading $source.dict.yaml …"
    if ! curl -fsSL --max-time 600 --retry 2 -o "$work/$source.dict.yaml" \
            "$repo/$source.dict.yaml"; then
        echo "fetch-dictionary: cannot download '$source'" >&2
        exit 1
    fi
    files+=("$work/$source.dict.yaml")
done

echo "converting $(( minimal ? 1 : 2 )) file(s) → $name.dict"
# --overwrite: this script already decided whether to rebuild.
"$dict_tool" rime "${files[@]}" --name "$name" --out "$out_dir" --overwrite

cat <<MSG

Installed: $target
Restart Fcitx5 so it loads the new dictionary:
  fcitx5-remote -e && fcitx5 -d
MSG
