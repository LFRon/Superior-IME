// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for the agent, for working on the overlay without Fcitx5.
//
// It broadcasts scenes and *answers commands*. A tool that only writes cannot
// check the half of the UI that matters: that a click turns into one intent,
// and that the intent comes back as a new scene. With the loop closed, "the
// toolbar did not move" is observable here instead of only on a live desktop.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "superior/api/Scene.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/protocol/SceneServer.hpp"
#include "superior/protocol/Wire.hpp"

namespace {

using namespace superior;

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

/// Everything the simulated agent owns. The poll thread reads it from the
/// command handler while the main thread animates, so it has one mutex.
struct State {
    std::mutex mutex;
    api::FrameId frameId = 1;
    int tick = 0;
    bool fullPunctuation = true;
    bool speechVisible = false;
};

void emit(protocol::SceneServer &server, State &state) {
    std::lock_guard<std::mutex> lock(state.mutex);

    const std::vector<std::string> preedits{"n", "ni", "nih", "niha", "nihao", "nihao!"};
    const std::vector<std::string> words{"你", "泥", "拟", "妮", "逆", "呢", "尼", "倪", "溺"};

    api::UiSceneFrame frame;
    frame.frameId = state.frameId++;
    frame.session = api::SessionKey{1, 1};

    frame.status.visible = true;
    frame.status.placement = api::StatusPlacement::BottomRight;
    frame.status.skinId = "org.superior.default";
    frame.status.items = {
        api::StatusItem{"inline-mode", "中", "", false, true, api::StatusRole::Text},
        // Matches what the Fcitx5 adapter emits: the engine's own action name,
        // with the role carrying the meaning.
        // The label names the switch, not the current width: the glyph carries
        // the state, and Fcitx5's tray menu shows this string verbatim.
        api::StatusItem{"punctuation", "全角标点", "", state.fullPunctuation, true,
                        api::StatusRole::Punctuation},
        api::StatusItem{"speech", "", "microphone", false, true, api::StatusRole::Speech},
    };

    const int phase = state.tick % 9;
    if (phase < 6) {
        frame.panel.visible = true;
        frame.panel.skinId = "org.superior.default";
        frame.panel.styleId = "h1";
        frame.panel.mode = "pinyin";
        frame.panel.preedit.text = preedits[static_cast<std::size_t>(phase)];
        frame.panel.preedit.cursor = frame.panel.preedit.text.size();

        const int count = std::min<int>(9, 3 + phase);
        for (int i = 0; i < count; ++i) {
            frame.panel.candidates.items.push_back(
                api::Candidate{static_cast<api::CandidateId>(i + 1), std::to_string(i + 1),
                               words[static_cast<std::size_t>(i)], ""});
        }
        frame.panel.candidates.pageIndex = 0;
        frame.panel.candidates.pageCount = 1;
        frame.panel.selected = static_cast<api::CandidateId>(phase % count + 1);
    }

    frame.speech.visible = state.speechVisible;
    if (state.speechVisible) {
        frame.speech.phase = api::SpeechPhase::Ready;
        frame.speech.modelId = "paraformer-zh";
        frame.speech.modelName = "Paraformer 中文";
        frame.speech.modelPresent = true;
        frame.speech.enginePresent = true;
        frame.speech.skinId = "org.superior.default";
    }

    server.broadcast(protocol::encodeFrame(protocol::MessageType::Scene,
                                           protocol::encodeScene(frame)));
}

/// Applies one UI command the way the agent would. Anything not handled is
/// reported as such instead of ignored, so a menu entry that sends the wrong
/// command shows up as a line of output instead of as a silent no-op.
bool apply(State &state, const api::UiCommand &command) {
    return std::visit(
        [&state](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, api::ToggleStatusItem>) {
                if (value.itemId == "punctuation") {
                    state.fullPunctuation = !state.fullPunctuation;
                    return true;
                }
                if (value.itemId == "speech") {
                    state.speechVisible = !state.speechVisible;
                    return true;
                }
                return false;
            } else {
                static_cast<void>(value);
                return false;
            }
        },
        command);
}

} // namespace

int main(int argc, char **argv) {
    using namespace superior;

    diagnostics::configureFromEnvironment();

    const std::string socketPath =
        argc > 1 ? std::string(argv[1]) : protocol::SceneServer::defaultSocketPath();

    State state;
    protocol::SceneServer server;
    std::string error;
    if (!server.listen(socketPath, error)) {
        std::cerr << "scene-sim: " << error << '\n';
        return 1;
    }

    // The handler receives a whole wire frame, not a payload: decode the
    // framing first, exactly like the agent does.
    server.setCommandHandler([&state, &server](std::string bytes) {
        protocol::DecodeError decodeError;
        std::size_t consumed = 0;
        protocol::WireMessage message;
        if (protocol::tryDecodeFrame(bytes, consumed, message, decodeError) !=
            protocol::FrameStatus::Complete) {
            std::cout << "scene-sim: malformed frame: " << decodeError.code << ' '
                      << decodeError.detail << std::endl;
            return;
        }
        const auto command = protocol::decodeCommand(message.payload, decodeError);
        if (!command) {
            std::cout << "scene-sim: malformed command: " << decodeError.code << ' '
                      << decodeError.detail << std::endl;
            return;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            changed = apply(state, *command);
        }
        std::cout << "scene-sim: command " << protocol::encodeCommand(*command)
                  << (changed ? " -> scene updated" : " -> not handled here") << std::endl;
        if (changed) {
            emit(server, state);
        }
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SUPERIOR_INFO(diagnostics::categories::Tool)
        .field("socket", socketPath)
        << "scene simulator broadcasting";

    while (!g_stop) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.tick;
        }
        emit(server, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }

    server.close();
    return 0;
}
