// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/KeyPolicy.hpp"

namespace superior::core {

bool isEditingKey(std::uint32_t sym) noexcept {
    return sym == kKeyBackspace || sym == kKeyDelete || sym == kKeyReturn ||
           sym == kKeyKpEnter || sym == kKeyTab;
}

KeyDecision decideKey(const KeyPolicyInput &input) noexcept {
    const KeyPress &key = input.key;

    if (key.release) {
        return KeyDecision{KeyIntent::Ignore, false};
    }

    // Shortcuts belong to the application. This runs before the composition
    // check so Ctrl+Backspace also reaches the client while composing.
    if (key.control || key.alt || key.super) {
        return KeyDecision{KeyIntent::Forward, false};
    }

    if (input.composing) {
        if (key.sym == kKeyEscape) {
            return KeyDecision{KeyIntent::Cancel, true};
        }
        return KeyDecision{KeyIntent::Engine, false};
    }

    if (input.english) {
        if (key.sym == kKeyEscape) {
            return KeyDecision{KeyIntent::ToggleLanguage, true};
        }
        if (isEditingKey(key.sym)) {
            return KeyDecision{KeyIntent::Forward, false};
        }
        if (input.printable) {
            return KeyDecision{KeyIntent::CommitInline, true};
        }
        return KeyDecision{KeyIntent::Forward, false};
    }

    // Chinese, no composition: a printable key starts one, everything else is
    // not ours. Navigation keys are forwarded so arrow keys keep working in
    // widgets that never compose anything.
    if (key.sym == kKeyEscape) {
        return KeyDecision{KeyIntent::Forward, false};
    }
    if (!input.printable) {
        return KeyDecision{KeyIntent::Forward, false};
    }
    return KeyDecision{KeyIntent::Engine, false};
}

} // namespace superior::core
