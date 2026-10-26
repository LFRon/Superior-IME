// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/stub/StubPorts.hpp"

namespace superior::stub {

void RecordingTextSession::apply(const api::SessionKey &session,
                                 std::span<const api::TextEdit> edits) {
    calls_.push_back(Call{session, std::vector<api::TextEdit>(edits.begin(), edits.end())});
}

std::size_t RecordingTextSession::count(api::TextEdit::Kind kind) const noexcept {
    std::size_t total = 0;
    for (const Call &call : calls_) {
        for (const api::TextEdit &edit : call.edits) {
            if (edit.kind == kind) {
                ++total;
            }
        }
    }
    return total;
}

std::string RecordingTextSession::committed() const {
    std::string text;
    for (const Call &call : calls_) {
        for (const api::TextEdit &edit : call.edits) {
            if (edit.kind == api::TextEdit::Kind::Commit) {
                text += edit.utf8;
            }
        }
    }
    return text;
}

std::string RecordingTextSession::draft() const {
    // Only the newest draft matters: an empty `Preedit` clears it, a `Clear`
    // drops the composition behind it.
    std::string text;
    for (const Call &call : calls_) {
        for (const api::TextEdit &edit : call.edits) {
            if (edit.kind == api::TextEdit::Kind::Preedit) {
                text = edit.utf8;
            } else if (edit.kind == api::TextEdit::Kind::Commit) {
                text.clear();
            }
        }
    }
    return text;
}

bool SilentAudioSource::start(const api::AudioRequest &request, api::AudioSink &sink,
                              std::string &error) {
    ++starts;
    static_cast<void>(request);
    static_cast<void>(sink);
    error = "this host has no audio capture";
    return false;
}

void SilentAudioSource::stop() { ++stops; }

bool SilentAudioSource::restoreDefault(const std::string &device) {
    static_cast<void>(device);
    return false;
}

} // namespace superior::stub
