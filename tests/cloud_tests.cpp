// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "support/TestSupport.hpp"
#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/cloudpinyin/CloudState.hpp"
#include "superior/cloudpinyin/Match.hpp"

namespace {

using namespace superior::cloudpinyin;

// Real payloads captured from the live endpoints (2026-11-02).
constexpr std::string_view kGoogleOk =
    R"(["SUCCESS",[["nihao",["你好"],[],{"annotation":["ni hao"],"candidate_type":[0],"lc":["16 16"]}]]])";
constexpr std::string_view kGoogleEmpty = R"(["SUCCESS",[["zzzzzz",[],[]]]])";
constexpr std::string_view kGoogleFailed = R"(["FAILED",["internal error"]])";
/// The endpoint the old implementation used; it answers this for every input.
constexpr std::string_view kBaiduDead =
    R"({"status":"F","errno":"19","errmsg":"Internal Error", "result":[]})";
constexpr std::string_view kBaiduNihao =
    R"({"status":"T","errno":"0","errmsg":"","result":[[["你好",5,{"pinyin":"ni'hao","type":"IMEDICT"}],)"
    R"(["你号",5,{"pinyin":"ni'hao","type":"IMEDICT"}],)"
    R"(["泥嚎",5,{"pinyin":"ni'hao","type":"IMEDICT"}]]]})";
/// A homophone-first answer: the reason full pinyin does not go to Baidu.
constexpr std::string_view kBaiduHomophones =
    R"({"status":"T","errno":"0","errmsg":"","result":[[["中果人",11,{"pinyin":"zhong'guo'ren","type":"IMEDICT"}],)"
    R"(["重国人",11,{"pinyin":"zhong'guo'ren","type":"IMEDICT"}]]]})";
/// `yyds`: the first entry is the pinyin echoed back, the useful word is second.
constexpr std::string_view kBaiduAbbreviated =
    R"({"status":"T","errno":"0","errmsg":"","result":[[["yyds",4,{"pinyin":"y'y'd's","type":"IMEDICT"}],)"
    R"(["永远的神",14,{"pinyin":"yong'yuan'de'shen","type":"IMEDICT"}],)"
    R"(["永远都是",14,{"pinyin":"yong'yuan'dou'shi","type":"IMEDICT"}]]]})";

/// The Sogou response is binary; these are real captures as hex.
std::string fromHex(std::string_view hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return 0;
    };
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        out.push_back(static_cast<char>((nibble(hex[index]) << 4) | nibble(hex[index + 1])));
    }
    return out;
}

/// `nihao` → 你好 / 你号 / 泥嚎, each covering 5 input characters.
constexpr std::string_view kSogouNihaoHex =
    "4b00050000000000000000000009000000000300"
    "0400604f7d590400db00750004000200050001"
    "0400604ff7530400db00750004000200050001"
    "0400e56c8e560400db00750004000200050001";
/// `yyds` → 永远都是 first: Sogou's abbreviated answers are generic, and its
/// syllable positions are one letter each, so no reading can be rebuilt.
constexpr std::string_view kSogouYydsHex =
    "6f00050000000000000000000009000000000300"
    "0800386cdc8ffd902f6608007201750149002c010800010002000300040001"
    "08000967004eb9708b4e080073016e0143002c010800010002000300040001"
    "0800386cdc8ff46e5e7908007201750141002a010800010002000300040001";

void testBackendNames() {
    CHECK(backendName(Backend::GoogleCn) == "googlecn");
    CHECK(backendName(Backend::Baidu) == "baidu");
    CHECK(backendName(Backend::Sogou) == "sogou");
}

void testBackendParsing() {
    CHECK(backendFromString("googlecn") == std::optional<Backend>(Backend::GoogleCn));
    CHECK(backendFromString("google-cn") == std::optional<Backend>(Backend::GoogleCn));
    CHECK(backendFromString("baidu") == std::optional<Backend>(Backend::Baidu));
    CHECK(backendFromString("sogou") == std::optional<Backend>(Backend::Sogou));
    // Unknown values must not silently select a provider.
    CHECK(!backendFromString("youdao").has_value());
    CHECK(!backendFromString("").has_value());
}

void testBackendRoundTrip() {
    for (const auto backend : allBackends()) {
        CHECK(backendFromString(backendName(backend)) == std::optional<Backend>(backend));
    }
}

void testUrlEncoding() {
    CHECK(urlEncode("nihao") == "nihao");
    // A query separator must never survive: otherwise a caller could add params.
    CHECK(urlEncode("ni&hao") == "ni%26hao");
    CHECK(urlEncode("a b") == "a%20b");
    CHECK(urlEncode("#") == "%23");
    CHECK(urlEncode("") == "");
}

void testRequestUrl() {
    CHECK(requestUrl(Backend::GoogleCn, "nihao") ==
          "https://www.google.cn/inputtools/request?ime=pinyin&text=nihao");
    CHECK(requestUrl(Backend::Baidu, "nihao") ==
          "https://olimenew.baidu.com/py?inputtype=py&resultcoding=utf-8&input=nihao");
    // Sogou's pinyin travels in the body; no URL means anything on its own.
    CHECK(requestUrl(Backend::Sogou, "nihao").empty());
}

void testRequestFor() {
    // GET providers: a URL and no body.
    const auto google = requestFor(Backend::GoogleCn, "nihao");
    CHECK(google.ok());
    CHECK(google.backend == Backend::GoogleCn);
    CHECK(google.body.empty());

    // Sogou: the mobile client's exact framing —
    // length | banner | length | pinyin | XOR of everything before it.
    const auto sogou = requestFor(Backend::Sogou, "nihao");
    CHECK(sogou.ok());
    CHECK(sogou.backend == Backend::Sogou);
    CHECK(sogou.contentType == "application/octet-stream");
    const std::string expected = fromHex("0f00050000000001056e6968616f6f");
    CHECK(sogou.body == expected);

    // `Auto` resolves before the request is built; the resolved provider is what
    // the answer has to be parsed with.
    CHECK(requestFor(Backend::Auto, "zhongguoren").backend == Backend::Sogou);
    CHECK(requestFor(Backend::Auto, "yyds").backend == Backend::Baidu);
}

void testGoogleParsing() {
    const auto ok = parseCandidates(Backend::GoogleCn, "nihao", kGoogleOk);
    CHECK(!ok.empty());
    CHECK(ok.front().text == "你好");
    // Google sends no syllable boundaries, so the reading is the echoed pinyin.
    // Getting this wrong leaves every Google candidate unverifiable, which is
    // how this branch silently stopped offering anything.
    CHECK(ok.front().reading == "nihao");
    // No candidate for this input is not an error, just no answer.
    CHECK(parseCandidates(Backend::GoogleCn, "zzzzzz", kGoogleEmpty).empty());
    CHECK(parseCandidates(Backend::GoogleCn, "x", kGoogleFailed).empty());
    CHECK(parseCandidates(Backend::GoogleCn, "x", "").empty());
    CHECK(parseCandidates(Backend::GoogleCn, "x", "not json at all").empty());
}

void testBaiduParsing() {
    const auto ok = parseCandidates(Backend::Baidu, "nihao", kBaiduNihao);
    CHECK(ok.size() == 3);
    CHECK(ok.front().text == "你好");
    CHECK(ok.front().covered == 5);
    CHECK(ok.front().reading == "ni'hao");
    // The old endpoint answers this for every input; it must read as
    // "unavailable", not as a candidate named "F".
    CHECK(parseCandidates(Backend::Baidu, "nihao", kBaiduDead).empty());
    CHECK(parseCandidates(Backend::Baidu, "nihao", "").empty());
    // A truncated body must stop the walk instead of reading past it.
    CHECK(parseCandidates(Backend::Baidu, "nihao", kBaiduNihao.substr(0, 60)).size() <= 1);
}

void testSogouParsing() {
    const std::string body = fromHex(kSogouNihaoHex);
    const auto ok = parseCandidates(Backend::Sogou, "nihao", body);
    CHECK(ok.size() == 3);
    CHECK(ok.front().text == "你好");
    CHECK(ok.front().covered == 5);
    // Rebuilt by slicing the input at the reported syllable positions.
    CHECK(ok.front().reading == "ni'hao");

    // An abbreviated answer has one-letter "syllables", which are initials, not
    // a reading: the reading is dropped so the candidate cannot be shown as if
    // it were verified.
    const std::string abbreviated = fromHex(kSogouYydsHex);
    const auto yyds = parseCandidates(Backend::Sogou, "yyds", abbreviated);
    CHECK(!yyds.empty());
    CHECK(yyds.front().text == "永远都是");
    CHECK(yyds.front().reading.empty());

    CHECK(parseCandidates(Backend::Sogou, "nihao", "").empty());
    CHECK(parseCandidates(Backend::Sogou, "nihao", std::string(10, '\0')).empty());
}

void testNoCrossProviderGuessing() {
    // A Google payload must not be parsed by the Baidu or Sogou parser: the
    // markers are different, and mis-parsing would emit a candidate nobody
    // asked for.
    CHECK(parseCandidates(Backend::Baidu, "nihao", kGoogleOk).empty());
    CHECK(parseCandidates(Backend::Sogou, "nihao", kGoogleOk).empty());
    CHECK(parseCandidates(Backend::GoogleCn, "nihao", kBaiduNihao).empty());
}

void testNormalizePinyin() {
    CHECK(normalizePinyin("nihao") == std::optional<std::string>("nihao"));
    CHECK(normalizePinyin("ni hao") == std::optional<std::string>("nihao"));
    CHECK(normalizePinyin("ni'hao") == std::optional<std::string>("nihao"));
    CHECK(normalizePinyin("ni  hao") == std::optional<std::string>("nihao"));
}

void testUmlaut() {
    // The preview shows ü while providers spell it v: `lü` must
    // reach them as `lv`, otherwise every word containing ü lost cloud pinyin.
    CHECK(normalizePinyin("l\xc3\xbc") == std::optional<std::string>("lv"));
    CHECK(normalizePinyin("gai l\xc3\xbc kong jian") ==
          std::optional<std::string>("gailvkongjian"));
    CHECK(normalizePinyin("n\xc3\x9c") == std::optional<std::string>("nv"));
}

void testNormalizeRejectsNonPinyin() {
    // Anything that is not pinyin must never be sent upstream: uppercase words,
    // English words, CJK, the placeholder glyph.
    CHECK(!normalizePinyin("Hello").has_value());
    CHECK(!normalizePinyin("ni3hao").has_value());
    CHECK(!normalizePinyin("你好").has_value());
    CHECK(!normalizePinyin("\xe2\x98\x81").has_value()); // ☁
    CHECK(!normalizePinyin("").has_value());
    // A pure separator run has nothing left after strips.
    CHECK(!normalizePinyin("   ").has_value());
}

void testMinimumLength() {
    CHECK(!worthQuerying("nih", 4));
    CHECK(worthQuerying("niha", 4));
    CHECK(worthQuerying("nihao", 4));
    // A nonsense threshold is clamped, not treated as "always query".
    CHECK(worthQuerying("a", 0));
    CHECK(!worthQuerying("abc", 64));
}

/// Boundaries meann "xi'an" and "xian" are different input, and only keeping
/// them lets a vowel-initial syllable be matched instead of guessed.
void testNormalizeSegments() {
    using superior::cloudpinyin::normalizeSegments;
    CHECK(normalizeSegments("ni hao") == std::optional<std::string>("ni'hao"));
    CHECK(normalizeSegments("ni  hao") == std::optional<std::string>("ni'hao"));
    CHECK(normalizeSegments("ni'hao") == std::optional<std::string>("ni'hao"));
    CHECK(normalizeSegments("xi'an") == std::optional<std::string>("xi'an"));
    CHECK(normalizeSegments("NIHAO") == std::optional<std::string>("nihao"));
    CHECK(!normalizeSegments("ni3hao").has_value());
    CHECK(!normalizeSegments("   ").has_value());
}

void testWholePinyinRouting() {
    using superior::cloudpinyin::isWholePinyin;
    // Whole syllables.
    CHECK(isWholePinyin("nihao"));
    CHECK(isWholePinyin("zhongguoren"));
    CHECK(isWholePinyin("xi'an"));
    CHECK(isWholePinyin("ku"));
    // Initials and half-typed syllables are abbreviations.
    CHECK(!isWholePinyin("yyds"));
    CHECK(!isWholePinyin("zhrmghg"));
    CHECK(!isWholePinyin("yuanshenqd"));
}

void testResolveBackend() {
    using superior::cloudpinyin::resolveBackend;
    // An explicit choice is never overridden.
    CHECK(resolveBackend(Backend::GoogleCn, "yyds") == Backend::GoogleCn);
    CHECK(resolveBackend(Backend::Baidu, "zhongguoren") == Backend::Baidu);
    // Auto: full pinyin to the search engine's lexicon, abbreviations to the
    // dictionary that guesses them well.
    CHECK(resolveBackend(Backend::Auto, "zhongguoren") == Backend::Sogou);
    CHECK(resolveBackend(Backend::Auto, "yyds") == Backend::Baidu);
}

void testAlignment() {
    using superior::cloudpinyin::alignReading;
    using superior::cloudpinyin::ReadingMatch;
    CHECK(alignReading("nihao", "ni'hao") == ReadingMatch::Full);
    // Abbreviated: every syllable present, some as a prefix.
    CHECK(alignReading("yyds", "yong'yuan'dou'shi") == ReadingMatch::Abbreviated);
    CHECK(alignReading("yuanshenqd", "yuan'shen'qi'dong") == ReadingMatch::Abbreviated);
    // The whole input has to be consumed.
    CHECK(alignReading("jintianqhh", "jin'tian'qu") == ReadingMatch::None);
    // A vowel-initial syllable needs a separator, or English reads as pinyin.
    CHECK(alignReading("deepseek", "de'e'pi'se'e'ka") == ReadingMatch::None);
    CHECK(alignReading("xi'an", "xi'an") == ReadingMatch::Full);
    CHECK(alignReading("", "ni'hao") == ReadingMatch::None);
    CHECK(alignReading("nihao", "") == ReadingMatch::None);
}

void testUsableCandidate() {
    using superior::cloudpinyin::Proposal;
    using superior::cloudpinyin::usableCandidate;
    // A plain match.
    CHECK(usableCandidate("nihao", Proposal{"你好", 5, "ni'hao"}) ==
          std::optional<std::string>("你好"));
    // Only part of the input covered.
    CHECK(!usableCandidate("nihao", Proposal{"你", 2, "ni"}).has_value());
    // A single character would only displace a local candidate.
    CHECK(!usableCandidate("ni", Proposal{"你", 2, "ni"}).has_value());
    // Mixed scripts cannot map one syllable per character.
    CHECK(!usableCandidate("nihao", Proposal{"nihao", 5, "ni'hao"}).has_value());
    // Character count must equal the syllable count.
    CHECK(!usableCandidate("ganjing", Proposal{"干二净", 7, "gan'jing"}).has_value());
    // No reading means the answer cannot be verified at all.
    CHECK(!usableCandidate("nihao", Proposal{"你好", 5, ""}).has_value());
    // Absurd sizes are refused before any table is allocated.
    CHECK(!usableCandidate(std::string(400, 'a'),
                           Proposal{"你好", 400, "ni'hao"}).has_value());
    CHECK(!usableCandidate("nihao", Proposal{"你好", 5, std::string(400, 'a')}).has_value());
}

void testPickCandidateKeepsProviderOrder() {
    using superior::cloudpinyin::pickCandidate;
    using superior::cloudpinyin::parseCandidates;
    // Baidu's `yyds` answer starts with the pinyin echoed back; the usable word
    // is the next one. Picking must skip it rather than trust position 0.
    const auto proposals = parseCandidates(Backend::Baidu, "yyds", kBaiduAbbreviated);
    CHECK(pickCandidate("yyds", proposals) == std::optional<std::string>("永远的神"));
    // Homophones are still "usable" by shape; what makes them wrong is that the
    // local dictionary has the real word, which is why Auto does not route full
    // pinyin here at all.
    const auto homophones = parseCandidates(Backend::Baidu, "zhongguoren", kBaiduHomophones);
    CHECK(pickCandidate("zhongguoren", homophones) == std::optional<std::string>("中果人"));
    // Sogou's real answer for full pinyin is the accurate one.
    const std::string sogou = fromHex(kSogouNihaoHex);
    CHECK(pickCandidate("nihao", parseCandidates(Backend::Sogou, "nihao", sogou)) ==
          std::optional<std::string>("你好"));
    // An abbreviated Sogou answer has no readable reading, so nothing is picked.
    const std::string abbreviated = fromHex(kSogouYydsHex);
    CHECK(!pickCandidate("yyds", parseCandidates(Backend::Sogou, "yyds", abbreviated))
               .has_value());
}

void testCacheHit() {
    AnswerCache cache(4);
    cache.insert("nihao", "你好");
    CHECK(cache.find("nihao") == std::optional<std::string>("你好"));
    CHECK(!cache.find("nih").has_value());
}

void testCacheEviction() {
    AnswerCache cache(2);
    cache.insert("aa", "1");
    cache.insert("bb", "2");
    cache.insert("cc", "3");
    // Oldest goes first.
    CHECK(!cache.find("aa").has_value());
    CHECK(cache.find("bb").has_value());
    CHECK(cache.find("cc").has_value());
    CHECK(cache.size() == 2);
}

void testCacheRecencyOnHit() {
    AnswerCache cache(2);
    cache.insert("aa", "1");
    cache.insert("bb", "2");
    CHECK(cache.find("aa").has_value()); // promotes "aa"
    cache.insert("cc", "3");
    CHECK(cache.find("aa").has_value());
    CHECK(!cache.find("bb").has_value());
}

void testCacheOverwriteAndClear() {
    AnswerCache cache(2);
    cache.insert("aa", "1");
    cache.insert("aa", "2");
    CHECK(cache.size() == 1);
    CHECK(cache.find("aa") == std::optional<std::string>("2"));
    cache.clear();
    CHECK(!cache.find("aa").has_value());
}

void testErrorBudgetBlocks() {
    ErrorBudget budget(3, 1000);
    CHECK(budget.allowed(0));
    budget.recordFailure(0);
    budget.recordFailure(1);
    CHECK(budget.allowed(2));
    budget.recordFailure(2);
    // Three consecutive failures → blocked.
    CHECK(!budget.allowed(3));
    CHECK(budget.consecutiveErrors() == 3);
}

void testErrorBudgetCooldownExpires() {
    ErrorBudget budget(2, 1000);
    budget.recordFailure(0);
    budget.recordFailure(10);
    CHECK(!budget.allowed(500));
    // After the cooldown, one more chance with a clean slate.
    CHECK(budget.allowed(1010));
    CHECK(budget.consecutiveErrors() == 0);
}

void testErrorBudgetRejectionsDoNotExtend() {
    ErrorBudget budget(2, 1000);
    budget.recordFailure(0);
    budget.recordFailure(0);
    // Attempts made while blocked must not push the deadline out; otherwise a
    // typing user would keep the provider off forever.
    budget.recordFailure(500);
    CHECK(!budget.allowed(900));
    CHECK(budget.allowed(1000));
}

void testErrorBudgetSuccessResets() {
    ErrorBudget budget(2, 1000);
    budget.recordFailure(0);
    budget.recordSuccess();
    CHECK(budget.allowed(1));
    CHECK(budget.consecutiveErrors() == 0);
}

void testErrorBudgetClampsArguments() {
    ErrorBudget budget(0, -5);
    CHECK(budget.maxConsecutiveErrors() == 1);
    budget.recordFailure(0);
    // A negative cooldown must not block forever.
    CHECK(budget.allowed(0));
}

} // namespace

int main() {
    testBackendNames();
    testBackendParsing();
    testBackendRoundTrip();
    testUrlEncoding();
    testRequestUrl();
    testRequestFor();
    testGoogleParsing();
    testBaiduParsing();
    testSogouParsing();
    testNoCrossProviderGuessing();
    testNormalizeSegments();
    testWholePinyinRouting();
    testResolveBackend();
    testAlignment();
    testUsableCandidate();
    testPickCandidateKeepsProviderOrder();
    testNormalizePinyin();
    testNormalizeRejectsNonPinyin();
    testUmlaut();
    testMinimumLength();
    testCacheHit();
    testCacheEviction();
    testCacheRecencyOnHit();
    testCacheOverwriteAndClear();
    testErrorBudgetBlocks();
    testErrorBudgetCooldownExpires();
    testErrorBudgetRejectionsDoNotExtend();
    testErrorBudgetSuccessResets();
    testErrorBudgetClampsArguments();
    return superior::test::finish("cloud pinyin tests");
}
