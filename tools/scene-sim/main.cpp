// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "superior/api/Scene.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/protocol/SceneServer.hpp"
#include "superior/protocol/Wire.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

} // namespace

int main(int argc, char **argv) {
    using namespace superior;

    diagnostics::configureFromEnvironment();

    const std::string socketPath =
        argc > 1 ? std::string(argv[1]) : protocol::SceneServer::defaultSocketPath();

    protocol::SceneServer server;
    std::string error;
    if (!server.listen(socketPath, error)) {
        std::cerr << "scene-sim: " << error << '\n';
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SUPERIOR_INFO(diagnostics::categories::Tool)
        .field("socket", socketPath)
        << "scene simulator broadcasting";

    const std::vector<std::string> preedits{"n", "ni", "nih", "niha", "nihao", "nihao!"};
    const std::vector<std::string> words{"你", "泥", "拟", "妮", "逆", "呢", "尼", "倪", "溺"};

    api::FrameId frameId = 1;
    for (int tick = 0; !g_stop; ++tick) {
        api::UiSceneFrame frame;
        frame.frameId = frameId++;
        frame.session = api::SessionKey{1, 1};

        frame.status.visible = true;
        frame.status.placement = api::StatusPlacement::BottomRight;
        frame.status.skinId = "org.superior.default";
        frame.status.items = {
            api::StatusItem{"ime", "拼", "", true, true},
            api::StatusItem{"punct", "，", "", true, true},
            api::StatusItem{"fullwidth", "半", "", false, true},
            api::StatusItem{"voice", "◎", "", false, true},
        };

        const int phase = tick % 9;
        if (phase < 6) {
            frame.panel.visible = true;
            frame.panel.skinId = "org.superior.default";
            frame.panel.styleId = "h1";
            frame.panel.mode = "pinyin";
            frame.panel.preedit.text = preedits[static_cast<std::size_t>(phase)];
            frame.panel.preedit.cursor = frame.panel.preedit.text.size();

            const int count = std::min<int>(9, 3 + phase);
            for (int i = 0; i < count; ++i) {
                frame.panel.candidates.items.push_back(api::Candidate{
                    static_cast<api::CandidateId>(i + 1), std::to_string(i + 1),
                    words[static_cast<std::size_t>(i)], ""});
            }
            frame.panel.candidates.pageIndex = 0;
            frame.panel.candidates.pageCount = 1;
            frame.panel.selected = static_cast<api::CandidateId>(phase % count + 1);
        }

        server.broadcast(protocol::encodeFrame(protocol::MessageType::Scene,
                                               protocol::encodeScene(frame)));
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }

    server.close();
    return 0;
}
