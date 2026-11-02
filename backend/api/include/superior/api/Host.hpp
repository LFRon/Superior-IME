// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "superior/api/Composition.hpp"
#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"
#include "superior/api/Scene.hpp"

namespace superior::api {

/// The composition as a host's *native* candidate list should draw it.
///
/// This is an effect, not a source of truth. The runtime builds it from the
/// capability's `CompositionFact` and assigns `CandidateId`s; a host that has a
/// native candidate list (Fcitx5) renders it. A host without one (TSF) leaves
/// `nativePanel()` null and only uses `ScenePresenter`.
struct NativePanel {
    bool visible = false;
    Preedit preedit;
    CandidatePage page;
    int pageIndex = 0;
    int pageCount = 0;
    bool vertical = true;
};

/// Receives the native candidate panel for hosts that own one.
///
/// Implementations must not read the panel back and must not reorder rows: the
/// runtime is the only producer. A click is reported through the host's normal
/// command path with the `CandidateId` it was given.
class NativePanelSink {
public:
    virtual ~NativePanelSink() = default;
    virtual void show(const SessionKey &session, const NativePanel &panel) = 0;
    virtual void hide(const SessionKey &session) = 0;
};

/// The host's own status area (tray menu, toolbar). Implementations mirror the
/// items; they do not decide them.
class StatusSink {
public:
    virtual ~StatusSink() = default;
    virtual void setStatusItems(std::span<const StatusItem> items) = 0;
    /// Activates a host-owned item (one the runtime does not understand).
    /// Returns false when the id is not a host action.
    virtual bool activate(std::string_view id) { (void)id; return false; }
};

} // namespace superior::api
