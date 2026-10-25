// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/rime/RimePaths.hpp"

#include <cstdlib>
#include <system_error>

namespace superior::rime {

namespace fs = std::filesystem;

namespace {

fs::path dataHome() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return fs::path(xdg);
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local" / "share";
    }
    return {};
}

} // namespace

fs::path defaultRimeRoot() {
    const fs::path home = dataHome();
    if (home.empty()) {
        return {};
    }
    return home / "superior-ime" / "rime";
}

Paths Paths::fromEnvironment() {
    const fs::path root = defaultRimeRoot();
    return withRoot(root.empty() ? fs::path{} : root.parent_path().parent_path());
}

Paths Paths::withRoot(const fs::path &dataHomeDir, std::string appName) {
    Paths paths;
    paths.appName = std::move(appName);
    if (dataHomeDir.empty()) {
        return paths;
    }
    const fs::path rime = dataHomeDir / "superior-ime" / "rime";
    paths.sharedData = rime / "shared";
    paths.userData = rime / "user";
    return paths;
}

bool hasCompiledArtifacts(const Paths &paths) {
    std::error_code error;
    if (paths.userData.empty()) {
        return false;
    }
    // librime writes into `<user>/build` unless a staging directory is set, but
    // older deployments put the `.bin` files directly in the user directory.
    // Both layouts have to count as "deployed".
    const std::filesystem::path roots[] = {paths.userData, paths.userData / "build"};
    for (const auto &root : roots) {
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
            if (error) {
                return false;
            }
            if (entry.is_regular_file(error) && entry.path().extension() == ".bin") {
                return true;
            }
        }
    }
    return false;
}

} // namespace superior::rime
