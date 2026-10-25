// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/SceneInput.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "superior/core/ImeSession.hpp"

namespace superior::core {

namespace {

constexpr int kMinPageSize = 1;
constexpr int kMaxPageSize = 32;

} // namespace

void updateSession(const EngineFacts &engine, const HostFacts &host, ImeSession &session,
                   std::string_view skinId, api::SkinGeneration generation) {
    session.setSkin(std::string(skinId), generation);
    session.setMode(engine.engineId);
    session.setStyle(engine.vertical ? "v1" : "h1");
    session.setStatus(host.statusItems);
    session.setStatusPlacement(api::StatusPlacement::BottomRight);
    session.setAnchor(api::Anchor{host.cursorRect, 0.0, 0.0, host.cursorValid});

    if (!engine.composing) {
        session.candidates().clear();
        session.clearPreedit();
        session.clearPageInfo();
        session.hide();
        return;
    }

    // L-4: a composition is visible even before the engine produces a candidate.
    // "Nothing to show" and "nothing to type" are different states, and a user
    // who sees no reaction assumes the input method is broken.
    session.show();

    api::Preedit preedit;
    preedit.text = engine.preedit;
    preedit.cursor = engine.preeditCursor;
    preedit.segments.reserve(engine.preeditRuns.size());
    for (const auto &run : engine.preeditRuns) {
        preedit.segments.push_back(api::TextSegment{run.begin, run.end, run.attribute});
    }
    session.setPreedit(std::move(preedit));

    std::vector<api::Candidate> candidates;
    candidates.reserve(engine.candidates.size());
    for (const auto &entry : engine.candidates) {
        api::Candidate candidate;
        candidate.id = 0; // assigned by CandidateModel
        candidate.label = entry.label;
        candidate.text = entry.text;
        candidate.comment = entry.comment;
        candidates.push_back(std::move(candidate));
    }

    const int pageSize = std::clamp(static_cast<int>(engine.candidates.size()), kMinPageSize,
                                    kMaxPageSize);
    session.candidates().set(std::move(candidates), pageSize);

    if (engine.pageCount > 0) {
        session.setPageInfo(engine.pageIndex, engine.pageCount);
    } else {
        session.clearPageInfo();
    }
    if (engine.highlighted >= 0) {
        session.candidates().selectIndex(engine.highlighted);
    }
}

} // namespace superior::core
