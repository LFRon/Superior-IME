// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx-utils/event.h>
#include <fcitx/instance.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "SpeechController.h"

namespace superior::fcitx5 {

/// Owns the `superior_speech_recognition` child process.
///
/// Why a child process at all: the recogniser loads a 600 MB model and blocks on
/// decoding. Doing that inside Fcitx5 would freeze every keystroke in every
/// application, and a crash would take the input method with it.
///
/// The child speaks JSON Lines on stdin/stdout; this class only moves bytes. All
/// interpretation stays in the agent's decision layer.
class SpeechProcess final : public SpeechCapability {
public:
    using LineHandler = SpeechCapability::LineHandler;

    explicit SpeechProcess(fcitx::Instance *instance);
    ~SpeechProcess() override;

    SpeechProcess(const SpeechProcess &) = delete;
    SpeechProcess &operator=(const SpeechProcess &) = delete;

    /// Starts the child. Returns false with `error` filled when the binary is
    /// missing or the pipes cannot be created.
    bool start(LineHandler handler, std::string &error) override;
    void stop() override;

    [[nodiscard]] bool running() const override { return pid_ > 0; }

    /// Writes one line. Returns false when the child is gone (the caller then
    /// reports the failure instead of silently swallowing a command).
    bool send(std::string_view line) override;

    /// `SUPERIOR_SPEECH_BINARY`, then the managed copy, then `PATH`.
    [[nodiscard]] static std::string resolveBinary();

private:
    bool handleReadable();

    fcitx::Instance *instance_ = nullptr;
    pid_t pid_ = -1;
    int fd_ = -1;
    /// The child's stdin: the write end of the first pipe.
    int stdinFd_ = -1;
    std::unique_ptr<fcitx::EventSourceIO> io_;
    LineHandler handler_;
    std::string buffer_;
};

} // namespace superior::fcitx5
