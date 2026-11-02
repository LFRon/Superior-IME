// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superior::cloudpinyin {

/// Cloud pinyin providers.
///
/// Deliberately a small set: each one is an endpoint that can disappear, and
/// maintaining a dead provider costs more than it earns.
enum class Backend {
    /// Pick a provider per query (see `resolveBackend`). The default.
    Auto,
    /// `www.google.cn/inputtools` — reachable from mainland China without a
    /// proxy (it resolves to a domestic CDN).
    GoogleCn,
    /// `olimenew.baidu.com/py` — strongest on abbreviated input (`yyds` →
    /// 永远的神), weakest on full pinyin (it answers homophones: `zhongguoren`
    /// → 中果人).
    Baidu,
    /// `shouji.sogou.com/web_ime/mobile.php` — the mobile client's binary
    /// protocol. Strongest on full pinyin (`zhongguoren` → 中国人), generic on
    /// abbreviations (`yyds` → 永远都是).
    Sogou,
};

[[nodiscard]] std::string_view backendName(Backend backend) noexcept;
[[nodiscard]] std::optional<Backend> backendFromString(std::string_view name) noexcept;
[[nodiscard]] std::vector<Backend> allBackends();

/// Percent-encodes a query value. Only `A-Za-z0-9-._~` pass through; everything
/// else becomes `%XX`, so a caller cannot smuggle `&` or `#` into the URL.
[[nodiscard]] std::string urlEncode(std::string_view value);

/// One candidate a provider answered with.
///
/// Providers return a list, not a single word, and the list is not sorted by
/// anything we can use: the first entry is sometimes the pinyin echoed back,
/// sometimes a homophone. Keeping the list is what lets `pickCandidate` apply a
/// rule instead of trusting position 0.
struct Proposal {
    /// UTF-8 candidate text.
    std::string text;
    /// How much of the input this candidate accounts for, in bytes of reading.
    ///
    /// Baidu reports it directly; for Sogou it is the last cumulative input
    /// position. A candidate only covers the composition when this reaches the
    /// input length — abbreviated answers report the *expanded* reading, which
    /// is longer (`yyds` → 14).
    int covered = 0;
    /// The provider's reading, syllables separated by `'`. Empty when the
    /// provider gave none or only syllable ids we cannot turn into letters.
    std::string reading;
};

/// One HTTP request, ready to send.
///
/// Providers differ in shape, not only in host: Baidu and Google are GETs with
/// the pinyin in the query, Sogou wants a POST with an XOR-checksummed binary
/// body. Modelling that here keeps the transport a transport.
struct HttpRequest {
    /// The provider that will answer — `Auto` already resolved, because the
    /// response has to be parsed by the parser of the provider that sent it.
    Backend backend = Backend::Auto;
    std::string url;
    /// Empty for a GET.
    std::string body;
    /// Sent as `Content-Type` when `body` is non-empty.
    std::string contentType;

    [[nodiscard]] bool ok() const noexcept { return !url.empty(); }
};

/// The request for one pinyin string, with `Auto` resolved.
[[nodiscard]] HttpRequest requestFor(Backend backend, std::string_view pinyin);

/// The provider `Auto` picks for this input, or `backend` unchanged.
///
/// Not a preference: the two usable providers are reliably good at opposite
/// things. Whole-syllable input goes to Sogou, anything else (initials, mixed
/// abbreviations, a half-typed last syllable) to Baidu. The measurement is in
/// `docs/cloud-pinyin.md`.
[[nodiscard]] Backend resolveBackend(Backend backend, std::string_view pinyin) noexcept;

/// Full request URL for one pinyin string, or empty when the provider needs more
/// than a URL. Kept for the GET providers and for diagnostics.
[[nodiscard]] std::string requestUrl(Backend backend, std::string_view pinyin);

/// Every candidate a provider answered with, in its own order.
///
/// `pinyin` is what was sent (bare letters). Sogou does not send readings, only
/// syllable positions, so its reading is rebuilt by slicing this input.
[[nodiscard]] std::vector<Proposal> parseCandidates(Backend backend, std::string_view pinyin,
                                                    std::string_view body);

/// Turns a composition into the pinyin the providers expect.
///
/// `ni hao` and `ni'hao` both become `nihao`. Returns `nullopt` when the text
/// is not pure `a-z` — an English word, a mixed composition or the `☁`
/// placeholder must never be sent to a third party.
[[nodiscard]] std::optional<std::string> normalizePinyin(std::string_view preedit);

/// Whether a normalized pinyin is worth a request.
///
/// Short input is skipped on purpose: one or two letters match far too much,
/// so the reply is noise rather than a useful candidate, and every request
/// leaks the user's typing to a third party.
[[nodiscard]] bool worthQuerying(std::string_view normalized, int minimumLength) noexcept;

} // namespace superior::cloudpinyin
