#!/usr/bin/env bash
# Asserts that the schema archive shipped in data/wanxiang is the one the
# deployer pins.
#
# Vendoring moved the dictionary into the repository, which created a new way
# for the tree to be wrong: someone replaces the zip (or regenerates it with a
# different compressor) and the pin in fetch-wanxiang-schema.sh keeps telling a
# nice story about bytes that are no longer there. The pin is the source of
# truth, so this compares the two instead of hardcoding the value twice.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deployer="$root/scripts/fetch-wanxiang-schema.sh"

fail() {
    echo "check-bundled-schema: $*" >&2
    exit 1
}

[[ -f "$deployer" ]] || fail "missing $deployer"

# Read the defaults the script would use, without running it.
pin="$(sed -n 's/^sha256="\${SUPERIOR_RIME_SHA256:-\([0-9a-f]\{64\}\)}"$/\1/p' "$deployer")"
name="$(sed -n 's/^archive="\(.*\)"$/\1/p' "$deployer")"
version="$(sed -n 's/^version="\${SUPERIOR_RIME_VERSION:-\(.*\)}"$/\1/p' "$deployer")"

[[ -n "$pin" ]] || fail "could not read sha256 from $deployer"
[[ -n "$name" ]] || fail "could not read the archive name from $deployer"
[[ -n "$version" ]] || fail "could not read the version from $deployer"

bundled="$root/data/wanxiang/$name"
[[ -f "$bundled" ]] || fail "missing $bundled (the deployer expects it for an offline install)"

actual="$(sha256sum "$bundled" | cut -d' ' -f1)"
if [[ "$actual" != "$pin" ]]; then
    fail "data/wanxiang/$name has sha256 $actual but $deployer pins $pin
  Either restore the pinned bytes, or bump version/sha256 together with the file."
fi

echo "check-bundled-schema: data/wanxiang/$name matches the pin ($version, $actual)"
