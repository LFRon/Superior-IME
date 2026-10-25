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
    /// Closes the recogniser's stdin so it flushes its last segment. Returns
    /// immediately: the flush can take seconds on a big model, and blocking here
    /// would freeze the panel's telemetry exactly when the user pressed stop.
    void beginStreamingClose();
    /// Joins and reaps the recogniser once its reader thread has finished.
    void reapStreamingRecogniser();
    /// Folds this session's measured back-pressure into the persisted step
    /// tuning. Called from `reapStreamingRecogniser`, the single place every
    /// streaming session ends.
    void learnStreamingPace();
    /// True while we are waiting for the recogniser to flush.
    [[nodiscard]] bool streamingClosing() const noexcept { return awaitingStreamingExit_; }
    /// Reads JSON events from the recogniser until it exits.
    void pumpRecogniser();
    /// Turns one recogniser event into our events, applying the commit cadence.
    ///
    /// The recogniser rewrites its transcript as it re-decodes, so a partial is a
    /// *guess*, not a prefix. The last two units of a draft stay uncommitted
    /// (see `stableTailPortion`); the rest is typed as soon as the cadence
    /// expires, so something appears while the user keeps talking.
    void handleStreamEvent(std::string kind, std::string text, std::uint64_t nowMs,
                           bool closing);
    /// Types out the part of the draft that is no longer likely to change.
    ///
    /// Called on a timer, not only on engine events: a streaming decoder emits
    /// nothing at all while the audio is quiet, so waiting for "the same partial
    /// twice" never commits the sentence the user just finished.
    /// Returns true when it committed and already emitted the follow-up draft.
    bool flushStreamTail(std::uint64_t nowMs);
    /// Longest prefix of a draft worth typing: everything except its last two
    /// units, where a unit is a Latin word or one character.
    [[nodiscard]] static std::string stableTailPortion(std::string_view tail);
    /// Longest prefix of `text` that has already been committed.
    [[nodiscard]] std::size_t committedPrefixLength(std::string_view text) const;
    /// Records a committed fragment, inserting a space at a Latin word seam.
    void commitSeam(const std::string &tail);
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
    /// Set by `stopCapture()` so the capture thread leaves its loop even while it
    /// is blocked writing to a busy recogniser. Without it, stopping could hang
    /// until the model finished decoding everything that was already queued.
    std::atomic<bool> captureStopping_{false};
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
    /// Did the recogniser say anything at all? A model that fails to load exits
    /// silently, and "no events" is the only signal we get; without this the panel
    /// would go back to Ready as if the user had said nothing.
    std::atomic<bool> asrProducedOutput_{false};
    /// Bytes the capture tool produced. Kept separate from `audio_.size()`: in
    /// streaming mode no audio is retained, so the buffer cannot answer "did we
    /// capture anything".
    std::atomic<std::uint64_t> capturedBytes_{0};
    /// Milliseconds the capture thread spent waiting for room in the recogniser's
    /// pipe. Non-zero means the recogniser was still decoding audio that arrived
    /// seconds ago: the one measurement that says "this step is too fine for this
    /// machine" without guessing from the CPU model. See `StepTuning`.
    std::atomic<std::uint64_t> teeStallMs_{0};
    /// Start of the current utterance, so a session shorter than a few seconds is
    /// not judged (the model is still loading, and nothing about the pace is
    /// known yet).
    std::uint64_t sessionStartMs_ = 0;
    /// Set by the reader thread when the recogniser's stdout reached EOF.
    std::atomic<bool> asrReaderDone_{false};
    /// Asked by `reapStreamingRecogniser` so the reader thread can leave a
    /// blocking `read()`. Without it a recogniser that dies while something else
    /// still holds its stdout open kept the reader (and the whole command loop,
    /// which joins it) blocked forever: the capability stayed alive but stopped
    /// answering, which is indistinguishable from "the input method is dead".
    std::atomic<bool> asrReaderStopping_{false};
    /// True between "stop requested" and "recogniser flushed".
    bool awaitingStreamingExit_ = false;
    /// When the recogniser's stdin was closed. A flush that never finishes must
    /// not leave the session "busy" forever: the panel shows "识别中", refuses
    /// every click, and nothing else ever moves it — exactly the state a report of
    /// "second utterance does nothing" turned out to be in.
    std::uint64_t closeRequestedMs_ = 0;
    /// Set when we killed the recogniser on purpose (cancel/shutdown). Its
    /// non-zero exit is then expected: reporting "the engine returned nothing"
    /// after a cancel showed a scary error for a deliberate action.
    std::atomic<bool> asrKilledOnPurpose_{false};

    // Commit cadence for continuous dictation.
    /// Guards `committed_` and the draft state: the recogniser thread writes them
    /// from `pumpRecogniser`, the cadence flushes them from `poll`.
    std::mutex streamMutex_;
    /// Text already emitted as `chunk` for the current utterance.
    std::string committed_;
    /// The part of the latest draft that has not been committed yet.
    std::string pendingTail_;
    /// When the draft last changed, and when it was last typed out.
    std::uint64_t pendingSinceMs_ = 0;
    std::uint64_t lastCommitMs_ = 0;
    /// How long to wait between two commits.
    std::uint64_t commitCadenceMs_ = 1000;

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
