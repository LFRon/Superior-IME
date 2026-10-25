// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/runtime/LatestOperation.hpp"
#include "superior/runtime/SerialPump.hpp"
#include "superior/runtime/SessionRuntime.hpp"

namespace {

using superior::api::Candidate;
using superior::api::SessionKey;
using superior::api::UiSceneFrame;
using superior::runtime::LatestOperation;
using superior::runtime::SerialPump;
using superior::runtime::SessionRuntime;

class FakeScheduler : public superior::api::Scheduler {
public:
    bool owner = true;

    struct FrameRequest {
        std::uint64_t id = 0;
        std::function<void(std::uint64_t)> callback;
    };

    std::vector<FrameRequest> frames;
    std::vector<std::function<void()>> posted;
    std::vector<std::uint64_t> cancelled;
    std::uint64_t nextId = 1;

    [[nodiscard]] bool onOwnerThread() const noexcept override { return owner; }

    void post(std::function<void()> work) override { posted.push_back(std::move(work)); }

    std::uint64_t after(std::uint64_t, std::function<void()>) override { return 0; }

    void cancel(std::uint64_t timerId) override { cancelled.push_back(timerId); }

    std::uint64_t nextFrame(std::function<void(std::uint64_t)> work) override {
        const std::uint64_t id = nextId++;
        frames.push_back(FrameRequest{id, std::move(work)});
        return id;
    }

    void fireFrames(std::uint64_t timeMs) {
        auto pending = std::move(frames);
        frames.clear();
        for (auto &request : pending) {
            request.callback(timeMs);
        }
    }

    void runPosted() {
        auto pending = std::move(posted);
        posted.clear();
        for (auto &work : pending) {
            work();
        }
    }
};

class FakePresenter : public superior::api::ScenePresenter {
public:
    std::vector<UiSceneFrame> frames;

    void present(const UiSceneFrame &frame) override { frames.push_back(frame); }
    void setHitMap(const SessionKey &, const superior::api::HitMap &) override {}
    void setVisible(const SessionKey &, bool) override {}
};

std::vector<Candidate> makeCandidates(int count) {
    std::vector<Candidate> candidates;
    for (int i = 1; i <= count; ++i) {
        candidates.push_back(Candidate{static_cast<superior::api::CandidateId>(i),
                                       std::to_string(i), "c" + std::to_string(i), ""});
    }
    return candidates;
}

void testLatestOperationSupersede() {
    std::vector<std::string> cancelled;
    LatestOperation<std::string> operation(
        [&cancelled](std::string &resource) { cancelled.push_back(resource); });

    const auto first = operation.begin();
    CHECK(operation.owns(first));
    operation.attach(first, "r1");

    const auto second = operation.begin();
    CHECK(cancelled.size() == 1);
    CHECK(cancelled.front() == "r1");
    CHECK(!operation.owns(first));
    CHECK(operation.owns(second));
    CHECK(!operation.complete(first));
    CHECK(operation.complete(second));
    CHECK(!operation.active());
}

void testLatestOperationCompleteBeforeAttach() {
    std::vector<std::string> cancelled;
    LatestOperation<std::string> operation(
        [&cancelled](std::string &resource) { cancelled.push_back(resource); });

    const auto ticket = operation.begin();
    CHECK(operation.complete(ticket));
    operation.attach(ticket, "late");
    CHECK(cancelled.empty());
}

void testLatestOperationCancelBeforeAttach() {
    std::vector<std::string> cancelled;
    LatestOperation<std::string> operation(
        [&cancelled](std::string &resource) { cancelled.push_back(resource); });

    const auto ticket = operation.begin();
    operation.begin();
    operation.attach(ticket, "late");
    CHECK(cancelled.size() == 1);
    CHECK(cancelled.front() == "late");
}

void testLatestOperationCancelActive() {
    std::vector<std::string> cancelled;
    LatestOperation<std::string> operation(
        [&cancelled](std::string &resource) { cancelled.push_back(resource); });

    const auto ticket = operation.begin();
    operation.attach(ticket, "r");
    operation.cancel();
    CHECK(cancelled.size() == 1);
    CHECK(!operation.active());
    CHECK(!operation.complete(ticket));
}

void testSerialPumpOrderAndReentrancy() {
    FakeScheduler scheduler;
    SerialPump pump(scheduler);

    std::vector<int> order;
    pump.post([&] {
        order.push_back(1);
        pump.post([&] { order.push_back(3); });
        order.push_back(2);
    });
    pump.post([&] { order.push_back(4); });

    CHECK(order.size() == 4);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
    CHECK(order[3] == 4);
    CHECK(!pump.draining());
    CHECK(pump.pending() == 0);
}

void testSerialPumpOffOwner() {
    FakeScheduler scheduler;
    scheduler.owner = false;
    SerialPump pump(scheduler);

    bool ran = false;
    pump.post([&] { ran = true; });
    CHECK(!ran);
    CHECK(scheduler.posted.size() == 1);

    scheduler.owner = true;
    scheduler.runPosted();
    CHECK(ran);
}

void testSessionRuntimeCoalescesFrames() {
    FakeScheduler scheduler;
    FakePresenter presenter;
    SessionRuntime runtime(SessionKey{1, 10}, presenter, scheduler);

    runtime.start();
    CHECK(scheduler.frames.size() == 1);
    CHECK(presenter.frames.empty());

    runtime.show();
    runtime.setPreedit(superior::api::Preedit{"ni", 2, {}});
    runtime.setCandidates(makeCandidates(3), 7);
    CHECK(scheduler.frames.size() == 1);

    scheduler.fireFrames(100);

    CHECK(presenter.frames.size() == 1);
    const auto &frame = presenter.frames.front();
    CHECK(frame.frameId == 1);
    CHECK(frame.panel.visible);
    CHECK(frame.panel.preedit.text == "ni");
    CHECK(frame.panel.candidates.items.size() == 3);
    CHECK(frame.session.epoch == 1);
    CHECK(frame.session.context == 10);

    runtime.select(2);
    CHECK(scheduler.frames.size() == 1);
    scheduler.fireFrames(116);
    CHECK(presenter.frames.size() == 2);
    CHECK(presenter.frames.back().frameId == 2);
    CHECK(runtime.snapshot().panel.candidates.items.size() == 3);
}

void testSessionRuntimeCommitHides() {
    FakeScheduler scheduler;
    FakePresenter presenter;
    SessionRuntime runtime(SessionKey{2, 20}, presenter, scheduler);

    runtime.show();
    runtime.setPreedit(superior::api::Preedit{"ni", 2, {}});
    runtime.setCandidates(makeCandidates(2), 7);
    scheduler.fireFrames(0);

    const auto committed = runtime.commit(2);
    CHECK(committed.has_value());
    CHECK(*committed == "c2");
    scheduler.fireFrames(1);
    CHECK(!presenter.frames.back().panel.visible);
}

void testSessionRuntimeOffOwnerPosts() {
    FakeScheduler scheduler;
    FakePresenter presenter;
    SessionRuntime runtime(SessionKey{3, 30}, presenter, scheduler);

    scheduler.owner = false;
    runtime.show();
    CHECK(scheduler.posted.size() == 1);
    CHECK(scheduler.frames.empty());

    scheduler.owner = true;
    scheduler.runPosted();
    CHECK(scheduler.frames.size() == 1);
}

void testSessionRuntimeStopCancelsFrame() {
    FakeScheduler scheduler;
    FakePresenter presenter;
    SessionRuntime runtime(SessionKey{4, 40}, presenter, scheduler);

    runtime.start();
    CHECK(scheduler.frames.size() == 1);
    const std::uint64_t timer = scheduler.frames.front().id;

    runtime.stop();
    CHECK(scheduler.cancelled.size() == 1);
    CHECK(scheduler.cancelled.front() == timer);

    scheduler.fireFrames(0);
    CHECK(presenter.frames.empty());
}

} // namespace

int main() {
    testLatestOperationSupersede();
    testLatestOperationCompleteBeforeAttach();
    testLatestOperationCancelBeforeAttach();
    testLatestOperationCancelActive();

    testSerialPumpOrderAndReentrancy();
    testSerialPumpOffOwner();

    testSessionRuntimeCoalescesFrames();
    testSessionRuntimeCommitHides();
    testSessionRuntimeOffOwnerPosts();
    testSessionRuntimeStopCancelsFrame();

    return superior::test::finish("runtime tests");
}
