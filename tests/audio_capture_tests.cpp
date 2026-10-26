// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Linux capture backend's contract, without touching a microphone: the argv
// it would build for each capture tool, and the rules for putting the desktop's
// default input back after WirePlumber re-elects it.
//
// These live here rather than in `speech_capability_tests.cpp` on purpose: they
// describe one platform's implementation, and the capability's own tests must
// keep running on a host that has no `pw-record` at all.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <functional>
#include <thread>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/linux/AudioCapture.hpp"

namespace {

namespace fs = std::filesystem;
using namespace superior::linux;

bool contains(const std::vector<std::string> &values, const std::string &needle) {
    for (const auto &value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

void testCaptureArgv() {
    const auto pipewire = buildCaptureCommand("pw-record");
    CHECK(pipewire.program == "pw-record");
    CHECK(contains(pipewire.args, "--rate=16000"));
    CHECK(contains(pipewire.args, "--channels=1"));
    CHECK(contains(pipewire.args, "-"));

    // Pinning: the stream must name the device, otherwise a mid-session flip of
    // the system default would silently move the recording.
    const auto pinnedPipewire = buildCaptureCommand("pw-record", 16000, 1, "bluez_input.headset");
    CHECK(contains(pinnedPipewire.args, "--target=bluez_input.headset"));
    const auto pinnedPulse = buildCaptureCommand("parec", 16000, 1, "bluez_input.headset");
    CHECK(contains(pinnedPulse.args, "--device=bluez_input.headset"));
    // Unpinned keeps the old argv exactly.
    CHECK(!contains(buildCaptureCommand("pw-record").args, "--target="));
    // arecord cannot name a sound-server node, so it ignores the target.
    CHECK(!contains(buildCaptureCommand("arecord", 16000, 1, "bluez_input.headset").args,
                    "--target=bluez_input.headset"));

    const auto pulse = buildCaptureCommand("parec");
    CHECK(pulse.program == "parec");
    CHECK(contains(pulse.args, "--format=s16le"));
    CHECK(contains(pulse.args, "--raw"));

    const auto alsa = buildCaptureCommand("arecord");
    CHECK(alsa.program == "arecord");
    CHECK(contains(alsa.args, "S16_LE"));
    CHECK(contains(alsa.args, "raw"));
}

/// Restoring the default input is the one thing an input method can do about
/// WirePlumber re-electing it after every recording. The rules must be
/// conservative: never invent a device, never override a deliberate change.
void testDefaultSourceRestoreRules() {
    const std::vector<std::string> available{"alsa_input.mic", "bluez_input.headset"};

    // The desktop flipped it: put it back.
    CHECK(shouldRestoreDefaultSource("bluez_input.headset", "alsa_input.mic", available));
    // Nothing changed: nothing to do.
    CHECK(!shouldRestoreDefaultSource("bluez_input.headset", "bluez_input.headset", available));
    // Never remembered (no pactl): stay out of the way.
    CHECK(!shouldRestoreDefaultSource("", "alsa_input.mic", available));
    // The user unplugged the headset: the name is gone, so do not force it.
    CHECK(!shouldRestoreDefaultSource("bluez_input.headset", "alsa_input.mic",
                                      std::vector<std::string>{"alsa_input.mic"}));
    // The user switched deliberately to another live device: still restored only
    // if the remembered one is still present, which is the trade-off we accept —
    // see the comment in Capture.hpp.
    CHECK(!shouldRestoreDefaultSource("gone", "alsa_input.mic", available));
}

/// Records what the backend delivered, on whichever thread it delivered it.
class CountingSink final : public superior::api::AudioSink {
public:
    void onAudio(std::span<const std::int16_t> samples) override {
        blocks.fetch_add(1);
        samples_ += samples.size();
        for (const auto sample : samples) {
            if (sample != 0) {
                nonZero.fetch_add(1);
            }
        }
    }
    void onAudioEnded() override { ended.store(true); }

    std::atomic<int> blocks{0};
    std::atomic<std::size_t> samples_{0};
    std::atomic<int> nonZero{0};
    std::atomic<bool> ended{false};
};

bool waitFor(const std::function<bool()> &condition, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

/// The whole point of the port is that the process, the pipe and the reader
/// thread work. A stand-in capture tool exercises all three without a
/// microphone, so this runs on a build machine.
void testStreamLifecycle() {
    const fs::path dir = fs::temp_directory_path() / "superior-audio-capture-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path tool = dir / "fake-capture.sh";

    // 100 blocks of 160 samples of a non-zero value, then exit on its own.
    {
        std::ofstream script(tool);
        script << "#!/bin/sh\n"
               << "i=0\n"
               << "while [ $i -lt 100 ]; do\n"
               << "  printf '\\x10\\x27%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
               << "  i=$((i+1))\n"
               << "done\n";
    }
    fs::permissions(tool, fs::perms::owner_all);
    ::setenv("SUPERIOR_CAPTURE_CMD", tool.string().c_str(), 1);

    LinuxAudioSource source;
    CHECK(source.available());

    {
        CountingSink sink;
        std::string error;
        superior::api::AudioRequest request;
        CHECK(source.start(request, sink, error));
        CHECK(waitFor([&sink] { return sink.ended.load(); }, 5000));
        CHECK(sink.samples_.load() > 0);
        CHECK(sink.nonZero.load() > 0);
        // The tool exited by itself, so `stop()` must be a no-op that does not
        // hang or report the end a second time.
        const int blocksBefore = sink.blocks.load();
        source.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK(sink.blocks.load() == blocksBefore);
    }

    // `stop()` while the tool is still producing: it must return promptly and the
    // sink must go quiet afterwards. A long-running tool makes that observable.
    {
        std::ofstream script(tool);
        script << "#!/bin/sh\n"
               << "while :; do\n"
               << "  printf '\\x10\\x27%.0s' 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
               << "  sleep 0.01\n"
               << "done\n";
    }
    {
        CountingSink sink;
        std::string error;
        superior::api::AudioRequest request;
        CHECK(source.start(request, sink, error));
        CHECK(waitFor([&sink] { return sink.blocks.load() > 0; }, 5000));
        const auto stopStart = std::chrono::steady_clock::now();
        source.stop();
        const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - stopStart)
                                .count();
        // Two poll ticks plus the reap. A blocking read would take as long as the
        // tool keeps talking, i.e. forever.
        CHECK(stopMs < 500);
        const int blocksAfterStop = sink.blocks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(sink.blocks.load() == blocksAfterStop);
        // A deliberate stop is not a device failure.
        CHECK(!sink.ended.load());
    }

    ::unsetenv("SUPERIOR_CAPTURE_CMD");
    fs::remove_all(dir);
}

} // namespace

int main() {
    testCaptureArgv();
    testDefaultSourceRestoreRules();
    testStreamLifecycle();
    return superior::test::finish("audio capture tests");
}
