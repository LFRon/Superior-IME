// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"
#include "superior/api/Scene.hpp"
#include "superior/api/TextEdit.hpp"

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

/// The only way text reaches a client.
///
/// One method instead of four, for a reason that cost two bugs: while the port
/// had separate `updatePreedit` / `commitText` / `clearPreedit` calls, each
/// caller had to remember the ordering dance "drop the engine's composition,
/// then commit, then clear the draft". Three call sites remembered it slightly
/// differently, and the two regressions both came from that.
///
/// Rules, in order of how expensive it is to break them:
///
/// 1. **synchronous and ordered** — when `apply` returns, the host has the
///    edits. An implementation may be asynchronous inside (Android's
///    `InputConnection` is), but must not leak that to the decision layer:
///    an asynchronous commit would grow a "pending / flush" pair again, and
///    that pair is where the bugs lived (§6.6).
/// 2. **the key is only a hint** — an implementation drops edits whose session
///    is not the focused one, silently. A stale click is normal, not an error
///    (§6.7).
/// 3. **no platform vocabulary** — no fd, no handle, no `QObject`, no keysym.
class TextSessionPort {
public:
    virtual ~TextSessionPort() = default;

    virtual void apply(const SessionKey &session, std::span<const TextEdit> edits) = 0;
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

/// What to record.
struct AudioRequest {
    int rate = 16000;
    int channels = 1;
    /// Pin the stream to one device. Empty means "whatever the host uses now".
    ///
    /// Pinning matters because a desktop re-elects its default input a couple of
    /// seconds after *any* stream closes: without a target, a second sentence
    /// spoken inside that window records from the wrong microphone.
    std::string device;
};

/// Where captured audio goes.
///
/// Called on the implementation's own thread, one block at a time, in order.
/// The span is only valid for the duration of the call: copying it is the
/// sink's decision, so a sink that does not need the samples does not pay for
/// them.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /// One block of mono signed 16-bit little-endian PCM.
    virtual void onAudio(std::span<const std::int16_t> samples) = 0;

    /// The stream ended without being asked to: device unplugged, tool exited,
    /// permission revoked. Without this the session would keep "recording"
    /// silence until its timeout.
    virtual void onAudioEnded() = 0;
};

/// Microphone capture.
///
/// Owns whatever process or handle the platform needs, and hides it: no fd, no
/// device node, no `QAudioSource` appears in this signature (§12.1). A platform
/// with no capture simply leaves the defaults alone and reports
/// `available() == false` — the capability then explains itself in the panel
/// instead of failing.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    [[nodiscard]] virtual bool available() const { return false; }
    /// Opens a stream and starts delivering to `sink`. Returns false with
    /// `error` filled when it cannot; `sink` must not be called after that.
    virtual bool start(const AudioRequest &request, AudioSink &sink, std::string &error) {
        static_cast<void>(request);
        static_cast<void>(sink);
        error = "audio capture is not implemented on this host";
        return false;
    }
    /// Stops capture and returns once the sink is quiet. Idempotent.
    virtual void stop() {}
    /// The device the host would use right now, or empty when unknown.
    ///
    /// Read before a stream opens, because desktops re-elect their default input
    /// a couple of seconds after *any* stream closes: only we know what the user
    /// had chosen, and `restoreDefault` needs it.
    [[nodiscard]] virtual std::string defaultDevice() const { return {}; }

    /// Puts the host's default input back to `device` if the host has since moved
    /// it elsewhere and `device` is still present.
    ///
    /// The whole rule lives behind the port on purpose: it is an artefact of how
    /// one desktop picks devices, not something a capability should reason about.
    /// Returns true only when it actually changed something back.
    virtual bool restoreDefault(const std::string &device) {
        static_cast<void>(device);
        return false;
    }
};

} // namespace superior::api
