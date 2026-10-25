// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/SpeechState.hpp"

#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::core {

namespace diag = ::superior::diagnostics;

namespace {

// Short and actionable, with no trailing full stop: these read as labels inside
// the panel, not as sentences in a document.
constexpr std::string_view kEngineMissing = "还没有安装语音识别引擎";
constexpr std::string_view kModelMissing = "还没有下载语音识别模型";

} // namespace

void SpeechState::enter(api::SpeechPhase phase) {
    if (phase_ == phase) {
        return;
    }
    phase_ = phase;
    SUPERIOR_DEBUG(diag::categories::SpeechRecognition)
        .field("phase", api::speechPhaseName(phase))
        << "speech panel phase";
}

void SpeechState::show(bool enginePresent, bool modelPresent, const std::string &setupMessage) {
    enginePresent_ = enginePresent;
    modelPresent_ = modelPresent;
    commitRevoked_ = false;
    level_ = 0.0;
    partial_.clear();

    if (!enginePresent_ || !modelPresent_) {
        message_ = setupMessage.empty()
                       ? std::string(enginePresent_ ? kModelMissing : kEngineMissing)
                       : setupMessage;
        enter(api::SpeechPhase::NeedsSetup);
        return;
    }
    message_.clear();
    enter(api::SpeechPhase::Ready);
}

void SpeechState::hide() {
    // Hiding is a cancel: a transcript that is still being produced must not
    // reach the document after the user closed the panel.
    commitRevoked_ = true;
    level_ = 0.0;
    partial_.clear();
    enter(api::SpeechPhase::Hidden);
}

void SpeechState::refreshSetup(bool enginePresent, bool modelPresent,
                               const std::string &setupMessage) {
    enginePresent_ = enginePresent;
    modelPresent_ = modelPresent;
    if (phase_ != api::SpeechPhase::NeedsSetup) {
        return;
    }
    if (!enginePresent_ || !modelPresent_) {
        message_ = setupMessage.empty()
                       ? std::string(enginePresent_ ? kModelMissing : kEngineMissing)
                       : setupMessage;
        return;
    }
    message_.clear();
    enter(api::SpeechPhase::Ready);
}

void SpeechState::setModel(std::string id, std::string name, std::uint64_t bytes) {
    modelId_ = std::move(id);
    modelName_ = std::move(name);
    modelBytes_ = bytes;
}

bool SpeechState::canStart() const noexcept {
    return phase_ == api::SpeechPhase::Ready || phase_ == api::SpeechPhase::Failed;
}

bool SpeechState::beginRecording() {
    if (!canStart()) {
        return false;
    }
    commitRevoked_ = false;
    lastText_.clear();
    partial_.clear();
    level_ = 0.0;
    message_.clear();
    enter(api::SpeechPhase::Recording);
    return true;
}

void SpeechState::recordingStarted() {
    if (phase_ == api::SpeechPhase::Recording) {
        message_.clear();
    }
}

void SpeechState::setLevel(double level) {
    if (phase_ != api::SpeechPhase::Recording) {
        return;
    }
    if (level < 0.0) {
        level = 0.0;
    }
    if (level > 1.0) {
        level = 1.0;
    }
    level_ = level;
}

void SpeechState::setPartial(std::string text) {
    if (phase_ != api::SpeechPhase::Recording) {
        return;
    }
    // A streaming recogniser may revise its draft downwards; that is expected,
    // unlike the pinyin composition which only grows.
    partial_ = std::move(text);
}

void SpeechState::beginTranscribing() {
    if (phase_ != api::SpeechPhase::Recording) {
        return;
    }
    level_ = 0.0;
    enter(api::SpeechPhase::Transcribing);
}

bool SpeechState::acceptFinal(std::string text) {
    if (phase_ != api::SpeechPhase::Transcribing) {
        return false;
    }
    if (commitRevoked_) {
        enter(api::SpeechPhase::Ready);
        return false;
    }
    lastText_ = std::move(text);
    partial_.clear();
    if (lastText_.empty()) {
        message_ = "没有听清，请再说一次。";
        enter(api::SpeechPhase::Ready);
        return false;
    }
    message_.clear();
    enter(api::SpeechPhase::Ready);
    return true;
}

void SpeechState::cancel() {
    commitRevoked_ = true;
    if (phase_ == api::SpeechPhase::Hidden) {
        return;
    }
    level_ = 0.0;
    partial_.clear();
    enter(api::SpeechPhase::Cancelling);
}

void SpeechState::idle() {
    if (phase_ != api::SpeechPhase::Cancelling && phase_ != api::SpeechPhase::Transcribing) {
        return;
    }
    level_ = 0.0;
    partial_.clear();
    enter(api::SpeechPhase::Ready);
}

void SpeechState::fail(std::string message) {
    message_ = std::move(message);
    level_ = 0.0;
    partial_.clear();
    enter(api::SpeechPhase::Failed);
}

bool SpeechState::busy() const noexcept {
    return phase_ == api::SpeechPhase::Recording || phase_ == api::SpeechPhase::Transcribing ||
           phase_ == api::SpeechPhase::Cancelling;
}

api::SpeechScene SpeechState::scene(std::string skinId,
                                    api::SkinGeneration generation) const {
    api::SpeechScene scene;
    scene.visible = visible();
    scene.phase = phase_;
    scene.modelId = modelId_;
    scene.modelName = modelName_;
    scene.modelBytes = modelBytes_;
    scene.modelPresent = modelPresent_;
    scene.enginePresent = enginePresent_;
    scene.message = message_;
    scene.level = level_;
    scene.partial = partial_;
    scene.skinId = std::move(skinId);
    scene.skinGeneration = generation;
    return scene;
}

} // namespace superior::core
