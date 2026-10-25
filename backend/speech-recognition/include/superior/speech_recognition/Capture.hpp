// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace superior::speech_recognition {

/// How to start audio capture. Output is raw signed 16-bit little-endian PCM on
/// stdout; the header is written by us, so a tool that silently switches to WAV
/// cannot corrupt the stream.
struct CaptureCommand {
    std::string program;
    std::vector<std::string> args;
};

/// Picks `pw-record`, then `parec`, then `arecord`, whichever is installed.
[[nodiscard]] CaptureCommand defaultCaptureCommand();

/// Builds the argv for one named tool. Exposed so the argument contract can be
/// tested without a sound server.
[[nodiscard]] CaptureCommand buildCaptureCommand(std::string_view tool, int rate = 16000,
                                                 int channels = 1);

/// True when at least one capture tool exists on PATH.
[[nodiscard]] bool captureAvailable();

/// Root-mean-square of a PCM block, normalised to 0..1.
[[nodiscard]] double rmsLevel(const std::int16_t *samples, std::size_t count) noexcept;

/// Writes a mono 16-bit PCM buffer as a `.wav` file. Returns false on any I/O
/// error; the caller reports it instead of feeding the engine a broken file.
bool writeWavFile(const std::filesystem::path &path, const std::vector<std::int16_t> &pcm,
                  int rate = 16000);

} // namespace superior::speech_recognition
