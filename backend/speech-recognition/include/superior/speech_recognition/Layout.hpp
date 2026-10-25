// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace superior::speech_recognition {

/// `$XDG_DATA_HOME/superior-ime/speech-recognition`.
///
/// Named after the capability, next to `pinyin/`: removing one directory removes
/// exactly one capability. `SUPERIOR_SPEECH_DIR` overrides it for tests.
[[nodiscard]] std::filesystem::path dataRoot();

/// Where the engine executable lives: `<dataRoot>/engine/crispasr`.
[[nodiscard]] std::filesystem::path engineDirectory();
[[nodiscard]] std::filesystem::path enginePath();

/// `<dataRoot>/models`.
[[nodiscard]] std::filesystem::path modelsRoot();
/// `<dataRoot>/models/<id>/<file>`; empty when the id is unknown.
[[nodiscard]] std::filesystem::path modelPath(std::string_view id);

/// Optional punctuation model (`models/punctuation/fireredpunc-q4_k.gguf`).
///
/// Empty when it was not downloaded. It is never fetched automatically: a CTC
/// recogniser works without punctuation, and a second surprise download is worse
/// than a missing comma.
[[nodiscard]] std::filesystem::path punctuationModelPath();

/// Silero VAD weights, when a local copy exists (`models/vad/…`), empty otherwise.
///
/// Passing `--vad` without a model path makes the engine fetch these weights from
/// `huggingface.co` into its own cache, and a failed fetch does *not* fall back to
/// "no gating": every recording is then reported as silence, which looks like a
/// dictation that types nothing at all. Our model downloads go through a mirror
/// precisely because huggingface.co is not reachable from everywhere we ship to,
/// so speech gating is only switched on when the weights are already on disk.
/// `SUPERIOR_ASR_VAD_MODEL` overrides the lookup (tests, custom setups).
[[nodiscard]] std::filesystem::path vadModelPath();

/// True when `SUPERIOR_ASR_CMD` replaces the whole engine invocation.
[[nodiscard]] bool customEngineConfigured();

/// True when an engine can be executed: `SUPERIOR_ASR_ENGINE`, the managed
/// copy, or `crispasr` on `PATH`, in that order.
[[nodiscard]] bool enginePresent();
/// The executable that would be used, or empty when none is available.
[[nodiscard]] std::string resolveEngine();

/// True when the model file exists and is non-empty.
[[nodiscard]] bool modelPresent(std::string_view id);

/// Temporary audio path under `$XDG_RUNTIME_DIR/superior-ime/speech-recognition/`.
///
/// Audio never touches the disk outside the runtime directory, and the directory
/// is removed when the process exits.
[[nodiscard]] std::filesystem::path temporaryAudioPath();

} // namespace superior::speech_recognition
