// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

#include "superior/api/Composition.hpp"

namespace superior::core {

class ImeSession;

/// Backwards-compatible names for the contract types. The facts moved to
/// `api` so a host can fill them without including the decision layer; these
/// aliases keep the folding code and existing tests reading naturally.
using TextRun = api::TextSegment;
using EngineCandidate = api::CandidateFact;
using EngineFacts = api::CompositionFact;
using HostFacts = api::HostFacts;

/// Folds one capability projection plus one host projection into the session.
///
/// This is the whole of the old `PanelMapper`, minus the parts that belonged to
/// the platform: it never touches Fcitx5, a window or a clock.
void updateSession(const EngineFacts &engine, const HostFacts &host, ImeSession &session,
                   std::string_view skinId = "org.superior.default",
                   api::SkinGeneration generation = 1);

} // namespace superior::core
