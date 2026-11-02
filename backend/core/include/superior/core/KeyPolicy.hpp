// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "superior/api/Key.hpp"
#include "superior/core/KeyCode.hpp"

namespace superior::core {

using api::KeyPress;

/// What the decision layer wants to happen with a key.
enum class KeyIntent {
    /// The keyboard event is not ours; let it reach the application untouched.
    Forward,
    /// Hand the key to the active capability (pinyin composer, handwriting, …).
    Engine,
    /// Commit the character this key produces (inline English mode).
    CommitInline,
    /// Switch Chinese/English.
    ToggleLanguage,
    /// Abandon the composition without committing anything.
    Cancel,
    /// Nothing to do (modifier release, dead key).
    Ignore,
};

struct KeyDecision {
    KeyIntent intent = KeyIntent::Forward;
    /// True when the event must not reach the application. `Engine` and
    /// `Forward` are never consumed here: the capability or the client decides.
    bool consumed = false;
};

/// Everything the policy needs to know about the current session.
struct KeyPolicyInput {
    KeyPress key;
    /// The engine currently holds a non-empty composition.
    bool composing = false;
    /// The *product* language mode, i.e. `core::Modes::english()`.
    bool english = false;
};

/// Pure key policy: the single place that decides "who handles this key".
///
/// It exists because the previous implementation answered that question inside
/// the Fcitx5 adapter, with a comment explaining why Backspace must never be
/// consumed (`SuperiorAgent.cpp`). That answer is a product decision, and it has
/// to be testable without a running Fcitx5.
///
/// Rules, in order:
/// 1. releases are ignored (the capability sees them, the policy does not);
/// 2. any Ctrl/Alt/Super combination is forwarded — shortcuts belong to the app;
/// 3. while composing, Escape cancels and everything else goes to the engine;
/// 4. in English mode, Escape returns to Chinese, editing/navigation keys are
///    forwarded, and printable keys are committed inline;
/// 5. otherwise a printable key starts a composition (engine).
[[nodiscard]] KeyDecision decideKey(const KeyPolicyInput &input) noexcept;

/// True for keys the application must always own, even in English mode.
///
/// Backspace is here on purpose: inline text is committed straight to the
/// client, so the client already owns those characters. Deleting them via
/// `deleteSurroundingText` broke Backspace on every client without surrounding
/// text support.
[[nodiscard]] bool isEditingKey(KeyCode code) noexcept;

using api::Gesture;

/// What the decision layer wants the gesture to do.
enum class GestureIntent {
    None,
    /// Switch Chinese/English, recording the choice.
    ToggleLanguage,
    /// Commit the letters the pinyin engine is currently composing, as they
    /// are, then switch to English.
    ///
    /// One intent rather than two because it is one product rule: Sogou's
    /// "Shift mid-word gives me what I typed *and* the next letters are
    /// English". Splitting it would let a port commit without switching, or
    /// switch first and lose the letters.
    CommitAndSwitchToEnglish,
    /// Open the dictation panel and start, or stop an ongoing recording.
    ToggleSpeech,
    /// Discard the utterance in flight without closing the panel.
    CancelSpeech,
};

/// Everything the gesture policy needs. `settings` are product switches from
/// `settings.json`; the rest are live session facts.
struct GestureInput {
    Gesture gesture = Gesture::None;
    bool english = false;
    /// The engine currently holds a non-empty composition.
    ///
    /// Only the Shift tap reads it today: a tap mid-word means "give me the
    /// letters I typed, and keep going in English".
    bool composing = false;
    /// `settings.json: inlineEnglish`.
    bool inlineEnglishEnabled = true;
    /// `settings.json: shiftCommitPinyin`.
    bool shiftCommitPinyin = true;
    /// `settings.json: speechRecognition`.
    bool speechEnabled = true;
    /// `settings.json: speechRecognitionHotkey`.
    bool speechHotkeyEnabled = true;
    /// `core::SpeechState::busy()`.
    bool speaking = false;
};

/// Pure gesture policy: the single place that says what a tap does.
///
/// It exists because these three gestures were decided in three different places
/// inside the Fcitx5 adapter, each with its own copy of the settings gate. A port
/// to Windows would have had to reproduce all three.
///
/// Rules: a Shift tap while composing commits the typed letters and switches to
/// English when `shiftCommitPinyin` is on; with it off the tap only toggles
/// language, abandoning the pinyin without putting it on screen. Switching
/// language at all requires inline English, since that feature is what the tap
/// exists for. A right-Ctrl tap
/// toggles dictation only when both the product switch and the hotkey switch are
/// on; Escape cancels only while a session is live, so it keeps its normal
/// meaning the rest of the time.
[[nodiscard]] GestureIntent decideGesture(const GestureInput &input) noexcept;

/// Sogou-style candidate paging: `+` (or keypad `+`) is "next page", keypad `-`
/// is "previous page", anything else is 0.
///
/// The rule lives here rather than in the schema because librime's `key_binder`
/// only ships `minus`/`equal`, and `+` is Shift+`=` — a key the punctuation
/// engine happily commits as text, ending the composition. Returning a step
/// instead of acting keeps this decision testable and reusable by the Windows
/// and Android frontends.
///
/// The asymmetry is deliberate and preserved from the shipped behaviour: the
/// main-row `-` (`KeyCode::Minus`) is **not** a paging key, because typing a
/// hyphen while a candidate list is open must type a hyphen. Only the keypad
/// key pages backwards.
///
/// Modifiers are the caller's business: `+` *is* Shift on most layouts, so a
/// plain "no Shift" test would reject the very key this describes.
[[nodiscard]] int pageStep(KeyCode code) noexcept;

} // namespace superior::core
