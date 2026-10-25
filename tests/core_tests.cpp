// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/core/CandidateModel.hpp"
#include "superior/core/ImeSession.hpp"
#include "superior/core/MotionTimeline.hpp"

namespace {

using superior::api::Candidate;
using superior::api::CandidateId;
using superior::api::SessionKey;
using superior::core::CandidateModel;
using superior::core::ImeSession;
using superior::core::MotionTimeline;
using superior::skin::MotionSpec;

std::vector<Candidate> makeCandidates(int count) {
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        Candidate candidate;
        candidate.id = static_cast<CandidateId>(i);
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
    CHECK(model.selectedId() == 1);

    const auto firstPage = model.page();
    CHECK(firstPage.items.size() == 5);
    CHECK(firstPage.hasPrevious == false);
    CHECK(firstPage.hasNext == true);

    CHECK(model.nextPage());
    CHECK(model.pageIndex() == 1);
    CHECK(model.selectedId() == 6);
    CHECK(model.page().hasPrevious);
    CHECK(model.page().hasNext);

    CHECK(model.previousPage());
    CHECK(model.pageIndex() == 0);
    CHECK(model.selectedId() == 1);
    CHECK(!model.previousPage());

    CHECK(model.select(7));
    CHECK(model.selectedIndex() == 6);
    CHECK(model.pageIndex() == 1);

    CHECK(model.selectNext());
    CHECK(model.selectedId() == 8);
    CHECK(model.selectPrevious());
    CHECK(model.selectedId() == 7);
}

void testCandidateBoundaries() {
    CandidateModel model;
    model.set(makeCandidates(3), 5);
    CHECK(!model.nextPage());
    CHECK(model.select(3));
    CHECK(!model.selectNext());
    CHECK(!model.select(99));
    CHECK(model.selectedId() == 3);

    model.clear();
    CHECK(model.empty());
    CHECK(model.pageCount() == 0);
    CHECK(model.page().items.empty());
    CHECK(!model.selectNext());
}

void testCandidateDuplicateAndZeroIds() {
    CandidateModel model;
    std::vector<Candidate> candidates{
        Candidate{1, "1", "a", ""},
        Candidate{1, "1", "duplicate", ""},
        Candidate{0, "", "zero", ""},
        Candidate{2, "2", "b", ""},
    };
    model.set(std::move(candidates), 7);
    CHECK(model.size() == 2);
    CHECK(model.find(1) != nullptr);
    CHECK(model.find(0) == nullptr);
    CHECK(model.find(2) != nullptr);
}

void testSelectionSurvivesListUpdate() {
    CandidateModel model;
    model.set(makeCandidates(5), 2);
    CHECK(model.select(4));

    auto updated = makeCandidates(6);
    updated[3].text = "updated";
    model.set(updated, 2);
    CHECK(model.selectedId() == 4);
    CHECK(model.pageIndex() == 1);

    auto without = makeCandidates(2);
    model.set(without, 2);
    CHECK(model.selectedId() == 1);
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

MotionSpec tweenSpec(double durationMs, std::string easing) {
    MotionSpec spec;
    spec.kind = MotionSpec::Kind::Tween;
    spec.tween.durationMs = durationMs;
    spec.tween.easing = std::move(easing);
    return spec;
}

MotionSpec springSpec(double stiffness, double damping, double mass) {
    MotionSpec spec;
    spec.kind = MotionSpec::Kind::Spring;
    spec.spring.stiffness = stiffness;
    spec.spring.damping = damping;
    spec.spring.mass = mass;
    return spec;
}

void testTweenIsDeterministic() {
    MotionTimeline timeline;
    timeline.start(tweenSpec(100.0, "linear"), 1000);

    auto sample = timeline.sample(1000);
    CHECK(superior::test::near(sample.value, 0.0));
    CHECK(!sample.finished);

    sample = timeline.sample(1050);
    CHECK(superior::test::near(sample.value, 0.5, 1e-12));
    CHECK(!sample.finished);

    sample = timeline.sample(1100);
    CHECK(superior::test::near(sample.value, 1.0));
    CHECK(sample.finished);

    // Sampling the same instant twice must not depend on internal history.
    timeline.start(tweenSpec(100.0, "linear"), 1000);
    CHECK(superior::test::near(timeline.sample(1075).value, 0.75, 1e-12));
    CHECK(superior::test::near(timeline.sample(1075).value, 0.75, 1e-12));
}

void testTweenEasing() {
    CHECK(superior::test::near(superior::core::applyEasing(superior::core::Easing::CubicOut, 0.5),
                               0.875));
    CHECK(superior::test::near(superior::core::applyEasing(superior::core::Easing::CubicIn, 0.5),
                               0.125));
    CHECK(superior::test::near(superior::core::applyEasing(superior::core::Easing::Linear, 0.5),
                               0.5));
    CHECK(superior::core::easingFromName("nope") == superior::core::Easing::CubicOut);
}

void testTweenBeforeStartAndZeroDuration() {
    MotionTimeline timeline;
    timeline.start(tweenSpec(100.0, "linear"), 2000, 0.25, 1.0);
    const auto before = timeline.sample(1999);
    CHECK(superior::test::near(before.value, 0.25));
    CHECK(!before.finished);

    MotionTimeline instant;
    instant.start(tweenSpec(0.0, "linear"), 10, 0.0, 1.0);
    CHECK(!instant.active());
    const auto done = instant.sample(10);
    CHECK(done.finished);
    CHECK(superior::test::near(done.value, 1.0));
}

void testSpringSettles() {
    MotionTimeline timeline;
    timeline.start(springSpec(420.0, 34.0, 1.0), 500);
    const auto start = timeline.sample(500);
    CHECK(superior::test::near(start.value, 0.0, 1e-9));
    CHECK(!start.finished);

    const auto settled = timeline.sample(5000);
    CHECK(settled.finished);
    CHECK(superior::test::near(settled.value, 1.0));

    timeline.start(springSpec(420.0, 34.0, 1.0), 500);
    const auto first = timeline.sample(620);
    const auto second = timeline.sample(620);
    CHECK(superior::test::near(first.value, second.value));
    CHECK(first.finished == second.finished);
}

void testSpringDegenerateAndCancel() {
    MotionTimeline timeline;
    timeline.start(springSpec(0.0, 0.0, 1.0), 0, 0.0, 2.0);
    const auto immediate = timeline.sample(0);
    CHECK(immediate.finished);
    CHECK(superior::test::near(immediate.value, 2.0));

    timeline.start(springSpec(420.0, 34.0, 1.0), 0, 0.0, 1.0);
    CHECK(timeline.active());
    timeline.cancel();
    CHECK(!timeline.active());
    const auto cancelled = timeline.sample(10);
    CHECK(cancelled.finished);
    CHECK(superior::test::near(cancelled.value, 1.0));
}

void testSessionSnapshot() {
    ImeSession session(SessionKey{7, 42});
    CHECK(session.key().epoch == 7);
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

void testSessionCommit() {
    ImeSession session(SessionKey{1, 1});
    session.show();
    session.setPreedit(superior::api::Preedit{"ni", 2, {}});
    session.candidates().set(makeCandidates(3), 7);
    session.setTransition("panel.show", 100, 0.0, 1.0);

    CHECK(!session.commit(99).has_value());
    CHECK(session.snapshot(1).panel.visible);

    const auto committed = session.commit(2);
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
    testCandidateDuplicateAndZeroIds();
    testSelectionSurvivesListUpdate();
    testTakeSelected();

    testTweenIsDeterministic();
    testTweenEasing();
    testTweenBeforeStartAndZeroDuration();
    testSpringSettles();
    testSpringDegenerateAndCancel();

    testSessionSnapshot();
    testSessionCommit();
    testSessionStatusAndCommitSelected();

    return superior::test::finish("core tests");
}
