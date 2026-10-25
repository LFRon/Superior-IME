// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"
#include "superior/api/Scene.hpp"

namespace superior::api {

/// Owner-thread serial scheduler. Time is monotonic milliseconds, supplied by
/// the platform; core code must never read a wall clock.
class Scheduler {
public:
    virtual ~Scheduler() = default;

    [[nodiscard]] virtual bool onOwnerThread() const noexcept = 0;
    virtual void post(std::function<void()> work) = 0;

    /// Returns a cancellation token id. Cancelling is idempotent.
    virtual std::uint64_t after(std::uint64_t delayMs, std::function<void()> work) = 0;
    virtual void cancel(std::uint64_t timerId) = 0;

    /// Requests the next presentation timestamp. Never calls back recursively.
    virtual std::uint64_t nextFrame(std::function<void(std::uint64_t timeMs)> work) = 0;
};

/// Text effects the engine may apply. The agent owns composition state; the
/// port only forwards committed text to Fcitx5.
class TextSessionPort {
public:
    virtual ~TextSessionPort() = default;

    virtual void updatePreedit(const SessionKey &session, const Preedit &preedit) = 0;
    virtual void commitText(const SessionKey &session, const std::string &utf8) = 0;
    virtual void deleteSurrounding(const SessionKey &session, int before, int after) = 0;
    virtual void clearPreedit(const SessionKey &session) = 0;
};

/// Presents a complete scene. Implementations differ in who owns the surface:
/// X11 shell owns an override-redirect window, Wayland shell owns a
/// layer-shell surface, and a future input-popup host owns the wl_surface.
class ScenePresenter {
public:
    virtual ~ScenePresenter() = default;

    virtual void present(const UiSceneFrame &frame) = 0;
    virtual void setHitMap(const SessionKey &session, const HitMap &hitMap) = 0;
    virtual void setVisible(const SessionKey &session, bool visible) = 0;
};

enum class DegradedReason {
    SkinUnavailable,
    PopupUnavailable,
    VoiceUnavailable,
    PresenterUnavailable,
    IpcDisconnected,
};

class LifecyclePort {
public:
    virtual ~LifecyclePort() = default;

    /// A subsystem is impaired but text input still works.
    virtual void degraded(DegradedReason reason, const std::string &detail) = 0;
    virtual void recovered(DegradedReason reason) = 0;

    /// Fatal to the session only. Committed text must still reach the client.
    virtual void sessionFailed(const SessionKey &session, const std::string &detail) = 0;
};

} // namespace superior::api
