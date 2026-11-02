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
    testCursorFollowsTheEngineHighlight();
    testEmptyCompositionHides();
    testPageInfoIsPassedThrough();
    testCandidateCap();
    return superior::test::finish("candidate-pipeline");
}
