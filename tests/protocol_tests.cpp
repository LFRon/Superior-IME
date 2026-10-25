// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/protocol/Wire.hpp"

namespace {

using namespace superior;
using protocol::DecodeError;
using protocol::FrameStatus;
using protocol::HelloMessage;
using protocol::MessageType;
using protocol::WireMessage;

bool hasCode(const DecodeError &error, std::string_view code) {
    return error.code == code;
}

void testFrameRoundTrip() {
    const std::string payload = R"({"hello":"world"})";
    const std::string frame = protocol::encodeFrame(MessageType::Scene, payload);
    CHECK(frame.size() == protocol::kHeaderSize + payload.size());
    CHECK(frame.substr(0, 4) == "SUPI");

    DecodeError error;
    const auto decoded = protocol::decodeFrame(frame, error);
    CHECK(decoded.has_value());
    CHECK(error.ok());
    CHECK(decoded->type == MessageType::Scene);
    CHECK(decoded->payload == payload);

    CHECK(protocol::messageTypeName(MessageType::Command) == "command");
}

void testFrameRejectsBadInput() {
    DecodeError error;
    CHECK(!protocol::decodeFrame("NOPE", error).has_value());
    CHECK(hasCode(error, "wire.magic"));

    error = {};
    std::string frame = protocol::encodeFrame(MessageType::Ping, "");
    frame[4] = static_cast<char>(99);
    CHECK(!protocol::decodeFrame(frame, error).has_value());
    CHECK(hasCode(error, "wire.version"));

    error = {};
    frame = protocol::encodeFrame(MessageType::Ping, "");
    frame.pop_back();
    CHECK(!protocol::decodeFrame(frame, error).has_value());
    CHECK(hasCode(error, "wire.truncated"));

    error = {};
    frame = protocol::encodeFrame(MessageType::Ping, "x");
    frame.push_back('y');
    CHECK(!protocol::decodeFrame(frame, error).has_value());
    CHECK(hasCode(error, "wire.trailing"));
}

void testStreamingDecode() {
    const std::string first = protocol::encodeFrame(MessageType::Ping, "a");
    const std::string second = protocol::encodeFrame(MessageType::Pong, "b");
    const std::string buffer = first + second;

    std::size_t consumed = 0;
    WireMessage message;
    DecodeError error;
    CHECK(protocol::tryDecodeFrame(buffer.substr(0, 5), consumed, message, error) ==
          FrameStatus::NeedMore);
    CHECK(error.ok());

    CHECK(protocol::tryDecodeFrame(buffer, consumed, message, error) == FrameStatus::Complete);
    CHECK(consumed == first.size());
    CHECK(message.type == MessageType::Ping);
    CHECK(message.payload == "a");

    CHECK(protocol::tryDecodeFrame(buffer.substr(consumed), consumed, message, error) ==
          FrameStatus::Complete);
    CHECK(message.type == MessageType::Pong);
    CHECK(message.payload == "b");
}

void testHelloRoundTrip() {
    HelloMessage hello;
    hello.shell.name = "superior-ui";
    hello.shell.version = "0.1.0";
    hello.shell.profile = api::UiProfile::Desktop;
    hello.shell.windowBackends = {"x11", "layer-shell"};
    hello.capabilities.layerShell = true;
    hello.capabilities.scale = 1.5;

    const std::string payload = protocol::encodeHello(hello);
    DecodeError error;
    const auto decoded = protocol::decodeHello(payload, error);
    CHECK(decoded.has_value());
    CHECK(error.ok());
    CHECK(decoded->shell.name == "superior-ui");
    CHECK(decoded->shell.windowBackends.size() == 2);
    CHECK(decoded->capabilities.layerShell);
    CHECK(superior::test::near(decoded->capabilities.scale, 1.5));

    error = {};
    CHECK(!protocol::decodeHello("not json", error).has_value());
    CHECK(hasCode(error, "wire.field"));
}

api::UiSceneFrame sampleScene() {
    api::UiSceneFrame frame;
    frame.frameId = 7;
    frame.session = api::SessionKey{3, 99};
    frame.panel.visible = true;
    frame.panel.scale = 1.25;
    frame.panel.skinId = "org.test";
    frame.panel.skinGeneration = 4;
    frame.panel.styleId = "h1";
    frame.panel.mode = "dark";
    frame.panel.anchor.cursor = api::Rect{10.0, 20.0, 0.0, 18.0};
    frame.panel.anchor.offsetX = 2.0;
    frame.panel.anchor.valid = true;
    frame.panel.preedit.text = "ni";
    frame.panel.preedit.cursor = 2;
    frame.panel.preedit.segments.push_back(api::TextSegment{0, 2, "underline"});
    frame.panel.candidates.items.push_back(api::Candidate{11, "1", "你", "ni"});
    frame.panel.candidates.items.push_back(api::Candidate{12, "2", "泥", ""});
    frame.panel.candidates.pageIndex = 0;
    frame.panel.candidates.pageCount = 1;
    frame.panel.candidates.hasNext = false;
    frame.panel.selected = 11;
    frame.panel.dragBounds = api::Rect{0.0, 0.0, 200.0, 40.0};
    frame.status.visible = true;
    frame.status.placement = api::StatusPlacement::TopLeft;
    frame.status.skinId = "org.test";
    frame.status.skinGeneration = 4;
    frame.status.items.push_back(api::StatusItem{"ime", "CN", "icon", true, true});
    frame.status.items.push_back(api::StatusItem{"punctuation", "\u5168\u89d2\u6807\u70b9", "", true,
                                                 true, api::StatusRole::Punctuation});
    frame.transition = api::TransitionSpec{"panel.show", 1234, 0.0, 1.0};
    return frame;
}

void testSceneRoundTrip() {
    const auto original = sampleScene();
    const std::string payload = protocol::encodeScene(original);
    DecodeError error;
    const auto decoded = protocol::decodeScene(payload, error);
    CHECK(decoded.has_value());
    CHECK(error.ok());
    if (!decoded) {
        return;
    }
    CHECK(decoded->frameId == original.frameId);
    CHECK(decoded->session.epoch == 3);
    CHECK(decoded->session.context == 99);
    CHECK(decoded->panel.visible);
    CHECK(superior::test::near(decoded->panel.scale, 1.25));
    CHECK(decoded->panel.preedit.text == "ni");
    CHECK(decoded->panel.preedit.segments.size() == 1);
    CHECK(decoded->panel.candidates.items.size() == 2);
    CHECK(decoded->panel.candidates.items.front().text == "你");
    CHECK(decoded->panel.selected == 11);
    CHECK(decoded->panel.dragBounds.has_value());
    CHECK(decoded->status.items.size() == 2);
    CHECK(decoded->status.items.front().role == api::StatusRole::Text);
    CHECK(decoded->status.items.back().role == api::StatusRole::Punctuation);
    CHECK(decoded->status.placement == api::StatusPlacement::TopLeft);
    CHECK(decoded->transition.has_value());
    CHECK(decoded->transition->motion == "panel.show");
    CHECK(decoded->transition->startMs == 1234);
}

/// The speech panel is part of the scene, so it must survive the same
/// round-trip as the candidate panel and the toolbar.
void testSpeechSceneRoundTrip() {
    api::UiSceneFrame original;
    original.frameId = 9;
    original.speech.visible = true;
    original.speech.phase = api::SpeechPhase::NeedsSetup;
    original.speech.modelId = "qwen3-asr-0.6b";
    original.speech.modelName = "Qwen3-ASR-0.6B";
    original.speech.modelBytes = 631000000;
    original.speech.modelPresent = false;
    original.speech.enginePresent = true;
    original.speech.message = "请先下载模型";
    original.speech.skinId = "org.superior.default";
    original.speech.skinGeneration = 12;

    DecodeError error;
    const auto decoded = protocol::decodeScene(protocol::encodeScene(original), error);
    CHECK(decoded.has_value());
    CHECK(error.ok());
    if (!decoded) {
        return;
    }
    CHECK(decoded->speech.visible);
    CHECK(decoded->speech.phase == api::SpeechPhase::NeedsSetup);
    CHECK(decoded->speech.modelId == "qwen3-asr-0.6b");
    CHECK(decoded->speech.modelBytes == 631000000);
    CHECK(!decoded->speech.modelPresent);
    CHECK(decoded->speech.enginePresent);
    CHECK(decoded->speech.message == "请先下载模型");
    CHECK(decoded->speech.skinGeneration == 12);

    // A frame from an older agent has no `speech` object at all: the panel must
    // default to hidden instead of failing the whole frame.
    const std::string withoutSpeech =
        R"({"frameId":1,"session":{"epoch":0,"context":0},)"
        R"("panel":{"anchor":{"cursor":{"x":0,"y":0,"width":0,"height":0}}},"status":{}})";
    const auto fallback = protocol::decodeScene(withoutSpeech, error);
    CHECK(fallback.has_value());
    if (fallback) {
        CHECK(!fallback->speech.visible);
        CHECK(fallback->speech.phase == api::SpeechPhase::Hidden);
    }
}

void testSceneRejectsGarbage() {
    DecodeError error;
    CHECK(!protocol::decodeScene("{}", error).has_value());
    CHECK(hasCode(error, "wire.field"));
}

void testAllCommandsRoundTrip() {
    const api::SessionKey session{1, 2};
    const std::vector<api::UiCommand> commands{
        api::ActivateCandidate{session, 9},
        api::PageCandidates{session, -1},
        api::SelectIndex{session, 3},
        api::SetInputMode{session, api::InputMode::English},
        api::ToggleStatusItem{"ime"},
        api::SetSkin{"org.test"},
        api::SetUiScale{1.5},
        api::MovePanel{session, 4.0, -2.0},
        api::ResetPanelPosition{session},
        api::SpeechStart{session, "default", "zh-CN"},
        api::SpeechStop{session, false},
        api::SpeechCancel{session},
        api::SpeechReload{},
        api::RequestSettings{"appearance"},
        api::ShellReady{"superior-ui", "0.1.0", api::UiProfile::Desktop, {"x11"}},
        api::ShellLost{"disconnected"},
    };

    for (const auto &command : commands) {
        const std::string payload = protocol::encodeCommand(command);
        DecodeError error;
        const auto decoded = protocol::decodeCommand(payload, error);
        CHECK(decoded.has_value());
        CHECK(error.ok());
        if (!decoded) {
            continue;
        }
        CHECK(decoded->index() == command.index());
        CHECK(protocol::encodeCommand(*decoded) == payload);
    }
}

void testUnknownCommandRejected() {
    DecodeError error;
    CHECK(!protocol::decodeCommand(R"({"kind":"nope"})", error).has_value());
    CHECK(hasCode(error, "wire.field"));
}

} // namespace

int main() {
    testFrameRoundTrip();
    testFrameRejectsBadInput();
    testStreamingDecode();
    testHelloRoundTrip();
    testSceneRoundTrip();
    testSpeechSceneRoundTrip();
    testSceneRejectsGarbage();
    testAllCommandsRoundTrip();
    testUnknownCommandRejected();
    return superior::test::finish("protocol tests");
}
