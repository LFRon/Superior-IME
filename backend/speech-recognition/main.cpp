// SPDX-License-Identifier: GPL-3.0-or-later
//
// superior_speech_recognition
//
// A small, deliberately dumb process: it owns the microphone and one engine
// call, and speaks JSON Lines on stdin/stdout. It never decides what to show and
// never types anything — the agent does that. Keeping the model runtime out of
// the input-method process is what makes a 600 MB decode harmless to typing.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <poll.h>
#include <unistd.h>

#include "superior/speech_recognition/Layout.hpp"
#include "superior/speech_recognition/Models.hpp"
#include "superior/speech_recognition/Protocol.hpp"
#include "superior/speech_recognition/Service.hpp"

namespace {

using namespace superior::speech_recognition;

void emit(const Event &event) {
    const std::string line = encodeEvent(event) + "\n";
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
    return config;
}

} // namespace

int main() {
    Service service(emit);
    EngineConfig config = defaults();
    service.configure(config);

    if (!enginePresent()) {
        emitError("engine_missing", "没有找到语音识别引擎。");
    } else if (!customEngineConfigured() && !modelPresent(defaultModel().id)) {
        emitError("model_missing", "还没有下载语音识别模型。");
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
                        emitError("model_missing", "还没有下载语音识别模型。");
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
