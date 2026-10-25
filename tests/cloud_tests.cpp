// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "support/TestSupport.hpp"
#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/cloudpinyin/CloudState.hpp"

namespace {

using namespace superior::cloudpinyin;

// Real payloads captured from the live endpoints (2026-10-24).
constexpr std::string_view kGoogleOk =
    R"(["SUCCESS",[["nihao",["你好"],[],{"annotation":["ni hao"],"candidate_type":[0],"lc":["16 16"]}]]])";
constexpr std::string_view kGoogleEmpty = R"(["SUCCESS",[["zzzzzz",[],[]]]])";
constexpr std::string_view kGoogleFailed = R"(["FAILED",["internal error"]])";
constexpr std::string_view kBaiduDead =
    R"({"status":"F","errno":"19","errmsg":"Internal Error", "result":[]})";
constexpr std::string_view kBaiduOk = R"([["你好","ni hao"],["拟好","ni hao"]])";

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
          "https://olime.baidu.com/py?rn=0&pn=1&ol=1&py=nihao");
    // Sogou has no endpoint; an empty URL makes the caller skip instead of
    // inventing a host.
    CHECK(requestUrl(Backend::Sogou, "nihao").empty());
}

void testGoogleParsing() {
    CHECK(parseTopCandidate(Backend::GoogleCn, kGoogleOk) == std::optional<std::string>("你好"));
    // No candidate for this input is not an error, just no answer.
    CHECK(!parseTopCandidate(Backend::GoogleCn, kGoogleEmpty).has_value());
    CHECK(!parseTopCandidate(Backend::GoogleCn, kGoogleFailed).has_value());
    CHECK(!parseTopCandidate(Backend::GoogleCn, "").has_value());
    CHECK(!parseTopCandidate(Backend::GoogleCn, "not json at all").has_value());
}

void testBaiduParsing() {
    CHECK(parseTopCandidate(Backend::Baidu, kBaiduOk) == std::optional<std::string>("你好"));
    // The endpoint currently answers this for every input; it must read as
    // "unavailable", not as a candidate named "F".
    CHECK(!parseTopCandidate(Backend::Baidu, kBaiduDead).has_value());
    CHECK(!parseTopCandidate(Backend::Baidu, "").has_value());
}

void testNoCrossProviderGuessing() {
    // A Google payload must not be parsed by the Baidu parser: the markers are
    // different, and mis-parsing would emit a candidate nobody asked for.
    CHECK(!parseTopCandidate(Backend::Baidu, kGoogleOk).has_value());
    CHECK(!parseTopCandidate(Backend::Sogou, kGoogleOk).has_value());
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
    testGoogleParsing();
    testBaiduParsing();
    testNoCrossProviderGuessing();
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
