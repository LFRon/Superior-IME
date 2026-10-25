// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "superior/skin/Diagnostic.hpp"
#include "superior/skin/Types.hpp"

/// `superior.skin/1` loader.
///
/// This module is deliberately free of Qt, image decoding and platform code:
/// it turns a package directory into an immutable, validated SkinSpec plus
/// stable diagnostics. Pixels stay on disk until the renderer asks for them.
namespace superior::skin {

struct LoadOptions {
    /// Warnings become errors. Use for CI and `skin-lint --strict`.
    bool strict = false;

    /// Hash asset contents into `SkinSpec::hash`. Disable only for very large
    /// packages during interactive editing.
    bool hashAssets = true;
};

struct LoadResult {
    std::optional<SkinSpec> spec;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] std::string summary() const;
};

/// Loads `<root>/skin.json` and every asset it references.
///
/// A package is only considered valid when no Error diagnostic was emitted.
/// With `strict`, warnings are promoted to errors.
[[nodiscard]] LoadResult loadSkin(const std::filesystem::path &root,
                                  const LoadOptions &options = {});

/// Discovers skins directly below `root` (each subdirectory with a skin.json).
/// Invalid packages are listed with `valid = false`; they never throw.
[[nodiscard]] std::vector<SkinSummary> scanSkins(const std::filesystem::path &root);

/// FNV-1a 64 over a byte range. Used for asset fingerprints and the spec hash;
/// not a cryptographic hash and not intended as one.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view bytes,
                                   std::uint64_t seed = 14695981039346656037ULL) noexcept;

/// Canonical JSON projection of a normalized spec. Deterministic: suitable for
/// diffing, golden tests and tooling output. Not a pretty printer.
[[nodiscard]] std::string toCanonicalJson(const SkinSpec &spec);

} // namespace superior::skin
