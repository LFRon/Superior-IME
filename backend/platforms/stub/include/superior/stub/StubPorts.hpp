// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "superior/api/Ports.hpp"

namespace superior::stub {

/// A `TextSessionPort` that records instead of typing.
///
/// It exists for two reasons (`架构设计.md` §11 P5.5):
///
/// 1. **A second implementation, on purpose.** The port is only real if it has a
///    consumer that is not Fcitx5. This one uses nothing but `superior/api/`, so
///    it compiles on every host — which is the actual test of "no platform
///    vocabulary leaked into the contract".
/// 2. **A conformance fake.** `tests/conformance/` drives the text policy through
///    it; a new platform's adapter can be diffed against these calls to see what
///    the same sequence must produce there.
class RecordingTextSession final : public api::TextSessionPort {
public:
    struct Call {
        api::SessionKey session;
        std::vector<api::TextEdit> edits;
    };

    void apply(const api::SessionKey &session, std::span<const api::TextEdit> edits) override;

    [[nodiscard]] const std::vector<Call> &calls() const noexcept { return calls_; }
    [[nodiscard]] std::size_t count(api::TextEdit::Kind kind) const noexcept;
    /// Everything that was actually typed, in order.
    [[nodiscard]] std::string committed() const;
    /// The last draft that was shown, or empty when none is showing.
    [[nodiscard]] std::string draft() const;

    void clear() { calls_.clear(); }

private:
    std::vector<Call> calls_;
};

/// An `AudioSource` for a host with no capture at all.
///
/// Reports itself unavailable and refuses to start, which is exactly what the
/// port's defaults are for (§6.8): the capability then explains the missing
/// feature in the panel instead of failing.
class SilentAudioSource final : public api::AudioSource {
public:
    [[nodiscard]] bool available() const override { return false; }
    bool start(const api::AudioRequest &request, api::AudioSink &sink,
               std::string &error) override;
    void stop() override;
    [[nodiscard]] std::string defaultDevice() const override { return {}; }
    bool restoreDefault(const std::string &device) override;

    /// How many times each method was called; the contract is "idempotent and
    /// quiet", and both are observable here.
    int starts = 0;
    int stops = 0;
};

} // namespace superior::stub
