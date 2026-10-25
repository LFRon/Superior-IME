// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "superior/speech_recognition/Protocol.hpp"

namespace superior::speech_recognition {

/// One recording session: capture, level metering and one engine call.
///
/// Everything blocking happens on a thread; `poll()` is the only place that
/// emits, so the owner keeps a single-threaded view of the state machine.
class Service {
public:
    using Emit = std::function<void(const Event &)>;

    explicit Service(Emit emit);
    ~Service();

    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    void configure(EngineConfig config);

    void start();
    void stop();
    void cancel();
    void shutdown();

    /// Emits whatever the capture thread and the recognition worker produced.
    /// Call it from the command loop, at least every ~50 ms.
    void poll();

    [[nodiscard]] bool capturing() const noexcept { return capturing_; }
    [[nodiscard]] bool working() const noexcept { return capturing_ || workerBusy_; }

private:
    void stopCapture();
    void startCaptureThread();
    void finishRecognition(std::uint64_t generation, std::vector<std::int16_t> audio);
    void emitPhase(const std::string &phase);

    Emit emit_;
    EngineConfig config_;

    // Capture
    bool capturing_ = false;
    bool stopping_ = false;
    int captureFd_ = -1;
    pid_t capturePid_ = -1;
    std::thread captureThread_;
    std::mutex captureMutex_;
    std::vector<std::int16_t> audio_;
    double pendingLevel_ = 0.0;
    std::uint64_t lastLevelMs_ = 0;

    // Recognition
    bool workerBusy_ = false;
    std::thread worker_;
    std::uint64_t generation_ = 0;
    struct WorkerResult {
        std::uint64_t generation = 0;
        bool ok = false;
        std::string text;
        std::string error;
    };
    std::mutex resultMutex_;
    std::optional<WorkerResult> result_;

    /// Set while the user has cancelled: a transcript that arrives afterwards is
    /// dropped, never committed.
    std::atomic<bool> revoked_{false};
};

} // namespace superior::speech_recognition
