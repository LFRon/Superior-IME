#!/usr/bin/env bash
# Deprecated name; the script is named after the capability now.
#
# Kept for one release so an existing shell history or note keeps working, and
# so the rename is visible instead of a silent "command not found".
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "fetch-rime-data.sh: renamed to fetch-pinyin-data.sh" >&2
exec "$root/fetch-pinyin-data.sh" "$@"
