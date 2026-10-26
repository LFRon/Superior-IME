// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parts of capture that are arithmetic, not platform: the level meter, the
// silence policy and the WAV writer. Everything that talks to a sound server or
// spawns a process lives in `platforms/linux/AudioCapture.cpp` — a capability
// must not know which tool records on this host (`scripts/check-boundaries.sh`).
#include "superior/speech_recognition/Capture.hpp"

#include <cmath>
#include <fstream>

namespace superior::speech_recognition {

namespace fs = std::filesystem;

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

bool SilenceWatch::limitReached(std::uint64_t nowMs) const noexcept {
    return begun_ && policy_.maxRecordingMs > 0 && elapsedMs(nowMs) >= policy_.maxRecordingMs;
}

std::uint64_t SilenceWatch::remainingMs(std::uint64_t nowMs) const noexcept {
    if (!begun_ || policy_.maxRecordingMs == 0) {
        return 0;
    }
    const std::uint64_t elapsed = elapsedMs(nowMs);
    return elapsed >= policy_.maxRecordingMs ? 0 : policy_.maxRecordingMs - elapsed;
}

bool SilenceWatch::finished(std::uint64_t nowMs) const {
    if (!begun_ || nowMs < startMs_) {
        return false;
    }
    if (limitReached(nowMs)) {
        return true;
    }
    // Silence before any speech is not an ending: the user pressed the button and
    // is still gathering their words. It does expire, just much later.
    if (voicedMs_ < policy_.minSpeechMs) {
        return elapsedMs(nowMs) >= policy_.noSpeechTimeoutMs;
    }
    if (!policy_.endOnSilence) {
        return false;
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
