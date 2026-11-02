// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/RimeComposer.hpp"

#include <algorithm>
#include <utility>

#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/pinyin/RimeKeys.hpp"

namespace superior::pinyin {

namespace {

namespace diag = ::superior::diagnostics;

/// Wait this long after the last key before asking the network. Typing `nihao`
/// must not fire four requests (`ni`, `nih`, `niha`, `nihao`).
constexpr std::uint64_t kCloudDebounceMs = 200;

} // namespace

RimeComposer::RimeComposer(std::unique_ptr<RimePort> port, const Paths &paths,
                           LearningStore &learning, CloudService &cloud,
                           const api::PipelineOptions &options, std::string &error)
    : port_(std::move(port)), learning_(learning), cloud_(cloud), options_(options) {
    started_ = engine_.start(paths, error);
    if (started_) {
        pipeline_.setEngineId(port_->schemaId());
    }
}

RimeComposer::~RimeComposer() {
    engine_.stop();
}

bool RimeComposer::available() const {
    return started_;
}

api::KeyOutcome RimeComposer::processKey(const api::KeyPress &key, std::uint64_t nowMs) {
    api::KeyOutcome outcome;
    if (!started_) {
        return outcome;
    }
    lastKeyMs_ = nowMs;
    cloud_.setOptions(options_);
    const RimeEngine::KeyResult result = engine_.processKey(rimeKeyFor(key));
    outcome.consumed = result.consumed;
    outcome.commit = result.commit;
    refresh();
    return outcome;
}

api::KeyOutcome RimeComposer::select(std::size_t row, std::uint64_t nowMs) {
    api::KeyOutcome outcome;
    if (!started_) {
        return outcome;
    }

    const PipelineSelection selection = pipeline_.selectionAt(row);
    if (selection.kind == PipelineSelection::Kind::None) {
        return outcome;
    }

    std::string commit;
    if (selection.kind == PipelineSelection::Kind::Engine) {
        if (!engine_.selectCandidate(selection.engineRow)) {
            return outcome;
        }
        engine_.takeCommit(commit);
    } else {
        // A learned or cloud row is not known to the engine, so its composition
        // is still open. Closing it is what makes the next key start a new
        // syllable instead of appending to the text we just committed.
        commit = selection.text;
        engine_.reset();
    }

    // Learning happens here, not in a commit event. At this point both the
    // pinyin and the text the engine actually produced are still in hand; the
    // old commit-event path had neither (the engine clears the composition
    // before the event arrives).
    if (!commit.empty() && !pinyin_.empty()) {
        learning_.record(pinyin_, commit, static_cast<std::int64_t>(nowMs));
        learning_.saveIfDirty();
    }

    cloudText_.clear();
    queried_.clear();
    refresh();

    outcome.consumed = true;
    outcome.commit = std::move(commit);
    return outcome;
}

bool RimeComposer::moveSelection(int delta) {
    return pipeline_.moveSelection(delta);
}

bool RimeComposer::page(int delta) {
    if (!started_ || delta == 0) {
        return false;
    }
    engine_.page(delta < 0);
    refresh();
    return true;
}

bool RimeComposer::commitLetters(std::string &out) {
    out.clear();
    if (!started_) {
        return false;
    }
    const Snapshot snapshot = engine_.snapshot();
    out = snapshot.composition.preedit;
    if (out.empty()) {
        return false;
    }
    engine_.reset();
    cloudText_.clear();
    queried_.clear();
    refresh();
    return true;
}

void RimeComposer::reset() {
    if (!started_) {
        return;
    }
    engine_.reset();
    cloudText_.clear();
    queried_.clear();
    refresh();
}

void RimeComposer::setOptions(const api::PipelineOptions &options) {
    const bool backendChanged = options.cloudBackend != options_.cloudBackend;
    options_ = options;
    cloud_.setOptions(options_);
    if (backendChanged || !options_.cloudEnabled) {
        // An answer from the previous provider (or from before cloud was
        // turned off) must not stay in the visible list.
        cloudText_.clear();
        queried_.clear();
    }
    if (started_) {
        refresh();
    }
}

void RimeComposer::setOption(std::string_view name, bool value) {
    if (started_) {
        engine_.setOption(name, value);
    }
}

bool RimeComposer::getOption(std::string_view name) {
    return started_ && engine_.getOption(name);
}

api::CompositionFact RimeComposer::frame() const {
    return pipeline_.frame();
}

void RimeComposer::tick(std::uint64_t nowMs) {
    if (!started_) {
        return;
    }
    cloud_.setOptions(options_);
    cloud_.poll(nowMs);

    const Snapshot snapshot = engine_.snapshot();
    const auto normalized = cloudpinyin::normalizePinyin(snapshot.composition.preedit);
    const std::string next = normalized.value_or("");

    bool dirty = false;
    if (next != pinyin_) {
        pinyin_ = next;
        cloudText_.clear();
        queried_.clear();
        dirty = true;
    }

    if (!pinyin_.empty()) {
        if (const auto hit = cloud_.find(pinyin_)) {
            if (*hit != cloudText_) {
                cloudText_ = *hit;
                dirty = true;
            }
        } else if (cloud_.enabled() && pinyin_ != queried_ &&
                   nowMs - lastKeyMs_ >= kCloudDebounceMs) {
            if (cloud_.submit(pinyin_, nowMs)) {
                queried_ = pinyin_;
            }
        }
    }

    if (dirty) {
        pipeline_.rebuild(snapshot, learning_.entries(), cloudText_, options_);
    }
}

void RimeComposer::refresh() {
    const Snapshot snapshot = engine_.snapshot();
    const auto normalized = cloudpinyin::normalizePinyin(snapshot.composition.preedit);
    const std::string next = normalized.value_or("");
    if (next != pinyin_) {
        pinyin_ = next;
        cloudText_.clear();
        queried_.clear();
    }
    pipeline_.rebuild(snapshot, learning_.entries(), cloudText_, options_);
}

RimeComposerFactory::RimeComposerFactory(Paths paths, LearningStore &learning, CloudService &cloud)
    : paths_(std::move(paths)), learning_(learning), cloud_(cloud) {
    LibrimePort probe;
    available_ = probe.available();
    if (!available_) {
        unavailable_ = probe.unavailableReason();
    }
}

bool RimeComposerFactory::available() const {
    return available_;
}

std::string RimeComposerFactory::unavailableReason() const {
    return unavailable_;
}

std::unique_ptr<api::Composer> RimeComposerFactory::create(std::string &error) {
    if (!available_) {
        error = unavailable_;
        return nullptr;
    }
    auto port = std::make_unique<LibrimePort>();
    if (!port->available()) {
        error = port->unavailableReason();
        return nullptr;
    }
    // Options are delivered by the runtime through `setOptions()`; the factory
    // deliberately does not keep a second copy of settings that could go stale.
    auto composer = std::make_unique<RimeComposer>(std::move(port), paths_, learning_, cloud_,
                                                   api::PipelineOptions{}, error);
    if (!composer->available()) {
        if (error.empty()) {
            error = "the pinyin engine could not open a session";
        }
        return nullptr;
    }
    SUPERIOR_DEBUG(diag::categories::Pinyin) << "pinyin composer created";
    return composer;
}

} // namespace superior::pinyin
