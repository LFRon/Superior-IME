// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "superior/api/Capabilities.hpp"
#include "superior/api/Commands.hpp"
#include "superior/api/Scene.hpp"

namespace superior::protocol {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 12;

/// Wire messages are length-delimited frames:
///   "SUPI" | u16 version | u16 type | u32 payload length | payload
/// The payload is UTF-8 JSON in v1. Control traffic is low frequency (typing
/// rate, not frame rate), so a self-describing payload is worth more than a
/// hand-packed binary format. A binary major version can be added later without
/// changing the framing.
enum class MessageType : std::uint16_t {
    Hello = 1,
    Scene = 2,
    Command = 3,
    Ping = 4,
    Pong = 5,
};

struct HelloMessage {
    api::UiShellInfo shell;
    api::UiCapabilities capabilities;
};

struct WireMessage {
    MessageType type = MessageType::Ping;
    std::string payload;
};

struct DecodeError {
    std::string code;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return code.empty(); }
};

enum class FrameStatus {
    Complete,
    NeedMore,
    Invalid,
};

/// Encodes one complete frame.
[[nodiscard]] std::string encodeFrame(MessageType type, std::string_view payload);

/// Decodes exactly one frame; trailing bytes are rejected.
[[nodiscard]] std::optional<WireMessage> decodeFrame(std::string_view bytes, DecodeError &error);

/// Streaming decode for socket buffers. On Complete, `consumed` is the number
/// of bytes belonging to the frame; the caller may retain the remainder.
[[nodiscard]] FrameStatus tryDecodeFrame(std::string_view buffer, std::size_t &consumed,
                                         WireMessage &message, DecodeError &error);

[[nodiscard]] std::string encodeHello(const HelloMessage &hello);
[[nodiscard]] std::optional<HelloMessage> decodeHello(std::string_view payload,
                                                      DecodeError &error);

[[nodiscard]] std::string encodeScene(const api::UiSceneFrame &frame);
[[nodiscard]] std::optional<api::UiSceneFrame> decodeScene(std::string_view payload,
                                                           DecodeError &error);

[[nodiscard]] std::string encodeCommand(const api::UiCommand &command);
[[nodiscard]] std::optional<api::UiCommand> decodeCommand(std::string_view payload,
                                                          DecodeError &error);

[[nodiscard]] std::string_view messageTypeName(MessageType type) noexcept;

} // namespace superior::protocol
