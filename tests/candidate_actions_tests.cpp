// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/TestSupport.hpp"
#include "superior/fcitx5/CandidateActions.hpp"

namespace {

using superior::fcitx5::candidateIndexFor;

void testFirstCandidate() {
    CHECK(candidateIndexFor(1, 7) == std::optional<int>(0));
}

void testLastCandidate() {
    CHECK(candidateIndexFor(7, 7) == std::optional<int>(6));
}

void testOutOfRangeRejected() {
    // A stale frame must never activate whatever now sits at that position.
    CHECK(!candidateIndexFor(8, 7).has_value());
    CHECK(!candidateIndexFor(100, 7).has_value());
}

void testZeroMeansNoSelection() {
    CHECK(!candidateIndexFor(0, 7).has_value());
    CHECK(!candidateIndexFor(0, 0).has_value());
}

void testEmptyList() {
    CHECK(!candidateIndexFor(1, 0).has_value());
    CHECK(!candidateIndexFor(1, -3).has_value());
}

} // namespace

int main() {
    testFirstCandidate();
    testLastCandidate();
    testOutOfRangeRejected();
    testZeroMeansNoSelection();
    testEmptyList();
    return superior::test::finish("candidate action tests");
}
