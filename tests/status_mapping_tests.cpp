// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatusGlyph.hpp"
#include "support/TestSupport.hpp"
#include "superior/fcitx5/StatusMapping.hpp"

namespace {

using superior::api::StatusItem;
using superior::api::StatusRole;
using superior::ui::Glyph;

StatusItem item(std::string id, StatusRole role, bool active) {
    StatusItem value;
    value.id = std::move(id);
    value.role = role;
    value.active = active;
    return value;
}

void testPredictionHidden() {
    // A configuration knob, not a toolbar button.
    CHECK(superior::fcitx5::isStatusActionHidden("pinyin-prediction"));
}

void testUsefulActionsStay() {
    CHECK(!superior::fcitx5::isStatusActionHidden("punctuation"));
    CHECK(!superior::fcitx5::isStatusActionHidden("fullwidth"));
    CHECK(!superior::fcitx5::isStatusActionHidden("chttrans"));
    CHECK(!superior::fcitx5::isStatusActionHidden(""));
}

void testIconStateSuffix() {
    CHECK(superior::fcitx5::statusIconIsOn("fcitx-punc-active"));
    CHECK(!superior::fcitx5::statusIconIsOn("fcitx-punc-inactive"));
    // A generic prefix must not be mistaken for state.
    CHECK(!superior::fcitx5::statusIconIsOn("fcitx-fullwidth-active-remember"));
    CHECK(!superior::fcitx5::statusIconIsOn(""));
    CHECK(!superior::fcitx5::statusIconIsOn("fcitx-remind"));
    CHECK(superior::fcitx5::statusIconIsOn("active"));
}

void testPunctuationRole() {
    CHECK(superior::fcitx5::statusRoleFor("punctuation") == StatusRole::Punctuation);
}

void testUnknownActionsFallBackToText() {
    // An addon we have never seen keeps working as a labelled button.
    CHECK(superior::fcitx5::statusRoleFor("fullwidth") == StatusRole::Text);
    CHECK(superior::fcitx5::statusRoleFor("") == StatusRole::Text);
    CHECK(superior::fcitx5::statusRoleFor("some-future-action") == StatusRole::Text);
}

void testHalfWidthIsMoon() {
    CHECK(superior::ui::glyphFor(item("punctuation", StatusRole::Punctuation, false)) ==
          Glyph::Moon);
}

void testFullWidthIsCircle() {
    CHECK(superior::ui::glyphFor(item("punctuation", StatusRole::Punctuation, true)) ==
          Glyph::Circle);
}

void testTextItemsHaveNoGlyph() {
    CHECK(superior::ui::glyphFor(item("fullwidth", StatusRole::Text, true)) == Glyph::None);
    CHECK(superior::ui::glyphFor(item("chttrans", StatusRole::Text, false)) == Glyph::None);
}

void testRoleIsIndependentOfActive() {
    // The shape follows the role even when the state flips, so the cell width
    // never changes between half and full width.
    for (bool active : {false, true}) {
        CHECK(superior::ui::glyphFor(item("punctuation", StatusRole::Punctuation, active)) !=
              Glyph::None);
    }
}

} // namespace

int main() {
    testPredictionHidden();
    testUsefulActionsStay();
    testPunctuationRole();
    testIconStateSuffix();
    testUnknownActionsFallBackToText();
    testHalfWidthIsMoon();
    testFullWidthIsCircle();
    testTextItemsHaveNoGlyph();
    testRoleIsIndependentOfActive();
    return superior::test::finish("status mapping tests");
}
