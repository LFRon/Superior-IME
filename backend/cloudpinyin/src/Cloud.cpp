// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/cloudpinyin/Cloud.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace superior::cloudpinyin {

namespace {

constexpr std::string_view kGoogleCnBase =
    "https://www.google.cn/inputtools/request?ime=pinyin&text=";
constexpr std::string_view kBaiduBase = "https://olime.baidu.com/py?rn=0&pn=1&ol=1&py=";

bool isUnreserved(unsigned char value) {
    return std::isalnum(value) != 0 || value == '-' || value == '.' || value == '_' ||
           value == '~';
}

/// A cloud pinyin candidate must be Chinese text.
///
/// Beyond the obvious, this is the guard that stops a provider payload from
/// being mis-parsed by the wrong parser: the markers differ between providers,
/// and an ASCII-only "candidate" is always a parsing artifact (usually the
/// pinyin echoed back), never something worth inserting.
bool looksLikeChinese(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char character) {
        return static_cast<unsigned char>(character) >= 0x80;
    });
}

char hexDigit(unsigned value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
}

} // namespace

std::string_view backendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::Baidu: return "baidu";
    case Backend::Sogou: return "sogou";
    case Backend::GoogleCn: break;
    }
    return "googlecn";
}

std::optional<Backend> backendFromString(std::string_view name) noexcept {
    if (name == "googlecn" || name == "google-cn" || name == "google") {
        return Backend::GoogleCn;
    }
    if (name == "baidu") {
        return Backend::Baidu;
    }
    if (name == "sogou") {
        return Backend::Sogou;
    }
    return std::nullopt;
}

std::vector<Backend> allBackends() {
    return {Backend::GoogleCn, Backend::Baidu, Backend::Sogou};
}

std::string urlEncode(std::string_view value) {
    std::string out;
    out.reserve(value.size() * 3);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (isUnreserved(byte)) {
            out.push_back(character);
            continue;
        }
        out.push_back('%');
        out.push_back(hexDigit(byte >> 4U));
        out.push_back(hexDigit(byte & 0x0FU));
    }
    return out;
}

std::string requestUrl(Backend backend, std::string_view pinyin) {
    const std::string encoded = urlEncode(pinyin);
    switch (backend) {
    case Backend::Baidu:
        return std::string(kBaiduBase) + encoded;
    case Backend::Sogou:
        // No public endpoint. Returning an empty URL makes the caller skip the
        // request instead of hitting some invented host.
        return {};
    case Backend::GoogleCn:
        break;
    }
    return std::string(kGoogleCnBase) + encoded;
}

std::optional<std::string> parseTopCandidate(Backend backend, std::string_view body) {
    if (body.empty()) {
        return std::nullopt;
    }

    // Google:  ["SUCCESS",[["nihao",["你好"],[],{...}]]]
    //          ^ the top candidate is the first string after `",[`
    // Baidu:   {"status":"F"...} on failure, otherwise [[ "^候选", ... ]]
    //
    // Both formats have been stable for years, but a provider may change them
    // without notice, so a missing marker simply means "no candidate".
    if (backend == Backend::GoogleCn) {
        // Failures still look like JSON arrays, so check the status word first:
        // otherwise `["FAILED",...]` would be parsed as if it had a candidate.
        if (body.find("\"SUCCESS\"") == std::string_view::npos) {
            return std::nullopt;
        }
        const auto start = body.find("\",[\"");
        if (start == std::string_view::npos) {
            return std::nullopt;
        }
        const auto textStart = start + 4; // strlen("\",[\"")
        const auto end = body.find('"', textStart);
        if (end == std::string_view::npos || end <= textStart) {
            return std::nullopt;
        }
        const auto candidate = body.substr(textStart, end - textStart);
        if (!looksLikeChinese(candidate)) {
            return std::nullopt;
        }
        return std::string(candidate);
    }

    if (backend == Backend::Baidu) {
        if (body.find("\"status\":\"F\"") != std::string_view::npos) {
            return std::nullopt;
        }
        const auto start = body.find("[[\"");
        if (start == std::string_view::npos) {
            return std::nullopt;
        }
        const auto textStart = start + 3; // strlen("[[\"")
        const auto end = body.find("\",", textStart);
        if (end == std::string_view::npos || end <= textStart) {
            return std::nullopt;
        }
        const auto candidate = body.substr(textStart, end - textStart);
        if (!looksLikeChinese(candidate)) {
            return std::nullopt;
        }
        return std::string(candidate);
    }

    return std::nullopt;
}

std::optional<std::string> normalizePinyin(std::string_view preedit) {
    if (preedit.empty()) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(preedit.size());
    for (std::size_t index = 0; index < preedit.size(); ++index) {
        const auto byte = static_cast<unsigned char>(preedit[index]);
        if (byte == ' ' || byte == '\'' || byte == '\t') {
            continue;
        }
        if (byte < 0x80) {
            if (byte < 'a' || byte > 'z') {
                // Uppercase, digits, punctuation: not pinyin. Sending them would
                // be a privacy leak with no upside.
                return std::nullopt;
            }
            out.push_back(static_cast<char>(byte));
            continue;
        }
        // `ü` (U+00FC) is how engines render ü in the composition, while the
        // providers spell it `v` — `lü` must be sent as `lv`, not dropped.
        if (byte == 0xC3 && index + 1 < preedit.size()) {
            const auto next = static_cast<unsigned char>(preedit[index + 1]);
            if (next == 0xBC || next == 0x9C) { // ü / Ü
                out.push_back('v');
                ++index;
                continue;
            }
        }
        // Anything else non-ASCII is CJK, the ☁ placeholder or punctuation.
        return std::nullopt;
    }
    return out.empty() ? std::nullopt : std::optional<std::string>(std::move(out));
}

bool worthQuerying(std::string_view normalized, int minimumLength) noexcept {
    const int threshold = std::clamp(minimumLength, 1, 64);
    return static_cast<int>(normalized.size()) >= threshold;
}

} // namespace superior::cloudpinyin
