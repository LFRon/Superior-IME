// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/paths/Roots.hpp"

#include <cstdlib>
#include <string>

namespace superior::paths {
namespace {

namespace fs = std::filesystem;

/// `$NAME`, or `fallback` when the variable is unset or empty.
fs::path envOr(const char *name, const fs::path &fallback) {
    const char *raw = std::getenv(name);
    if (raw != nullptr && raw[0] != '\0') {
        return fs::path(raw);
    }
    return fallback;
}

/// A per-user XDG directory: the variable wins, otherwise `$HOME/<suffix>`.
///
/// Returns empty — not `<suffix>` — when `$HOME` is missing too. That
/// distinction matters: an empty path means "I do not know", while
/// `.local/share` would silently write into the current working directory,
/// which for a running input method is whatever process happened to spawn it.
fs::path xdgDir(const char *name, const fs::path &home, const char *suffix) {
    const char *raw = std::getenv(name);
    if (raw != nullptr && raw[0] != '\0') {
        return fs::path(raw);
    }
    if (home.empty()) {
        return {};
    }
    return home / suffix;
}

std::vector<fs::path> splitPathList(const char *raw) {
    std::vector<fs::path> result;
    if (raw == nullptr) {
        return result;
    }
    const std::string text(raw);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t colon = text.find(':', start);
        const std::size_t end = colon == std::string::npos ? text.size() : colon;
        if (end > start) {
            result.emplace_back(text.substr(start, end - start));
        }
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }
    return result;
}

} // namespace

Roots Roots::fromEnvironment() {
    Roots roots;
    roots.home = envOr("HOME", {});
    roots.data = xdgDir("XDG_DATA_HOME", roots.home, ".local/share");
    roots.config = xdgDir("XDG_CONFIG_HOME", roots.home, ".config");
    roots.state = xdgDir("XDG_STATE_HOME", roots.home, ".local/state");
    roots.cache = xdgDir("XDG_CACHE_HOME", roots.home, ".cache");
    roots.runtime = envOr("XDG_RUNTIME_DIR", {});

    roots.dataDirs = splitPathList(std::getenv("XDG_DATA_DIRS"));
    if (roots.dataDirs.empty()) {
        roots.dataDirs.emplace_back("/usr/local/share");
        roots.dataDirs.emplace_back("/usr/share");
    }
    return roots;
}

} // namespace superior::paths
