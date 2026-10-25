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
    /// `www.google.cn/inputtools` — reachable from mainland China without a
    /// proxy (it resolves to a domestic CDN), which is why it is the default.
    GoogleCn,
    /// `olime.baidu.com/py` — kept as an option, but the endpoint currently
    /// answers `{"status":"F","errno":"19"}` for every input, so the probe
    /// marks it unavailable instead of letting it look "enabled but broken".
    Baidu,
    /// Sogou has no documented endpoint; the slot exists so a future provider
    /// can be added without touching call sites.
    Sogou,
};

[[nodiscard]] std::string_view backendName(Backend backend) noexcept;
[[nodiscard]] std::optional<Backend> backendFromString(std::string_view name) noexcept;
[[nodiscard]] std::vector<Backend> allBackends();

/// Percent-encodes a query value. Only `A-Za-z0-9-._~` pass through; everything
/// else becomes `%XX`, so a caller cannot smuggle `&` or `#` into the URL.
[[nodiscard]] std::string urlEncode(std::string_view value);

/// Full request URL for one pinyin string.
[[nodiscard]] std::string requestUrl(Backend backend, std::string_view pinyin);

/// Top candidate from a provider response.
///
/// Returns `nullopt` for every "no answer" shape: empty body, dead endpoint,
/// rate limit, malformed JSON. Callers must treat that as "nothing to offer",
/// never as a reason to guess.
[[nodiscard]] std::optional<std::string> parseTopCandidate(Backend backend,
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
