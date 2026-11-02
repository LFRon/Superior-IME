// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "superior/api/Scene.hpp"

namespace superior::runtime {

/// The speech capability as the runtime sees it.
///
/// The runtime owns *when* dictation starts and stops (it is the product
/// decision); the host owns *how* the recogniser process is started, because
/// spawning a child is a platform operation. The interface is deliberately
/// small: four commands, four facts and the panel scene.
class SpeechPort {
public:
    virtual ~SpeechPort() = default;

    [[nodiscard]] virtual bool enabled() const = 0;
    [[nodiscard]] virtual bool hotkeyEnabled() const = 0;
    [[nodiscard]] virtual bool busy() const = 0;
    [[nodiscard]] virtual bool recording() const = 0;
    [[nodiscard]] virtual bool canStart() const = 0;

    virtual void setEnabled(bool enabled) = 0;
    virtual void setHotkeyEnabled(bool enabled) = 0;
    virtual void setModelId(std::string id) = 0;

    virtual void toggle() = 0;
    virtual void toggleByHotkey() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void cancel(std::string_view reason) = 0;
    virtual void reload() = 0;
    virtual void tick() = 0;
    /// Focus left while dictating: keep recording, stop typing.
    virtual void holdCommits() = 0;
    /// Focus came back: type whatever was said while nothing was focused.
    virtual void flushHeld() = 0;

    [[nodiscard]] virtual api::SpeechScene scene(std::string_view skinId,
                                                 api::SkinGeneration generation) const = 0;
    virtual void shutdown() = 0;
};

/// A speech port for hosts that do not implement dictation yet.
class NullSpeechPort final : public SpeechPort {
public:
    [[nodiscard]] bool enabled() const override { return false; }
    [[nodiscard]] bool hotkeyEnabled() const override { return false; }
    [[nodiscard]] bool busy() const override { return false; }
    [[nodiscard]] bool recording() const override { return false; }
    [[nodiscard]] bool canStart() const override { return false; }
    void setEnabled(bool) override {}
    void setHotkeyEnabled(bool) override {}
    void setModelId(std::string) override {}
    void toggle() override {}
    void toggleByHotkey() override {}
    void start() override {}
    void stop() override {}
    void cancel(std::string_view) override {}
    void reload() override {}
    void tick() override {}
    void holdCommits() override {}
    void flushHeld() override {}
    [[nodiscard]] api::SpeechScene scene(std::string_view, api::SkinGeneration) const override {
        return {};
    }
    void shutdown() override {}
};

} // namespace superior::runtime
