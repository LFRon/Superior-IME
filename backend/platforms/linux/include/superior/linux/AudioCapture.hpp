// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "superior/api/Ports.hpp"

namespace superior::linux {

/// argv for one named capture tool.
///
/// Exposed because the argument contract is the part that breaks silently: a
/// tool that starts writing a WAV header, or one that follows the desktop's
/// device re-election instead of the pinned target, both still "work" and both
/// produce garbage downstream.
struct CaptureCommand {
    std::string program;
    std::vector<std::string> args;
};

[[nodiscard]] CaptureCommand buildCaptureCommand(std::string_view tool, int rate = 16000,
                                                 int channels = 1,
                                                 std::string_view target = {});

/// True when at least one capture tool exists on `PATH`, or when the user
/// configured one.
[[nodiscard]] bool captureToolPresent();

/// Should the desktop's default input be put back after a recording?
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

/// `api::AudioSource` for a Linux desktop.
///
/// Records by running `pw-record`, `parec` or `arecord` and reading its stdout,
/// which is raw s16le because we pass the format flags ourselves. Why a child
/// process instead of libpipewire: the three tools cover PipeWire, PulseAudio
/// and bare ALSA with one code path, and a hung sound server then costs one
/// process instead of the whole session.
///
/// The child, the pipe and the reader thread are all private. Once `start`
/// returns, the caller only sees `onAudio` callbacks.
class LinuxAudioSource : public api::AudioSource {
public:
    LinuxAudioSource() = default;
    ~LinuxAudioSource() override;

    LinuxAudioSource(const LinuxAudioSource &) = delete;
    LinuxAudioSource &operator=(const LinuxAudioSource &) = delete;

    [[nodiscard]] bool available() const override;
    bool start(const api::AudioRequest &request, api::AudioSink &sink,
               std::string &error) override;
    void stop() override;
    [[nodiscard]] std::string defaultDevice() const override;
    bool restoreDefault(const std::string &device) override;

private:
    /// Reads the child's stdout until it exits or `stop()` is called.
    void readLoop();
    /// Names the sound server would accept for `selectDefault`.
    [[nodiscard]] std::vector<std::string> availableDevices() const;
    bool selectDefault(const std::string &name);

    api::AudioSink *sink_ = nullptr;
    pid_t pid_ = -1;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    /// Set by `stop()` so the deliberate end of a stream is not reported as a
    /// device failure.
    std::atomic<bool> endedReported_{true};
    std::thread reader_;
};

} // namespace superior::linux
