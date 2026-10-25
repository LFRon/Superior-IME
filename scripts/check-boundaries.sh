#!/usr/bin/env bash
# Architecture boundary check.
#
# These rules are deliberately mechanical: if a layer starts reaching into a
# layer it must not know about, this script fails the build instead of relying
# on review discipline alone.
set -uo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
root="$(cd "$root" && pwd)"

fail=0
checked=0

scan() {
    local dir="$1" pattern="$2" label="$3"
    local path="$root/$dir"
    [[ -d "$path" ]] || return 0
    while IFS= read -r -d '' file; do
        checked=$((checked + 1))
        if grep -nE -- "$pattern" "$file" >/dev/null 2>&1; then
            echo "boundary violation [$label]: ${file#"$root/"}" >&2
            grep -nE -- "$pattern" "$file" >&2
            fail=1
        fi
    done < <(find "$path" -type f \
        \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) -print0)
}

# The UI must not reach the engine, the runtime, or the framework adapter.
# Pure data modules (api, skin, config, theme, protocol) are allowed.
scan "frontend" 'superior/(core|runtime|platforms)/' "frontend -> backend internals"
scan "frontend" 'superior::(core|runtime|platforms)' "frontend -> backend internals"
scan "frontend" '#include[[:space:]]*[<"]fcitx' "frontend -> fcitx5"
scan "frontend" '#include[[:space:]]*[<"]Fcitx5' "frontend -> fcitx5"

# Contracts must not depend on an implementation.
scan "backend/api" '#include[[:space:]]*[<"]superior/(core|runtime|skin|config|theme)/' "api -> implementation"
scan "backend/api" '#include[[:space:]]*[<"]fcitx' "api -> fcitx5"

# Core logic must stay platform- and clock-free.
scan "backend/core" '#include[[:space:]]*[<"](Q|Qt)' "core -> Qt"
scan "backend/core" '#include[[:space:]]*[<"]fcitx' "core -> fcitx5"
scan "backend/core" 'std::chrono::(system_clock|steady_clock)' "core -> wall clock"
scan "backend/core" 'std::this_thread::sleep' "core -> blocking sleep"

# Pure data modules must stay free of Qt and Fcitx5.
scan "backend/config" '#include[[:space:]]*[<"](Q|Qt)' "config -> Qt"
scan "backend/config" '#include[[:space:]]*[<"]fcitx' "config -> fcitx5"
scan "backend/theme" '#include[[:space:]]*[<"](Q|Qt)' "theme -> Qt"
scan "backend/theme" '#include[[:space:]]*[<"]fcitx' "theme -> fcitx5"

# The skin model is data only: no Qt, no pixel decoding, no framework.
scan "backend/skin" '#include[[:space:]]*[<"](Q|Qt)' "skin -> Qt"
scan "backend/skin" '#include[[:space:]]*[<"]fcitx' "skin -> fcitx5"
scan "backend/skin" '\bQImage\b|\bQPixmap\b|\bQPainter\b' "skin -> pixel decoding"

# Runtime may use core, but its public headers must not expose core types.
scan "backend/runtime/include" '#include[[:space:]]*[<"]superior/core/' "runtime public header -> core"
scan "backend/runtime/include" 'superior::core' "runtime public header -> core"

if (( fail != 0 )); then
    echo "check-boundaries: FAILED" >&2
    exit 1
fi

echo "check-boundaries: ok (${checked} file(s) scanned)"
