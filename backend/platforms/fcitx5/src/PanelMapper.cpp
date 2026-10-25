// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/PanelMapper.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace superior::fcitx5 {

namespace {
constexpr int kMinPageSize = 1;
constexpr int kMaxPageSize = 32;
} // namespace

void PanelMapper::apply(const PanelFacts &facts, core::ImeSession &session,
                        std::string_view skinId, api::SkinGeneration skinGeneration) {
    session.setSkin(std::string(skinId), skinGeneration);
    session.setMode(facts.inputMethod);
    session.setStyle(facts.vertical ? "v1" : "h1");
    session.setStatus(facts.statusItems);
    session.setStatusPlacement(api::StatusPlacement::BottomRight);
    session.setAnchor(api::Anchor{facts.cursorRect, 0.0, 0.0, facts.cursorValid});

    if (!facts.composing) {
        session.candidates().clear();
        session.clearPreedit();
        session.clearPageInfo();
        session.hide();
        return;
    }

    session.show();

    api::Preedit preedit;
    preedit.text = facts.preedit;
    preedit.cursor = facts.preeditCursor;
    for (const auto &run : facts.preeditRuns) {
        preedit.segments.push_back(api::TextSegment{run.begin, run.end, run.attribute});
    }
    session.setPreedit(std::move(preedit));

    std::vector<api::Candidate> candidates;
    candidates.reserve(facts.candidates.size());
    for (const auto &entry : facts.candidates) {
        api::Candidate candidate;
        candidate.id = entry.id;
        candidate.label = entry.label;
        candidate.text = entry.text;
        candidate.comment = entry.comment;
        candidates.push_back(std::move(candidate));
    }
    const int pageSize = std::clamp(facts.pageSize, kMinPageSize, kMaxPageSize);
    session.candidates().set(std::move(candidates), pageSize);
    if (facts.pageCount > 0) {
        session.setPageInfo(facts.pageIndex, facts.pageCount);
    } else {
        session.clearPageInfo();
    }
    if (facts.cursorIndex >= 0) {
        session.candidates().selectIndex(facts.cursorIndex);
    }
}

} // namespace superior::fcitx5
