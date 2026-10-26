// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx/inputcontext.h>

#include <functional>

#include "superior/api/Ports.hpp"

namespace superior::fcitx5 {

/// Which input context a `SessionKey` names, right now.
///
/// The adapter answers this from its own state (only the active context may be
/// written to), so the port never has to reason about focus. Returning nullptr
/// is normal, not an error: a click on a window that already lost focus must be
/// dropped, not redirected (`架构设计.md` §6.7).
using ContextResolver = std::function<fcitx::InputContext *(const api::SessionKey &)>;
using DestinationQuery = std::function<api::SessionKey()>;

/// `api::TextSessionPort` for Fcitx5.
///
/// This is the only code in the product that writes text into a client. Two
/// properties make it worth being a separate class rather than two member
/// functions on the agent:
///
/// - **the composition is always dropped first.** `commitString` while the
///   engine still owns a composition produced "你好" followed by the leftover
///   pinyin; `InputContext::reset()` reaches the engine's `reset()`, while
///   `Instance::resetCompose` would only reset XKB;
/// - **a sensitive field never receives a draft or a transcript.** The check is
///   here, once, instead of in each caller.
class TextSession : public api::TextSessionPort {
public:
    TextSession(ContextResolver resolver, DestinationQuery destination);

    void apply(const api::SessionKey &session, std::span<const api::TextEdit> edits) override;

    /// Last refusal, for the caller to log. Cleared by `takeRefusal`.
    enum class Refusal {
        None,
        /// The session is no longer the focused one.
        StaleSession,
        /// The field is a password / sensitive input.
        SensitiveField,
        /// Nothing is focused.
        NoDestination,
    };
    [[nodiscard]] Refusal takeRefusal();

private:
    void applyOne(fcitx::InputContext &context, const api::TextEdit &edit);

    ContextResolver resolver_;
    DestinationQuery destination_;
    Refusal refusal_ = Refusal::None;
};

} // namespace superior::fcitx5
