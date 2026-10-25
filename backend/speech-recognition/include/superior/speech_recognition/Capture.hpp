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
/// `target` is honoured when the tool can name a device.
[[nodiscard]] CaptureCommand defaultCaptureCommand(std::string_view target = {});

/// Builds the argv for one named tool. Exposed so the argument contract can be
/// tested without a sound server.
///
/// `target` pins the stream to one device. That matters because the desktop
/// re-elects the default input in the middle of a session: without a target we
/// would follow the flip and record from the wrong microphone.
[[nodiscard]] CaptureCommand buildCaptureCommand(std::string_view tool, int rate = 16000,
                                                 int channels = 1,
                                                 std::string_view target = {});

/// True when at least one capture tool exists on PATH.
[[nodiscard]] bool captureAvailable();

/// Root-mean-square of a PCM block, normalised to 0..1.
[[nodiscard]] double rmsLevel(const std::int16_t *samples, std::size_t count) noexcept;

/// When to end a recording without the user asking.
///
/// Energy-based, because a model-based VAD would mean shipping a second model
/// just to know when to stop listening. The thresholds are deliberately generous:
/// stopping too late costs a little latency, stopping too early truncates a
/// sentence the user already spoke.
struct SilencePolicy {
    /// Level (0..1, same scale as `rmsLevel`) that counts as speech.
    double voiceLevel = 0.06;
    /// Silence after speech that ends the utterance.
    std::uint64_t holdMs = 1200;
    /// Ignore a short burst: a cough or a click must not end the recording.
    std::uint64_t minSpeechMs = 300;
    /// Absolute cap, so a forgotten panel cannot record forever.
    std::uint64_t maxRecordingMs = 60000;
    /// How long to wait for the first word before giving up. Without this a user
    /// who pressed the button and changed their mind waits a full minute.
    std::uint64_t noSpeechTimeoutMs = 8000;
};

/// Tracks level samples and answers one question: is this utterance over?
///
/// Kept free of I/O and clocks so the policy can be unit-tested with synthetic
/// timelines; `Service` feeds it `steady_clock` milliseconds.
class SilenceWatch {
public:
    explicit SilenceWatch(SilencePolicy policy = {});

    void begin(std::uint64_t nowMs);
    void observe(double level, std::uint64_t nowMs);

    /// True once the utterance is over: enough speech followed by enough silence,
    /// or the absolute cap was hit.
    [[nodiscard]] bool finished(std::uint64_t nowMs) const;

    /// Whether any speech was heard at all. Drives the "you said nothing" hint.
    [[nodiscard]] bool heardSpeech() const noexcept { return voicedMs_ > 0; }

    [[nodiscard]] std::uint64_t elapsedMs(std::uint64_t nowMs) const;

private:
    SilencePolicy policy_;
    std::uint64_t startMs_ = 0;
    std::uint64_t lastObserveMs_ = 0;
    std::uint64_t lastVoiceMs_ = 0;
    std::uint64_t voicedMs_ = 0;
    bool begun_ = false;
};

/// Name of the sound server's current default input, or an empty string when it
/// cannot be read (no sound server, no `pactl`, a restricted sandbox).
[[nodiscard]] std::string defaultSourceName();

/// Names of every input the sound server currently offers.
[[nodiscard]] std::vector<std::string> availableSources();

/// Points the sound server's default input at `name`. False when the command
/// failed, which is only ever logged.
bool setDefaultSource(std::string_view name);

/// Should the default input be put back after a recording?
///
/// WirePlumber re-elects the default input about two seconds after *any* capture
/// stream ends: with the headset selected it has been observed flipping to the
/// internal microphone and, with the internal microphone selected, flipping to
/// the headset. It happens for `parec` and for `pw-record --target=…` alike, and
/// it ignores `default.configured.audio.source`, so it is the desktop's policy,
/// not our choice of device. Restoring the user's own selection afterwards is the
/// only thing an input method can do about it.
///
/// The rules are deliberately conservative: nothing is restored unless the name
/// is known, the device still exists, and the value actually changed. A user who
/// switches device *during* a recording therefore keeps their new choice.
[[nodiscard]] bool shouldRestoreDefaultSource(const std::string &remembered,
                                              const std::string &current,
                                              const std::vector<std::string> &available);

/// Writes a mono 16-bit PCM buffer as a `.wav` file. Returns false on any I/O
/// error; the caller reports it instead of feeding the engine a broken file.
bool writeWavFile(const std::filesystem::path &path, const std::vector<std::int16_t> &pcm,
                  int rate = 16000);

} // namespace superior::speech_recognition
