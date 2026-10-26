// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/KeyPolicy.hpp"

namespace superior::core {

bool isEditingKey(KeyCode code) noexcept {
    switch (code) {
    case KeyCode::Backspace:
    case KeyCode::Delete:
    case KeyCode::Return:
    case KeyCode::Tab:
        return true;
    default:
        return false;
    }
}

int pageStep(KeyCode code) noexcept {
    switch (code) {
    case KeyCode::Plus:
        return 1;
    case KeyCode::KpSubtract:
        return -1;
    default:
        return 0;
    }
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
        if (key.code == KeyCode::Escape) {
            return KeyDecision{KeyIntent::Cancel, true};
        }
        return KeyDecision{KeyIntent::Engine, false};
    }

    if (input.english) {
        if (key.code == KeyCode::Escape) {
            return KeyDecision{KeyIntent::ToggleLanguage, true};
        }
        if (isEditingKey(key.code)) {
            return KeyDecision{KeyIntent::Forward, false};
        }
        if (key.text != 0) {
            return KeyDecision{KeyIntent::CommitInline, true};
        }
        return KeyDecision{KeyIntent::Forward, false};
    }

    // Chinese, no composition: a printable key starts one, everything else is
    // not ours. Navigation keys are forwarded so arrow keys keep working in
    // widgets that never compose anything.
    if (key.code == KeyCode::Escape) {
        return KeyDecision{KeyIntent::Forward, false};
    }
    if (key.text == 0) {
        return KeyDecision{KeyIntent::Forward, false};
    }
    return KeyDecision{KeyIntent::Engine, false};
}

} // namespace superior::core
