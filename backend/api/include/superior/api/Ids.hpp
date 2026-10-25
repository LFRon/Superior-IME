// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace superior::api {

/// Monotonic epoch of a process/session. A restarted agent or a recycled
/// input context must use a new epoch so stale async results are rejected.
using Epoch = std::uint64_t;

/// Stable identity of one input context. Never reuse a live id.
using ContextId = std::uint64_t;

/// Identifies one logical input session (one focused text field).
struct SessionKey {
    Epoch epoch = 0;
    ContextId context = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return epoch != 0; }

    friend constexpr bool operator==(const SessionKey &, const SessionKey &) = default;
};

/// Monotonic frame counter, owned by the producer of a scene frame.
using FrameId = std::uint64_t;

/// Candidate identity, unique within its session. UI commands refer to this,
/// never to a positional index, so an in-flight click cannot select a different
/// candidate after a list update.
using CandidateId = std::uint32_t;

/// Content generation of a skin. The UI must drop frames carrying a generation
/// different from the one it is currently rendering.
using SkinGeneration = std::uint64_t;

} // namespace superior::api
