// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "superior/skin/Types.hpp"

namespace superior::skin::detail {

/// Canonical JSON projection of the *normalized* spec: unknown input keys are
/// gone, ordering is deterministic, defaults are materialized. Used for the
/// content hash and for `skin-lint --print-spec`.
[[nodiscard]] std::string specToCanonicalJson(const SkinSpec &spec);

/// FNV-1a 64 over the canonical JSON plus referenced asset fingerprints.
[[nodiscard]] std::string computeSpecHash(const SkinSpec &spec);

} // namespace superior::skin::detail
