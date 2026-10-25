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
speech_dir="${SUPERIOR_SPEECH_DIR:-$data_home/superior-ime/speech-recognition}"
target="${speech_dir}/engine"

# Speech gating (`--vad`) needs Silero weights next to the engine. The engine can
# fetch them itself on first use, but it does so from huggingface.co and its
# failure mode is not "no gating": every recording then comes back as silence, so
# dictation types nothing at all. Installing them here — once, with a pinned
# digest, from a URL we chose — keeps that decision out of the recording path.
vad_asset="ggml-silero-v6.2.0.bin"
vad_url="https://huggingface.co/ggml-org/whisper-vad/resolve/main/${vad_asset}"
vad_sha256="2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987"
vad_target="${speech_dir}/models/vad/${vad_asset}"

echo "engine   : ${target}"
echo "asset    : ${asset} (${version})"
echo "vad      : ${vad_target}"

if (( dry_run )); then
    if [[ -f "$vad_target" ]]; then
        echo "vad      : present, will be kept"
    else
        echo "vad src  : ${vad_url}"
    fi
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

# The weights are the one piece of this install that may legitimately be missing:
# a network that cannot reach huggingface.co must not fail an engine install that
# already succeeded, so this warns instead. Speech still works without them, it
# just types nonsense when handed silence.
if [[ -f "$vad_target" ]]; then
    echo "vad      : already present"
else
    mkdir -p "$(dirname "$vad_target")"
    if curl -fSL --retry 3 --connect-timeout 20 -o "${vad_target}.part" "$vad_url"; then
        actual="$(sha256sum "${vad_target}.part" | cut -d' ' -f1)"
        if [[ "$actual" != "$vad_sha256" ]]; then
            echo "install-speech-engine: VAD checksum mismatch" >&2
            echo "  expected $vad_sha256" >&2
            echo "  actual   $actual" >&2
            rm -f "${vad_target}.part"
            echo "install-speech-engine: speech gating stays off" >&2
        else
            mv "${vad_target}.part" "$vad_target"
            echo "vad      : installed ${vad_target}"
        fi
    else
        rm -f "${vad_target}.part"
        echo "install-speech-engine: could not fetch the VAD weights" >&2
        echo "  speech gating stays off; dictation may type nonsense on silence" >&2
    fi
fi
echo
echo "Install the model from the input method's speech panel (it is downloaded"
echo "there, into \"$data_home/superior-ime/speech-recognition/models\")."
