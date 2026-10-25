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

/// Builds the *streaming* invocation: PCM on stdin, JSON events on stdout.
///
/// Streaming is what makes dictation feel like typing: the recogniser reports a
/// partial transcript while the user is still speaking, and promotes a segment to
/// `final` after a short pause. `stepMs` is how much audio each re-decode sees —
/// smaller is more responsive and more expensive.
///
/// `windowMs` is the rolling context the recogniser keeps. It has to cover the
/// whole session: the decoder's draft is a *prefix* of the utterance only while
/// the window has not slid past the beginning. Once it slides, the draft loses its
/// head and no longer starts with the text already typed, which is the one thing
/// the commit logic relies on. Measured with Paraformer-zh on 55 s of audio:
/// a full-length window costs 0.48x realtime, a sliding 10 s window 0.18x — the
/// extra cost buys correct continuous dictation, and it is still twice as fast as
/// the microphone.
///
/// `SUPERIOR_ASR_STREAM_CMD` overrides the whole command line, with the same
/// `{model}`/`{threads}`/`{language}`/`{backend}` placeholders as the batch form,
/// so the pipeline can be tested without a model.
[[nodiscard]] std::optional<EngineCommand> buildStreamingEngineCommand(const EngineConfig &config,
                                                                      std::string &error,
                                                                      int stepMs = 2000,
                                                                      int windowMs = 60000);

/// Runs the command with stdout captured; returns the trimmed transcript.
///
/// Blocking: callers run it on a worker thread. `error` is filled on a non-zero
/// exit, a missing binary or an empty transcript.
[[nodiscard]] std::optional<std::string> runEngine(const EngineCommand &command,
                                                   std::string &error);

/// Cleans one recogniser transcript.
///
/// CTC backends emit special tokens literally — a silence-only decode came back
/// as `<sil>` — and the same model can return a lone `?` once punctuation is
/// restored. Either would be committed into the document as if the user had typed
/// it, so they are removed here, at the boundary, for batch and streaming alike.
/// Returns an empty string when nothing worth committing is left.
[[nodiscard]] std::string cleanTranscript(std::string_view raw);

/// Splits a command line on spaces, honouring double quotes.
[[nodiscard]] std::vector<std::string> splitCommandLine(std::string_view line);

} // namespace superior::speech_recognition
