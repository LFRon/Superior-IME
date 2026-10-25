// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/core/CandidateModel.hpp"
#include "superior/core/ImeSession.hpp"

namespace {

using superior::api::Candidate;
using superior::api::CandidateId;
using superior::api::SessionKey;
using superior::core::CandidateModel;
using superior::core::ImeSession;

std::vector<Candidate> makeCandidates(int count) {
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        Candidate candidate;
        candidate.text = "c" + std::to_string(i);
        candidate.label = std::to_string(i);
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

void testCandidatePagingAndSelection() {
    CandidateModel model;
    model.set(makeCandidates(12), 5);

    CHECK(model.pageCount() == 3);
    CHECK(model.pageIndex() == 0);
    CHECK(model.selectedIndex() == 0);
    CHECK(model.selectedId() == model.idAt(0));

    const auto firstPage = model.page();
    CHECK(firstPage.items.size() == 5);
    CHECK(firstPage.hasPrevious == false);
    CHECK(firstPage.hasNext == true);

    CHECK(model.nextPage());
    CHECK(model.pageIndex() == 1);
    CHECK(model.selectedIndex() == 5);
    CHECK(model.page().hasPrevious);
    CHECK(model.page().hasNext);

    CHECK(model.previousPage());
    CHECK(model.pageIndex() == 0);
    CHECK(model.selectedIndex() == 0);
    CHECK(!model.previousPage());

    CHECK(model.select(model.idAt(6)));
    CHECK(model.selectedIndex() == 6);
    CHECK(model.pageIndex() == 1);

    CHECK(model.selectNext());
    CHECK(model.selectedIndex() == 7);
    CHECK(model.selectPrevious());
    CHECK(model.selectedIndex() == 6);
}

void testCandidateBoundaries() {
    CandidateModel model;
    model.set(makeCandidates(3), 5);
    CHECK(!model.nextPage());
    CHECK(model.select(model.idAt(2)));
    CHECK(!model.selectNext());
    CHECK(!model.select(99));
    CHECK(model.selectedIndex() == 2);

    model.clear();
    CHECK(model.empty());
    CHECK(model.pageCount() == 0);
    CHECK(model.page().items.empty());
    CHECK(!model.selectNext());
    CHECK(model.idAt(0) == 0);
}

/// Ids must never collide across pages, which is what the old `index + 1`
/// scheme did: page 2 restarted at 1 and aliased page 1.
void testIdsAreUniqueAcrossPages() {
    CandidateModel model;
    model.set(makeCandidates(12), 5);
    std::vector<CandidateId> ids;
    for (std::size_t i = 0; i < model.size(); ++i) {
        ids.push_back(model.idAt(i));
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(ids[i] != 0);
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            CHECK(ids[i] != ids[j]);
        }
    }
}

/// A list rebuild invalidates earlier ids instead of silently reinterpreting
/// them: this is what makes an in-flight click safe.
void testStaleIdIsRejected() {
    CandidateModel model;
    model.set(makeCandidates(5), 5);
    const auto stale = model.idAt(3);
    CHECK(model.select(stale));

    auto updated = makeCandidates(5);
    updated[0].text = "different";
    model.set(std::move(updated), 5);

    CHECK(!model.select(stale));
    CHECK(model.positionOf(stale) == std::nullopt);
    CHECK(model.positionOf(model.idAt(3)).has_value());

    // Same content (a repaint) keeps the generation, so a click still lands.
    model.set(makeCandidates(5), 5);
    const auto current = model.idAt(2);
    model.set(makeCandidates(5), 5);
    CHECK(model.positionOf(current).has_value());
}

void testSelectionSurvivesListUpdate() {
    CandidateModel model;
    model.set(makeCandidates(5), 2);
    CHECK(model.select(model.idAt(3)));

    auto updated = makeCandidates(6);
    updated[3].text = "updated";
    model.set(updated, 2);
    CHECK(model.selectedIndex() == 3);
    CHECK(model.pageIndex() == 1);

    auto shorter = makeCandidates(2);
    model.set(shorter, 2);
    CHECK(model.selectedIndex() == 0);
}

void testTakeSelected() {
    CandidateModel model;
    model.set(makeCandidates(2), 5);
    const auto text = model.takeSelected();
    CHECK(text.has_value());
    CHECK(*text == "c1");
    CHECK(model.empty());
    CHECK(!model.takeSelected().has_value());
}

void testSessionSnapshot() {
    ImeSession session(SessionKey{1, 42});
    CHECK(session.key().context == 42);

    auto snapshot = session.snapshot(1);
    CHECK(snapshot.frameId == 1);
    CHECK(!snapshot.panel.visible);

    session.show();
    session.setPreedit(superior::api::Preedit{"ni", 2, {}});
    session.candidates().set(makeCandidates(3), 7);
    session.setAnchor(superior::api::Anchor{superior::api::Rect{10, 20, 0, 18}, 0, 0, true});
    session.setSkin("org.test", 3);
    session.setStyle("h1");
    session.setMode("dark");

    snapshot = session.snapshot(2);
    CHECK(snapshot.frameId == 2);
    CHECK(snapshot.panel.visible);
    CHECK(snapshot.panel.preedit.text == "ni");
    CHECK(snapshot.panel.candidates.items.size() == 3);
    CHECK(snapshot.panel.skinId == "org.test");
    CHECK(snapshot.panel.skinGeneration == 3);
    CHECK(snapshot.panel.styleId == "h1");
    CHECK(snapshot.panel.mode == "dark");
    CHECK(snapshot.panel.anchor.valid);
    CHECK(!snapshot.status.visible);
}

/// L-4: a composition with no candidates is still visible. "Nothing to show"
/// and "nothing to type" are different states.
void testCompositionWithoutCandidatesIsVisible() {
    ImeSession session(SessionKey{1, 7});
    session.show();
    session.setPreedit(superior::api::Preedit{"y", 1, {}});
    const auto frame = session.snapshot(1);
    CHECK(frame.panel.visible);
    CHECK(frame.panel.candidates.items.empty());
}

void testSessionCommit() {
    ImeSession session(SessionKey{1, 1});
    session.show();
    session.setPreedit(superior::api::Preedit{"ni", 2, {}});
    session.candidates().set(makeCandidates(3), 7);
    session.setTransition("panel.show", 100, 0.0, 1.0);

    CHECK(!session.commit(99).has_value());
    CHECK(session.snapshot(1).panel.visible);

    const auto second = session.candidates().idAt(1);
    const auto committed = session.commit(second);
    CHECK(committed.has_value());
    CHECK(*committed == "c2");
    CHECK(session.candidates().empty());
    CHECK(session.preedit().text.empty());
    CHECK(!session.visible());
    CHECK(!session.snapshot(2).transition.has_value());
}

void testSessionStatusAndCommitSelected() {
    ImeSession session(SessionKey{1, 2});
    session.setStatus({superior::api::StatusItem{"ime", "CN", "", true, true}});
    session.setStatusPlacement(superior::api::StatusPlacement::TopLeft);
    auto snapshot = session.snapshot(3);
    CHECK(snapshot.status.visible);
    CHECK(snapshot.status.items.size() == 1);
    CHECK(snapshot.status.placement == superior::api::StatusPlacement::TopLeft);

    session.show();
    session.candidates().set(makeCandidates(2), 7);
    const auto committed = session.commitSelected();
    CHECK(committed.has_value());
    CHECK(*committed == "c1");
}

} // namespace

int main() {
    testCandidatePagingAndSelection();
    testCandidateBoundaries();
    testIdsAreUniqueAcrossPages();
    testStaleIdIsRejected();
    testSelectionSurvivesListUpdate();
    testTakeSelected();

    testSessionSnapshot();
    testCompositionWithoutCandidatesIsVisible();
    testSessionCommit();
    testSessionStatusAndCommitSelected();

    return superior::test::finish("core tests");
}
