// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"
#include "superior/api/Scene.hpp"

namespace superior::api {

/// One candidate exactly as the capability produced it.
///
/// No id: ids are assigned by `core::CandidateModel`, which owns the
/// generation. A capability that tried to invent ids would re-introduce the
/// "position as identity" confusion that `CandidateId` exists to remove.
///
/// `source` tells the pipeline how to commit this row:
/// - `Engine` selects `engineRow` in the language engine and takes its commit;
/// - `Learned` / `Cloud` commit `text` directly.
///
/// `engineRow` is the row in the engine's **current page**, not the row in the
/// composed page. A learned word or a cloud word may be inserted above it; the
/// engine row must not shift when that happens.
struct CandidateFact {
    enum class Source : std::uint8_t {
        Engine,
        Learned,
        Cloud,
    };

    std::string text;
    std::string label;
    std::string comment;
    Source source = Source::Engine;
    std::uint32_t engineRow = 0;
};

/// Everything the active capability contributes to one frame.
///
/// This is deliberately *not* a host control-tree projection. The capability
/// owns the composition; the host only presents it. A Windows TSF or Android
/// host has no `InputPanel`, so the composition cannot be derived from one.
struct CompositionFact {
    bool visible = false;
    std::string preedit;
    /// Byte offset into `preedit`.
    std::size_t preeditCursor = 0;
    std::vector<TextSegment> preeditRuns;

    std::vector<CandidateFact> candidates;
    /// 1-based, as the engine reports it; 0 means "unknown".
    int pageIndex = 0;
    /// 0 means "unknown": librime only reports whether this is the last page.
    int pageCount = 0;
    /// Index into `candidates` of the highlighted entry, or -1.
    int highlighted = -1;
    bool vertical = false;

    /// True when there is something to show, even without candidates.
    bool composing = false;
    /// Engine/schema identifier, shown in the panel's mode field.
    std::string engineId;
};

/// Everything the *host* contributes: where the cursor is, which application is
/// focused, which capabilities the client announced.
///
/// The decision layer may read these, but must never branch on `program` for
/// behaviour other than an explicit per-application setting.
/// Product switches the pinyin capability needs. Pure data, filled by the
/// runtime from `settings.json`; the capability never reads the file itself.
struct PipelineOptions {
    bool cloudEnabled = false;
    int cloudMinLength = 2;
    int cloudIndex = 2;
    std::string cloudBackend = "auto";
};

struct HostFacts {
    Rect cursorRect;
    bool cursorValid = false;
    double scale = 1.0;
    std::string program;
    std::string inputMethod;
    std::vector<StatusItem> statusItems;

    // Client capability bits. Effects use them to pick an implementation; the
    // policy uses them to choose between two correct behaviours.
    bool surroundingText = false;
    bool clientPreedit = false;
    bool password = false;
    bool focused = true;
};

} // namespace superior::api
