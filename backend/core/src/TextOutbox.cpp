// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/TextOutbox.hpp"

#include <algorithm>
#include <utility>

namespace superior::core {

namespace {

/// Splits the edits so a commit can be handed over without the draft.
[[nodiscard]] bool hasCommit(std::span<const api::TextEdit> edits) {
    return std::any_of(edits.begin(), edits.end(), [](const api::TextEdit &edit) {
        return edit.kind == api::TextEdit::Kind::Commit;
    });
}

/// The words a commit would produce, concatenated. Used for the hold buffer.
[[nodiscard]] std::string commitText(std::span<const api::TextEdit> edits) {
    std::string text;
    for (const api::TextEdit &edit : edits) {
        if (edit.kind == api::TextEdit::Kind::Commit) {
            text += edit.utf8;
        }
    }
    return text;
}

} // namespace

void TextOutbox::setDestination(api::SessionKey key) { destination_ = key; }

bool TextOutbox::apply(const api::SessionKey &target, std::span<const api::TextEdit> edits,
                       WhenUnfocused when) {
    if (edits.empty()) {
        return false;
    }
    const api::SessionKey resolved = target.valid() ? target : destination_;
    if (!resolved.valid()) {
        // Nothing is focused. A draft has nowhere to be drawn; a commit is
        // either worth keeping or not, and `when` is the caller's answer.
        if (when == WhenUnfocused::Hold && hasCommit(edits)) {
            held_ += commitText(edits);
            return false;
        }
        draftShown_ = false;
        return false;
    }
    send(resolved, edits);
    return true;
}

bool TextOutbox::commit(const api::SessionKey &target, std::string text, WhenUnfocused when) {
    const api::TextEdit edit = api::TextEdit::commit(std::move(text));
    return apply(target, std::span<const api::TextEdit>{&edit, 1}, when);
}

bool TextOutbox::showDraft(std::string text) {
    if (!destination_.valid() || text.empty()) {
        return false;
    }
    const api::TextEdit edit = api::TextEdit::preedit(std::move(text));
    send(destination_, std::span<const api::TextEdit>{&edit, 1});
    draftShown_ = true;
    return true;
}

void TextOutbox::clearDraft() {
    if (!draftShown_) {
        return;
    }
    draftShown_ = false;
    if (!destination_.valid()) {
        return;
    }
    // An empty `Preedit` is "clear the draft", not "drop the composition": a
    // cancelled transcript must not reset pinyin the user is still typing.
    const api::TextEdit edit = api::TextEdit::preedit({});
    send(destination_, std::span<const api::TextEdit>{&edit, 1});
}

void TextOutbox::abandonComposition() {
    draftShown_ = false;
    if (!destination_.valid()) {
        return;
    }
    const api::TextEdit edit = api::TextEdit::clear();
    send(destination_, std::span<const api::TextEdit>{&edit, 1});
}

void TextOutbox::flush() {
    if (held_.empty()) {
        return;
    }
    if (!destination_.valid()) {
        // Still nowhere to type. Keep them; the next focus-in tries again.
        return;
    }
    std::string text = std::move(held_);
    held_.clear();
    const api::TextEdit edit = api::TextEdit::commit(std::move(text));
    send(destination_, std::span<const api::TextEdit>{&edit, 1});
}

void TextOutbox::discardHeld() { held_.clear(); }

void TextOutbox::send(const api::SessionKey &key, std::span<const api::TextEdit> edits) {
    // A commit supersedes any draft; the port drops the draft as part of
    // committing, so the bookkeeping here only has to agree with that.
    if (hasCommit(edits)) {
        draftShown_ = false;
    }
    session_.apply(key, edits);
}

} // namespace superior::core
