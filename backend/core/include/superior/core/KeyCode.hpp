// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "superior/api/Key.hpp"

namespace superior::core {

/// The key model is part of the contract now: hosts, the runtime and every
/// capability share it, so it cannot live in `core` without making every
/// capability depend on the decision layer.
using api::KeyCode;

} // namespace superior::core
