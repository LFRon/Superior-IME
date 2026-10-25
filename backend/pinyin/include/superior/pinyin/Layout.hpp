// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "superior/pinyin/Types.hpp"

namespace superior::pinyin {

/// Where the pinyin capability keeps its data:
/// `$XDG_DATA_HOME/superior-ime/pinyin`.
///
/// The directory is named after the **capability**, not after librime: a future
/// engine would still store its data here. Separate from Fcitx5's own Rime
/// directory on purpose — our schemas, our `userdb`, and no chance of
/// overwriting a user's existing setup.
[[nodiscard]] std::filesystem::path defaultPinyinRoot();

/// Which directory a read should use.
///
/// Resolution is **pure**: it only decides where the data is, and never moves
/// anything. A library call that renames directories is a footgun — a CLI or a
/// test binary would be able to pull the data out from under a running Fcitx5.
enum class LayoutDecision {
    /// `.../superior-ime/pinyin` exists, or nothing exists yet (fresh install).
    UseNew,
    /// Only the pre-rename `.../superior-ime/rime` exists; read it as-is until a
    /// migration is requested.
    UseLegacy,
};

[[nodiscard]] LayoutDecision decideLayout(bool newExists, bool legacyExists) noexcept;

struct LayoutResolution {
    std::filesystem::path root;
    LayoutDecision decision = LayoutDecision::UseNew;
};

/// Locates the data root for reading. Never touches the filesystem.
[[nodiscard]] LayoutResolution locatePinyinRoot(const std::filesystem::path &dataHome);
[[nodiscard]] LayoutResolution locatePinyinRootFromEnvironment();

/// Result of an explicit migration attempt.
struct MigrationResult {
    bool migrated = false;
    /// Non-empty when a rename was attempted and failed, or was deferred.
    std::string detail;
    std::filesystem::path root;
};

/// Renames `rime/` to `pinyin/` when that is the only directory present.
///
/// Idempotent, and deliberately conservative:
/// - never merges: when both exist, the new one wins and the legacy one is left
///   untouched for the user to inspect;
/// - never renames while a leveldb in `rime/user` is locked by a live process,
///   because two writers on one database corrupt it (observed in practice);
/// - never deletes anything.
[[nodiscard]] MigrationResult migrateLegacyLayout(const std::filesystem::path &dataHome);
[[nodiscard]] MigrationResult migrateLegacyLayoutFromEnvironment();

/// True when the user data directory already contains compiled schema artifacts.
///
/// librime writes `<schema>.prism.bin` / `<schema>.table.bin` into a `build`
/// subdirectory (or, in older deployments, directly into the user directory);
/// their absence means a deployment is required before the engine can produce
/// candidates.
[[nodiscard]] bool hasCompiledArtifacts(const Paths &paths);

} // namespace superior::pinyin
