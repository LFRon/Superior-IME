// SPDX-License-Identifier: GPL-3.0-or-later
//
// The speech capability's contract, without a microphone or a model:
// capture argv, PCM level, WAV framing, engine argv and the JSON Lines protocol.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/speech_recognition/Capture.hpp"
#include "superior/speech_recognition/Engine.hpp"
#include "superior/speech_recognition/Protocol.hpp"

namespace {

using namespace superior::speech_recognition;

namespace fs = std::filesystem;

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

    const auto pulse = buildCaptureCommand("parec");
    CHECK(pulse.program == "parec");
    CHECK(contains(pulse.args, "--format=s16le"));
    CHECK(contains(pulse.args, "--raw"));

    const auto alsa = buildCaptureCommand("arecord");
    CHECK(alsa.program == "arecord");
    CHECK(contains(alsa.args, "S16_LE"));
    CHECK(contains(alsa.args, "raw"));
}

void testLevelMath() {
    CHECK(superior::test::near(rmsLevel(nullptr, 0), 0.0));
    std::vector<std::int16_t> silence(160, 0);
    CHECK(superior::test::near(rmsLevel(silence.data(), silence.size()), 0.0));

    std::vector<std::int16_t> loud(160, 32767);
    CHECK(superior::test::near(rmsLevel(loud.data(), loud.size()), 1.0));

    std::vector<std::int16_t> quiet(160, 3000);
    const double level = rmsLevel(quiet.data(), quiet.size());
    CHECK(level > 0.0);
    CHECK(level < 0.5);
}

void testWavHeader() {
    const fs::path path = fs::temp_directory_path() / "superior-speech-wav-test.wav";
    std::error_code error;
    fs::remove(path, error);

    std::vector<std::int16_t> pcm{1, -2, 3, -4};
    CHECK(writeWavFile(path, pcm, 16000));

    std::ifstream in(path, std::ios::binary);
    std::string header(44, '\0');
    in.read(header.data(), 44);
    CHECK(header.size() == 44);
    CHECK(header.compare(0, 4, "RIFF") == 0);
    CHECK(header.compare(8, 4, "WAVE") == 0);
    CHECK(header.compare(36, 4, "data") == 0);

    const auto size = fs::file_size(path, error);
    // 44-byte header plus four 16-bit samples.
    CHECK(size == 44 + 8);

    // An empty buffer still produces a valid, empty file rather than a failure:
    // "no audio" is a recording problem, not an I/O problem.
    CHECK(writeWavFile(path, {}, 16000));
    CHECK(fs::file_size(path, error) == 44);
    fs::remove(path, error);
}

/// A pause after speech ends the utterance; a pause before it does not.
void testSilenceWatchEndsUtterance() {
    SilencePolicy policy;
    SilenceWatch watch(policy);
    watch.begin(1000);

    // 600 ms of speech, then 700 ms of silence: still recording.
    for (std::uint64_t t = 0; t <= 600; t += 100) {
        watch.observe(0.4, 1000 + t);
    }
    watch.observe(0.0, 1700);
    CHECK(watch.heardSpeech());
    CHECK(!watch.finished(1700));

    // Past `holdMs` (1200) of silence, the utterance is over.
    CHECK(!watch.finished(2100));
    CHECK(watch.finished(2900));
}

/// Silence from the start must not end the recording immediately, but it must
/// not wait forever either.
void testSilenceWatchNoSpeechTimeout() {
    SilenceWatch watch(SilencePolicy{});
    watch.begin(0);
    for (std::uint64_t t = 0; t <= 4000; t += 100) {
        watch.observe(0.0, t);
    }
    CHECK(!watch.heardSpeech());
    CHECK(!watch.finished(4000));
    CHECK(watch.finished(8000));
}

/// A single click is not a sentence: `minSpeechMs` guards the hold timer.
void testSilenceWatchIgnoresBlip() {
    SilenceWatch watch(SilencePolicy{});
    watch.begin(0);
    watch.observe(0.9, 50);
    watch.observe(0.0, 100);
    // Even after a long silence the blip is not enough speech to end on.
    CHECK(!watch.finished(6000));
    // The absolute cap still ends it.
    CHECK(watch.finished(60001));
}

/// Levels at or above the threshold count as speech, below do not.
void testSilenceThresholdBoundary() {
    const SilencePolicy policy;
    SilenceWatch watch(policy);
    watch.begin(0);
    watch.observe(policy.voiceLevel, 100);
    watch.observe(policy.voiceLevel - 0.01, 200);
    CHECK(watch.heardSpeech());
}

void testEngineCommandDefault() {
    ::setenv("SUPERIOR_ASR_CMD", "", 1);
    EngineConfig config;
    config.enginePath = "/opt/crispasr";
    config.modelPath = "/models/qwen.gguf";
    config.threads = 6;
    config.language = "zh";

    std::string error;
    const auto command = buildEngineCommand(config, "/tmp/a.wav", error);
    CHECK(command.has_value());
    if (!command) {
        return;
    }
    CHECK(command->program == "/opt/crispasr");
    CHECK(contains(command->args, "--backend"));
    CHECK(contains(command->args, "qwen3"));
    CHECK(contains(command->args, "/models/qwen.gguf"));
    CHECK(contains(command->args, "/tmp/a.wav"));
    CHECK(contains(command->args, "6"));
    CHECK(contains(command->args, "-l"));
}

void testEngineCommandOverride() {
    ::setenv("SUPERIOR_ASR_CMD",
             R"(fake-asr --model "{model}" --audio "{wav}" --threads {threads})", 1);
    EngineConfig config;
    config.modelPath = "/m/x.gguf";
    config.threads = 3;

    std::string error;
    const auto command = buildEngineCommand(config, "/tmp/b.wav", error);
    CHECK(command.has_value());
    if (!command) {
        return;
    }
    CHECK(command->program == "fake-asr");
    CHECK(contains(command->args, "/m/x.gguf"));
    CHECK(contains(command->args, "/tmp/b.wav"));
    CHECK(contains(command->args, "3"));
    ::unsetenv("SUPERIOR_ASR_CMD");
}

void testEngineRequiresModel() {
    ::setenv("SUPERIOR_ASR_CMD", "", 1);
    EngineConfig config;
    config.enginePath = "/opt/crispasr";
    std::string error;
    CHECK(!buildEngineCommand(config, "/tmp/a.wav", error).has_value());
    CHECK(!error.empty());
}

void testSplitCommandLine() {
    const auto tokens = splitCommandLine(R"(cmd "a b" c)");
    CHECK(tokens.size() == 3);
    CHECK(tokens[0] == "cmd");
    CHECK(tokens[1] == "a b");
    CHECK(tokens[2] == "c");
}

void testProtocolRoundTrip() {
    Event phase;
    phase.kind = Event::Kind::Phase;
    phase.phase = "recording";
    CHECK(encodeEvent(phase) == R"({"ev":"phase","name":"recording"})");

    Event level;
    level.kind = Event::Kind::Level;
    level.level = 0.25;
    CHECK(encodeEvent(level).find("\"rms\":0.25") != std::string::npos);

    Event error;
    error.kind = Event::Kind::Error;
    error.code = "engine_missing";
    error.message = "没有引擎";
    const std::string payload = encodeEvent(error);
    CHECK(payload.find("\"code\":\"engine_missing\"") != std::string::npos);

    const auto start = decodeCommand(R"({"cmd":"start"})");
    CHECK(start.has_value());
    CHECK(start->kind == Command::Kind::Start);

    const auto configure = decodeCommand(
        R"({"cmd":"configure","config":{"engine":"/e","model":"/m","language":"zh","threads":8}})");
    CHECK(configure.has_value());
    if (configure) {
        CHECK(configure->kind == Command::Kind::Configure);
        CHECK(configure->config.enginePath == "/e");
        CHECK(configure->config.modelPath == "/m");
        CHECK(configure->config.threads == 8);
    }

    CHECK(!decodeCommand("not json").has_value());
    CHECK(!decodeCommand("[]").has_value());
    CHECK(decodeCommand(R"({"cmd":"quit"})")->kind == Command::Kind::Quit);

    // Encode then decode must be lossless: that is the whole point of having a
    // single spelling for the wire format.
    Command outbound;
    outbound.kind = Command::Kind::Configure;
    outbound.config.enginePath = "/opt/asr";
    outbound.config.modelPath = "/models/m.gguf";
    outbound.config.threads = 6;
    const auto echoed = decodeCommand(encodeCommand(outbound));
    CHECK(echoed.has_value());
    if (echoed) {
        CHECK(echoed->kind == Command::Kind::Configure);
        CHECK(echoed->config.enginePath == "/opt/asr");
        CHECK(echoed->config.modelPath == "/models/m.gguf");
        CHECK(echoed->config.threads == 6);
    }

    Command cancel;
    cancel.kind = Command::Kind::Cancel;
    CHECK(encodeCommand(cancel) == R"({"cmd":"cancel"})");
}

} // namespace

int main() {
    testCaptureArgv();
    testLevelMath();
    testSilenceWatchEndsUtterance();
    testSilenceWatchNoSpeechTimeout();
    testSilenceWatchIgnoresBlip();
    testSilenceThresholdBoundary();
    testWavHeader();
    testEngineCommandDefault();
    testEngineCommandOverride();
    testEngineRequiresModel();
    testSplitCommandLine();
    testProtocolRoundTrip();
    return superior::test::finish("speech capability tests");
}
