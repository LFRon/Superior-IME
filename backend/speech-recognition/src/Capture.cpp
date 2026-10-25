// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

CaptureCommand buildCaptureCommand(std::string_view tool, int rate, int channels,
                                   std::string_view target) {
    const std::string rateText = std::to_string(rate);
    const std::string channelsText = std::to_string(channels);
    if (tool == "parec") {
        // PulseAudio; also works through pipewire-pulse.
        CaptureCommand command{"parec",
                               {"--format=s16le", "--rate=" + rateText,
                                "--channels=" + channelsText, "--raw"}};
        if (!target.empty()) {
            command.args.insert(command.args.begin(), "--device=" + std::string(target));
        }
        return command;
    }
    if (tool == "arecord") {
        // ALSA fallback, for machines without a sound server.
        return CaptureCommand{"arecord",
                              {"-q", "-t", "raw", "-f", "S16_LE", "-r", rateText, "-c",
                               channelsText}};
    }
    // PipeWire default. "-" means stdout, which pw-cat keeps headerless.
    CaptureCommand command{"pw-record",
                           {"--format=s16", "--rate=" + rateText, "--channels=" + channelsText,
                            "-"}};
    if (!target.empty()) {
        command.args.insert(command.args.begin(), "--target=" + std::string(target));
    }
    return command;
}

CaptureCommand defaultCaptureCommand(std::string_view target) {
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
        return buildCaptureCommand("pw-record", 16000, 1, target);
    }
    if (existsInPath("parec")) {
        return buildCaptureCommand("parec", 16000, 1, target);
    }
    // arecord wants an ALSA device name, which is not what the sound server
    // reports, so it deliberately stays on the system default.
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

/// Runs a command and returns its first line of stdout, trimmed.
///
/// `pactl` is the one tool every sound server on Linux accepts (PipeWire keeps a
/// pulse-compatible layer), it is what the desktop's own mixer uses, and the
/// commands below are read-only. A machine without it simply has no default to
/// restore, which the empty return value expresses.
std::string runAndReadLine(const char *command) {
    FILE *pipe = ::popen(command, "r");
    if (pipe == nullptr) {
        return {};
    }
    std::string line;
    char buffer[256];
    if (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line = buffer;
    }
    ::pclose(pipe);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

std::string defaultSourceName() {
    std::string name = runAndReadLine("pactl get-default-source 2>/dev/null");
    if (name.empty()) {
        // Very old pulse servers predate `get-default-source`.
        name = runAndReadLine("pactl info 2>/dev/null | sed -n 's/^Default Source: //p'");
    }
    return name;
}

std::vector<std::string> availableSources() {
    // `short` output: index, name, driver, spec, state.
    FILE *pipe = ::popen("pactl list short sources 2>/dev/null", "r");
    std::vector<std::string> names;
    if (pipe == nullptr) {
        return names;
    }
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        std::size_t pos = line.find('\t');
        if (pos == std::string::npos) {
            continue;
        }
        const std::size_t start = pos + 1;
        const std::size_t end = line.find('\t', start);
        if (end == std::string::npos) {
            continue;
        }
        names.push_back(line.substr(start, end - start));
    }
    ::pclose(pipe);
    return names;
}

bool setDefaultSource(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    // Only ever called with a name that came from the sound server itself, so
    // quoting is about spaces, not about escaping.
    std::string command = "pactl set-default-source '";
    for (const char ch : name) {
        if (ch == '\'') {
            command += "'\\''";
        } else {
            command.push_back(ch);
        }
    }
    command += "' >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

bool shouldRestoreDefaultSource(const std::string &remembered, const std::string &current,
                                const std::vector<std::string> &available) {
    if (remembered.empty() || remembered == current) {
        return false;
    }
    return std::find(available.begin(), available.end(), remembered) != available.end();
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
