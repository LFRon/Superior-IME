// SPDX-License-Identifier: GPL-3.0-or-later
//
// superior_speech_recognition
//
// A small, deliberately dumb process: it owns the microphone and one engine
// call, and speaks JSON Lines on stdin/stdout. It never decides what to show and
// never types anything — the agent does that. Keeping the model runtime out of
// the input-method process is what makes a 600 MB decode harmless to typing.
//
// This file is the process entry, i.e. the host (D-13): it is the only place in
// this program that names a platform. `Service` itself only knows
// `api::AudioSource`, so the same logic runs unchanged on a host whose capture
// is a Qt object rather than a child process.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <poll.h>
#include <unistd.h>

#include "superior/linux/AudioCapture.hpp"
#include "superior/speech_recognition/Layout.hpp"
#include "superior/speech_recognition/Models.hpp"
#include "superior/speech_recognition/Protocol.hpp"
#include "superior/speech_recognition/Service.hpp"

namespace {

using namespace superior::speech_recognition;

/// Guards `stdout`.
///
/// Events are produced by the main loop *and* by the recogniser's reader thread.
/// `std::FILE` keeps its own buffer lock, but two threads calling `fwrite` +
/// `fflush` independently can interleave a line boundary, which corrupts the
/// JSON-Lines stream — observed as a `final` arriving after `idle`, and as the
/// agent rejecting a half-written line.
std::mutex g_emitMutex;

void emit(const Event &event) {
    const std::string line = encodeEvent(event) + "\n";
    std::lock_guard<std::mutex> lock(g_emitMutex);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void emitError(const std::string &code, const std::string &message) {
    Event event;
    event.kind = Event::Kind::Error;
    event.code = code;
    event.message = message;
    emit(event);
}

/// Fills in the parts of the configuration the agent does not have to repeat on
/// every command: the engine and the selected model.
EngineConfig defaults() {
    EngineConfig config;
    config.enginePath = resolveEngine();
    const ModelEntry &model = defaultModel();
    const std::filesystem::path path = modelPath(model.id);
    config.modelPath = path.string();
    // The backend belongs to the model. Leaving it empty made the engine fall
    // back to "qwen3" and load a FireRed GGUF with it, which fails with a wall of
    // "required tensor … not found" instead of saying what is wrong.
    config.backend = model.backend;
    return config;
}

std::uint64_t positiveFromEnvironment(const char *name, std::uint64_t fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, nullptr, 10);
    return errno == 0 && parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallback;
}

/// The pause that ends an utterance depends on the room and the microphone, so it
/// is tunable without a rebuild. Everything else stays at the defaults.
SilencePolicy policyFromEnvironment() {
    SilencePolicy policy;
    policy.holdMs = positiveFromEnvironment("SUPERIOR_SPEECH_SILENCE_MS", policy.holdMs);
    return policy;
}

} // namespace

int main() {
    // Writing to a child's stdin is part of this program's job. If that child
    // dies (model load failure, OOM kill), the next write must return EPIPE and
    // be handled — not terminate us with SIGPIPE, which is what a default action
    // does and how a broken recogniser took the whole capability down silently.
    ::signal(SIGPIPE, SIG_IGN);
    // The capture backend is chosen here and nowhere else.
    superior::linux::LinuxAudioSource audio;
    Service service(emit, audio);
    EngineConfig config = defaults();
    service.configure(config);
    service.setSilencePolicy(policyFromEnvironment());

    if (!enginePresent()) {
        emitError("engine_missing", "没有找到语音识别引擎。");
    } else if (!customEngineConfigured() && !modelPresent(defaultModel().id)) {
        emitError("model_missing", "还没有下载语音识别模型");
    } else {
        Event ready;
        ready.kind = Event::Kind::Ready;
        emit(ready);
    }

    std::string buffer;
    bool running = true;
    while (running) {
        // A short poll keeps the recognition worker's result flowing while the
        // process is otherwise idle.
        struct pollfd descriptor {};
        descriptor.fd = STDIN_FILENO;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            char chunk[512];
            const ssize_t count = ::read(STDIN_FILENO, chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(count));
            std::size_t newline = 0;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const auto command = decodeCommand(line);
                if (!command) {
                    emitError("bad_command", "无法解析的命令");
                    continue;
                }
                switch (command->kind) {
                case Command::Kind::Configure:
                    config = command->config;
                    service.configure(config);
                    break;
                case Command::Kind::Start:
                    if (!enginePresent()) {
                        emitError("engine_missing", "没有找到语音识别引擎。");
                        break;
                    }
                    if (!customEngineConfigured() && !modelPresent(defaultModel().id) &&
                        config.modelPath.empty()) {
                        emitError("model_missing", "还没有下载语音识别模型");
                        break;
                    }
                    service.start();
                    break;
                case Command::Kind::Stop:
                    service.stop();
                    break;
                case Command::Kind::Cancel:
                    service.cancel();
                    break;
                case Command::Kind::Quit:
                    running = false;
                    break;
                }
            }
        }
        service.poll();
    }

    service.shutdown();
    return 0;
}
