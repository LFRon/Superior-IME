// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/Modes.hpp"

namespace superior::core {

namespace {

constexpr std::string_view kAsciiMode = "ascii_mode";
constexpr std::string_view kAsciiPunct = "ascii_punct";
constexpr std::string_view kFullShape = "full_shape";

} // namespace

bool Modes::setLanguage(Language value) noexcept {
    if (language_ == value) {
        return false;
    }
    language_ = value;
    return true;
}

bool Modes::setPunctuation(Punctuation value) noexcept {
    if (punctuation_ == value) {
        return false;
    }
    punctuation_ = value;
    return true;
}

bool Modes::setShape(Shape value) noexcept {
    if (shape_ == value) {
        return false;
    }
    shape_ = value;
    return true;
}

bool Modes::toggleLanguage() noexcept {
    return setLanguage(language_ == Language::Chinese ? Language::English : Language::Chinese);
}

bool Modes::togglePunctuation() noexcept {
    return setPunctuation(punctuation_ == Punctuation::Full ? Punctuation::Half
                                                            : Punctuation::Full);
}

bool Modes::toggleShape() noexcept {
    return setShape(shape_ == Shape::Normal ? Shape::FullShape : Shape::Normal);
}

bool Modes::owns(std::string_view name) noexcept {
    return name == kAsciiMode || name == kAsciiPunct || name == kFullShape;
}

bool Modes::optionValue(std::string_view name) const noexcept {
    if (name == kAsciiMode) {
        return english();
    }
    if (name == kAsciiPunct) {
        return halfPunctuation();
    }
    if (name == kFullShape) {
        return fullShape();
    }
    return false;
}

bool Modes::setOption(std::string_view name, bool value) noexcept {
    if (name == kAsciiMode) {
        return setLanguage(value ? Language::English : Language::Chinese);
    }
    if (name == kAsciiPunct) {
        return setPunctuation(value ? Punctuation::Half : Punctuation::Full);
    }
    if (name == kFullShape) {
        return setShape(value ? Shape::FullShape : Shape::Normal);
    }
    return false;
}

} // namespace superior::core
