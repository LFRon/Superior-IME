// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Capture.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <unistd.h>

namespace superior::speech_recognition {

namespace fs = std::filesystem;

namespace {

bool existsInPath(std::string_view name) {
    const char *path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }
    std::string_view rest(path);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto entry = rest.substr(0, colon);
        if (!entry.empty()) {
            const fs::path candidate = fs::path(entry) / name;
            if (::access(candidate.c_str(), X_OK) == 0) {
                return true;
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(colon + 1);
    }
    return false;
}

/// Splits on spaces, honouring double quotes (same grammar as the engine
/// command, so one shell-quoting habit works for both).
std::vector<std::string> splitCaptureLine(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (const char ch : line) {
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (ch == ' ' && !quoted) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

} // namespace

CaptureCommand buildCaptureCommand(std::string_view tool, int rate, int channels) {
    const std::string rateText = std::to_string(rate);
    const std::string channelsText = std::to_string(channels);
    if (tool == "parec") {
        // PulseAudio; also works through pipewire-pulse.
        return CaptureCommand{"parec",
                              {"--format=s16le", "--rate=" + rateText,
                               "--channels=" + channelsText, "--raw"}};
    }
    if (tool == "arecord") {
        // ALSA fallback, for machines without a sound server.
        return CaptureCommand{"arecord",
                              {"-q", "-t", "raw", "-f", "S16_LE", "-r", rateText, "-c",
                               channelsText}};
    }
    // PipeWire default. "-" means stdout, which pw-cat keeps headerless.
    return CaptureCommand{"pw-record",
                          {"--format=s16", "--rate=" + rateText, "--channels=" + channelsText,
                           "-"}};
}

CaptureCommand defaultCaptureCommand() {
    // A custom command keeps the capability testable and lets a user plug in a
    // different audio stack without rebuilding.
    if (const char *custom = std::getenv("SUPERIOR_CAPTURE_CMD");
        custom != nullptr && custom[0] != '\0') {
        const auto tokens = splitCaptureLine(custom);
        if (!tokens.empty()) {
            CaptureCommand command;
            command.program = tokens.front();
            command.args.assign(tokens.begin() + 1, tokens.end());
            return command;
        }
    }
    if (existsInPath("pw-record")) {
        return buildCaptureCommand("pw-record");
    }
    if (existsInPath("parec")) {
        return buildCaptureCommand("parec");
    }
    return buildCaptureCommand("arecord");
}

bool captureAvailable() {
    // A configured command is itself the answer: the user may be recording with
    // something we do not know about, and none of the three tools.
    const char *custom = std::getenv("SUPERIOR_CAPTURE_CMD");
    if (custom != nullptr && custom[0] != '\0') {
        return true;
    }
    return existsInPath("pw-record") || existsInPath("parec") || existsInPath("arecord");
}

double rmsLevel(const std::int16_t *samples, std::size_t count) noexcept {
    if (samples == nullptr || count == 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(samples[i]) / 32768.0;
        sum += value * value;
    }
    const double rms = std::sqrt(sum / static_cast<double>(count));
    // Speech sits well below full scale; a little gain makes the meter usable
    // without turning the panel into a solid bar on loud input.
    const double scaled = rms * 3.0;
    return scaled > 1.0 ? 1.0 : scaled;
}

SilenceWatch::SilenceWatch(SilencePolicy policy) : policy_(policy) {}

void SilenceWatch::begin(std::uint64_t nowMs) {
    startMs_ = nowMs;
    lastObserveMs_ = nowMs;
    lastVoiceMs_ = nowMs;
    voicedMs_ = 0;
    begun_ = true;
}

void SilenceWatch::observe(double level, std::uint64_t nowMs) {
    if (!begun_ || nowMs < lastObserveMs_) {
        return;
    }
    const std::uint64_t delta = nowMs - lastObserveMs_;
    lastObserveMs_ = nowMs;
    if (level >= policy_.voiceLevel) {
        lastVoiceMs_ = nowMs;
        voicedMs_ += delta;
    }
}

std::uint64_t SilenceWatch::elapsedMs(std::uint64_t nowMs) const {
    return begun_ && nowMs >= startMs_ ? nowMs - startMs_ : 0;
}

bool SilenceWatch::finished(std::uint64_t nowMs) const {
    if (!begun_ || nowMs < startMs_) {
        return false;
    }
    if (elapsedMs(nowMs) >= policy_.maxRecordingMs) {
        return true;
    }
    // Silence before any speech is not an ending: the user pressed the button and
    // is still gathering their words. It does expire, just much later.
    if (voicedMs_ < policy_.minSpeechMs) {
        return elapsedMs(nowMs) >= policy_.noSpeechTimeoutMs;
    }
    return nowMs >= lastVoiceMs_ && nowMs - lastVoiceMs_ >= policy_.holdMs;
}

bool writeWavFile(const fs::path &path, const std::vector<std::int16_t> &pcm, int rate) {
    if (path.empty()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    const auto dataBytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const auto byteRate = static_cast<std::uint32_t>(rate * 2);
    const auto write32 = [&out](std::uint32_t value) {
        const char bytes[4] = {static_cast<char>(value & 0xFF),
                               static_cast<char>((value >> 8) & 0xFF),
                               static_cast<char>((value >> 16) & 0xFF),
                               static_cast<char>((value >> 24) & 0xFF)};
        out.write(bytes, 4);
    };
    const auto write16 = [&out](std::uint16_t value) {
        const char bytes[2] = {static_cast<char>(value & 0xFF),
                               static_cast<char>((value >> 8) & 0xFF)};
        out.write(bytes, 2);
    };

    out.write("RIFF", 4);
    write32(36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write32(16);          // PCM chunk size
    write16(1);           // format: PCM
    write16(1);           // channels
    write32(static_cast<std::uint32_t>(rate));
    write32(byteRate);
    write16(2);           // block align
    write16(16);          // bits per sample
    out.write("data", 4);
    write32(dataBytes);
    out.write(reinterpret_cast<const char *>(pcm.data()),
              static_cast<std::streamsize>(dataBytes));
    return out.good();
}

} // namespace superior::speech_recognition
