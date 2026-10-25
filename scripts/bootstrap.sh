#!/usr/bin/env bash
# Checks build prerequisites without installing anything.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

missing=0

check_command() {
    local name="$1" hint="$2"
    if command -v "$name" >/dev/null 2>&1; then
        printf '  ok      %-14s %s\n' "$name" "$("$name" --version 2>/dev/null | head -n1)"
    else
        printf '  MISSING %-14s %s\n' "$name" "$hint"
        missing=1
    fi
}

check_cmake() {
    if ! command -v cmake >/dev/null 2>&1; then
        printf '  MISSING %-14s install cmake >= 3.25\n' cmake
        missing=1
        return
    fi
    local version
    version="$(cmake --version | head -n1 | awk '{print $3}')"
    local major minor
    major="${version%%.*}"
    minor="$(printf '%s' "$version" | cut -d. -f2)"
    if (( major > 3 )) || { (( major == 3 )) && (( minor >= 25 )); }; then
        printf '  ok      %-14s %s\n' cmake "$version"
    else
        printf '  MISSING %-14s found %s, need >= 3.25\n' cmake "$version"
        missing=1
    fi
}

check_header() {
    local header="$1" hint="$2"
    if printf '#include <%s>\nint main(){return 0;}\n' "$header" |
        "${CXX:-c++}" -std=c++20 -x c++ -fsyntax-only - >/dev/null 2>&1; then
        printf '  ok      %-14s\n' "$header"
    else
        printf '  MISSING %-14s %s\n' "$header" "$hint"
        missing=1
    fi
}

echo "Superior-IME bootstrap check"
echo
check_cmake
check_command ninja "install ninja-build"
check_command "${CXX:-c++}" "install a C++20 compiler (g++ >= 13 or clang >= 17)"
check_command pkg-config "install pkg-config"
check_header nlohmann/json.hpp "install nlohmann-json3-dev"

echo
if (( missing == 0 )); then
    echo "All prerequisites found. Next: scripts/configure.sh && scripts/build.sh && scripts/test.sh"
else
    cat <<'EOF'
Install hints:
  Debian/Ubuntu/Deepin : sudo apt install build-essential cmake ninja-build pkg-config nlohmann-json3-dev
  Fedora               : sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config nlohmann-json-devel
  Arch                 : sudo pacman -S base-devel cmake ninja pkgconf nlohmann-json
EOF
    exit 1
fi
