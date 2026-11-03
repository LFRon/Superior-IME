// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/api/Composition.hpp"
#include "superior/config/Learning.hpp"
#include "superior/pinyin/CandidatePipeline.hpp"

namespace {

using superior::api::CandidateFact;
using superior::api::PipelineOptions;
using superior::config::LearnedWord;
using superior::pinyin::CandidatePipeline;
using superior::pinyin::PipelineSelection;
using superior::pinyin::Snapshot;

Snapshot enginePage(std::string preedit, std::vector<std::string> words, int highlighted = 0,
                    int pageIndex = 1, int pageCount = 1) {
    Snapshot snapshot;
    snapshot.composition.preedit = std::move(preedit);
    snapshot.menu.pageIndex = pageIndex;
    snapshot.menu.pageCount = pageCount;
    snapshot.menu.highlighted = highlighted;
    for (auto &word : words) {
        superior::pinyin::Candidate candidate;
        candidate.text = std::move(word);
        snapshot.menu.candidates.push_back(std::move(candidate));
    }
    return snapshot;
}

std::vector<LearnedWord> learned(std::string pinyin, std::string word, int count = 1) {
    std::vector<LearnedWord> entries;
    LearnedWord entry;
    entry.pinyin = std::move(pinyin);
    entry.word = std::move(word);
    entry.count = count;
    entry.lastUsedMs = count;
    entries.push_back(std::move(entry));
    return entries;
}

PipelineOptions cloudOn(int index = 2) {
    PipelineOptions options;
    options.cloudEnabled = true;
    options.cloudMinLength = 2;
    options.cloudIndex = index;
    return options;
}

std::vector<std::string> texts(const CandidatePipeline &pipeline) {
    std::vector<std::string> out;
    for (const auto &entry : pipeline.frame().candidates) {
        out.push_back(entry.text);
    }
    return out;
}

void testLearnedMovesTheEngineRow() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}), learned("wo", "握"), {},
                     PipelineOptions{});
    CHECK(texts(pipeline) == std::vector<std::string>({"握", "我", "窝"}));
    CHECK(pipeline.frame().candidates[0].source == CandidateFact::Source::Engine);
    CHECK(pipeline.frame().candidates[0].engineRow == 1U);
    const PipelineSelection selection = pipeline.selectionAt(0);
    CHECK(selection.kind == PipelineSelection::Kind::Engine);
    CHECK(selection.engineRow == 1U);
}

void testLearnedIsPrependedWhenTheEngineLacksIt() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("xiu fu", {"修复", "修福"}), learned("xiufu", "秀夫"), {},
                     PipelineOptions{});
    CHECK(texts(pipeline) == std::vector<std::string>({"秀夫", "修复", "修福"}));
    CHECK(pipeline.frame().candidates[0].source == CandidateFact::Source::Learned);
    const PipelineSelection selection = pipeline.selectionAt(0);
    CHECK(selection.kind == PipelineSelection::Kind::Direct);
    CHECK(selection.text == "秀夫");
}

void testCloudGoesAfterTheLearnedRow() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}), learned("wo", "握"), "沃",
                     cloudOn(2));
    CHECK(texts(pipeline) == std::vector<std::string>({"握", "我", "沃", "窝"}));
    CHECK(pipeline.frame().candidates[2].source == CandidateFact::Source::Cloud);
}

void testCloudDuplicateIsSkipped() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}), {}, "我", cloudOn(2));
    CHECK(texts(pipeline) == std::vector<std::string>({"我", "握", "窝"}));
}

void testDuplicateRowsAreCollapsed() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"握", "我", "握"}), {}, {}, PipelineOptions{});
    CHECK(texts(pipeline) == std::vector<std::string>({"握", "我"}));
}

/// The user's learned word leads the list, so the marker must lead with it.
///
/// Following the engine's own row 0 put the marker on the *second* row as soon
/// as the promotion moved that row one position down, and space then committed
/// the engine's row rather than the word shown first.
void testPromotedLearnedRowIsHighlighted() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}, 0), learned("wo", "握"), {},
                     PipelineOptions{});
    CHECK(texts(pipeline) == std::vector<std::string>({"握", "我", "窝"}));
    CHECK(pipeline.frame().highlighted == 0);
    const PipelineSelection selection = pipeline.selectionAt(0);
    CHECK(selection.kind == PipelineSelection::Kind::Engine);
    CHECK(selection.engineRow == 1U);
}

/// A learned word the engine does not offer is prepended; the same rule applies.
void testPrependedLearnedRowIsHighlighted() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("xiu fu", {"修复", "修福"}, 0), learned("xiufu", "秀夫"), {},
                     PipelineOptions{});
    CHECK(texts(pipeline) == std::vector<std::string>({"秀夫", "修复", "修福"}));
    CHECK(pipeline.frame().highlighted == 0);
}

/// A *real* engine highlight (a schema key moved it) is still followed.
void testRealEngineHighlightIsFollowed() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}, 2), learned("wo", "握"), {},
                     PipelineOptions{});
    CHECK(texts(pipeline) == std::vector<std::string>({"握", "我", "窝"}));
    CHECK(pipeline.frame().highlighted == 2);
}

/// The cloud answer lands while the user is looking at the second row: the
/// marker stays on that word instead of jumping to the new insertion.
void testCloudAnswerKeepsTheHighlightedWord() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}, 0), {}, {}, cloudOn(2));
    CHECK(pipeline.frame().highlighted == 0);
    CHECK(pipeline.moveSelection(1));
    CHECK(texts(pipeline) == std::vector<std::string>({"我", "握", "窝"}));

    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}, 0), {}, "沃", cloudOn(2));
    CHECK(texts(pipeline) == std::vector<std::string>({"我", "沃", "握", "窝"}));
    CHECK(pipeline.frame().highlighted == 2);
}

/// Typing another letter is a different word: the marker returns to the top.
void testNewWordResetsTheHighlight() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}, 0), {}, {}, PipelineOptions{});
    CHECK(pipeline.moveSelection(1));
    CHECK(pipeline.frame().highlighted == 1);

    pipeline.rebuild(enginePage("woz", {"我咋", "我擦"}, 0), {}, {}, PipelineOptions{});
    CHECK(pipeline.frame().highlighted == 0);

    pipeline.rebuild(enginePage("", {}), {}, {}, PipelineOptions{});
    pipeline.rebuild(enginePage("z", {"做", "再", "在"}, 0), learned("wo", "握"), {},
                     PipelineOptions{});
    CHECK(pipeline.frame().highlighted == 0);
}

/// Paging shows a different page; its first row is the one to act on, even if
/// the old highlighted text happens to appear on the new page too.
void testPageChangeStartsAtTheTop() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握"}, 0, 1, 3), {}, {}, PipelineOptions{});
    CHECK(pipeline.moveSelection(1));
    CHECK(pipeline.frame().highlighted == 1);

    pipeline.rebuild(enginePage("wo", {"我", "卧"}, 0, 2, 3), {}, {}, PipelineOptions{});
    CHECK(pipeline.frame().highlighted == 0);
}

void testCursorFollowsTheEngineHighlight() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握", "窝"}, 2), {}, {}, PipelineOptions{});
    CHECK(pipeline.frame().highlighted == 2);
    CHECK(pipeline.moveSelection(-1));
    CHECK(pipeline.frame().highlighted == 1);
    CHECK(pipeline.moveSelection(-5));
    CHECK(pipeline.frame().highlighted == 0);
}

void testEmptyCompositionHides() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("", {}), learned("wo", "握"), "沃", cloudOn());
    CHECK(!pipeline.frame().visible);
    CHECK(!pipeline.composing());
    CHECK(pipeline.frame().candidates.empty());
}

/// A preedit can exist without candidates (a half-typed syllable the engine
/// cannot resolve). The highlight must stay at 0 instead of clamping against a
/// negative bound, and typing the next letter must recover.
void testComposingWithoutCandidates() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wq", {}), {}, {}, PipelineOptions{});
    CHECK(pipeline.composing());
    CHECK(pipeline.frame().candidates.empty());
    CHECK(pipeline.frame().highlighted == -1);

    pipeline.rebuild(enginePage("w", {"我", "握"}), {}, {}, PipelineOptions{});
    CHECK(pipeline.frame().highlighted == 0);
}

void testPageInfoIsPassedThrough() {
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", {"我", "握"}, 0, 2, 4), {}, {}, PipelineOptions{});
    CHECK(pipeline.frame().pageIndex == 2);
    CHECK(pipeline.frame().pageCount == 4);
}

void testCandidateCap() {
    std::vector<std::string> words;
    for (int index = 0; index < 40; ++index) {
        words.push_back("c" + std::to_string(index));
    }
    CandidatePipeline pipeline;
    pipeline.rebuild(enginePage("wo", words), {}, {}, PipelineOptions{});
    CHECK(pipeline.frame().candidates.size() == CandidatePipeline::kMaxCandidates);
}

} // namespace

int main() {
    testLearnedMovesTheEngineRow();
    testLearnedIsPrependedWhenTheEngineLacksIt();
    testCloudGoesAfterTheLearnedRow();
    testCloudDuplicateIsSkipped();
    testDuplicateRowsAreCollapsed();
    testPromotedLearnedRowIsHighlighted();
    testPrependedLearnedRowIsHighlighted();
    testRealEngineHighlightIsFollowed();
    testCloudAnswerKeepsTheHighlightedWord();
    testNewWordResetsTheHighlight();
    testPageChangeStartsAtTheTop();
    testComposingWithoutCandidates();
    testCursorFollowsTheEngineHighlight();
    testEmptyCompositionHides();
    testPageInfoIsPassedThrough();
    testCandidateCap();
    return superior::test::finish("candidate-pipeline");
}
