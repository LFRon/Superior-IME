// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "superior/rime/Types.hpp"

namespace superior::rime {

/// `$XDG_DATA_HOME/superior-ime/rime` (or `~/.local/share/...` when unset).
///
/// Separate from Fcitx5's Rime directory on purpose: our schemas, our `userdb`,
/// and no chance of overwriting a user's existing setup.
[[nodiscard]] std::filesystem::path defaultRimeRoot();

/// True when the user data directory already contains compiled schema artifacts.
///
/// librime writes `<schema>.prism.bin` / `<schema>.table.bin` next to the
/// dictionaries; their absence means a deployment is required before the engine
/// can produce candidates.
[[nodiscard]] bool hasCompiledArtifacts(const Paths &paths);

} // namespace superior::rime
