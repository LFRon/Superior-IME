// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "superior/api/Composition.hpp"
#include "superior/api/Key.hpp"

namespace superior::api {

/// What one key or one selection produced.
///
/// `commit` is the text the host must type; the runtime routes it through
/// `core::TextOutbox`, so the capability never writes to a client directly.
struct KeyOutcome {
    /// True when the capability consumed the key. The host must not forward a
    /// consumed key to the application.
    bool consumed = false;
    /// Text to commit now, if any. Empty when nothing was produced.
    std::string commit;
    /// True when the composition changed enough to warrant a new frame.
    bool compositionChanged = true;
};

/// The narrow contract every composing capability implements.
///
/// One instance per input context, owned by the runtime. The capability does
/// not know `SessionKey`, the host, the clock or the client; it only knows
/// keys, options and its own composition.
class Composer {
public:
    virtual ~Composer() = default;

    /// False when the underlying engine could not be loaded or has no schema.
    /// The runtime then reports the reason and forwards keys untouched.
    [[nodiscard]] virtual bool available() const = 0;

    virtual KeyOutcome processKey(const KeyPress &key, std::uint64_t nowMs) = 0;

    /// Selects row `row` of the current page (0-based).
    ///
    /// The returned commit is the engine's own text for an `Engine` row, or the
    /// candidate text for a `Learned` / `Cloud` row. The implementation records
    /// the user's choice in its learning store before returning.
    virtual KeyOutcome select(std::size_t row, std::uint64_t nowMs) = 0;

    /// Moves the highlighted row by `delta`, clamped to the current page.
    /// Returns true when the highlight changed.
    virtual bool moveSelection(int delta) = 0;

    /// Pages by one step: `delta > 0` is next, `delta < 0` is previous.
    virtual bool page(int delta) = 0;

    /// Commits the letters currently being composed, as they are, and ends the
    /// composition. Used by the Shift tap when `shiftCommitPinyin` is on.
    virtual bool commitLetters(std::string &out) = 0;

    /// Abandons the composition without committing anything.
    virtual void reset() = 0;

    /// Replaces the product switches (cloud pinyin, its length floor and slot).
    /// The runtime owns `settings.json`; the capability only consumes the data.
    virtual void setOptions(const PipelineOptions &options) = 0;

    /// Generic schema switch, for options such as `ascii_punct` / `emoji`.
    virtual void setOption(std::string_view name, bool value) = 0;
    [[nodiscard]] virtual bool getOption(std::string_view name) = 0;

    /// The complete current composition. Cheap enough to call per event.
    [[nodiscard]] virtual CompositionFact frame() const = 0;

    /// Cloud debounce, result collection and timeouts. Driven by the runtime's
    /// scheduler; the capability never reads a wall clock.
    virtual void tick(std::uint64_t nowMs) = 0;
};

/// Creates one composer per input context.
///
/// A factory rather than a shared composer because a librime session handle
/// belongs to exactly one context: sharing it would make every window type into
/// the same composition.
class ComposerFactory {
public:
    virtual ~ComposerFactory() = default;

    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual std::string unavailableReason() const = 0;

    /// Returns null with `error` filled when a session cannot be opened.
    virtual std::unique_ptr<Composer> create(std::string &error) = 0;
};

} // namespace superior::api
