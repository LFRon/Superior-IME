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
#include "superior/speech_recognition/Models.hpp"
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

/// A mirror can answer 200 with an HTML page; a truncated file is just as bad.
/// Both must be rejected at download time rather than at engine time.
void testModelFileCheck() {
    const fs::path dir = fs::temp_directory_path() / "superior-model-check";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const fs::path good = dir / "good.gguf";
    {
        std::ofstream out(good, std::ios::binary);
        out.write("GGUF", 4);
        const std::string body(996, '\0');
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    CHECK(checkModelFile(good, 1000) == ModelFileCheck::Ok);

    const fs::path html = dir / "error.gguf";
    {
        std::ofstream out(html, std::ios::binary);
        const std::string page = "<!DOCTYPE html><html>404</html>";
        out.write(page.data(), static_cast<std::streamsize>(page.size()));
    }
    CHECK(checkModelFile(html, 1000) == ModelFileCheck::NotGguf);

    const fs::path truncated = dir / "partial.gguf";
    {
        std::ofstream out(truncated, std::ios::binary);
        out.write("GGUF", 4);
    }
    CHECK(checkModelFile(truncated, 1000) == ModelFileCheck::TooSmall);

    CHECK(checkModelFile(dir / "absent.gguf", 1000) == ModelFileCheck::Missing);

    // An unknown expected size must not reject a valid file.
    CHECK(checkModelFile(good, 0) == ModelFileCheck::Ok);
    fs::remove_all(dir);
}

void testEngineCommandDefault() {
    ::setenv("SUPERIOR_ASR_CMD", "", 1);
    EngineConfig config;
    config.enginePath = "/opt/crispasr";
    config.modelPath = "/models/qwen.gguf";
    // An explicit backend wins; an *empty* one must come from the catalog rather
    // than a hard-coded guess, which is what made a FireRed GGUF load as qwen3.
    config.backend = "qwen3";
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

/// The recogniser speaks in vocabulary artefacts; the document must not.
void testTranscriptCleaning() {
    // CTC silence token; the real FireRedASR2 decode of a silent clip returned
    // exactly this, and it would have been typed into the editor.
    CHECK(cleanTranscript("<sil>").empty());
    CHECK(cleanTranscript("<sil> hello").empty() == false);
    CHECK(cleanTranscript("<sil> hello") == "hello");
    CHECK(cleanTranscript("你好 <sil> 世界") == "你好 世界");
    CHECK(cleanTranscript("[noise] 测试") == "测试");
    CHECK(cleanTranscript("  multiple   spaces  ") == "multiple spaces");
    // Punctuation-only output says nothing.
    CHECK(cleanTranscript("?").empty());
    CHECK(cleanTranscript("。？！").empty());
    CHECK(cleanTranscript("你好，世界。") == "你好，世界。");
    CHECK(cleanTranscript("").empty());
}

/// Every model is loaded with its own backend, and punctuation is either ours or
/// switched off — never auto-downloaded by the engine.
void testBackendAndPunctuationArgs() {
    EngineConfig config;
    config.enginePath = "/usr/bin/crispasr";
    config.modelPath = "/models/firered.gguf";
    config.backend = "firered-asr";
    std::string error;

    const auto batch = buildEngineCommand(config, "/tmp/a.wav", error);
    CHECK(batch.has_value());
    if (batch) {
        CHECK(contains(batch->args, "firered-asr"));
        CHECK(contains(batch->args, "--punc-model"));
        CHECK(contains(batch->args, "off"));
    }

    const auto streamed = buildStreamingEngineCommand(config, error, 2000);
    CHECK(streamed.has_value());
    if (streamed) {
        CHECK(contains(streamed->args, "firered-asr"));
        CHECK(contains(streamed->args, "--stream-json"));
        CHECK(contains(streamed->args, "--punc-model"));
        CHECK(contains(streamed->args, "off"));
    }

    // A downloaded punctuation model is passed through by path.
    config.puncPath = "/models/punctuation/fireredpunc-q4_k.gguf";
    const auto withPunc = buildStreamingEngineCommand(config, error, 2000);
    CHECK(withPunc.has_value());
    if (withPunc) {
        CHECK(contains(withPunc->args, "/models/punctuation/fireredpunc-q4_k.gguf"));
        CHECK(!contains(withPunc->args, "off"));
    }
}

/// The default has to be a model that exists *and* a backend the engine knows:
/// the id is written into settings and the backend into the engine command line,
/// so a typo in either one only shows up as "recognition returns nothing".
void testCatalogDefault() {
    CHECK(!modelCatalog().empty());
    const ModelEntry &defaults = defaultModel();
    CHECK(defaults.id == "paraformer-zh");
    CHECK(defaults.backend == "paraformer");
    // Non-autoregressive: one forward pass, no token loop. This is the reason it
    // is the default on a laptop; FireRed stays available for accuracy.
    CHECK(defaults.bytes > 0 && defaults.bytes < 300000000u);
    CHECK(modelCatalog().size() >= 3);

    // FireRed is still offered, just no longer first.
    const ModelEntry *firered = findModel("firered-asr2-aed");
    CHECK(firered != nullptr);
    if (firered != nullptr) {
        CHECK(firered->backend == "firered-asr");
    }

    // Ids are directory names: duplicates would make two entries share a file.
    std::vector<std::string> ids;
    for (const auto &entry : modelCatalog()) {
        CHECK(!entry.id.empty());
        CHECK(!entry.file.empty());
        CHECK(entry.file.find('/') == std::string::npos);
        CHECK(!entry.backend.empty());
        for (const auto &seen : ids) {
            CHECK(seen != entry.id);
        }
        ids.push_back(entry.id);
    }
}

/// A stored model id is not always a model that exists any more.
void testResolveStoredModel() {
    // Nothing stored: the default.
    CHECK(resolveStoredModel("", 2).id == "paraformer-zh");

    // The v1 default, from a v1 file: migrated once, and flagged so it is logged.
    const ModelChoice legacy = resolveStoredModel("firered-asr2-aed", 1);
    CHECK(legacy.id == "paraformer-zh");
    CHECK(legacy.migrated);

    // The same id in a v2 file is a deliberate choice: keep it.
    const ModelChoice chosen = resolveStoredModel("firered-asr2-aed", 2);
    CHECK(chosen.id == "firered-asr2-aed");
    CHECK(!chosen.migrated);

    // A model that was dropped (Qwen3-ASR-0.6B) must not survive as a dead id.
    const ModelChoice dropped = resolveStoredModel("qwen3-asr-0.6b", 2);
    CHECK(dropped.id == "paraformer-zh");
    CHECK(dropped.unknown);
    CHECK(!dropped.migrated);
}

/// The non-autoregressive model must reach the engine with its own backend.
void testParaformerBackendArgs() {
    EngineConfig config;
    config.enginePath = "/usr/bin/crispasr";
    config.modelPath = "/models/paraformer-zh/paraformer-zh-q4_k.gguf";
    config.backend = defaultModel().backend;
    config.language = "zh";
    std::string error;

    const auto streamed = buildStreamingEngineCommand(config, error, 2000);
    CHECK(streamed.has_value());
    if (streamed) {
        CHECK(contains(streamed->args, "paraformer"));
        CHECK(contains(streamed->args, "--stream"));
        CHECK(contains(streamed->args, "/models/paraformer-zh/paraformer-zh-q4_k.gguf"));
    }

    const auto batch = buildEngineCommand(config, "/tmp/a.wav", error);
    CHECK(batch.has_value());
    if (batch) {
        CHECK(contains(batch->args, "paraformer"));
    }
}

/// Continuous dictation: a mid-sentence pause must not end the session, and the
/// cap must be visible before it arrives.
void testContinuousModeNeverEndsOnSilence() {
    SilencePolicy policy;
    policy.endOnSilence = false;
    policy.maxRecordingMs = 60000;
    SilenceWatch watch(policy);
    watch.begin(0);

    // Half a minute of speech with long thinking pauses.
    for (std::uint64_t now = 100; now <= 30000; now += 100) {
        // Loud for a second, quiet for two: the old policy would have ended it.
        watch.observe((now / 100) % 3 == 0 ? 0.4 : 0.0, now);
        CHECK(watch.finished(now) == false);
    }
    CHECK(watch.limitReached(30000) == false);
    CHECK(watch.remainingMs(30000) == 30000);

    // The cap still fires, and it is what ends the session.
    CHECK(watch.limitReached(60000));
    CHECK(watch.finished(60000));
    CHECK(watch.remainingMs(60000) == 0);

    // Silence before the first word still expires, so a stuck microphone is not
    // left listening forever.
    SilencePolicy idle;
    idle.endOnSilence = false;
    SilenceWatch idleWatch(idle);
    idleWatch.begin(0);
    for (std::uint64_t now = 100; now <= 9000; now += 100) {
        idleWatch.observe(0.0, now);
    }
    CHECK(idleWatch.finished(9000));
}

int main() {
    testCaptureArgv();
    testContinuousModeNeverEndsOnSilence();
    testTranscriptCleaning();
    testBackendAndPunctuationArgs();
    testCatalogDefault();
    testResolveStoredModel();
    testParaformerBackendArgs();
    testDefaultSourceRestoreRules();
    testLevelMath();
    testSilenceWatchEndsUtterance();
    testSilenceWatchNoSpeechTimeout();
    testSilenceWatchIgnoresBlip();
    testSilenceThresholdBoundary();
    testWavHeader();
    testModelFileCheck();
    testEngineCommandDefault();
    testEngineCommandOverride();
    testEngineRequiresModel();
    testSplitCommandLine();
    testProtocolRoundTrip();
    return superior::test::finish("speech capability tests");
}
