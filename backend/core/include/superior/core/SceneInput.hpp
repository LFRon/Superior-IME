// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "superior/api/Content.hpp"
#include "superior/api/Scene.hpp"

namespace superior::core {

class ImeSession;

/// A styled run inside the pre-edit string.
struct TextRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string attribute;
};

/// One candidate exactly as the language engine produced it.
///
/// No id: ids are assigned by `CandidateModel`, which owns the generation. An
/// engine that tried to invent ids would be re-introducing the position/id
/// confusion this class exists to remove.
struct EngineCandidate {
    std::string label;
    std::string text;
    std::string comment;
};

/// Everything the *language engine* contributes to one frame.
///
/// This is deliberately separate from `HostFacts`: the previous design read a
/// single `PanelFacts` built from Fcitx5's `InputPanel`, which meant the scene
/// could not be produced without Fcitx5. A Windows TSF or Android backend has no
/// `InputPanel`, so the two kinds of facts had to be split.
struct EngineFacts {
    std::string preedit;
    /// Byte offset into `preedit`.
    std::size_t preeditCursor = 0;
    std::vector<TextRun> preeditRuns;

    std::vector<EngineCandidate> candidates;
    /// 1-based, as the engine reports it; 0 means "unknown".
    int pageIndex = 0;
    /// 0 means "unknown": librime only reports whether this is the last page.
    int pageCount = 0;
    /// Index of the highlighted candidate, or -1.
    int highlighted = -1;
    bool vertical = false;

    /// True when there is something to show, even without candidates (L-4).
    bool composing = false;
    /// Engine/schema identifier, shown in the panel's mode field.
    std::string engineId;
};

/// Everything the *host* contributes: where the cursor is, which application is
/// focused, which capabilities the client announced.
///
/// The decision layer may read these, but must never branch on `program` for
/// behaviour other than an explicit per-application setting.
struct HostFacts {
    api::Rect cursorRect;
    bool cursorValid = false;
    double scale = 1.0;
    std::string program;
    std::string inputMethod;
    std::vector<api::StatusItem> statusItems;

    // Client capability bits. Effects use them to pick an implementation; the
    // policy uses them to choose between two correct behaviours (see Backspace).
    bool surroundingText = false;
    bool clientPreedit = false;
    bool password = false;
};

/// Folds one engine projection plus one host projection into the session.
///
/// This is the whole of the old `PanelMapper`, minus the parts that belonged to
/// the platform: it never touches Fcitx5, a window or a clock.
void updateSession(const EngineFacts &engine, const HostFacts &host, ImeSession &session,
                   std::string_view skinId = "org.superior.default",
                   api::SkinGeneration generation = 1);

} // namespace superior::core
