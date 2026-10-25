// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Service.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <utility>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "superior/speech_recognition/Capture.hpp"
#include "superior/speech_recognition/Engine.hpp"
#include "superior/speech_recognition/Layout.hpp"

namespace superior::speech_recognition {

namespace {

std::uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

constexpr std::uint64_t kLevelIntervalMs = 50;

/// Reads the whole capture stream, appending samples and keeping an RMS value.
void pumpCapture(int fd, std::vector<std::int16_t> &audio, std::mutex &mutex,
                 double &level, std::atomic<bool> &stopping, std::atomic<bool> &ended) {
    std::vector<std::int16_t> chunk(1600); // 100 ms at 16 kHz
    while (!stopping.load(std::memory_order_relaxed)) {
        const ssize_t bytes = ::read(fd, chunk.data(), chunk.size() * sizeof(std::int16_t));
        if (bytes <= 0) {
            break;
        }
        const auto samples = static_cast<std::size_t>(bytes) / sizeof(std::int16_t);
        const double measured = rmsLevel(chunk.data(), samples);
        std::lock_guard<std::mutex> lock(mutex);
        audio.insert(audio.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(samples));
        level = measured;
    }
    // The tool exited (device busy, unplugged, killed). Say so: without this the
    // service would keep "recording" silence until the no-speech timeout.
    ended.store(true, std::memory_order_relaxed);
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

    const CaptureCommand command = defaultCaptureCommand();

    int pipeFd[2] = {-1, -1};
    if (::pipe(pipeFd) != 0) {
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "capture_failed";
        event.message = std::string("cannot create a pipe: ") + std::strerror(errno);
        emit_(event);
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
    captureThread_ = std::thread([this] {
        pumpCapture(captureFd_, audio_, captureMutex_, pendingLevel_, revoked_, captureEnded_);
    });
    // The clock starts when capture starts: time spent waiting for the panel to
    // open is not part of the utterance.
    silence_.begin(nowMs());
    emitPhase("recording");
}

void Service::start() {
    if (capturing_ || workerBusy_) {
        return;
    }
    if (!captureAvailable()) {
        Event event;
        event.kind = Event::Kind::Error;
        event.code = "microphone_missing";
        event.message = "没有找到录音工具（pw-record / parec / arecord）。";
        emit_(event);
        return;
    }
    startCaptureThread();
}

void Service::stopCapture() {
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
}

void Service::stop() {
    if (!capturing_) {
        emitPhase("idle");
        return;
    }
    stopCapture();

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
                outcome.error = error;
            } else if (const auto text = runEngine(*command, error)) {
                outcome.ok = true;
                outcome.text = *text;
            } else {
                outcome.error = error;
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
    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        audio_.clear();
    }
    emitPhase("idle");
}

void Service::poll() {
    const std::uint64_t now = nowMs();
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
            emit_(event);
        }

        // `capturing_` is still true here, so this EOF came from the tool, not
        // from a stop we asked for.
        if (captureEnded_.load(std::memory_order_relaxed)) {
            std::size_t captured = 0;
            {
                std::lock_guard<std::mutex> lock(captureMutex_);
                captured = audio_.size();
            }
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
    if (worker_.joinable()) {
        worker_.join();
    }
    workerBusy_ = false;
}

} // namespace superior::speech_recognition
