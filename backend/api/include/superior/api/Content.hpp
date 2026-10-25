// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "superior/api/Ids.hpp"

namespace superior::api {

/// A run of text with an optional visual attribute. Attributes are opaque
/// style names resolved by the renderer (for example "underline" or "error").
struct TextSegment {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string attribute;
};

/// Pre-edit text owned by the engine. `cursor` is a byte offset in UTF-8.
struct Preedit {
    std::string text;
    std::size_t cursor = 0;
    std::vector<TextSegment> segments;
};

/// One candidate as presented to the user. `label` is optional (for example
/// "1." or a pinyin hint); the renderer must not invent one.
struct Candidate {
    CandidateId id = 0;
    std::string label;
    std::string text;
    std::string comment;
};

struct CandidatePage {
    std::vector<Candidate> items;
    /// Zero-based page index; the renderer adds 1 for display. Fcitx5 reports
    /// 1-based pages, the mapper converts once here so no layer has to guess.
    int pageIndex = 0;
    int pageCount = 0;
    bool hasPrevious = false;
    bool hasNext = false;
};

/// One status-area control. `action` is a semantic action name (for example
/// "toggle-mode"), never a pixel position.
struct StatusItem {
    std::string id;
    std::string label;
    std::string icon;
    bool active = false;
    bool enabled = true;
};

} // namespace superior::api
