// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "superior/api/Capabilities.hpp"
#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"
#include "superior/api/Scene.hpp"

namespace superior::api {

enum class InputMode {
    Chinese,
    English,
};

/// Commands flow UI -> agent. They are semantic: the agent owns state and
/// decides what a command means. Absolute geometry, candidate lists and skin
/// internals are deliberately not expressible.
struct ActivateCandidate {
    SessionKey session;
    CandidateId candidate = 0;
};

struct PageCandidates {
    SessionKey session;
    int delta = 0;
};

struct SelectIndex {
    SessionKey session;
    int index = 0;
};

struct SetInputMode {
    SessionKey session;
    InputMode mode = InputMode::Chinese;
};

struct ToggleStatusItem {
    std::string itemId;
};

struct SetSkin {
    std::string skinId;
};

struct SetUiScale {
    double scale = 1.0;
};

struct MovePanel {
    SessionKey session;
    double deltaX = 0.0;
    double deltaY = 0.0;
};

struct ResetPanelPosition {
    SessionKey session;
};

/// Starts a speech-recognition session. The agent decides whether the request
/// is legal (panel ready, model present, field not a password field).
struct SpeechStart {
    SessionKey session;
    std::string deviceId;
    std::string language;
};

/// Stops capture and asks for the final transcript.
struct SpeechStop {
    SessionKey session;
    bool commit = true;
};

/// Discards the session; a transcript that arrives later is dropped.
struct SpeechCancel {
    SessionKey session;
};

/// "The setup may have changed" — sent after a model or engine download so the
/// agent can re-check the files instead of watching the filesystem.
struct SpeechReload {};

struct RequestSettings {
    std::string page;
};

struct ShellReady {
    std::string shellName;
    std::string shellVersion;
    UiProfile profile = UiProfile::Desktop;
    std::vector<std::string> windowBackends;
};

struct ShellLost {
    std::string reason;
};

using UiCommand = std::variant<
    ActivateCandidate,
    PageCandidates,
    SelectIndex,
    SetInputMode,
    ToggleStatusItem,
    SetSkin,
    SetUiScale,
    MovePanel,
    ResetPanelPosition,
    SpeechStart,
    SpeechStop,
    SpeechCancel,
    SpeechReload,
    RequestSettings,
    ShellReady,
    ShellLost>;

} // namespace superior::api
