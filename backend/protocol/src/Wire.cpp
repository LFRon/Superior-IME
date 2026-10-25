// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/protocol/Wire.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "superior/diagnostics/Log.hpp"

namespace superior::protocol {
namespace {

using Json = nlohmann::json;

constexpr std::array<char, 4> kMagic{'S', 'U', 'P', 'I'};
constexpr std::size_t kMaxPayload = 8U * 1024U * 1024U;

void putU16(std::string &out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void putU32(std::string &out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

std::uint16_t readU16(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[offset]) |
        (static_cast<unsigned char>(bytes[offset + 1]) << 8U));
}

std::uint32_t readU32(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24U);
}

void fail(DecodeError &error, std::string code, std::string detail) {
    if (error.code.empty()) {
        error.code = std::move(code);
        error.detail = std::move(detail);
    }
}

Json rectJson(const api::Rect &rect) {
    return Json{{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

api::Rect rectFrom(const Json &json) {
    api::Rect rect;
    rect.x = json.at("x").get<double>();
    rect.y = json.at("y").get<double>();
    rect.width = json.at("width").get<double>();
    rect.height = json.at("height").get<double>();
    return rect;
}

Json sessionJson(const api::SessionKey &session) {
    return Json{{"epoch", session.epoch}, {"context", session.context}};
}

api::SessionKey sessionFrom(const Json &json) {
    return api::SessionKey{json.at("epoch").get<api::Epoch>(),
                           json.at("context").get<api::ContextId>()};
}

Json preeditJson(const api::Preedit &preedit) {
    Json segments = Json::array();
    for (const auto &segment : preedit.segments) {
        segments.push_back(
            Json{{"begin", segment.begin}, {"end", segment.end}, {"attribute", segment.attribute}});
    }
    return Json{{"text", preedit.text}, {"cursor", preedit.cursor}, {"segments", segments}};
}

api::Preedit preeditFrom(const Json &json) {
    api::Preedit preedit;
    preedit.text = json.value("text", std::string{});
    preedit.cursor = json.value("cursor", std::size_t{0});
    if (json.contains("segments")) {
        for (const auto &entry : json.at("segments")) {
            api::TextSegment segment;
            segment.begin = entry.at("begin").get<std::size_t>();
            segment.end = entry.at("end").get<std::size_t>();
            segment.attribute = entry.value("attribute", std::string{});
            preedit.segments.push_back(std::move(segment));
        }
    }
    return preedit;
}

Json candidateJson(const api::Candidate &candidate) {
    return Json{{"id", candidate.id},
                {"label", candidate.label},
                {"text", candidate.text},
                {"comment", candidate.comment}};
}

api::Candidate candidateFrom(const Json &json) {
    api::Candidate candidate;
    candidate.id = json.at("id").get<api::CandidateId>();
    candidate.label = json.value("label", std::string{});
    candidate.text = json.value("text", std::string{});
    candidate.comment = json.value("comment", std::string{});
    return candidate;
}

Json pageJson(const api::CandidatePage &page) {
    Json items = Json::array();
    for (const auto &candidate : page.items) {
        items.push_back(candidateJson(candidate));
    }
    return Json{{"items", items},
                {"pageIndex", page.pageIndex},
                {"pageCount", page.pageCount},
                {"hasPrevious", page.hasPrevious},
                {"hasNext", page.hasNext}};
}

api::CandidatePage pageFrom(const Json &json) {
    api::CandidatePage page;
    page.pageIndex = json.value("pageIndex", 0);
    page.pageCount = json.value("pageCount", 0);
    page.hasPrevious = json.value("hasPrevious", false);
    page.hasNext = json.value("hasNext", false);
    if (json.contains("items")) {
        for (const auto &entry : json.at("items")) {
            page.items.push_back(candidateFrom(entry));
        }
    }
    return page;
}

const char *placementName(api::StatusPlacement placement) {
    switch (placement) {
    case api::StatusPlacement::BottomRight:
        return "bottomRight";
    case api::StatusPlacement::BottomLeft:
        return "bottomLeft";
    case api::StatusPlacement::TopRight:
        return "topRight";
    case api::StatusPlacement::TopLeft:
        return "topLeft";
    }
    return "bottomRight";
}

api::StatusPlacement placementFrom(std::string_view name) {
    if (name == "bottomLeft") {
        return api::StatusPlacement::BottomLeft;
    }
    if (name == "topRight") {
        return api::StatusPlacement::TopRight;
    }
    if (name == "topLeft") {
        return api::StatusPlacement::TopLeft;
    }
    return api::StatusPlacement::BottomRight;
}

const char *statusRoleName(api::StatusRole role) {
    switch (role) {
    case api::StatusRole::Punctuation: return "punctuation";
    case api::StatusRole::Speech: return "speech";
    case api::StatusRole::Text: break;
    }
    return "text";
}

api::StatusRole statusRoleFrom(std::string_view name) {
    // Unknown roles degrade to text: a newer agent talking to an older UI must
    // not make an item disappear or render as an empty box.
    if (name == "punctuation") {
        return api::StatusRole::Punctuation;
    }
    if (name == "speech") {
        return api::StatusRole::Speech;
    }
    return api::StatusRole::Text;
}

Json statusItemJson(const api::StatusItem &item) {
    return Json{{"id", item.id},
                {"label", item.label},
                {"icon", item.icon},
                {"active", item.active},
                {"enabled", item.enabled},
                {"role", statusRoleName(item.role)}};
}

api::StatusItem statusItemFrom(const Json &json) {
    api::StatusItem item;
    item.id = json.at("id").get<std::string>();
    item.label = json.value("label", std::string{});
    item.icon = json.value("icon", std::string{});
    item.active = json.value("active", false);
    item.enabled = json.value("enabled", true);
    item.role = statusRoleFrom(json.value("role", std::string{"text"}));
    return item;
}

api::SpeechPhase speechPhaseFrom(std::string_view name) {
    if (name == "needs-setup") {
        return api::SpeechPhase::NeedsSetup;
    }
    if (name == "ready") {
        return api::SpeechPhase::Ready;
    }
    if (name == "recording") {
        return api::SpeechPhase::Recording;
    }
    if (name == "transcribing") {
        return api::SpeechPhase::Transcribing;
    }
    if (name == "cancelling") {
        return api::SpeechPhase::Cancelling;
    }
    if (name == "failed") {
        return api::SpeechPhase::Failed;
    }
    return api::SpeechPhase::Hidden;
}

const char *profileName(api::UiProfile profile) {
    return profile == api::UiProfile::Mobile ? "mobile" : "desktop";
}

api::UiProfile profileFrom(std::string_view name) {
    return name == "mobile" ? api::UiProfile::Mobile : api::UiProfile::Desktop;
}

const char *inputModeName(api::InputMode mode) {
    return mode == api::InputMode::English ? "english" : "chinese";
}

api::InputMode inputModeFrom(std::string_view name) {
    return name == "english" ? api::InputMode::English : api::InputMode::Chinese;
}

template <typename Type, typename Function>
std::optional<Type> guarded(Function &&function, DecodeError &error) {
    try {
        return function();
    } catch (const std::exception &exception) {
        fail(error, "wire.field", exception.what());
        return std::nullopt;
    }
}

} // namespace

std::string_view messageTypeName(MessageType type) noexcept {
    switch (type) {
    case MessageType::Hello:
        return "hello";
    case MessageType::Scene:
        return "scene";
    case MessageType::Command:
        return "command";
    case MessageType::Ping:
        return "ping";
    case MessageType::Pong:
        return "pong";
    }
    return "unknown";
}

std::string encodeFrame(MessageType type, std::string_view payload) {
    std::string frame;
    frame.reserve(kHeaderSize + payload.size());
    frame.append(kMagic.data(), kMagic.size());
    putU16(frame, kProtocolVersion);
    putU16(frame, static_cast<std::uint16_t>(type));
    putU32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.append(payload);
    return frame;
}

std::optional<WireMessage> decodeFrame(std::string_view bytes, DecodeError &error) {
    std::size_t consumed = 0;
    WireMessage message;
    const auto status = tryDecodeFrame(bytes, consumed, message, error);
    if (status == FrameStatus::NeedMore) {
        fail(error, "wire.truncated", "frame is incomplete");
        return std::nullopt;
    }
    if (status == FrameStatus::Invalid) {
        return std::nullopt;
    }
    if (consumed != bytes.size()) {
        fail(error, "wire.trailing", "unexpected trailing bytes after frame");
        return std::nullopt;
    }
    return message;
}

FrameStatus tryDecodeFrame(std::string_view buffer, std::size_t &consumed, WireMessage &message,
                           DecodeError &error) {
    consumed = 0;
    const std::size_t magicPrefix = std::min(buffer.size(), kMagic.size());
    for (std::size_t i = 0; i < magicPrefix; ++i) {
        if (buffer[i] != kMagic[i]) {
            fail(error, "wire.magic", "not a Superior-IME frame");
            return FrameStatus::Invalid;
        }
    }
    if (buffer.size() < kHeaderSize) {
        return FrameStatus::NeedMore;
    }

    const std::uint16_t version = readU16(buffer, 4);
    if (version != kProtocolVersion) {
        fail(error, "wire.version", "unsupported protocol version");
        return FrameStatus::Invalid;
    }

    const std::uint16_t rawType = readU16(buffer, 6);
    if (rawType < static_cast<std::uint16_t>(MessageType::Hello) ||
        rawType > static_cast<std::uint16_t>(MessageType::Pong)) {
        fail(error, "wire.type", "unknown message type");
        return FrameStatus::Invalid;
    }

    const std::uint32_t payloadSize = readU32(buffer, 8);
    if (payloadSize > kMaxPayload) {
        fail(error, "wire.length", "payload exceeds the 8 MiB limit");
        return FrameStatus::Invalid;
    }
    if (buffer.size() < kHeaderSize + payloadSize) {
        return FrameStatus::NeedMore;
    }

    message.type = static_cast<MessageType>(rawType);
    message.payload.assign(buffer.substr(kHeaderSize, payloadSize));
    consumed = kHeaderSize + payloadSize;

    SUPERIOR_TRACE(::superior::diagnostics::categories::Protocol)
        .field("type", messageTypeName(message.type))
        .field("bytes", static_cast<std::uint64_t>(consumed))
        << "frame decoded";

    return FrameStatus::Complete;
}

std::string encodeHello(const HelloMessage &hello) {
    Json capabilities{
        {"profile", profileName(hello.capabilities.profile)},
        {"hover", hello.capabilities.hover},
        {"rightClickMenu", hello.capabilities.rightClickMenu},
        {"multiScreen", hello.capabilities.multiScreen},
        {"layerShell", hello.capabilities.layerShell},
        {"inputPopupSurface", hello.capabilities.inputPopupSurface},
        {"touch", hello.capabilities.touch},
        {"safeArea", hello.capabilities.safeArea},
        {"prefersReducedMotion", hello.capabilities.prefersReducedMotion},
        {"scale", hello.capabilities.scale},
    };
    Json shell{
        {"name", hello.shell.name},
        {"version", hello.shell.version},
        {"profile", profileName(hello.shell.profile)},
        {"windowBackends", hello.shell.windowBackends},
    };
    return Json{{"shell", std::move(shell)}, {"capabilities", std::move(capabilities)}}.dump();
}

std::optional<HelloMessage> decodeHello(std::string_view payload, DecodeError &error) {
    return guarded<HelloMessage>(
        [payload] {
            const Json json = Json::parse(payload);
            HelloMessage hello;
            const auto &shell = json.at("shell");
            hello.shell.name = shell.value("name", std::string{});
            hello.shell.version = shell.value("version", std::string{});
            hello.shell.profile = profileFrom(shell.value("profile", std::string{"desktop"}));
            hello.shell.windowBackends =
                shell.value("windowBackends", std::vector<std::string>{});
            const auto &capabilities = json.at("capabilities");
            hello.capabilities.profile =
                profileFrom(capabilities.value("profile", std::string{"desktop"}));
            hello.capabilities.hover = capabilities.value("hover", true);
            hello.capabilities.rightClickMenu = capabilities.value("rightClickMenu", true);
            hello.capabilities.multiScreen = capabilities.value("multiScreen", true);
            hello.capabilities.layerShell = capabilities.value("layerShell", false);
            hello.capabilities.inputPopupSurface =
                capabilities.value("inputPopupSurface", false);
            hello.capabilities.touch = capabilities.value("touch", false);
            hello.capabilities.safeArea = capabilities.value("safeArea", false);
            hello.capabilities.prefersReducedMotion =
                capabilities.value("prefersReducedMotion", false);
            hello.capabilities.scale = capabilities.value("scale", 1.0);
            return hello;
        },
        error);
}

std::string encodeScene(const api::UiSceneFrame &frame) {
    Json panel{
        {"visible", frame.panel.visible},
        {"scale", frame.panel.scale},
        {"skinId", frame.panel.skinId},
        {"skinGeneration", frame.panel.skinGeneration},
        {"styleId", frame.panel.styleId},
        {"mode", frame.panel.mode},
        {"preedit", preeditJson(frame.panel.preedit)},
        {"candidates", pageJson(frame.panel.candidates)},
        {"selected", frame.panel.selected},
    };
    panel["anchor"] = Json{{"cursor", rectJson(frame.panel.anchor.cursor)},
                           {"offsetX", frame.panel.anchor.offsetX},
                           {"offsetY", frame.panel.anchor.offsetY},
                           {"valid", frame.panel.anchor.valid}};
    if (frame.panel.dragBounds) {
        panel["dragBounds"] = rectJson(*frame.panel.dragBounds);
    }

    Json items = Json::array();
    for (const auto &item : frame.status.items) {
        items.push_back(statusItemJson(item));
    }
    Json status{{"visible", frame.status.visible},
                {"placement", placementName(frame.status.placement)},
                {"skinId", frame.status.skinId},
                {"skinGeneration", frame.status.skinGeneration},
                {"items", std::move(items)}};

    Json speech{{"visible", frame.speech.visible},
                {"phase", api::speechPhaseName(frame.speech.phase)},
                {"modelId", frame.speech.modelId},
                {"modelName", frame.speech.modelName},
                {"modelBytes", frame.speech.modelBytes},
                {"modelPresent", frame.speech.modelPresent},
                {"enginePresent", frame.speech.enginePresent},
                {"message", frame.speech.message},
                {"level", frame.speech.level},
                {"partial", frame.speech.partial},
                {"skinId", frame.speech.skinId},
                {"skinGeneration", frame.speech.skinGeneration}};

    Json json{{"frameId", frame.frameId},
              {"session", sessionJson(frame.session)},
              {"panel", std::move(panel)},
              {"status", std::move(status)},
              {"speech", std::move(speech)}};
    if (frame.transition) {
        json["transition"] = Json{{"motion", frame.transition->motion},
                                  {"startMs", frame.transition->startMs},
                                  {"fromOpacity", frame.transition->fromOpacity},
                                  {"toOpacity", frame.transition->toOpacity}};
    }
    return json.dump();
}

std::optional<api::UiSceneFrame> decodeScene(std::string_view payload, DecodeError &error) {
    return guarded<api::UiSceneFrame>(
        [payload] {
            const Json json = Json::parse(payload);
            api::UiSceneFrame frame;
            frame.frameId = json.at("frameId").get<api::FrameId>();
            frame.session = sessionFrom(json.at("session"));

            const auto &panel = json.at("panel");
            frame.panel.visible = panel.value("visible", false);
            frame.panel.scale = panel.value("scale", 1.0);
            frame.panel.skinId = panel.value("skinId", std::string{});
            frame.panel.skinGeneration = panel.value("skinGeneration", api::SkinGeneration{0});
            frame.panel.styleId = panel.value("styleId", std::string{});
            frame.panel.mode = panel.value("mode", std::string{});
            const auto &anchor = panel.at("anchor");
            frame.panel.anchor.cursor = rectFrom(anchor.at("cursor"));
            frame.panel.anchor.offsetX = anchor.value("offsetX", 0.0);
            frame.panel.anchor.offsetY = anchor.value("offsetY", 0.0);
            frame.panel.anchor.valid = anchor.value("valid", false);
            if (panel.contains("dragBounds")) {
                frame.panel.dragBounds = rectFrom(panel.at("dragBounds"));
            }
            if (panel.contains("preedit")) {
                frame.panel.preedit = preeditFrom(panel.at("preedit"));
            }
            if (panel.contains("candidates")) {
                frame.panel.candidates = pageFrom(panel.at("candidates"));
            }
            frame.panel.selected = panel.value("selected", api::CandidateId{0});

            const auto &status = json.at("status");
            frame.status.visible = status.value("visible", false);
            frame.status.placement =
                placementFrom(status.value("placement", std::string{"bottomRight"}));
            frame.status.skinId = status.value("skinId", std::string{});
            frame.status.skinGeneration =
                status.value("skinGeneration", api::SkinGeneration{0});
            if (status.contains("items")) {
                for (const auto &entry : status.at("items")) {
                    frame.status.items.push_back(statusItemFrom(entry));
                }
            }

            if (json.contains("speech")) {
                const auto &speech = json.at("speech");
                frame.speech.visible = speech.value("visible", false);
                frame.speech.phase = speechPhaseFrom(speech.value("phase", std::string{"hidden"}));
                frame.speech.modelId = speech.value("modelId", std::string{});
                frame.speech.modelName = speech.value("modelName", std::string{});
                frame.speech.modelBytes = speech.value("modelBytes", std::uint64_t{0});
                frame.speech.modelPresent = speech.value("modelPresent", false);
                frame.speech.enginePresent = speech.value("enginePresent", false);
                frame.speech.message = speech.value("message", std::string{});
                frame.speech.level = speech.value("level", 0.0);
                frame.speech.partial = speech.value("partial", std::string{});
                frame.speech.skinId = speech.value("skinId", std::string{});
                frame.speech.skinGeneration =
                    speech.value("skinGeneration", api::SkinGeneration{0});
            }

            if (json.contains("transition")) {
                const auto &transition = json.at("transition");
                api::TransitionSpec spec;
                spec.motion = transition.value("motion", std::string{});
                spec.startMs = transition.value("startMs", std::uint64_t{0});
                spec.fromOpacity = transition.value("fromOpacity", 1.0);
                spec.toOpacity = transition.value("toOpacity", 1.0);
                frame.transition = std::move(spec);
            }
            return frame;
        },
        error);
}

std::string encodeCommand(const api::UiCommand &command) {
    return std::visit(
        [](const auto &value) -> std::string {
            using Type = std::decay_t<decltype(value)>;
            Json json;
            if constexpr (std::is_same_v<Type, api::ActivateCandidate>) {
                json = Json{{"kind", "activateCandidate"},
                            {"session", sessionJson(value.session)},
                            {"candidate", value.candidate}};
            } else if constexpr (std::is_same_v<Type, api::PageCandidates>) {
                json = Json{{"kind", "pageCandidates"},
                            {"session", sessionJson(value.session)},
                            {"delta", value.delta}};
            } else if constexpr (std::is_same_v<Type, api::SelectIndex>) {
                json = Json{{"kind", "selectIndex"},
                            {"session", sessionJson(value.session)},
                            {"index", value.index}};
            } else if constexpr (std::is_same_v<Type, api::SetInputMode>) {
                json = Json{{"kind", "setInputMode"},
                            {"session", sessionJson(value.session)},
                            {"mode", inputModeName(value.mode)}};
            } else if constexpr (std::is_same_v<Type, api::ToggleStatusItem>) {
                json = Json{{"kind", "toggleStatusItem"}, {"itemId", value.itemId}};
            } else if constexpr (std::is_same_v<Type, api::SetSkin>) {
                json = Json{{"kind", "setSkin"}, {"skinId", value.skinId}};
            } else if constexpr (std::is_same_v<Type, api::SetUiScale>) {
                json = Json{{"kind", "setUiScale"}, {"scale", value.scale}};
            } else if constexpr (std::is_same_v<Type, api::MovePanel>) {
                json = Json{{"kind", "movePanel"},
                            {"session", sessionJson(value.session)},
                            {"deltaX", value.deltaX},
                            {"deltaY", value.deltaY}};
            } else if constexpr (std::is_same_v<Type, api::ResetPanelPosition>) {
                json = Json{{"kind", "resetPanelPosition"},
                            {"session", sessionJson(value.session)}};
            } else if constexpr (std::is_same_v<Type, api::SpeechStart>) {
                json = Json{{"kind", "speechStart"},
                            {"session", sessionJson(value.session)},
                            {"deviceId", value.deviceId},
                            {"language", value.language}};
            } else if constexpr (std::is_same_v<Type, api::SpeechStop>) {
                json = Json{{"kind", "speechStop"},
                            {"session", sessionJson(value.session)},
                            {"commit", value.commit}};
            } else if constexpr (std::is_same_v<Type, api::SpeechCancel>) {
                json = Json{{"kind", "speechCancel"}, {"session", sessionJson(value.session)}};
            } else if constexpr (std::is_same_v<Type, api::SpeechReload>) {
                json = Json{{"kind", "speechReload"}};
            } else if constexpr (std::is_same_v<Type, api::RequestSettings>) {
                json = Json{{"kind", "requestSettings"}, {"page", value.page}};
            } else if constexpr (std::is_same_v<Type, api::ShellReady>) {
                json = Json{{"kind", "shellReady"},
                            {"shellName", value.shellName},
                            {"shellVersion", value.shellVersion},
                            {"profile", profileName(value.profile)},
                            {"windowBackends", value.windowBackends}};
            } else if constexpr (std::is_same_v<Type, api::ShellLost>) {
                json = Json{{"kind", "shellLost"}, {"reason", value.reason}};
            }
            return json.dump();
        },
        command);
}

std::optional<api::UiCommand> decodeCommand(std::string_view payload, DecodeError &error) {
    return guarded<api::UiCommand>(
        [payload]() -> api::UiCommand {
            const Json json = Json::parse(payload);
            const std::string kind = json.at("kind").get<std::string>();
            if (kind == "activateCandidate") {
                return api::ActivateCandidate{sessionFrom(json.at("session")),
                                              json.at("candidate").get<api::CandidateId>()};
            }
            if (kind == "pageCandidates") {
                return api::PageCandidates{sessionFrom(json.at("session")),
                                           json.at("delta").get<int>()};
            }
            if (kind == "selectIndex") {
                return api::SelectIndex{sessionFrom(json.at("session")),
                                        json.at("index").get<int>()};
            }
            if (kind == "setInputMode") {
                return api::SetInputMode{
                    sessionFrom(json.at("session")),
                    inputModeFrom(json.at("mode").get<std::string>())};
            }
            if (kind == "toggleStatusItem") {
                return api::ToggleStatusItem{json.at("itemId").get<std::string>()};
            }
            if (kind == "setSkin") {
                return api::SetSkin{json.at("skinId").get<std::string>()};
            }
            if (kind == "setUiScale") {
                return api::SetUiScale{json.at("scale").get<double>()};
            }
            if (kind == "movePanel") {
                return api::MovePanel{sessionFrom(json.at("session")),
                                      json.at("deltaX").get<double>(),
                                      json.at("deltaY").get<double>()};
            }
            if (kind == "resetPanelPosition") {
                return api::ResetPanelPosition{sessionFrom(json.at("session"))};
            }
            if (kind == "speechStart") {
                return api::SpeechStart{sessionFrom(json.at("session")),
                                       json.value("deviceId", std::string{}),
                                       json.value("language", std::string{"zh-CN"})};
            }
            if (kind == "speechStop") {
                return api::SpeechStop{sessionFrom(json.at("session")),
                                      json.value("commit", true)};
            }
            if (kind == "speechCancel") {
                return api::SpeechCancel{sessionFrom(json.at("session"))};
            }
            if (kind == "speechReload") {
                return api::SpeechReload{};
            }
            if (kind == "requestSettings") {
                return api::RequestSettings{json.value("page", std::string{})};
            }
            if (kind == "shellReady") {
                return api::ShellReady{json.value("shellName", std::string{}),
                                       json.value("shellVersion", std::string{}),
                                       profileFrom(json.value("profile", std::string{"desktop"})),
                                       json.value("windowBackends", std::vector<std::string>{})};
            }
            if (kind == "shellLost") {
                return api::ShellLost{json.value("reason", std::string{})};
            }
            throw std::runtime_error("unknown command kind '" + kind + "'");
        },
        error);
}

} // namespace superior::protocol
