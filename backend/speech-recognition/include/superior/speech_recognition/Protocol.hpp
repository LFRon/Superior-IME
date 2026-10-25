// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace superior::speech_recognition {

/// Everything the recogniser needs to start a session.
struct EngineConfig {
    std::string enginePath;
    std::string modelPath;
    std::string language = "zh";
    int threads = 4;
};

/// agent -> recogniser, one JSON object per line.
struct Command {
    enum class Kind {
        Configure,
        Start,
        Stop,
        Cancel,
        Quit,
    };

    Kind kind = Kind::Quit;
    EngineConfig config;
};

/// recogniser -> agent, one JSON object per line.
struct Event {
    enum class Kind {
        Ready,
        Phase,
        Level,
        Partial,
        Final,
        Error,
    };

    Kind kind = Kind::Ready;
    /// `recording`, `idle` — only meaningful for `Phase`.
    std::string phase;
    /// 0..1, only meaningful for `Level`.
    double level = 0.0;
    /// Transcript, only meaningful for `Partial` / `Final`.
    std::string text;
    /// Stable machine-readable error code, for `Error`.
    std::string code;
    /// Human-readable detail, for `Error`.
    std::string message;
};

[[nodiscard]] std::string encodeEvent(const Event &event);

/// Decodes one line. Returns `nullopt` for malformed input so a bad line can be
/// reported and skipped instead of killing the session.
[[nodiscard]] std::optional<Command> decodeCommand(std::string_view line);

} // namespace superior::speech_recognition
