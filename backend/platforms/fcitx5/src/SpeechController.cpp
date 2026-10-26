// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpeechController.h"

#include <utility>

#include <nlohmann/json.hpp>

#include "superior/core/TextOutbox.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/speech_recognition/Models.hpp"
#include "superior/speech_recognition/Protocol.hpp"

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;
namespace sr = ::superior::speech_recognition;

namespace {

/// Commands without a payload are one-liners; going through `encodeCommand`
/// keeps their spelling in the protocol header rather than in this file.
std::string encodeCommand(sr::Command::Kind kind) {
    sr::Command command;
    command.kind = kind;
    return sr::encodeCommand(command);
}

/// The catalogue entry the user selected, falling back to the recommended one so
/// a hand-edited settings file cannot leave voice permanently unusable.
const sr::ModelEntry &selectedModel(const std::string &id) {
    if (const auto *entry = sr::findModel(id)) {
        return *entry;
    }
    return sr::defaultModel();
}

} // namespace

SpeechController::SpeechController(Deps deps) : deps_(std::move(deps)) {
    if (!deps_.nowMs) {
        deps_.nowMs = [] { return std::uint64_t{0}; };
    }
}

SpeechController::~SpeechController() { shutdown(); }

void SpeechController::setEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        state_.hide();
    }
}

void SpeechController::setModelId(std::string id) {
    if (!id.empty()) {
        modelId_ = std::move(id);
    }
}

bool SpeechController::engineReady() const {
    return deps_.layout.enginePresent ? deps_.layout.enginePresent() : false;
}

bool SpeechController::modelReady() const {
    return deps_.layout.modelPresent ? deps_.layout.modelPresent(modelId_) : false;
}

void SpeechController::applyModel() {
    const sr::ModelEntry &entry = selectedModel(modelId_);
    state_.setModel(entry.id, entry.displayName, entry.bytes);
}

void SpeechController::repaint() const {
    if (deps_.repaint) {
        deps_.repaint();
    }
}

void SpeechController::refreshSetup() {
    applyModel();
    state_.refreshSetup(engineReady(), modelReady());
}

void SpeechController::togglePanel() {
    if (state_.visible()) {
        // Closing is a cancel: a transcript still in flight must not be typed
        // into the document after the user closed the panel.
        cancel("panel closed");
        state_.hide();
        repaint();
        return;
    }

    const bool engine = engineReady();
    const bool model = modelReady();
    applyModel();
    state_.show(engine, model);
    if (engine && model) {
        ensureCapability();
    }
    repaint();
}

void SpeechController::toggleByHotkey() {
    if (state_.recording()) {
        stop();
        return;
    }
    if (!state_.visible()) {
        // Open *before* asking whether it can start: `canStart()` is false while
        // the panel is hidden, and a recording with no visible state would be a
        // hidden microphone. Opening also computes the setup state, so a missing
        // engine or model turns into an explanation instead of a dead hotkey.
        togglePanel();
    }
    if (state_.canStart()) {
        start();
    }
}

bool SpeechController::ensureCapability() {
    if (capability_ && capability_->running()) {
        return true;
    }
    if (!deps_.createCapability) {
        return false;
    }
    if (!capability_) {
        capability_ = deps_.createCapability();
    }
    if (!capability_) {
        return false;
    }
    std::string error;
    if (!capability_->start([this](std::string_view line) { handleEvent(line); }, error)) {
        SUPERIOR_INFO(diag::categories::SpeechRecognition)
            .field("error", error)
            << "speech recognition process is unavailable";
        return false;
    }
    reconfigure();
    return true;
}

void SpeechController::reconfigure() {
    if (!capability_ || !capability_->running()) {
        return;
    }
    // Tell the child which model to load. Sending it after every (re)start keeps
    // the two in sync without the child reading our settings file.
    const sr::ModelEntry &model = selectedModel(modelId_);
    const std::string resolvedModel =
        deps_.layout.modelPath ? deps_.layout.modelPath(model.id) : std::string{};
    if (resolvedModel.empty()) {
        return;
    }
    sr::Command configure;
    configure.kind = sr::Command::Kind::Configure;
    configure.config.enginePath = deps_.layout.enginePath ? deps_.layout.enginePath() : "";
    configure.config.modelPath = resolvedModel;
    configure.config.backend = model.backend;
    // Optional punctuation model: FireRedASR is a CTC model and emits no
    // punctuation at all, so a downloaded FireRedPunc is used when present.
    configure.config.puncPath =
        deps_.layout.punctuationPath ? deps_.layout.punctuationPath() : "";
    configure.config.language = "zh";
    configure.config.threads = 0; // 0 = let the capability size it to the machine
    // The model, not the user, decides how much audio one re-decode covers: a
    // light non-autoregressive model stays ahead of the microphone at half the
    // delay, while an autoregressive one only does it at the coarser step.
    configure.config.stepMs = model.stepMs;
    capability_->send(sr::encodeCommand(configure));
}

bool SpeechController::start() {
    if (!state_.canStart()) {
        return false;
    }
    if (deps_.caretRefusesDictation && deps_.caretRefusesDictation()) {
        state_.fail("密码输入框不允许语音输入。");
        repaint();
        return false;
    }
    if (!ensureCapability()) {
        state_.fail("语音识别组件未安装。");
        repaint();
        return false;
    }
    // Starting speech abandons a half-typed pinyin composition: two input modes
    // must never own the same commit.
    if (deps_.outbox) {
        core::TextOutbox &outbox = deps_.outbox();
        outbox.clearDraft();
        outbox.discardHeld();
        outbox.abandonComposition();
    }
    if (state_.beginRecording()) {
        if (!capability_->send(encodeCommand(sr::Command::Kind::Start))) {
            // The pipe is gone: the child died between the readiness check and
            // the command. Saying so beats a panel that animates a microphone
            // nobody is listening to.
            state_.fail("语音识别组件未响应，请重试。");
        }
    }
    repaint();
    return state_.recording();
}

void SpeechController::stop() {
    state_.beginTranscribing();
    if (capability_) {
        capability_->send(encodeCommand(sr::Command::Kind::Stop));
    }
    repaint();
}

void SpeechController::cancel(std::string_view reason) {
    if (!state_.busy() || state_.phase() == api::SpeechPhase::Cancelling) {
        return;
    }
    setDraft({});
    if (deps_.outbox) {
        deps_.outbox().discardHeld();
    }
    // The decision layer revokes the commit *first*: a `final` that is already
    // on the wire must not be typed after the user asked to discard it.
    state_.cancel();
    if (capability_) {
        capability_->send(encodeCommand(sr::Command::Kind::Cancel));
    }
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("reason", std::string(reason))
        .field("phase", api::speechPhaseName(state_.phase()))
        << "speech cancelled";
    repaint();
}

void SpeechController::tick() {
    // Whatever the cause — a dead child, a phase event lost on the way — a
    // session must not stay "busy" forever. This is the difference between "one
    // utterance failed" and "the input method is dead until Fcitx5 restarts".
    //
    // The limits are per phase on purpose: flushing a finished utterance takes a
    // second or two, so waiting a minute there would be a minute of nothing for
    // no reason, while recording legitimately lasts until the 60 s cap.
    const std::uint64_t stuckMs = [this] {
        switch (state_.phase()) {
        case api::SpeechPhase::Recording:
            return std::uint64_t{65000};
        case api::SpeechPhase::Transcribing:
            return std::uint64_t{20000};
        case api::SpeechPhase::Cancelling:
            return std::uint64_t{5000};
        default:
            return std::uint64_t{0};
        }
    }();
    const std::uint64_t now = deps_.nowMs();
    if (state_.busy() && stuckMs != 0 && phaseSinceMs_ != 0 && now - phaseSinceMs_ > stuckMs) {
        SUPERIOR_WARN(diag::categories::SpeechRecognition)
            .field("phase", api::speechPhaseName(state_.phase()))
            .field("idleMs", static_cast<std::int64_t>(now - phaseSinceMs_))
            << "speech session appears stuck; resetting the panel";
        phaseSinceMs_ = now;
        state_.fail("语音识别超时，已停止。");
        repaint();
        return;
    }
    if (capability_ && capability_->running()) {
        return;
    }
    // A dead child is harmless while nothing is in flight: the next start simply
    // spawns a new one. It is fatal exactly when the user is waiting for it.
    if (!state_.busy()) {
        return;
    }
    SUPERIOR_WARN(diag::categories::SpeechRecognition)
        << "speech capability exited mid-session; ending the session";
    state_.fail("语音识别组件意外退出，请重试。");
    repaint();
}

void SpeechController::handleEvent(std::string_view line) {
    // The child's protocol is small; decoding it here keeps the decision layer
    // (`core::SpeechState`) free of JSON.
    const auto parsed = nlohmann::json::parse(std::string(line), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return;
    }
    const std::string kind = parsed.value("ev", std::string{});
    phaseSinceMs_ = deps_.nowMs();

    if (kind == "ready") {
        state_.refreshSetup(true, true);
    } else if (kind == "phase") {
        // The child drives the session as well as the button does: it ends an
        // utterance on its own after a pause. Following its phase events is what
        // keeps `acceptFinal` from dropping a transcript whose stop nobody
        // clicked. Every transition is idempotent, so the UI-driven ones win.
        const std::string name = parsed.value("name", std::string{});
        if (name == "recording") {
            state_.recordingStarted();
        } else if (name == "transcribing") {
            state_.beginTranscribing();
        } else if (name == "idle") {
            state_.idle();
        }
    } else if (kind == "level") {
        state_.setLevel(parsed.value("rms", 0.0));
        state_.setRemaining(parsed.value("remainingMs", std::int64_t{-1}));
    } else if (kind == "partial") {
        state_.setPartial(parsed.value("text", std::string{}));
        // The draft lives in the composition line, not in the document: the user
        // sees words appear, and any rewrite happens before anything is real.
        setDraft(state_.partial());
    } else if (kind == "chunk") {
        // A finalized segment. Commit it now and keep listening — this is what
        // "writes while I speak" means.
        if (state_.acceptChunk(parsed.value("text", std::string{}))) {
            setDraft({});
            commit(state_.lastText());
        }
    } else if (kind == "final") {
        if (state_.acceptFinal(parsed.value("text", std::string{}))) {
            setDraft({});
            commit(state_.lastText());
        }
    } else if (kind == "error") {
        state_.fail(parsed.value("message", std::string{"语音识别失败"}));
    }

    repaint();
}

void SpeechController::setDraft(std::string_view text) {
    if (!deps_.outbox) {
        return;
    }
    // The draft belongs inline at the caret, in the application, exactly where
    // the committed text will land. `TextSession` decides whether this client can
    // draw it; the outbox only remembers that a draft is showing so it is
    // cleared exactly once.
    if (text.empty()) {
        deps_.outbox().clearDraft();
    } else {
        deps_.outbox().showDraft(std::string(text));
    }
}

void SpeechController::commit(std::string_view text) {
    if (!deps_.outbox || text.empty()) {
        return;
    }
    // `Hold`: this is speech the user already spoke, so losing it because the
    // window closed mid-sentence is not acceptable.
    core::TextOutbox &outbox = deps_.outbox();
    if (!outbox.commit({}, std::string(text), core::WhenUnfocused::Hold)) {
        SUPERIOR_DEBUG(diag::categories::SpeechRecognition)
            .field("held", static_cast<std::int64_t>(outbox.held().size()))
            << "nowhere to type; the transcript is held";
        return;
    }
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("chars", static_cast<std::int64_t>(text.size()))
        << "transcript committed";
}

void SpeechController::holdCommits() {
    if (!state_.busy()) {
        return;
    }
    // The draft cannot be drawn anywhere either; drop it and let the next chunk
    // carry the text once a client is focused again.
    if (deps_.outbox) {
        deps_.outbox().clearDraft();
        SUPERIOR_DEBUG(diag::categories::SpeechRecognition)
            .field("held", static_cast<std::int64_t>(deps_.outbox().held().size()))
            << "no focused client; holding the transcript";
    }
}

void SpeechController::flushHeld() {
    if (!deps_.outbox) {
        return;
    }
    core::TextOutbox &outbox = deps_.outbox();
    if (!outbox.holding()) {
        return;
    }
    const std::size_t chars = outbox.held().size();
    outbox.flush();
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("chars", static_cast<std::int64_t>(chars))
        << "flushing the transcript held while nothing was focused";
}

void SpeechController::shutdown() {
    if (capability_) {
        capability_->stop();
        capability_.reset();
    }
}

} // namespace superior::fcitx5
