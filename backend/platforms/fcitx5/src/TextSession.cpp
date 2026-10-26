// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/TextSession.hpp"

#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

#include <string>
#include <utility>

namespace superior::fcitx5 {

namespace {

/// True when the client asks us never to show its content anywhere but in the
/// application itself.
[[nodiscard]] bool sensitive(const fcitx::InputContext &context) {
    return context.capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive);
}

/// True when the client is willing to draw a draft inline, at its own caret.
[[nodiscard]] bool acceptsClientPreedit(const fcitx::InputContext &context) {
    return !sensitive(context) &&
           context.capabilityFlags().test(fcitx::CapabilityFlag::Preedit);
}

/// Drops whatever the engine still owns.
///
/// `InputContext::reset()` reaches the input method's `reset()`, so the engine
/// forgets the composition. `InputPanel::reset()` clears the list and both
/// preedits. Doing only one of the two leaves a stale candidate window or a
/// composition that reappears on the next keystroke.
void dropComposition(fcitx::InputContext &context) {
    context.reset();
    context.inputPanel().reset();
}

} // namespace

TextSession::TextSession(ContextResolver resolver, DestinationQuery destination)
    : resolver_(std::move(resolver)), destination_(std::move(destination)) {}

TextSession::Refusal TextSession::takeRefusal() {
    const Refusal refusal = refusal_;
    refusal_ = Refusal::None;
    return refusal;
}

void TextSession::apply(const api::SessionKey &session, std::span<const api::TextEdit> edits) {
    if (edits.empty()) {
        return;
    }
    // The port only transports the key; the adapter decides which context it
    // names. An unstamped key means "wherever the caret is now", which is what
    // speech needs: the user may have clicked into the field after speaking.
    fcitx::InputContext *context = session.valid() ? resolver_(session) : resolver_(destination_());
    if (context == nullptr) {
        refusal_ = session.valid() ? Refusal::StaleSession : Refusal::NoDestination;
        return;
    }
    for (const api::TextEdit &edit : edits) {
        applyOne(*context, edit);
    }
    context->updatePreedit();
}

void TextSession::applyOne(fcitx::InputContext &context, const api::TextEdit &edit) {
    switch (edit.kind) {
    case api::TextEdit::Kind::Clear: {
        // The composition will never be committed, so it has to go: leaving it
        // would show a candidate list for text the user is no longer typing.
        dropComposition(context);
        return;
    }
    case api::TextEdit::Kind::Preedit: {
        // An empty draft is "clear what you drew": the engine's own composition
        // is not ours to touch here, so this must not reset it.
        const bool clearOnly = edit.utf8.empty() || sensitive(context) ||
                               !acceptsClientPreedit(context);
        if (sensitive(context)) {
            refusal_ = Refusal::SensitiveField;
        }
        // The draft belongs at the caret, in the application, exactly where the
        // committed text will land: setting the *panel* preedit would put it in
        // our own candidate strip, a second box next to the text field, which
        // reads as "the input method is writing somewhere else".
        //
        // A client without the `Preedit` capability gets nothing; the committed
        // chunks still appear, which is still direct typing.
        context.inputPanel().setPreedit(fcitx::Text{});
        if (clearOnly) {
            context.inputPanel().setClientPreedit(fcitx::Text{});
            return;
        }
        fcitx::Text draft{edit.utf8};
        draft.setCursor(static_cast<int>(edit.utf8.size()));
        context.inputPanel().setClientPreedit(draft);
        return;
    }
    case api::TextEdit::Kind::Commit: {
        if (edit.utf8.empty()) {
            return;
        }
        if (sensitive(context)) {
            refusal_ = Refusal::SensitiveField;
            return;
        }
        dropComposition(context);
        // `dropComposition` already cleared both preedits, so the draft cannot
        // appear a second time under the committed text.
        context.commitString(edit.utf8);
        return;
    }
    }
}

} // namespace superior::fcitx5
