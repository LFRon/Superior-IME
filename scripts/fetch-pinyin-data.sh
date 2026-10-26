#!/usr/bin/env bash
# Deprecated name; the script now says which package it deploys.
#
# Kept so an existing shell history, note or CI job keeps working, and so the
# rename is visible instead of a silent "command not found".
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "fetch-pinyin-data.sh: renamed to fetch-wanxiang-schema.sh" >&2
exec "$root/fetch-wanxiang-schema.sh" "$@"
