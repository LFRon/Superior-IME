// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "support/TestSupport.hpp"
#include "superior/fcitx5/RimeDict.hpp"

namespace {

using superior::fcitx5::calibrateRimeWeight;
using superior::fcitx5::convertRimeDictionary;
using superior::fcitx5::RimeConvertOptions;
using superior::fcitx5::RimeConvertStats;

// Shaped like a real rime-ice file: front matter, then tab-separated entries.
constexpr std::string_view kSample = R"(# Rime dictionary
---
name: sample
version: "1"
sort: by_weight
...
你好	ni hao	100
世界	shi jie	500
女	nü	800
绿	lǜ	700
略	lüe	50
概率空间	gai lv kong jian	3
A	A
阿爸	ā bà	716
你好	ni hao	100
hello	ni hao	100
坏拼音	ni3 hao	100
坏权重	ni hao	abc
)";

std::string convert(std::string_view yaml, RimeConvertOptions options, RimeConvertStats &stats) {
    return convertRimeDictionary(yaml, options, stats);
}

void testBasicConversion() {
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = false;
    const std::string text = convert(kSample, options, stats);
    // Spaces become apostrophes: libime silently produces an EMPTY dictionary
    // when the syllables are space separated.
    CHECK(text.find("你好\tni'hao\t100") != std::string::npos);
    CHECK(text.find("世界\tshi'jie\t500") != std::string::npos);
    // Tone marks are stripped.
    CHECK(text.find("阿爸\ta'ba\t716") != std::string::npos);
    // ü becomes v, which is how libime and the cloud providers spell it.
    CHECK(text.find("女\tnv\t800") != std::string::npos);
    CHECK(text.find("绿\tlv\t700") != std::string::npos);
    CHECK(text.find("略\tlve\t50") != std::string::npos);
    CHECK(stats.kept == 7);
}

void testFrontMatterSkipped() {
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = false;
    const std::string text = convert(kSample, options, stats);
    // Metadata lines before `...` must not become entries.
    CHECK(text.find("version") == std::string::npos);
    CHECK(text.find("sort") == std::string::npos);
    CHECK(text.find("name:") == std::string::npos);
}

void testRejections() {
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = false;
    convert(kSample, options, stats);
    // "hello" and the `A	A` aux line: neither word is Han characters.
    CHECK(stats.notHanzi == 2);
    CHECK(stats.badPinyin == 1);  // ni3 hao
    CHECK(stats.badWeight == 1);  // abc
    CHECK(stats.duplicate == 1);  // 你好 twice
}

void testMissingWeightDefaults() {
    constexpr std::string_view sample = "---\n...\n你好\tni hao\n";
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = false;
    const std::string text = convert(sample, options, stats);
    CHECK(text.find("你好\tni'hao\t100") != std::string::npos);
    CHECK(stats.kept == 1);
}

void testWeightCalibration() {
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = true;
    const std::string text = convert(kSample, options, stats);
    // Rime's common default (100) must land above libime's own language model
    // preference, which we measured to be somewhere above 100 and below ~6600.
    CHECK(text.find("你好\tni'hao\t6658") != std::string::npos);
}

void testCalibrateFunction() {
    // Monotonic: a bigger Rime weight never produces a smaller libime weight.
    std::int32_t previous = -1;
    for (double weight : {0.0, 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 19260817.0}) {
        const auto value = calibrateRimeWeight(weight);
        CHECK(value >= previous);
        previous = value;
    }
    CHECK(calibrateRimeWeight(0.0) == 0);
    CHECK(calibrateRimeWeight(-5.0) == 0);
    CHECK(calibrateRimeWeight(100.0) == 6658);
    // Clamped so one absurd value cannot own every candidate for its pinyin.
    CHECK(calibrateRimeWeight(19260817.0, 60000) <= 60000);
    // Only a value far beyond anything a real dictionary contains reaches the
    // clamp; 19 million still fits (log2 gives ~24k).
    CHECK(calibrateRimeWeight(1e19) == 60000);
}

void testMaxEntries() {
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.maxEntries = 2;
    options.calibrateWeights = false;
    convert(kSample, options, stats);
    CHECK(stats.kept == 2);
}

void testEmptyBody() {
    RimeConvertStats stats;
    RimeConvertOptions options;
    const std::string text = convert("---\nname: x\n...\n", options, stats);
    CHECK(text.empty());
    CHECK(stats.kept == 0);
}

void testNoBodyMarker() {
    // A file without `...` has no entries we can trust; converting anyway would
    // risk importing metadata as words.
    RimeConvertStats stats;
    RimeConvertOptions options;
    const std::string text = convert("你好\tni hao\t100\n", options, stats);
    CHECK(text.empty());
    CHECK(stats.kept == 0);
}

void testCrlfLines() {
    constexpr std::string_view sample = "---\r\n...\r\n你好\tni hao\t100\r\n";
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = false;
    const std::string text = convert(sample, options, stats);
    CHECK(text.find("你好\tni'hao\t100") != std::string::npos);
}

void testMultiSyllable() {
    constexpr std::string_view sample = "---\n...\n崩坏星穹铁道\tbeng huai xing qiong tie dao\t100\n";
    RimeConvertStats stats;
    RimeConvertOptions options;
    options.calibrateWeights = false;
    const std::string text = convert(sample, options, stats);
    CHECK(text.find("崩坏星穹铁道\tbeng'huai'xing'qiong'tie'dao\t100") != std::string::npos);
}

} // namespace

int main() {
    testBasicConversion();
    testFrontMatterSkipped();
    testRejections();
    testMissingWeightDefaults();
    testWeightCalibration();
    testCalibrateFunction();
    testMaxEntries();
    testEmptyBody();
    testNoBodyMarker();
    testCrlfLines();
    testMultiSyllable();
    return superior::test::finish("rime dictionary tests");
}
