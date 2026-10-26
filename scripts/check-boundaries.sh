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
# No platform key numbering in the decision layer. A keysym literal here means
# the policy knows one platform's values, and the other two platforms then have
# to imitate them forever (`KeyCode.hpp` explains the alternative).
scan "backend/core" '0x[fF][fF][0-9a-fA-F][0-9a-fA-F]' "core -> X11 keysym literal"
scan "backend/core" '\bkKey[A-Z]' "core -> platform key constant"

# Path roots come from Superior::Paths only. A capability that parses XDG_* or
# HOME on its own becomes a second place to update when a port changes the root
# set (Windows: KnownFolders), and the copies drift.
scan "backend/api" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "api -> environment path"
scan "backend/config" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "config -> environment path"
scan "backend/core" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "core -> environment path"
scan "backend/pinyin" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "pinyin -> environment path"
scan "backend/runtime" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "runtime -> environment path"
scan "backend/skin" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "skin -> environment path"
scan "backend/speech-recognition" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "speech -> environment path"
scan "backend/theme" 'getenv\("(XDG_[A-Z_]+|HOME)"\)' "theme -> environment path"

# Pure data modules must stay free of Qt and Fcitx5.
scan "backend/config" '#include[[:space:]]*[<"](Q|Qt)' "config -> Qt"
scan "backend/config" '#include[[:space:]]*[<"]fcitx' "config -> fcitx5"
scan "backend/theme" '#include[[:space:]]*[<"](Q|Qt)' "theme -> Qt"
scan "backend/theme" '#include[[:space:]]*[<"]fcitx' "theme -> fcitx5"

# The skin model is data only: no Qt, no pixel decoding, no framework.
scan "backend/skin" '#include[[:space:]]*[<"](Q|Qt)' "skin -> Qt"
scan "backend/skin" '#include[[:space:]]*[<"]fcitx' "skin -> fcitx5"
scan "backend/skin" '\bQImage\b|\bQPixmap\b|\bQPainter\b' "skin -> pixel decoding"

# Cloud pinyin is transport + parsing only: it must not know about the framework
# or the UI toolkit, otherwise it cannot be unit tested or reused on Windows.
scan "backend/cloudpinyin" '#include[[:space:]]*[<"](Q|Qt)' "cloudpinyin -> Qt"
scan "backend/cloudpinyin" '#include[[:space:]]*[<"]fcitx' "cloudpinyin -> fcitx5"

# The Rime engine must stay free of any toolkit and of Fcitx5: it is the piece
# that has to be reusable on Windows/Android later. librime itself is reached
# only through a port, and only in a .cpp file.
scan "backend/pinyin" '#include[[:space:]]*[<"](Q|Qt)' "pinyin -> Qt"
scan "backend/pinyin" '#include[[:space:]]*[<"]fcitx' "pinyin -> fcitx5"
scan "backend/pinyin/include" '#include[[:space:]]*[<"](rime_api|dlfcn)' "pinyin public header -> librime/dlfcn"

# Speech recognition is capture + one external process: it must stay usable on
# Windows and Android, so no toolkit and no Fcitx5 may leak in.
scan "backend/speech-recognition" '#include[[:space:]]*[<"](Q|Qt)' "speech-recognition -> Qt"
scan "backend/speech-recognition" '#include[[:space:]]*[<"]fcitx' "speech-recognition -> fcitx5"

# The decision layer talks to capabilities through its own contract; it must not
# depend on a concrete capability either.
scan "backend/core" '#include[[:space:]]*[<"]superior/(pinyin|speech-recognition|cloudpinyin)/' "core -> capability"

# Runtime may use core, but its public headers must not expose core types.
scan "backend/runtime/include" '#include[[:space:]]*[<"]superior/core/' "runtime public header -> core"
scan "backend/runtime/include" 'superior::core' "runtime public header -> core"

if (( fail != 0 )); then
    echo "check-boundaries: FAILED" >&2
    exit 1
fi

echo "check-boundaries: ok (${checked} file(s) scanned)"
