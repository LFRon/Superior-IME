// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "superior/api/Commands.hpp"
#include "superior/api/Composer.hpp"
#include "superior/api/Host.hpp"
#include "superior/api/Key.hpp"
#include "superior/api/Ports.hpp"
#include "superior/api/Scene.hpp"
#include "superior/config/Settings.hpp"
#include "superior/core/ImeSession.hpp"
#include "superior/core/Modes.hpp"
#include "superior/core/TextOutbox.hpp"
#include "superior/runtime/SpeechPort.hpp"

namespace superior::runtime {

/// What the host provides. Every reference must outlive the runtime.
struct Ports {
    api::TextSessionPort &text;
    api::ScenePresenter &scene;
    api::Scheduler &scheduler;
    api::LifecyclePort &lifecycle;
    api::NativePanelSink *nativePanel = nullptr;
    api::StatusSink *status = nullptr;
};

struct Options {
    std::filesystem::path settingsPath;
    std::string defaultSkinId = "org.superior.default";
};

/// The platform-neutral centre of the input method.
///
/// It owns one `Session` per input context, the mode state, the settings and
/// the effect queue. Hosts translate events into its four entry points and
/// execute the effects it produces; they never mutate a session themselves.
///
/// The runtime does not include any capability implementation. It talks to
/// `api::Composer` and `SpeechPort`, both injected by the host's composition
/// root.
class Runtime {
public:
    Runtime(Ports ports, api::ComposerFactory &pinyin, SpeechPort &speech, Options options);
    ~Runtime();

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    // ------------------------------------------------------------- lifecycle
    void focusIn(const api::SessionKey &key, const api::HostFacts &host);
    void focusOut(const api::SessionKey &key);
    void destroy(const api::SessionKey &key);

    // ------------------------------------------------------------- entries
    /// The only key entry. Returns true when the key was consumed.
    bool key(const api::SessionKey &key, const api::KeyPress &press, const api::HostFacts &host);

    /// The only gesture entry. The host has already decided a tap happened.
    bool gesture(const api::SessionKey &key, api::Gesture gesture, const api::HostFacts &host);

    /// The only command entry.
    void command(const api::UiCommand &command);

    /// Drives cloud debounce, speech timeouts and the settings poll.
    void tick();

    /// A host fact changed (cursor, focus, client capabilities, status area).
    void hostUpdate(const api::SessionKey &key, const api::HostFacts &host);

    /// Re-reads `settings.json` if its mtime changed.
    void reloadSettings();

    /// The host reports that the client reset the input context (XResetIC).
    /// This is an entry point, not a state mutation: the runtime decides what
    /// a reset means.
    void reset(const api::SessionKey &key);

    // ------------------------------------------------------------- queries
    [[nodiscard]] bool composing(const api::SessionKey &key) const;
    [[nodiscard]] bool english(const api::SessionKey &key) const;
    [[nodiscard]] api::Epoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] const config::Settings &settings() const noexcept { return settings_; }
    [[nodiscard]] std::string_view skinId() const noexcept { return settings_.skinId; }
    [[nodiscard]] const api::PipelineOptions &pipelineOptions() const noexcept {
        return pipelineOptions_;
    }

private:
    struct Session;

    Session *find(const api::SessionKey &key);
    [[nodiscard]] const Session *find(const api::SessionKey &key) const;
    Session &ensure(const api::SessionKey &key, const api::HostFacts &host);
    void createComposer(Session &session);
    void applyModes(Session &session);
    void applyPreferences(Session &session);
    void setLanguage(Session &session, bool english, const char *reason);
    void commitLettersAndSwitch(Session &session);
    void applyOutcome(Session &session, const api::KeyOutcome &outcome,
                      core::WhenUnfocused when);
    void applyText(const api::SessionKey &key, api::TextEdit edit, core::WhenUnfocused when);
    void present(Session &session);
    void presentHidden(const api::SessionKey &key);
    void publishStatus(Session &session);
    [[nodiscard]] std::vector<api::StatusItem> statusItems(const Session &session) const;
    void handleToggleStatus(const std::string &itemId);
    void handleSpeechStart(const api::SessionKey &key);
    void handleSpeechStop(const api::SessionKey &key, bool commit);
    void handleSpeechCancel();
    void handleSpeechReload();
    void handleCandidate(const api::ActivateCandidate &command);
    void handlePage(const api::PageCandidates &command);
    void handleSelectIndex(const api::SelectIndex &command);
    [[nodiscard]] std::uint64_t nowMs() const;

    Ports ports_;
    api::ComposerFactory &pinyin_;
    SpeechPort &speech_;
    Options options_;
    core::TextOutbox outbox_{ports_.text};

    config::Settings settings_;
    api::PipelineOptions pipelineOptions_;
    std::filesystem::file_time_type settingsStamp_{};
    bool settingsLoaded_ = false;
    std::chrono::steady_clock::time_point settingsChecked_{};

    std::vector<std::unique_ptr<Session>> sessions_;
    api::SessionKey active_{};
    api::Epoch epoch_ = 1;
    api::ContextId nextContext_ = 1;
    api::FrameId nextFrame_ = 1;
};

} // namespace superior::runtime
