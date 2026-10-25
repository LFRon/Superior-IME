// SPDX-License-Identifier: GPL-3.0-or-later
//
// The streaming step is the one speech setting that can make dictation worse, so
// both halves are checked here without a microphone: the arithmetic that turns a
// baseline and a strike count into a step, and the rule that decides when a
// session earns or forgives a strike.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/speech_recognition/Engine.hpp"
#include "superior/speech_recognition/Models.hpp"
#include "superior/speech_recognition/StepTuning.hpp"

namespace {

using namespace superior::speech_recognition;

namespace fs = std::filesystem;

void testStepNotches() {
    // No strikes: the model's own baseline, untouched.
    CHECK(stepMsWithStrikes(1000, 0) == 1000);
    CHECK(stepMsWithStrikes(2000, 0) == 2000);
    // Each strike is one notch coarser.
    CHECK(stepMsWithStrikes(1000, 1) == 1500);
    CHECK(stepMsWithStrikes(1000, 2) == 2000);
    // Never past the ceiling, however many strikes pile up (or a hand-edited
    // file claims).
    CHECK(stepMsWithStrikes(2000, kMaxStrikes) == kMaxStepMs);
    CHECK(stepMsWithStrikes(1000, 99) == kMaxStepMs);
    // Negative strikes (a bad file) must not sharpen the step below the baseline.
    CHECK(stepMsWithStrikes(1000, -3) == 1000);
    // And nothing can go finer than the floor, which is where the engine starts
    // burning a core per second of speech.
    CHECK(stepMsWithStrikes(10, 0) == kMinStepMs);
    CHECK(stepMsWithStrikes(0, 0) == kMinStepMs);
}

void testFallingBehindCostsOneStrike() {
    StepTuning tuning;
    tuning.cleanRuns = 2;
    const auto after = tunedAfterSession(tuning, /*fellBehind=*/true, /*sessionMs=*/1000);
    CHECK(after.strikes == 1);
    // The clean run is broken: the evidence for relaxing is gone.
    CHECK(after.cleanRuns == 0);

    // Bounded, because the step is bounded and the file must stay sane.
    tuning.strikes = kMaxStrikes;
    CHECK(tunedAfterSession(tuning, true, 20000).strikes == kMaxStrikes);
}

void testShortSessionSaysNothing() {
    StepTuning tuning;
    tuning.strikes = 2;
    // A two-word utterance cannot tell a fast machine from a slow one: the model
    // was still loading for most of it.
    const auto shortSession = tunedAfterSession(tuning, false, 2000);
    CHECK(shortSession.strikes == 2);
    CHECK(shortSession.cleanRuns == 0);
}

void testThreeCleanSessionsForgive() {
    StepTuning tuning;
    tuning.strikes = 2;
    const std::uint64_t longEnough = kMeasurableSessionMs + 1000;

    tuning = tunedAfterSession(tuning, false, longEnough);
    CHECK(tuning.cleanRuns == 1);
    CHECK(tuning.strikes == 2);
    tuning = tunedAfterSession(tuning, false, longEnough);
    CHECK(tuning.cleanRuns == 2);
    CHECK(tuning.strikes == 2);
    // One session that falls behind resets the count: recovery has to be
    // consecutive to mean anything.
    tuning = tunedAfterSession(tuning, true, longEnough);
    CHECK(tuning.cleanRuns == 0);
    CHECK(tuning.strikes == 3);

    // Relieving the pressure: three clean sessions in a row give a notch back,
    // and never below the baseline.
    StepTuning relax;
    relax.strikes = 1;
    for (int i = 0; i < kCleanRunsToRelax; ++i) {
        relax = tunedAfterSession(relax, false, longEnough);
    }
    CHECK(relax.strikes == 0);
    CHECK(relax.cleanRuns == 0);

    StepTuning atBaseline;
    for (int i = 0; i < kCleanRunsToRelax; ++i) {
        atBaseline = tunedAfterSession(atBaseline, false, longEnough);
    }
    CHECK(atBaseline.strikes == 0);
}

void testModelBaselines() {
    // The light, non-autoregressive model is the one that can afford 1000 ms:
    // measured 0.38x realtime over 55 s of speech behind the VAD, against 2.96x
    // for an autoregressive model at the same step.
    const ModelEntry *paraformer = findModel("paraformer-zh");
    CHECK(paraformer != nullptr);
    if (paraformer != nullptr) {
        CHECK(paraformer->stepMs == 1000);
    }
    for (const auto &model : modelCatalog()) {
        CHECK(model.stepMs >= kMinStepMs);
        CHECK(model.stepMs <= kMaxStepMs);
        if (model.id != "paraformer-zh") {
            CHECK(model.stepMs == 2000);
        }
    }
}

void testTuningFileRoundTrip() {
    const fs::path dir = fs::temp_directory_path() / "superior-step-tuning-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path path = dir / "streaming-step";

    // Missing file: a fresh machine must simply start at the baseline.
    const StepTuning fresh = readTuning(path);
    CHECK(fresh.strikes == 0);
    CHECK(fresh.cleanRuns == 0);

    StepTuning written;
    written.strikes = 3;
    written.cleanRuns = 1;
    CHECK(writeTuning(path, written));
    const StepTuning read = readTuning(path);
    CHECK(read.strikes == 3);
    CHECK(read.cleanRuns == 1);

    // Overwrites in place, without leaving the temporary behind.
    written.strikes = 0;
    written.cleanRuns = 0;
    CHECK(writeTuning(path, written));
    CHECK(readTuning(path).strikes == 0);
    CHECK(!fs::exists(path.string() + ".tmp"));

    // A hand-mangled file is not fatal: dictation must still work.
    {
        std::ofstream file(path, std::ios::trunc);
        file << "this is not a tuning file\nstrikes\n";
    }
    const StepTuning mangled = readTuning(path);
    CHECK(mangled.strikes == 0);
    CHECK(mangled.cleanRuns == 0);

    // Out-of-range values are clamped instead of trusted.
    {
        std::ofstream file(path, std::ios::trunc);
        file << "strikes 999\ncleanRuns -4\n";
    }
    const StepTuning clamped = readTuning(path);
    CHECK(clamped.strikes == kMaxStrikes);
    CHECK(clamped.cleanRuns == 0);

    // Empty path (no state directory) is a no-op, not a crash.
    CHECK(readTuning({}).strikes == 0);
    CHECK(!writeTuning({}, written));

    fs::remove_all(dir);
}

void testEngineGetsTheStep() {
    EngineConfig config;
    config.enginePath = "crispasr";
    config.modelPath = "/models/paraformer-zh-q4_k.gguf";
    config.backend = "paraformer";
    std::string error;
    const auto command = buildStreamingEngineCommand(config, error, /*stepMs=*/1000,
                                                     /*windowMs=*/60000);
    CHECK(error.empty());
    CHECK(command.has_value());
    if (!command.has_value()) {
        return;
    }
    // The step reaches the engine as `--stream-step`, next to the window it has
    // to stay inside: a step longer than the window would never re-decode.
    bool sawStep = false;
    bool sawWindow = false;
    for (std::size_t i = 0; i + 1 < command->args.size(); ++i) {
        if (command->args[i] == "--stream-step") {
            CHECK(command->args[i + 1] == "1000");
            sawStep = true;
        } else if (command->args[i] == "--stream-length") {
            CHECK(command->args[i + 1] == "60000");
            sawWindow = true;
        }
    }
    CHECK(sawStep);
    CHECK(sawWindow);
}

} // namespace

int main() {
    testStepNotches();
    testFallingBehindCostsOneStrike();
    testShortSessionSaysNothing();
    testThreeCleanSessionsForgive();
    testModelBaselines();
    testTuningFileRoundTrip();
    testEngineGetsTheStep();
    return superior::test::finish("speech step tests");
}
