// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/CompositorPanel.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::compositorPlacesCandidatePanel;
using superior::ui::kCandidatePanelAtom;

/// Only Treeland implements the candidate-panel channel; claiming it elsewhere
/// would replace our own caret placement with nothing at all.
void testDetection() {
    CHECK(compositorPlacesCandidatePanel("Treeland", "DDE"));
    CHECK(compositorPlacesCandidatePanel("treeland", ""));
    CHECK(compositorPlacesCandidatePanel("", "Treeland"));
    // Deepin's X11 session and every other desktop keep our own placement.
    CHECK(!compositorPlacesCandidatePanel("Deepin", "DDE"));
    CHECK(!compositorPlacesCandidatePanel("", "GNOME"));
    CHECK(!compositorPlacesCandidatePanel("", ""));
    // A substring from an unrelated name must not match.
    CHECK(!compositorPlacesCandidatePanel("", "KDE"));
}

/// The property name is a published convention in Treeland's source
/// (`IMCandidatePanelManager::IM_CANDIDATE_PANEL`); a typo here fails silently,
/// so it is pinned.
void testAtomName() {
    CHECK(kCandidatePanelAtom == "_DEEPIN_IM_CANDIDATE_PANEL");
}

} // namespace

int main() {
    testDetection();
    testAtomName();
    return superior::test::finish("compositor panel tests");
}
