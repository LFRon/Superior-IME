// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/Layout.hpp"

#include <cstdlib>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "superior/diagnostics/Log.hpp"

namespace superior::pinyin {

namespace fs = std::filesystem;
namespace diag = ::superior::diagnostics;

namespace {

constexpr std::string_view kAppDir = "superior-ime";
constexpr std::string_view kCapabilityDir = "pinyin";
constexpr std::string_view kLegacyDir = "rime";

fs::path dataHomeFromEnvironment() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return fs::path(xdg);
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local" / "share";
    }
    return {};
}

bool isDirectory(const fs::path &path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(path, error);
}

} // namespace

fs::path defaultPinyinRoot() {
    const fs::path home = dataHomeFromEnvironment();
    if (home.empty()) {
        return {};
    }
    return home / kAppDir / kCapabilityDir;
}

namespace {

/// True when a leveldb in `userDir` is held by a live process.
///
/// librime keeps its `userdb` locked for the whole session. Renaming the
/// directory underneath the running process would leave two writers on the same
/// files (the old process keeps its file descriptors, the new one opens the new
/// path), which is exactly how a leveldb ends up recovering from a `lost/`
/// directory. So a migration is skipped while the data is in use, and the caller
/// keeps reading the legacy path.
bool databaseInUse(const fs::path &userDir) {
#if defined(__unix__) || defined(__APPLE__)
    const fs::path lockPath = userDir / "LOCK";
    std::error_code error;
    if (!fs::is_regular_file(lockPath, error)) {
        return false;
    }
    const int fd = ::open(lockPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct flock lock {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    const bool held = ::fcntl(fd, F_SETLK, &lock) == -1;
    if (!held) {
        lock.l_type = F_UNLCK;
        ::fcntl(fd, F_SETLK, &lock);
    }
    ::close(fd);
    return held;
#else
    (void)userDir;
    return false;
#endif
}

} // namespace

LayoutDecision decideLayout(bool newExists, bool legacyExists) noexcept {
    if (newExists) {
        // A half-finished migration leaves both behind; the new directory wins
        // and the legacy one is reported, never merged.
        return LayoutDecision::UseNew;
    }
    if (legacyExists) {
        return LayoutDecision::RenameLegacy;
    }
    return LayoutDecision::UseNew;
}

LayoutResolution resolvePinyinRoot(const fs::path &dataHome) {
    LayoutResolution resolution;
    if (dataHome.empty()) {
        return resolution;
    }

    const fs::path appDir = dataHome / kAppDir;
    const fs::path newRoot = appDir / kCapabilityDir;
    const fs::path legacyRoot = appDir / kLegacyDir;

    // An explicit override wins over every heuristic.
    if (const char *override = std::getenv("SUPERIOR_PINYIN_DIR");
        override != nullptr && override[0] != '\0') {
        resolution.root = override;
        resolution.decision = LayoutDecision::UseNew;
        return resolution;
    }

    const bool newExists = isDirectory(newRoot);
    const bool legacyExists = isDirectory(legacyRoot);
    resolution.decision = decideLayout(newExists, legacyExists);

    switch (resolution.decision) {
    case LayoutDecision::UseNew:
        resolution.root = newRoot;
        if (newExists && legacyExists) {
            SUPERIOR_WARN(diag::categories::Pinyin)
                .field("new", newRoot.string())
                .field("legacy", legacyRoot.string())
                << "both pinyin data directories exist; using the new one and "
                   "leaving the legacy directory untouched";
        }
        return resolution;

    case LayoutDecision::RenameLegacy:
    case LayoutDecision::UseLegacyFallback:
        break;
    }

    if (databaseInUse(legacyRoot / "user")) {
        resolution.root = legacyRoot;
        resolution.decision = LayoutDecision::UseLegacyFallback;
        resolution.detail = "the data directory is in use";
        SUPERIOR_INFO(diag::categories::Pinyin)
            .field("root", legacyRoot.string())
            << "pinyin data migration deferred: the directory is in use; "
               "restart Fcitx5 to finish it";
        return resolution;
    }

    std::error_code error;
    fs::rename(legacyRoot, newRoot, error);
    if (!error) {
        resolution.root = newRoot;
        resolution.migrated = true;
        SUPERIOR_INFO(diag::categories::Pinyin)
            .field("from", legacyRoot.string())
            .field("to", newRoot.string())
            << "migrated the pinyin data directory";
        return resolution;
    }

    // Do not pretend the data is gone: keep reading the old directory so the
    // user can still type, and say exactly why the rename failed.
    resolution.root = legacyRoot;
    resolution.decision = LayoutDecision::UseLegacyFallback;
    resolution.detail = error.message();
    SUPERIOR_WARN(diag::categories::Pinyin)
        .field("from", legacyRoot.string())
        .field("to", newRoot.string())
        .field("error", error.message())
        << "cannot migrate the pinyin data directory; using the legacy path";
    return resolution;
}

LayoutResolution resolvePinyinRootFromEnvironment() {
    return resolvePinyinRoot(dataHomeFromEnvironment());
}

Paths Paths::fromEnvironment() {
    Paths paths;
    const LayoutResolution resolution = resolvePinyinRootFromEnvironment();
    if (resolution.root.empty()) {
        return paths;
    }
    paths.sharedData = resolution.root / "shared";
    paths.userData = resolution.root / "user";
    return paths;
}

Paths Paths::withRoot(const std::filesystem::path &dataHome, std::string appName) {
    Paths paths;
    paths.appName = std::move(appName);
    if (dataHome.empty()) {
        return paths;
    }
    const fs::path root = dataHome / kAppDir / kCapabilityDir;
    paths.sharedData = root / "shared";
    paths.userData = root / "user";
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
    const fs::path roots[] = {paths.userData, paths.userData / "build"};
    for (const auto &root : roots) {
        if (!fs::is_directory(root, error)) {
            continue;
        }
        for (const auto &entry : fs::directory_iterator(root, error)) {
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

} // namespace superior::pinyin
