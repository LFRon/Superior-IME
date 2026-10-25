#!/usr/bin/env bash
# Installs the speech-recognition engine (CrispASR) into the user's data dir.
#
# Why a script and not a button in the panel:
#   * the release is a tarball with a shared-library bundle, and untarring is the
#     one thing a Qt process should not do by hand;
#   * the engine is 41 MB and versioned, so a user may prefer to install it with
#     their package manager — `resolveEngine()` also accepts anything on PATH.
#
# The whole bundle lands in one directory because the binary's RUNPATH is
# `$ORIGIN`: copy only `crispasr` and it cannot find libopenblas.
set -euo pipefail

version="v0.8.44"
from=""
dry_run=0

usage() {
    echo "usage: scripts/install-speech-engine.sh [--version <tag>] [--from <tarball>] [--dry-run]"
}

# `while` + `shift`, not `for`: a `for` loop would still visit the value that
# `shift` just consumed.
while (( $# > 0 )); do
    argument="$1"
    shift
    case "$argument" in
        --version) version="${1:?--version needs a value}"; shift ;;
        --version=*) version="${argument#*=}" ;;
        --from) from="${1:?--from needs a path}"; shift ;;
        --from=*) from="${argument#*=}" ;;
        --dry-run) dry_run=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install-speech-engine: unknown argument '$argument'" >&2; usage >&2; exit 2 ;;
    esac
done

case "$(uname -m)" in
    x86_64|amd64) arch="x86_64" ;;
    aarch64|arm64) arch="arm64" ;;
    *) echo "install-speech-engine: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

asset="crispasr-linux-${arch}.tar.gz"
url="https://github.com/CrispStrobe/CrispASR/releases/download/${version}/${asset}"

# Pinned digests are the only thing standing between a corrupted download and a
# mysterious engine crash, so they are recorded per release and architecture.
expected_sha256=""
if [[ "$version" == "v0.8.44" && "$arch" == "x86_64" ]]; then
    expected_sha256="9754a21d00463fabbd61efa27d91e2103357b95ff52a8de8208d33c6569285ce"
fi

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
target="${SUPERIOR_SPEECH_DIR:-$data_home/superior-ime/speech-recognition}/engine"

echo "engine   : ${target}"
echo "asset    : ${asset} (${version})"

if (( dry_run )); then
    echo "dry run  : nothing written"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

archive="$work/$asset"
if [[ -n "$from" ]]; then
    echo "source   : ${from}"
    cp "$from" "$archive"
else
    echo "download : ${url}"
    curl -fSL --retry 3 --connect-timeout 20 -o "$archive" "$url"
fi

if [[ -n "$expected_sha256" ]]; then
    actual="$(sha256sum "$archive" | cut -d' ' -f1)"
    if [[ "$actual" != "$expected_sha256" ]]; then
        echo "install-speech-engine: checksum mismatch" >&2
        echo "  expected $expected_sha256" >&2
        echo "  actual   $actual" >&2
        exit 1
    fi
fi

tar -xzf "$archive" -C "$work"
# The bundle unpacks into a single top-level directory whose name is not part of
# the contract, so find it by its contents instead.
source_dir="$(dirname "$(find "$work" -maxdepth 3 -type f -name crispasr -print -quit)")"
if [[ -z "$source_dir" || ! -x "$source_dir/crispasr" ]]; then
    echo "install-speech-engine: the archive does not contain crispasr" >&2
    exit 1
fi

mkdir -p "$target"
# Stale libraries from an older release must not stay behind: the binary loads
# them by name from its own directory.
rm -rf "${target:?}"/*
cp -a "$source_dir"/. "$target"/

"$target/crispasr" --version 2>/dev/null | sed -n '1,3p' || true
echo "installed: ${target}/crispasr"
echo
echo "Install the model from the input method's speech panel (it is downloaded"
echo "there, into \"$data_home/superior-ime/speech-recognition/models\")."
