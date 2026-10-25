// SPDX-License-Identifier: GPL-3.0-or-later
#include "CandidateLabel.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::formatCandidateLabel;

void testNumberedWithEngineLabel() {
    const auto parts = formatCandidateLabel("1", "原神", 0);
    CHECK(parts.prefix == "1.");
    CHECK(parts.text == "原神");
}

void testSeparatorNormalised() {
    // "1." and "1、" must render the same way as "1".
    for (const char *label : {"1.", "1、", "1。", "1)", "1）", "1:", "1："}) {
        const auto parts = formatCandidateLabel(label, "原神", 4);
        CHECK(parts.prefix == "1.");
        CHECK(parts.text == "原神");
    }
}

void testDoubleSeparator() {
    const auto parts = formatCandidateLabel("2.、", "璃月", 1);
    CHECK(parts.prefix == "2.");
}

void testBlankLabelFallsBackToIndex() {
    const auto first = formatCandidateLabel("", "原神", 0);
    CHECK(first.prefix == "1.");
    CHECK(first.text == "原神");

    const auto seventh = formatCandidateLabel("", "七七", 6);
    CHECK(seventh.prefix == "7.");
    CHECK(seventh.text == "七七");
}

void testWhitespaceOnlyLabelFallsBack() {
    const auto parts = formatCandidateLabel("   ", "蒙德", 2);
    CHECK(parts.prefix == "3.");
}

void testCjkNumeralKept() {
    // Engines that use ① keep their own identity.
    const auto parts = formatCandidateLabel("①", "稻妻", 0);
    CHECK(parts.prefix == "①.");
    CHECK(parts.text == "稻妻");
}

void testLabelOnlyCandidateIsNotNumbered() {
    // Some engines put the whole candidate in the label; numbering would show
    // "1. 1".
    const auto parts = formatCandidateLabel("1", "", 0);
    CHECK(parts.prefix.empty());
    CHECK(parts.text == "1");
}

void testTextTrimmed() {
    const auto parts = formatCandidateLabel("1", "  原神  ", 0);
    CHECK(parts.text == "原神");
}

void testBothEmpty() {
    const auto parts = formatCandidateLabel("", "", 0);
    CHECK(parts.prefix.empty());
    CHECK(parts.text.empty());
}

void testIndexIsOneBasedEvenWithLabels() {
    // Two-digit indices must not be truncated.
    const auto parts = formatCandidateLabel("", "x", 11);
    CHECK(parts.prefix == "12.");
}

void testAsciiLabelPreserved() {
    const auto parts = formatCandidateLabel("A", "alpha", 0);
    CHECK(parts.prefix == "A.");
    CHECK(parts.text == "alpha");
}

} // namespace

int main() {
    testNumberedWithEngineLabel();
    testSeparatorNormalised();
    testDoubleSeparator();
    testBlankLabelFallsBackToIndex();
    testWhitespaceOnlyLabelFallsBack();
    testCjkNumeralKept();
    testLabelOnlyCandidateIsNotNumbered();
    testTextTrimmed();
    testBothEmpty();
    testIndexIsOneBasedEvenWithLabels();
    testAsciiLabelPreserved();
    return superior::test::finish("candidate label tests");
}
