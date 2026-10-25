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

/// What to do about the pre-rename layout (`…/superior-ime/rime`).
///
/// Split out as a pure decision so it can be tested without a filesystem, and so
/// the fallback is explicit: if the rename fails we must keep using the old
/// directory rather than starting with no data at all.
enum class LayoutDecision {
    /// Neither directory exists (fresh install) or the new one already does.
    UseNew,
    /// Only the legacy directory exists; try to rename it.
    RenameLegacy,
    /// The rename failed; keep using the legacy directory so typing still works.
    UseLegacyFallback,
};

[[nodiscard]] LayoutDecision decideLayout(bool newExists, bool legacyExists) noexcept;

/// Result of actually resolving the data root, including a migration attempt.
struct LayoutResolution {
    std::filesystem::path root;
    LayoutDecision decision = LayoutDecision::UseNew;
    /// True when a rename actually happened, for logging.
    bool migrated = false;
    /// Non-empty when the rename was attempted and failed.
    std::string detail;
};

/// Resolves the data root, renaming `rime/` to `pinyin/` when that is the only
/// directory present. Idempotent; safe to call on every start.
[[nodiscard]] LayoutResolution resolvePinyinRoot(const std::filesystem::path &dataHome);

/// Same as `resolvePinyinRoot(Paths::fromEnvironment())`, exposed for callers
/// that only have a `Paths`.
[[nodiscard]] LayoutResolution resolvePinyinRootFromEnvironment();

/// True when the user data directory already contains compiled schema artifacts.
///
/// librime writes `<schema>.prism.bin` / `<schema>.table.bin` into a `build`
/// subdirectory (or, in older deployments, directly into the user directory);
/// their absence means a deployment is required before the engine can produce
/// candidates.
[[nodiscard]] bool hasCompiledArtifacts(const Paths &paths);

} // namespace superior::pinyin
