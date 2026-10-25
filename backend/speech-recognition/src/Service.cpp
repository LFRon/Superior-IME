// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Service.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>
#include <filesystem>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "superior/diagnostics/Log.hpp"
#include "superior/speech_recognition/Capture.hpp"
#include "superior/speech_recognition/Engine.hpp"
#include "superior/speech_recognition/Layout.hpp"

namespace superior::speech_recognition {

namespace diag = ::superior::diagnostics;

namespace {

std::uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr std::uint64_t kLevelIntervalMs = 50;

/// Reads the capture stream, keeping the samples for the level meter and (when
/// streaming) forwarding them to the recogniser's stdin.
///
/// The write happens *outside* the lock: a slow recogniser must never stall the
/// level meter, or the panel's ring would freeze while the model is busy.
void pumpCapture(int fd, std::vector<std::int16_t> &audio, std::mutex &mutex,
                 double &level, std::atomic<bool> &stopping, std::atomic<bool> &ended,
                 int teeFd, bool keepAudio, std::atomic<std::uint64_t> &capturedBytes) {
    std::vector<std::int16_t> chunk(1600); // 100 ms at 16 kHz
    while (!stopping.load(std::memory_order_relaxed)) {
        const ssize_t bytes = ::read(fd, chunk.data(), chunk.size() * sizeof(std::int16_t));
        if (bytes <= 0) {
            break;
        }
        const auto samples = static_cast<std::size_t>(bytes) / sizeof(std::int16_t);
        capturedBytes.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
        const double measured = rmsLevel(chunk.data(), samples);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (keepAudio) {
                audio.insert(audio.end(), chunk.begin(),
                             chunk.begin() + static_cast<std::ptrdiff_t>(samples));
            }
            level = measured;
        }
        if (teeFd >= 0) {
            const auto *pcm = reinterpret_cast<const char *>(chunk.data());
            const std::size_t total = samples * sizeof(std::int16_t);
            std::size_t written = 0;
            while (written < total) {
                const ssize_t result = ::write(teeFd, pcm + written, total - written);
                if (result > 0) {
                    written += static_cast<std::size_t>(result);
                    continue;
                }
                if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // The recogniser is decoding behind us; wait for room, but
                    // stay responsive to a stop. Blocking here is what made
                    // "stop" take as long as the model's backlog.
                    pollfd writable{teeFd, POLLOUT, 0};
                    if (stopping.load(std::memory_order_relaxed)) {
                        break;
                    }
                    ::poll(&writable, 1, 100);
                    continue;
                }
                // The recogniser is gone; stop feeding it and let the reader
                // report the failure.
                teeFd = -1;
                break;
            }
        }
    }
    // The tool exited (device busy, unplugged, killed). Say so: without this the
    // service would keep "recording" silence until the no-speech timeout.
    ended.store(true, std::memory_order_relaxed);
}

/// Splits one JSON line from the recogniser into (kind, text).
bool decodeStreamEvent(const std::string &line, std::string &kind, std::string &text) {
    const auto parsed = nlohmann::json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    kind = parsed.value("type", std::string{});
    text = parsed.value("text", std::string{});
    return !kind.empty();
}

/// `SUPERIOR_KEEP_DEFAULT_SOURCE=1` leaves the desktop's audio policy alone:
/// no pinning, no restore, exactly the behaviour before this was added.
bool restoreDefaultSourceEnabled() {
    const char *value = std::getenv("SUPERIOR_KEEP_DEFAULT_SOURCE");
    return value == nullptr || std::strcmp(value, "1") != 0;
}

/// Threads to give the model.
///
/// Not "all of them": a dictation session competes with the compositor, the
/// editor and the browser. Half the machine, at least 2 and at most 8, leaves the
/// desktop responsive on a 4-core laptop and still finishes a 0.6B decode fast on
/// a 16-core desktop. `SUPERIOR_SPEECH_THREADS` overrides it.
int autoThreads() {
    if (const char *value = std::getenv("SUPERIOR_SPEECH_THREADS"); value != nullptr) {
        const int parsed = std::atoi(value);
        if (parsed > 0) {
            return parsed;
        }
    }
    const unsigned cores = std::thread::hardware_concurrency();
    const int half = cores == 0 ? 2 : static_cast<int>(cores / 2);
    return std::clamp(half, 2, 8);
}

/// Absolute cap for one continuous dictation session.
///
/// A safety net, not a feature: a microphone left open by accident must not listen
/// for an hour. The panel starts a countdown before it expires, and because every
/// segment is committed as it stabilises, reaching the cap loses nothing the user
/// said. `SUPERIOR_SPEECH_MAX_MS` raises it for long-form dictation.
std::uint64_t sessionCapMs() {
    if (const char *value = std::getenv("SUPERIOR_SPEECH_MAX_MS"); value != nullptr) {
        const long long parsed = std::atoll(value);
        if (parsed > 0) {
            return static_cast<std::uint64_t>(parsed);
        }
    }
    return 60000;
}

/// How often the uncommitted draft is typed out.
///
/// The recogniser rewrites its transcript while it re-decodes, so the last couple
/// of characters are never typed (see `stableTailPortion`); the rest is. Waiting
/// for a draft to become *completely* stable sounds safer but only means "commit
/// nothing while the user is still talking", because a growing transcript never
/// stops changing. `SUPERIOR_SPEECH_COMMIT_MS` tunes it; 1000 ms is the point
/// where the text still keeps up with speech.
std::uint64_t commitCadenceFromEnvironment() {
    if (const char *value = std::getenv("SUPERIOR_SPEECH_COMMIT_MS"); value != nullptr) {
        const long long parsed = std::atoll(value);
        if (parsed > 0) {
            return static_cast<std::uint64_t>(parsed);
        }
    }
    return 1000;
}

/// Rolling context the streaming decoder keeps.
///
/// It must span the whole session, otherwise the decoder's window slides past the
/// beginning of the utterance and its draft stops being a *prefix* of what was said
/// — it loses its head. `committed_` is a prefix of the draft, and that is the one
/// invariant the commit logic has; with a sliding window, continuous dictation went
/// silent after ten seconds. Measured cost of the bigger window (Paraformer-zh,
/// 55 s of audio): 0.48x realtime instead of 0.18x, i.e. still twice as fast as
/// the microphone. `SUPERIOR_SPEECH_WINDOW_MS` overrides it.
int streamingWindowMs() {
    if (const char *value = std::getenv("SUPERIOR_SPEECH_WINDOW_MS"); value != nullptr) {
        const long parsed = std::atol(value);
        if (parsed >= 2000) {
            return static_cast<int>(std::min<long>(parsed, 600000));
        }
    }
    // Only ever larger than the session cap: a window shorter than the session is
    // exactly the bug this exists to prevent.
    return static_cast<int>(std::max<std::uint64_t>(sessionCapMs(), 10000));
}

/// How long a recogniser may take to exit after its stdin was closed.
///
/// A real flush re-decodes the utterance: one to two seconds for a minute of
/// speech. Fifteen is a wide margin, and `SUPERIOR_SPEECH_FLUSH_MS` makes it
/// testable. Waiting forever is not an option: while it waits, the session is
/// "finishing", and nothing the user does can move it.
std::uint64_t flushTimeoutMs() {
    if (const char *value = std::getenv("SUPERIOR_SPEECH_FLUSH_MS"); value != nullptr) {
        const long long parsed = std::atoll(value);
        if (parsed > 0) {
            return static_cast<std::uint64_t>(parsed);
        }
    }
    return 15000;
}

/// How long a draft must exist before any of it is typed.
///
/// Without this the very first partial of a burst would be typed the instant it
/// arrived, i.e. exactly the guess the decoder is about to revise.
constexpr std::uint64_t kDraftDwellMs = 250;

/// How much audio each streaming re-decode sees.
///
/// This is the knob that trades latency against CPU, and it is not a free knob:
/// the streaming mode re-encodes the *whole* utterance on every step, so halving
/// the step roughly doubles the work. Measured on a Ryzen 7 8845HS (8 cores of a
/// 16-core chip) with FireRedASR2-AED q4_k, 12 s of audio, in CPU core-seconds:
///
///     2000 ms -> 83-88 s   (1.1x realtime: keeps up with headroom)
///     1500 ms -> 85 s      (1.0x realtime: exactly at the edge)
///     1000 ms -> 147 s     (0.65x realtime: falls behind, audio is lost)
///
/// 2000 ms is the default because this is an input method, not a benchmark: it
/// has to work on a laptop that is also running a browser and an IDE, and the
/// numbers above were taken with nothing else running. A CPU half as fast still
/// keeps up at 2000 ms and would fall behind at 1000 ms — and falling behind is
/// not "a bit slower": the capture pipe fills, the sound server drops audio, and
/// words disappear from the middle of sentences.
///
/// Only a machine that is genuinely saturated gets the coarser 3000 ms step.
/// `SUPERIOR_SPEECH_STEP_MS` overrides both (a GPU build or a smaller model can
/// afford less).
int streamingStepMs() {
    if (const char *value = std::getenv("SUPERIOR_SPEECH_STEP_MS"); value != nullptr) {
        const long parsed = std::atol(value);
        if (parsed >= 200) {
            return static_cast<int>(std::min<long>(parsed, 10000));
        }
    }
    double load = 0.0;
    if (std::FILE *file = std::fopen("/proc/loadavg", "r"); file != nullptr) {
        if (std::fscanf(file, "%lf", &load) != 1) {
            load = 0.0;
        }
        std::fclose(file);
    }
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return load < 0.9 * static_cast<double>(cores) ? 2000 : 3000;
}

/// The recogniser's reasons are technical and English; the panel shows the
/// message verbatim, so the user-facing sentence is chosen here. The raw reason
/// is logged, so a diagnosis never gets lost in translation.
std::string describeEngineFailure(const std::string &reason) {
    if (reason == "no speech recognized") {
        // The common case: silence, or a microphone nobody spoke into.
        return "没有听清，请再说一次";
    }
    if (reason == "the recogniser engine is not installed" || reason == "no recogniser engine") {
        return "语音识别引擎未安装";
    }
    if (reason == "the recogniser engine failed") {
        return "语音识别引擎出错";
    }
    return "语音识别失败，请重试";
}

} // namespace

Service::Service(Emit emit) : emit_(std::move(emit)) {}

Service::~Service() {
    shutdown();
}

void Service::configure(EngineConfig config) {
    config_ = std::move(config);
}

void Service::setSilencePolicy(SilencePolicy policy) {
    silence_ = SilenceWatch(policy);
}

void Service::emitPhase(const std::string &phase) {
    Event event;
    event.kind = Event::Kind::Phase;
    event.phase = phase;
    emit_(event);
}

void Service::startCaptureThread() {
    capturePid_ = -1;
    captureFd_ = -1;
    audio_.clear();
    pendingLevel_ = 0.0;
    lastLevelMs_ = 0;
    revoked_.store(false);

    // Remember the user's own choice before the stream exists: the desktop
    // re-elects the default input a couple of seconds after the stream closes,
    // and only we know what it was. While a restore is still pending the value
    // is kept instead of re-read, so a second sentence spoken inside that window
    // does not adopt the flipped default as "the user's choice".
    // Continuous dictation: the microphone stays open until the user stops it.
    // The pause rule is off and the only limit is the announced cap, so thinking
    // mid-sentence cannot end the session.
    {
        SilencePolicy policy;
        if (streaming_) {
            policy.endOnSilence = false;
            policy.maxRecordingMs = sessionCapMs();
        }
        silence_ = SilenceWatch(policy);
        std::lock_guard<std::mutex> lock(streamMutex_);
        commitCadenceMs_ = commitCadenceFromEnvironment();
        committed_.clear();
        pendingTail_.clear();
        pendingSinceMs_ = 0;
        lastCommitMs_ = nowMs();
        capturedBytes_.store(0, std::memory_order_relaxed);
    }

    if (restoreDefaultAtMs_ == 0 || defaultSourceBefore_.empty()) {
        defaultSourceBefore_ =
            restoreDefaultSourceEnabled() ? defaultSourceName() : std::string{};
    }
    restoreDefaultAtMs_ = 0;

    // Pinning the stream to that device keeps the *audio* on the right
    // microphone even if the flip happens before this call.
    const CaptureCommand command =
        defaultCaptureCommand(restoreDefaultSourceEnabled() ? defaultSourceBefore_ : std::string{});

    // Both early returns below leave the recogniser we just started with nothing to
    // feed it: it would sit on a model in RAM until the next session. End it here.
    const auto abandonRecogniser = [this] {
        if (!streaming_) {
            return;
        }
        beginStreamingClose();
        asrKilledOnPurpose_.store(true, std::memory_order_relaxed);
        if (asrPid_ > 0) {
            ::kill(asrPid_, SIGTERM);
        }
        reapStreamingRecogniser();
    };

    int pipeFd[2] = {-1, -1};
    if (::pipe(pipeFd) != 0) {
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "capture_failed";
        event.message = std::string("cannot create a pipe: ") + std::strerror(errno);
        emit_(event);
        abandonRecogniser();
        return;
    }

    std::vector<std::string> argvStorage;
    argvStorage.push_back(command.program);
    argvStorage.insert(argvStorage.end(), command.args.begin(), command.args.end());
    std::vector<char *> argv;
    argv.reserve(argvStorage.size() + 1);
    for (auto &item : argvStorage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "capture_failed";
        event.message = std::string("cannot fork: ") + std::strerror(errno);
        emit_(event);
        abandonRecogniser();
        return;
    }
    if (pid == 0) {
        ::dup2(pipeFd[1], STDOUT_FILENO);
        // Capture tools are chatty; keep their stderr away from our protocol.
        const int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDERR_FILENO);
        }
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        ::execvp(command.program.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(pipeFd[1]);
    capturePid_ = pid;
    captureFd_ = pipeFd[0];
    capturing_ = true;

    captureEnded_.store(false, std::memory_order_relaxed);
    captureStopping_.store(false, std::memory_order_relaxed);
    const int teeFd = streaming_ ? asrWriteFd_ : -1;
    // Streaming does not keep the audio: batch fallback is the only consumer, and
    // an hour of dictation would otherwise sit in RAM (32 KB/s ≈ 115 MB/h).
    const bool keepAudio = !streaming_;
    captureThread_ = std::thread([this, teeFd, keepAudio] {
        pumpCapture(captureFd_, audio_, captureMutex_, pendingLevel_, captureStopping_,
                    captureEnded_, teeFd, keepAudio, capturedBytes_);
    });
    // The clock starts when capture starts: time spent waiting for the panel to
    // open is not part of the utterance.
    silence_.begin(nowMs());
    emitPhase("recording");
}

std::size_t Service::committedPrefixLength(std::string_view text) const {
    const std::size_t limit = std::min(text.size(), committed_.size());
    std::size_t index = 0;
    while (index < limit && text[index] == committed_[index]) {
        ++index;
    }
    // A prefix may only end *between* characters. The byte at `index` is the
    // first one that did not match, so a continuation byte there means the two
    // strings differ inside one character and the match has to back up to that
    // character's lead byte. Testing `text[index - 1]` instead would back up over
    // every multi-byte character that did match, which made each partial after a
    // commit look like the model had rewritten already-typed text.
    while (index > 0 && index < text.size() &&
           (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) {
        --index;
    }
    return index;
}

void Service::handleStreamEvent(std::string kind, std::string text, std::uint64_t nowMs,
                                bool closing) {
    text = cleanTranscript(text);

    if (kind == "partial") {
        bool agrees = true;
        std::string draft;
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            const std::size_t prefix = committedPrefixLength(text);
            // Only a draft that still starts with everything already typed can be
            // diffed; the rest of the character work is plain substring arithmetic.
            agrees = prefix >= committed_.size() || prefix == text.size();
            if (agrees) {
                const std::string tail = text.substr(std::min(committed_.size(), text.size()));
                if (tail != pendingTail_) {
                    pendingTail_ = tail;
                    pendingSinceMs_ = nowMs;
                }
                draft = pendingTail_;
            } else {
                // The model revised text we already typed. Nothing can take that
                // back, so this draft is not trusted and nothing is committed until
                // it agrees again. Only the part *after* what is already typed is
                // shown: repeating the typed prefix in the draft would read as if
                // the sentence had been doubled on screen.
                pendingTail_.clear();
                pendingSinceMs_ = nowMs;
                std::size_t start = std::min(committed_.size(), text.size());
                while (start > 0 && start < text.size() &&
                       (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
                    --start;
                }
                draft = text.substr(start);
            }
        }
        // A partial can be the exact moment the cadence expires, and the engine may
        // then go silent for the rest of the sentence; flushing here is what keeps
        // the text moving.
        if (agrees && flushStreamTail(nowMs)) {
            return; // the flush already emitted the new draft
        }
        Event event;
        event.kind = Event::Kind::Partial;
        event.text = std::move(draft);
        emit_(event);
        return;
    }

    if (kind == "final") {
        // Whatever is left beyond the committed prefix is the tail of this
        // segment. During continuous dictation the engine's final is just a
        // pause — commit it and keep listening; only our own close ends the
        // session, or the text spoken since the last chunk would be lost.
        std::string remainder;
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            // Typing from the common prefix instead of from `committed_.size()`
            // means a revised beginning is not typed a second time in full; the
            // characters that changed after it cannot be taken back either way.
            remainder = text.substr(committedPrefixLength(text));
            pendingTail_.clear();
            pendingSinceMs_ = nowMs;
            if (!closing) {
                commitSeam(remainder);
                lastCommitMs_ = nowMs;
            }
        }
        Event event;
        event.kind = closing ? Event::Kind::Final : Event::Kind::Chunk;
        event.text = remainder;
        emit_(event);
        if (closing) {
            std::lock_guard<std::mutex> lock(streamMutex_);
            committed_.clear();
            lastCommitMs_ = 0;
        }
    }
}

bool Service::flushStreamTail(std::uint64_t nowMs) {
    std::vector<Event> queued;
    {
        std::lock_guard<std::mutex> lock(streamMutex_);
        // While closing, the engine's own final is the last word: typing a guess
        // now would only race it.
        if (asrClosing_.load(std::memory_order_relaxed) || pendingTail_.empty() ||
            commitCadenceMs_ == 0) {
            return false;
        }
        if (nowMs - pendingSinceMs_ < kDraftDwellMs ||
            nowMs - lastCommitMs_ < commitCadenceMs_) {
            return false;
        }
        const std::string stable = stableTailPortion(pendingTail_);
        if (stable.empty()) {
            return false;
        }
        pendingTail_.erase(0, stable.size());
        pendingSinceMs_ = nowMs;
        lastCommitMs_ = nowMs;
        commitSeam(stable);

        Event chunk;
        chunk.kind = Event::Kind::Chunk;
        chunk.text = stable;
        queued.push_back(std::move(chunk));

        // What is left is still a guess, so it stays in the composition line.
        Event draft;
        draft.kind = Event::Kind::Partial;
        draft.text = pendingTail_;
        queued.push_back(std::move(draft));
    }
    for (const auto &event : queued) {
        emit_(event);
    }
    return true;
}

std::string Service::stableTailPortion(std::string_view tail) {
    const auto isWordByte = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    // Hold back the last two units, where a Latin word counts as one unit and
    // anything else counts per character. That is the region a streaming decoder
    // revises ("一二三四" becomes "一二三思"); everything before it has not moved
    // for a whole decode step and is as final as a guess ever gets.
    std::size_t cursor = tail.size();
    for (int units = 0; units < 2 && cursor > 0; ++units) {
        if (isWordByte(static_cast<unsigned char>(tail[cursor - 1]))) {
            while (cursor > 0 && isWordByte(static_cast<unsigned char>(tail[cursor - 1]))) {
                --cursor;
            }
        } else {
            --cursor;
            while (cursor > 0 && (static_cast<unsigned char>(tail[cursor]) & 0xC0) == 0x80) {
                --cursor;
            }
        }
    }
    return std::string(tail.substr(0, cursor));
}


/// Appends `tail` to the record of committed text, with a space when the seam is
/// between two Latin words. Chinese needs no separator, English does.
void Service::commitSeam(const std::string &tail) {
    if (tail.empty()) {
        return;
    }
    const auto isWordByte = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    };
    if (!committed_.empty() && isWordByte(static_cast<unsigned char>(committed_.back())) &&
        isWordByte(static_cast<unsigned char>(tail.front()))) {
        committed_.push_back(' ');
    }
    committed_ += tail;
}

bool Service::startStreamingRecogniser() {
    if (std::getenv("SUPERIOR_DISABLE_STREAMING") != nullptr) {
        return false;
    }
    EngineConfig streaming = config_;
    if (streaming.threads <= 0) {
        streaming.threads = autoThreads();
    }
    if (streaming.puncPath.empty()) {
        // Without this, a CTC backend silently downloads FireRedPunc into
        // ~/.cache on first use. Our rule is that nothing is fetched behind the
        // user's back; `off` keeps the CTC output as-is (no punctuation).
        streaming.puncPath = "off";
    }
    const int step = streamingStepMs();
    const int window = streamingWindowMs();
    std::string error;
    const auto command = buildStreamingEngineCommand(streaming, error, step, window);
    if (!command) {
        SUPERIOR_DEBUG(diag::categories::SpeechRecognition)
            .field("reason", error)
            << "streaming unavailable, falling back to one-shot decoding";
        return false;
    }

    int toChild[2] = {-1, -1};
    int fromChild[2] = {-1, -1};
    if (::pipe(toChild) != 0) {
        return false;
    }
    if (::pipe(fromChild) != 0) {
        ::close(toChild[0]);
        ::close(toChild[1]);
        return false;
    }

    std::vector<std::string> storage;
    storage.push_back(command->program);
    storage.insert(storage.end(), command->args.begin(), command->args.end());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(toChild[0]);
        ::close(toChild[1]);
        ::close(fromChild[0]);
        ::close(fromChild[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(toChild[0], STDIN_FILENO);
        ::dup2(fromChild[1], STDOUT_FILENO);
        // stderr stays connected on purpose: when a 900 MB model fails to load,
        // the reason is the only thing that makes the failure diagnosable, and
        // silencing it once cost an hour of guessing.
        ::close(toChild[0]);
        ::close(toChild[1]);
        ::close(fromChild[0]);
        ::close(fromChild[1]);
        ::execvp(command->program.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(toChild[0]);
    ::close(fromChild[1]);
    asrPid_ = pid;
    asrWriteFd_ = toChild[1];
    // Non-blocking so the capture thread can honour a stop request instead of
    // sitting inside `write()` until the model catches up.
    const int flags = ::fcntl(asrWriteFd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(asrWriteFd_, F_SETFL, flags | O_NONBLOCK);
    }
    asrReadFd_ = fromChild[0];
    streaming_ = true;
    asrClosing_.store(false, std::memory_order_relaxed);
    asrProducedOutput_.store(false, std::memory_order_relaxed);
    asrReaderDone_.store(false, std::memory_order_relaxed);
    asrReaderStopping_.store(false, std::memory_order_relaxed);
    asrKilledOnPurpose_.store(false, std::memory_order_relaxed);
    awaitingStreamingExit_ = false;
    asrThread_ = std::thread([this] { pumpRecogniser(); });
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("pid", static_cast<std::int64_t>(pid))
        .field("threads", static_cast<std::int64_t>(streaming.threads))
        .field("stepMs", static_cast<std::int64_t>(step))
        .field("windowMs", static_cast<std::int64_t>(window))
        << "streaming recogniser started (threads and step chosen from current load)";
    return true;
}

void Service::pumpRecogniser() {
    std::string buffer;
    std::array<char, 4096> chunk{};
    while (!asrReaderStopping_.load(std::memory_order_relaxed)) {
        // Never block in `read()`: the fd stays readable only while the
        // recogniser (or anything that inherited its stdout) is alive, and a
        // blocking read there was able to freeze the whole process.
        pollfd readable{asrReadFd_, POLLIN, 0};
        const int ready = ::poll(&readable, 1, 100);
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        const ssize_t count = ::read(asrReadFd_, chunk.data(), chunk.size());
        if (count <= 0) {
            break;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(count));
        std::size_t newline = 0;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            std::string kind;
            std::string text;
            if (!decodeStreamEvent(line, kind, text)) {
                continue;
            }
            asrProducedOutput_.store(true, std::memory_order_relaxed);
            const bool closing = asrClosing_.load(std::memory_order_relaxed);
            handleStreamEvent(kind, text, nowMs(), closing);
        }
    }
    ::close(asrReadFd_);
    asrReadFd_ = -1;
    asrReaderDone_.store(true, std::memory_order_relaxed);
}

void Service::beginStreamingClose() {
    if (!streaming_) {
        return;
    }
    asrClosing_.store(true, std::memory_order_relaxed);
    if (asrWriteFd_ >= 0) {
        // EOF is the "utterance over" signal the engine understands; it flushes
        // its last segment before exiting. Closing is instant; the flush is not,
        // which is why the wait happens in `poll()`.
        ::close(asrWriteFd_);
        asrWriteFd_ = -1;
    }
    awaitingStreamingExit_ = true;
    closeRequestedMs_ = nowMs();
}

void Service::reapStreamingRecogniser() {
    if (!streaming_) {
        awaitingStreamingExit_ = false;
        return;
    }
    // Tell the reader to leave its poll loop, then join: bounded by the 100 ms
    // poll interval, never by the recogniser's lifetime.
    asrReaderStopping_.store(true, std::memory_order_relaxed);
    if (asrThread_.joinable()) {
        asrThread_.join();
    }
    if (asrPid_ > 0) {
        // Bounded too: `waitpid(…, 0)` would block on a recogniser that ignores
        // SIGTERM, and this runs inside the command loop.
        int status = 0;
        bool reaped = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            const pid_t result = ::waitpid(asrPid_, &status, WNOHANG);
            if (result == asrPid_) {
                reaped = true;
                break;
            }
            if (result < 0 && errno != EINTR) {
                break;
            }
            ::usleep(50000); // 50 ms
        }
        if (!reaped) {
            ::kill(asrPid_, SIGKILL);
            ::waitpid(asrPid_, &status, WNOHANG);
        }
        asrPid_ = -1;
        const bool cleanExit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        const bool killed = asrKilledOnPurpose_.load(std::memory_order_relaxed);
        if (!killed && (!cleanExit || !asrProducedOutput_.load(std::memory_order_relaxed))) {
            Event event;
            event.kind = Event::Kind::Error;
            event.code = "recognition_failed";
            event.message = "语音识别引擎没有返回结果，请检查日志";
            emit_(event);
        }
    }
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("producedOutput", asrProducedOutput_.load(std::memory_order_relaxed))
        << "streaming recogniser reaped";
    streaming_ = false;
    awaitingStreamingExit_ = false;
    closeRequestedMs_ = 0;
}

void Service::start() {
    if (capturing_ || workerBusy_) {
        return;
    }
    if (streaming_) {
        // The previous utterance is still flushing its last segment. Starting a
        // second recogniser now would assign over a joinable thread (terminate)
        // and overwrite the fds of the one still being read, so this one is ended
        // first. It is a leftover: `stop()` already asked the engine for its final.
        beginStreamingClose();
        asrKilledOnPurpose_.store(true, std::memory_order_relaxed);
        if (asrPid_ > 0) {
            ::kill(asrPid_, SIGTERM);
        }
        reapStreamingRecogniser();
    }
    if (!captureAvailable()) {
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "microphone_missing";
        event.message = "没有找到录音工具（pw-record / parec / arecord）。";
        emit_(event);
        return;
    }
    // Start the recogniser first: the capture thread writes straight into it.
    if (!startStreamingRecogniser()) {
        streaming_ = false;
    }
    startCaptureThread();
}

void Service::stopCapture() {
    captureStopping_.store(true, std::memory_order_relaxed);
    if (capturePid_ > 0) {
        ::kill(capturePid_, SIGTERM);
    }
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (captureFd_ >= 0) {
        ::close(captureFd_);
        captureFd_ = -1;
    }
    if (capturePid_ > 0) {
        int status = 0;
        ::waitpid(capturePid_, &status, 0);
        capturePid_ = -1;
    }
    capturing_ = false;
    // The desktop flips the default ~2 s after the last stream closes, so the
    // restore has to wait for it; doing it now would restore nothing.
    if (!defaultSourceBefore_.empty()) {
        // The flip happens 2.0–2.6 s after the stream closes, so the check opens
        // just before that and keeps looking until it is well past due. A single
        // check at a guessed instant was measured to miss it.
        restoreDefaultAtMs_ = nowMs() + 1800;
        restoreGiveUpAtMs_ = nowMs() + 6000;
        lastRestoreCheckMs_ = 0;
    }
}

void Service::pollDefaultSourceRestore(std::uint64_t nowMs) {
    if (restoreDefaultAtMs_ == 0 || nowMs < restoreDefaultAtMs_) {
        return;
    }
    if (capturing_) {
        // A new utterance owns the microphone now; its own restore is queued.
        restoreDefaultAtMs_ = 0;
        return;
    }
    // `pactl` costs a few milliseconds, so it is not polled at command rate.
    if (lastRestoreCheckMs_ != 0 && nowMs - lastRestoreCheckMs_ < 250) {
        return;
    }
    lastRestoreCheckMs_ = nowMs;

    const std::string current = defaultSourceName();
    if (shouldRestoreDefaultSource(defaultSourceBefore_, current, availableSources())) {
        restoreDefaultAtMs_ = 0;
        if (setDefaultSource(defaultSourceBefore_)) {
            SUPERIOR_INFO(diag::categories::SpeechRecognition)
                .field("source", defaultSourceBefore_)
                .field("was", current)
                << "default input restored after recording";
        }
        return;
    }
    if (nowMs >= restoreGiveUpAtMs_) {
        restoreDefaultAtMs_ = 0;
    }
}

void Service::stop() {
    if (awaitingStreamingExit_) {
        // Already stopping: the recogniser is flushing its last segment, and the
        // `final` that arrives from that flush must not be pre-empted by an
        // `idle`. Emitting them out of order used to drop the last thing said.
        return;
    }
    if (!capturing_) {
        emitPhase("idle");
        return;
    }
    const bool wasStreaming = streaming_;
    stopCapture();
    if (wasStreaming) {
        // The recogniser already reported every segment while we were recording;
        // closing its stdin makes it flush the last one, and `pumpRecogniser`
        // emits it as the session's final. `poll()` reaps it when it is done, so
        // the panel keeps receiving telemetry meanwhile.
        emitPhase("transcribing");
        beginStreamingClose();
        return;
    }

    std::vector<std::int16_t> audio;
    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        audio.swap(audio_);
    }

    emitPhase("transcribing");

    ++generation_;
    const std::uint64_t generation = generation_;
    workerBusy_ = true;
    worker_ = std::thread([this, generation, audio = std::move(audio)]() mutable {
        WorkerResult outcome;
        outcome.generation = generation;

        const std::filesystem::path wav = temporaryAudioPath();
        if (audio.empty() || !writeWavFile(wav, audio, 16000)) {
            outcome.error = "没有录到声音。";
        } else {
            std::string error;
            const auto command = buildEngineCommand(config_, wav, error);
            if (!command) {
                outcome.error = describeEngineFailure(error);
            } else if (const auto text = runEngine(*command, error)) {
                outcome.ok = true;
                outcome.text = *text;
            } else {
                outcome.error = describeEngineFailure(error);
            }
        }

        std::error_code cleanup;
        std::filesystem::remove(wav, cleanup);

        std::lock_guard<std::mutex> lock(resultMutex_);
        result_ = std::move(outcome);
    });
}

void Service::cancel() {
    revoked_.store(true);
    if (capturing_) {
        stopCapture();
    }
    if (streaming_) {
        // Cancelling must not commit anything, so the recogniser is killed rather
        // than flushed: its pending final is exactly what we do not want. Killing
        // also makes the reader hit EOF immediately, so this reap is fast.
        beginStreamingClose();
        asrKilledOnPurpose_.store(true, std::memory_order_relaxed);
        if (asrPid_ > 0) {
            ::kill(asrPid_, SIGTERM);
        }
        reapStreamingRecogniser();
    }
    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        audio_.clear();
    }
    emitPhase("idle");
}

void Service::poll() {
    const std::uint64_t now = nowMs();
    pollDefaultSourceRestore(now);
    // Type out the part of the draft that has settled. This has to be a timer:
    // the recogniser emits nothing at all while the audio is quiet, so a commit
    // rule driven by engine events would sit on a finished sentence forever.
    flushStreamTail(now);
    if (awaitingStreamingExit_ && asrReaderDone_.load(std::memory_order_relaxed)) {
        reapStreamingRecogniser();
        emitPhase("idle");
    }
    // A flush that does not finish: the recogniser is still alive long after its
    // stdin was closed, which means it either never saw EOF or is wedged inside a
    // decode. Left alone it keeps `awaitingStreamingExit_` set forever, so every
    // later `stop()` returns early and the session never ends.
    if (awaitingStreamingExit_ && !asrReaderDone_.load(std::memory_order_relaxed) &&
        closeRequestedMs_ != 0 && now - closeRequestedMs_ > flushTimeoutMs()) {
        SUPERIOR_WARN(diag::categories::SpeechRecognition)
            .field("waitedMs", static_cast<std::int64_t>(now - closeRequestedMs_))
            << "recogniser did not exit after its stdin was closed; killing it";
        asrKilledOnPurpose_.store(true, std::memory_order_relaxed);
        if (asrPid_ > 0) {
            ::kill(asrPid_, SIGTERM);
        }
        reapStreamingRecogniser();
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "recognition_timeout";
        event.message = "语音识别没有在预期时间内结束，已停止";
        emit_(event);
        emitPhase("idle");
    }
    // The recogniser died while the microphone was still open: a crash, an OOM
    // kill, a model file that disappeared mid-session. Nothing used to notice. The
    // capture thread only sees its pipe go away and keeps metering the level, so
    // the panel looked like it was listening while every word went nowhere, until
    // the session cap expired a minute later. Ending the session at once is the
    // honest answer, and the error says why.
    if (capturing_ && streaming_ && !awaitingStreamingExit_ &&
        asrReaderDone_.load(std::memory_order_relaxed)) {
        stopCapture();
        // Abandon it on purpose: it is already dead, and a blocking `waitpid` on a
        // process that only closed stdout would freeze the command loop.
        asrKilledOnPurpose_.store(true, std::memory_order_relaxed);
        if (asrPid_ > 0) {
            ::kill(asrPid_, SIGTERM);
        }
        reapStreamingRecogniser();
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "recognition_failed";
        event.message = "语音识别引擎意外退出，请检查日志";
        emit_(event);
        emitPhase("idle");
        return;
    }
    if (capturing_) {
        double level = 0.0;
        {
            std::lock_guard<std::mutex> lock(captureMutex_);
            level = pendingLevel_;
        }
        if (now - lastLevelMs_ >= kLevelIntervalMs) {
            lastLevelMs_ = now;
            Event event;
            event.kind = Event::Kind::Level;
            event.level = level;
            event.remainingMs = static_cast<std::int64_t>(silence_.remainingMs(now));
            emit_(event);
        }

        // `capturing_` is still true here, so this EOF came from the tool, not
        // from a stop we asked for.
        if (captureEnded_.load(std::memory_order_relaxed) && !awaitingStreamingExit_) {
            // Bytes, not buffer size: streaming keeps no audio at all.
            const std::uint64_t captured = capturedBytes_.load(std::memory_order_relaxed);
            if (captured > 0) {
                stop();
            } else {
                cancel();
                Event event;
                event.kind = Event::Kind::Error;
                event.code = "capture_failed";
                event.message = "麦克风不可用或已被占用。";
                emit_(event);
            }
            return;
        }

        // Auto-stop: the user speaks, pauses, and the utterance ends by itself.
        // `stop()` is the same path a manual stop takes, so nothing downstream
        // needs to know which one happened.
        silence_.observe(level, now);
        if (silence_.finished(now)) {
            if (silence_.heardSpeech()) {
                stop();
            } else {
                // Nothing was said, so loading the model would only waste time.
                cancel();
                Event event;
                event.kind = Event::Kind::Error;
                event.code = "no_speech";
                event.message = "没有说话，已停止录音。";
                emit_(event);
            }
        }
    }

    std::optional<WorkerResult> outcome;
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        if (result_) {
            outcome = std::move(result_);
            result_.reset();
        }
    }
    if (!outcome) {
        return;
    }

    if (worker_.joinable()) {
        worker_.join();
    }
    workerBusy_ = false;

    if (outcome->ok && !revoked_.load()) {
        Event event;
        event.kind = Event::Kind::Final;
        event.text = outcome->text;
        emit_(event);
    } else if (!outcome->ok) {
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "recognition_failed";
        event.message = outcome->error;
        emit_(event);
    }
    emitPhase("idle");
}

void Service::shutdown() {
    revoked_.store(true);
    if (capturing_) {
        stopCapture();
    }
    if (streaming_) {
        // Shutting down must not wait for a model to flush: kill it and reap.
        beginStreamingClose();
        asrKilledOnPurpose_.store(true, std::memory_order_relaxed);
        if (asrPid_ > 0) {
            ::kill(asrPid_, SIGTERM);
        }
        reapStreamingRecogniser();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    workerBusy_ = false;
}

} // namespace superior::speech_recognition
