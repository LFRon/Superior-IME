// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "superior/api/Content.hpp"
#include "superior/api/Scene.hpp"

namespace superior::fcitx5 {

/// Platform-neutral view of what Fcitx5 is composing right now.
///
/// This header must not include any Fcitx5 header: it is the testable boundary
/// between the framework adapter and the pure mapper.
struct TextRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string attribute;
};

struct CandidateEntry {
    api::CandidateId id = 0;
    std::string label;
    std::string text;
    std::string comment;
};

struct PanelFacts {
    std::string preedit;
    std::size_t preeditCursor = 0;
    /// True when `preedit` came from the client preedit (engine only filled
    /// that field). Observability only.
    bool preeditFromClient = false;
    std::vector<TextRun> preeditRuns;

    std::vector<CandidateEntry> candidates;
    int pageSize = 7;
    /// Page reported by Fcitx5 itself (1-based index in the panel, 0 = unknown).
    int pageIndex = 0;
    int pageCount = 0;
    int cursorIndex = -1;
    bool vertical = false;

    api::Rect cursorRect;
    bool cursorValid = false;
    double scale = 1.0;

    std::string inputMethod;
    std::string program;
    std::vector<api::StatusItem> statusItems;

    /// True when there is something to show in the candidate panel.
    bool composing = false;
};

} // namespace superior::fcitx5
