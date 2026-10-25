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

#include "superior/speech_recognition/Capture.hpp"
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
    /// Overrides the default silence policy; mainly for tests and tuning.
    void setSilencePolicy(SilencePolicy policy);

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
    /// Starts the streaming recogniser and the thread that reads its events.
    /// Returns false when it cannot, which puts the session back on the batch
    /// path.
    bool startStreamingRecogniser();
    /// Closes the recogniser's stdin so it flushes its last segment, then reaps
    /// it. Called before the capture pipe disappears.
    void finishStreamingRecogniser();
    /// Reads JSON events from the recogniser until it exits.
    void pumpRecogniser();
    void finishRecognition(std::uint64_t generation, std::vector<std::int16_t> audio);
    /// Puts the sound server's default input back once the desktop's own
    /// re-election has settled. See `shouldRestoreDefaultSource`.
    void pollDefaultSourceRestore(std::uint64_t nowMs);
    void emitPhase(const std::string &phase);

    Emit emit_;
    EngineConfig config_;

    // Capture
    bool capturing_ = false;
    int captureFd_ = -1;
    pid_t capturePid_ = -1;
    std::thread captureThread_;
    std::mutex captureMutex_;
    std::vector<std::int16_t> audio_;
    double pendingLevel_ = 0.0;
    std::uint64_t lastLevelMs_ = 0;
    /// Set by the capture thread when the tool exits without being asked to.
    std::atomic<bool> captureEnded_{false};
    /// Ends the utterance when the user stops talking, so the panel does not
    /// require a second click after every sentence.
    SilenceWatch silence_;
    // Streaming recogniser, alive for the whole utterance.
    bool streaming_ = false;
    pid_t asrPid_ = -1;
    int asrWriteFd_ = -1;
    int asrReadFd_ = -1;
    std::thread asrThread_;
    /// Set when the session is closing: the recogniser's next `final` is the end
    /// of the utterance, not a segment to commit mid-flight.
    std::atomic<bool> asrClosing_{false};

    /// Default input as it was when recording started, and when to put it back.
    /// Restoring is scheduled rather than immediate because the desktop flips it
    /// ~2 s after the stream closes, i.e. *after* we are done.
    std::string defaultSourceBefore_;
    std::uint64_t restoreDefaultAtMs_ = 0;
    std::uint64_t restoreGiveUpAtMs_ = 0;
    std::uint64_t lastRestoreCheckMs_ = 0;

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
