// SPDX-License-Identifier: GPL-3.0-or-later
//
// `Roots` is the single place that turns the environment into directories, so
// its fallback chain is pinned here rather than being re-derived by hand in four
// capabilities.
//
// The cases that matter:
//
// 1. an XDG variable wins over `$HOME`;
// 2. `$HOME` alone still produces the historical dot-directories, because that
//    is what every existing installation already wrote to;
// 3. with neither, the result is *empty*, not a relative path — a relative path
//    would drop state into whatever directory the input method was started in;
// 4. `runtime` has no fallback at all (the spec says it is session-scoped), and
//    `dataDirs` falls back to the two system prefixes.

#include "support/TestSupport.hpp"
#include "superior/paths/Roots.hpp"

#include <cstdlib>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using superior::paths::Roots;

/// Saves and restores one variable so each case starts from a known state.
class ScopedEnv {
public:
    ScopedEnv(const char *name, const char *value) : name_(name) {
        if (const char *previous = std::getenv(name); previous != nullptr) {
            previous_ = std::string(previous);
        }
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

    ~ScopedEnv() {
        if (previous_) {
            ::setenv(name_, previous_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

private:
    const char *name_;
    std::optional<std::string> previous_;
};

/// Clears every variable `Roots` reads, so a case does not inherit the shell.
void clearEnvironment() {
    for (const char *name : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                             "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "XDG_DATA_DIRS"}) {
        ::unsetenv(name);
    }
}

void testXdgVariablesWin() {
    ScopedEnv home("HOME", "/home/alice");
    ScopedEnv data("XDG_DATA_HOME", "/data");
    ScopedEnv config("XDG_CONFIG_HOME", "/config");
    ScopedEnv state("XDG_STATE_HOME", "/state");
    ScopedEnv cache("XDG_CACHE_HOME", "/cache");
    ScopedEnv runtime("XDG_RUNTIME_DIR", "/run/user/1000");

    const Roots roots = Roots::fromEnvironment();
    CHECK(roots.home == fs::path("/home/alice"));
    CHECK(roots.data == fs::path("/data"));
    CHECK(roots.config == fs::path("/config"));
    CHECK(roots.state == fs::path("/state"));
    CHECK(roots.cache == fs::path("/cache"));
    CHECK(roots.runtime == fs::path("/run/user/1000"));
}

void testHomeFallbacks() {
    clearEnvironment();
    ScopedEnv home("HOME", "/home/alice");

    const Roots roots = Roots::fromEnvironment();
    CHECK(roots.data == fs::path("/home/alice/.local/share"));
    CHECK(roots.config == fs::path("/home/alice/.config"));
    CHECK(roots.state == fs::path("/home/alice/.local/state"));
    CHECK(roots.cache == fs::path("/home/alice/.cache"));
    // $XDG_RUNTIME_DIR must not be invented: it is a per-session directory.
    CHECK(roots.runtime.empty());
}

void testEmptyEnvironmentStaysEmpty() {
    clearEnvironment();

    const Roots roots = Roots::fromEnvironment();
    CHECK(roots.home.empty());
    CHECK(roots.data.empty());
    CHECK(roots.config.empty());
    CHECK(roots.state.empty());
    CHECK(roots.cache.empty());
    CHECK(roots.runtime.empty());
}

void testEmptyValueIsTreatedAsUnset() {
    ScopedEnv home("HOME", "/home/alice");
    ScopedEnv data("XDG_DATA_HOME", "");

    CHECK(Roots::fromEnvironment().data == fs::path("/home/alice/.local/share"));
}

void testDataDirsFallback() {
    clearEnvironment();
    const std::vector<fs::path> systemDirs{"/usr/local/share", "/usr/share"};
    CHECK(Roots::fromEnvironment().dataDirs == systemDirs);

    ScopedEnv dirs("XDG_DATA_DIRS", "/opt/share::/usr/share");
    // An empty entry in the middle is skipped, not turned into the cwd.
    const std::vector<fs::path> overridden{"/opt/share", "/usr/share"};
    CHECK(Roots::fromEnvironment().dataDirs == overridden);
}

} // namespace

int main() {
    testXdgVariablesWin();
    testHomeFallbacks();
    testEmptyEnvironmentStaysEmpty();
    testEmptyValueIsTreatedAsUnset();
    testDataDirsFallback();
    return superior::test::finish("paths");
}
