// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "superior/speech_recognition/Protocol.hpp"

namespace superior::speech_recognition {

/// One external command: the recogniser never links a model runtime.
///
/// The engine is a separate program (crispasr today) invoked once per utterance.
/// Keeping it out of process means the UI and the input method stay responsive
/// while a 600 MB model decodes, and a crash cannot take the keyboard down.
struct EngineCommand {
    std::string program;
    std::vector<std::string> args;
};

/// Builds the recogniser invocation for one audio file.
///
/// `SUPERIOR_ASR_CMD` overrides the whole command line and supports
/// `{model}`, `{wav}`, `{threads}`, `{language}` and `{backend}` placeholders —
/// that is how a distribution, a test or a user with a different engine plugs in
/// without touching the input method.
[[nodiscard]] std::optional<EngineCommand> buildEngineCommand(const EngineConfig &config,
                                                              const std::filesystem::path &wav,
                                                              std::string &error);

/// Runs the command with stdout captured; returns the trimmed transcript.
///
/// Blocking: callers run it on a worker thread. `error` is filled on a non-zero
/// exit, a missing binary or an empty transcript.
[[nodiscard]] std::optional<std::string> runEngine(const EngineCommand &command,
                                                   std::string &error);

/// Splits a command line on spaces, honouring double quotes.
[[nodiscard]] std::vector<std::string> splitCommandLine(std::string_view line);

} // namespace superior::speech_recognition
