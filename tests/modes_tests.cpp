// SPDX-License-Identifier: GPL-3.0-or-later
#include <string_view>

#include "support/TestSupport.hpp"
#include "superior/core/Modes.hpp"

namespace {

using namespace std::literals;

using superior::core::Language;
using superior::core::Modes;
using superior::core::Punctuation;
using superior::core::Shape;

void testDefaults() {
    Modes modes;
    CHECK(modes.language() == Language::Chinese);
    CHECK(modes.punctuation() == Punctuation::Full);
    CHECK(modes.shape() == Shape::Normal);
    CHECK(!modes.english());
    CHECK(!modes.halfPunctuation());
    CHECK(!modes.fullShape());
}

/// Setters report whether anything changed, so a no-op cannot trigger an engine
/// round-trip or a repaint.
void testSettersReportChange() {
    Modes modes;
    CHECK(!modes.setLanguage(Language::Chinese));
    CHECK(modes.setLanguage(Language::English));
    CHECK(!modes.setLanguage(Language::English));

    CHECK(!modes.setPunctuation(Punctuation::Full));
    CHECK(modes.setPunctuation(Punctuation::Half));
    CHECK(!modes.setPunctuation(Punctuation::Half));

    CHECK(!modes.setShape(Shape::Normal));
    CHECK(modes.setShape(Shape::FullShape));
    CHECK(!modes.setShape(Shape::FullShape));
}

void testToggles() {
    Modes modes;
    CHECK(modes.toggleLanguage());
    CHECK(modes.english());
    CHECK(modes.toggleLanguage());
    CHECK(!modes.english());

    CHECK(modes.togglePunctuation());
    CHECK(modes.halfPunctuation());
    CHECK(modes.togglePunctuation());
    CHECK(!modes.halfPunctuation());

    CHECK(modes.toggleShape());
    CHECK(modes.fullShape());
}

/// The option names are the contract with every engine backend: if they drift,
/// punctuation silently stops working (the toolbar lies about the width).
void testOptionMapping() {
    Modes modes;
    CHECK(Modes::owns("ascii_mode"));
    CHECK(Modes::owns("ascii_punct"));
    CHECK(Modes::owns("full_shape"));
    CHECK(!Modes::owns("simplification"));
    CHECK(!Modes::owns(""));

    CHECK(!modes.optionValue("ascii_mode"));
    CHECK(!modes.optionValue("ascii_punct"));
    CHECK(!modes.optionValue("full_shape"));
    CHECK(!modes.optionValue("unknown"));

    CHECK(modes.setOption("ascii_mode", true));
    CHECK(modes.english());
    CHECK(modes.optionValue("ascii_mode"));

    CHECK(modes.setOption("ascii_punct", true));
    CHECK(modes.halfPunctuation());

    CHECK(modes.setOption("full_shape", true));
    CHECK(modes.fullShape());

    // Unknown names must be ignored, not treated as false: a schema may carry
    // switches this class does not own.
    CHECK(!modes.setOption("simplification", true));
    CHECK(modes.english());
}

/// Reading an option after a toggle must reflect the new state; this is the
/// property the Fcitx5 toolbar depends on.
void testOptionRoundTrip() {
    Modes modes;
    modes.togglePunctuation();
    CHECK(modes.optionValue("ascii_punct"));
    modes.togglePunctuation();
    CHECK(!modes.optionValue("ascii_punct"));
}

void testOptionNamesAreStable() {
    const auto names = Modes::optionNames();
    CHECK(names[0] == "ascii_mode"sv);
    CHECK(names[1] == "ascii_punct"sv);
    CHECK(names[2] == "full_shape"sv);
}

} // namespace

int main() {
    testDefaults();
    testSettersReportChange();
    testToggles();
    testOptionMapping();
    testOptionRoundTrip();
    testOptionNamesAreStable();
    return superior::test::finish("modes tests");
}
