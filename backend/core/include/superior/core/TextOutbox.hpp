// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "superior/api/Ids.hpp"
#include "superior/api/Ports.hpp"
#include "superior/api/TextEdit.hpp"

namespace superior::core {

/// What to do with a commit that has nowhere to go.
///
/// This is the whole reason the type exists. The two callers differ here and
/// only here:
///
/// - a **transcript** is speech the user already spoke. Losing it because a
///   window closed mid-sentence is unacceptable, so it is held until a caret
///   exists again (`Hold`);
/// - a **clicked candidate** belongs to one specific session. If focus moved,
///   the click is stale and typing it into the new window would be wrong
///   (`Drop`).
enum class WhenUnfocused {
    Drop,
    Hold,
};

/// The single exit for text.
///
/// Everything the product wants to type goes through `apply`, which means the
/// ordering rules are written once:
///
/// 1. resolve the destination — an invalid `session` means "wherever the caret
///    is now";
/// 2. if there is no destination, apply `when` (hold the words, or drop them);
/// 3. otherwise hand the edits to the port **in order**. Committing is what
///    ends a draft: the port drops it, and this class only keeps its own
///    bookkeeping in step.
///
/// The class is deliberately free of platform types: it takes an already
/// resolved destination and an `api::TextSessionPort`, so it can be tested with
/// a fake port and no window system. `platforms/fcitx5/TextSession` is the only
/// implementation of the port today.
class TextOutbox {
public:
    explicit TextOutbox(api::TextSessionPort &session) : session_(session) {}

    /// Where the caret is, as of the last focus change. Invalid = nowhere.
    void setDestination(api::SessionKey key);
    [[nodiscard]] api::SessionKey destination() const noexcept { return destination_; }

    /// True while words are waiting for a caret.
    [[nodiscard]] bool holding() const noexcept { return !held_.empty(); }
    [[nodiscard]] const std::string &held() const noexcept { return held_; }

    /// Applies `edits` at `target`, or at the current destination when `target`
    /// is invalid.
    ///
    /// Returns true when the edits were handed to the port. A `false` means they
    /// were dropped or held, which the caller may want to log but must not
    /// retry: retrying is what makes text arrive twice.
    bool apply(const api::SessionKey &target, std::span<const api::TextEdit> edits,
               WhenUnfocused when);

    /// Convenience for the single-edit call sites.
    bool commit(const api::SessionKey &target, std::string text, WhenUnfocused when);
    bool showDraft(std::string text);
    void clearDraft();

    /// Drops the draft *and* the engine's composition, producing nothing.
    ///
    /// Needed when the product is about to change what the region means:
    /// switching to English, or starting dictation. Both used to call
    /// `InputContext::reset()` directly, which is the same operation expressed
    /// in platform terms.
    void abandonComposition();

    /// True while a draft we drew is on screen.
    [[nodiscard]] bool draftShowing() const noexcept { return draftShown_; }

    /// Writes whatever was held, in order, then forgets it.
    ///
    /// Called on focus gain. Idempotent: nothing held means nothing happens.
    void flush();

    /// Throws away held words. Used when the user cancels the thing that was
    /// going to produce them — a held transcript typed after `Esc` would be the
    /// cancel button failing.
    void discardHeld();

private:
    void send(const api::SessionKey &key, std::span<const api::TextEdit> edits);

    api::TextSessionPort &session_;
    api::SessionKey destination_{};
    std::string held_;
    /// Whether a draft is currently showing, so it is cleared exactly once and
    /// never left behind.
    bool draftShown_ = false;
};

} // namespace superior::core
