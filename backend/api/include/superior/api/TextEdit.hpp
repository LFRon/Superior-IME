// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace superior::api {

/// One change to the text the user sees.
///
/// There is exactly one kind of thing an input method can do to a document, and
/// this is it. Everything else in the product decides *what* the text should be;
/// this type is the last mile.
///
/// Three kinds, not more, because each one has a distinct effect on the
/// composition the engine may still own:
///
/// - `Commit` ends the composition and produces text (a transcript, a
///   candidate, an inline English character);
/// - `Preedit` shows a draft *without* committing it, and must not disturb a
///   composition that is not ours. An empty `utf8` clears the draft and leaves
///   the engine alone, which is what cancelling a transcript needs;
/// - `Clear` drops the draft **and** whatever composition the engine still
///   owns, producing nothing. Switching to English or starting dictation with
///   half-typed pinyin must not leave a candidate list behind.
///
/// A "delete surrounding text" edit existed here and had zero consumers: no
/// call site wanted it once Backspace stopped going through the input method
/// (see `core::decideKey`). It comes back when a platform needs it, not before.
struct TextEdit {
    enum class Kind {
        Commit,
        Preedit,
        Clear,
    };

    Kind kind = Kind::Commit;
    /// UTF-8. Ignored for `Clear`.
    std::string utf8;

    [[nodiscard]] static TextEdit commit(std::string text) {
        return TextEdit{Kind::Commit, std::move(text)};
    }
    [[nodiscard]] static TextEdit preedit(std::string text) {
        return TextEdit{Kind::Preedit, std::move(text)};
    }
    [[nodiscard]] static TextEdit clear() { return TextEdit{Kind::Clear, {}}; }
};

} // namespace superior::api
