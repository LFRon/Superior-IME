// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace superior::core {

/// Which language the user is typing in.
///
/// This is a *product* mode, not an engine mode: Sogou-style switching keeps the
/// same input method and the same keyboard layout, and only changes what a
/// printable key means.
enum class Language {
    Chinese,
    English,
};

/// Punctuation width, the crescent / filled-circle button in the toolbar.
enum class Punctuation {
    Full,
    Half,
};

/// Letter and digit width. Currently only exposed through Rime options; kept
/// here so it has one owner as well.
enum class Shape {
    Normal,
    FullShape,
};

/// The three mode switches of one input method, and their **only** owner.
///
/// Every layer that used to keep its own boolean — the Fcitx5 adapter, librime's
/// session options, the toolbar action — is a *view* of this object. When two of
/// them disagreed the result was the "switched to English, switched back, still
/// English" bug. The fix is not a guard but a single writer: see
/// `docs/rime-engine.md` and 架构设计.md P-1.
///
/// Mapping to engine options is part of the contract, because every backend
/// speaks the same option names:
///
/// | mode | option | true means |
/// |---|---|---|
/// | language | `ascii_mode` | English |
/// | punctuation | `ascii_punct` | half width |
/// | shape | `full_shape` | full width |
class Modes {
public:
    [[nodiscard]] Language language() const noexcept { return language_; }
    [[nodiscard]] Punctuation punctuation() const noexcept { return punctuation_; }
    [[nodiscard]] Shape shape() const noexcept { return shape_; }

    [[nodiscard]] bool english() const noexcept { return language_ == Language::English; }
    [[nodiscard]] bool halfPunctuation() const noexcept {
        return punctuation_ == Punctuation::Half;
    }
    [[nodiscard]] bool fullShape() const noexcept { return shape_ == Shape::FullShape; }

    /// Each setter reports whether the value actually changed, so callers can
    /// skip an engine round-trip and a repaint on a no-op.
    bool setLanguage(Language value) noexcept;
    bool setPunctuation(Punctuation value) noexcept;
    bool setShape(Shape value) noexcept;

    bool toggleLanguage() noexcept;
    bool togglePunctuation() noexcept;
    bool toggleShape() noexcept;

    /// Option names this class owns, in a stable order.
    [[nodiscard]] static constexpr std::array<std::string_view, 3> optionNames() noexcept {
        return {"ascii_mode", "ascii_punct", "full_shape"};
    }

    /// True when `name` is one of `optionNames()`.
    [[nodiscard]] static bool owns(std::string_view name) noexcept;

    /// Value of an owned option. Unknown names return false; callers must check
    /// `owns()` first when "unknown" and "false" have to be distinguished.
    [[nodiscard]] bool optionValue(std::string_view name) const noexcept;

    /// Applies an owned option. Returns true when the state changed; unknown
    /// names are ignored so a schema with extra switches cannot corrupt us.
    bool setOption(std::string_view name, bool value) noexcept;

private:
    Language language_ = Language::Chinese;
    Punctuation punctuation_ = Punctuation::Full;
    Shape shape_ = Shape::Normal;
};

} // namespace superior::core
