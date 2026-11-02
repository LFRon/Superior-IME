// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/cloudpinyin/Cloud.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>

#include "superior/cloudpinyin/Match.hpp"

namespace superior::cloudpinyin {

namespace {

constexpr std::string_view kGoogleCnBase =
    "https://www.google.cn/inputtools/request?ime=pinyin&text=";
/// The pinyin goes in `input=`, not `py=`: this is the current endpoint, and the
/// old `olime.baidu.com/py?py=` one answers `{"status":"F","errno":"19"}` for
/// every input.
constexpr std::string_view kBaiduBase =
    "https://olimenew.baidu.com/py?inputtype=py&resultcoding=utf-8&input=";
/// The Android client's endpoint. Fixed query, the pinyin travels in the body.
constexpr std::string_view kSogouBase =
    "http://shouji.sogou.com/web_ime/mobile.php?durtot=0&h=000000000000000"
    "&r=store_mf_wandoujia&v=3.7";
/// Sogou's fixed protocol banner. Not a version we control: it is what the
/// mobile client sends, and the server rejects anything else.
constexpr std::array<std::uint8_t, 7> kSogouToken{0, 5, 0, 0, 0, 0, 1};

bool isUnreserved(unsigned char value) {
    return std::isalnum(value) != 0 || value == '-' || value == '.' || value == '_' ||
           value == '~';
}

char hexDigit(unsigned value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
}

std::uint16_t readU16(std::string_view body, std::size_t at) noexcept {
    if (at + 2 > body.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(static_cast<unsigned char>(body[at]) |
                                      (static_cast<unsigned char>(body[at + 1]) << 8U));
}

/// Appends one UTF-16LE code unit's worth of text as UTF-8.
void appendUtf8(std::string &out, char32_t value) {
    if (value < 0x80) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
}

/// Decodes `bytes` of UTF-16LE into UTF-8, dropping control characters.
std::string decodeUtf16(std::string_view body, std::size_t at, std::size_t bytes) {
    std::string out;
    out.reserve(bytes / 2);
    for (std::size_t offset = 0; offset + 1 < bytes; offset += 2) {
        char32_t value = readU16(body, at + offset);
        if (value >= 0xD800 && value <= 0xDBFF && offset + 3 < bytes) {
            const char32_t low = readU16(body, at + offset + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                value = 0x10000 + ((value - 0xD800) << 10U) + (low - 0xDC00);
                offset += 2;
            }
        }
        if (value < 0x20) {
            continue;
        }
        appendUtf8(out, value);
    }
    return out;
}

/// One `<text, covered, {"pinyin": "..."}>` record of a Baidu result.
///
/// Hand-scanned rather than parsed with a JSON library: this module has no JSON
/// dependency, and the shape is fixed by a server we already treat as
/// untrusted — every read is bounded, and anything unexpected ends the list.
bool readBaiduRecord(std::string_view body, std::size_t &at, Proposal &out) {
    if (at >= body.size() || body[at] != '"') {
        return false;
    }
    const std::size_t textEnd = body.find('"', at + 1);
    if (textEnd == std::string_view::npos || textEnd == at + 1) {
        return false;
    }
    out.text.assign(body.substr(at + 1, textEnd - at - 1));
    at = textEnd + 1;
    if (at >= body.size() || body[at] != ',') {
        return false;
    }
    ++at;
    int covered = 0;
    while (at < body.size() && std::isdigit(static_cast<unsigned char>(body[at])) != 0) {
        covered = covered * 10 + (body[at] - '0');
        ++at;
    }
    out.covered = covered;
    // The reading lives in the metadata object that follows. Bounded by the end
    // of this record so a missing object cannot borrow the next one's reading.
    const std::size_t recordEnd = body.find(']', at);
    const std::size_t limit = recordEnd == std::string_view::npos ? body.size() : recordEnd;
    const std::size_t marker = body.find("\"pinyin\":\"", at);
    if (marker != std::string_view::npos && marker < limit) {
        const std::size_t readingStart = marker + 10; // strlen("\"pinyin\":\"")
        const std::size_t readingEnd = body.find('"', readingStart);
        if (readingEnd != std::string_view::npos && readingEnd > readingStart) {
            out.reading.assign(body.substr(readingStart, readingEnd - readingStart));
        }
    }
    at = limit;
    return !out.text.empty();
}

/// Decodes one record of the Sogou response.
///
/// Layout per record: u16 wordBytes, UTF-16LE word, u16 pinyinBytes (syllable
/// ids we do not use), u16 positionBytes, u16[] cumulative input positions,
/// one flag byte. The last position is how much input the word covers; the
/// positions themselves are the syllable boundaries.
bool readSogouRecord(std::string_view pinyin, std::string_view body, std::size_t &at,
                     Proposal &out) {
    const auto wordBytes = readU16(body, at);
    if (wordBytes == 0 || wordBytes % 2 != 0 || at + 2 + wordBytes > body.size()) {
        return false;
    }
    at += 2;
    out.text = decodeUtf16(body, at, wordBytes);
    at += wordBytes;

    const auto pinyinBytes = readU16(body, at);
    if (at + 2 + pinyinBytes > body.size()) {
        return false;
    }
    at += 2 + pinyinBytes;

    const auto positionBytes = readU16(body, at);
    if (positionBytes % 2 != 0 || at + 2 + positionBytes + 1 > body.size()) {
        return false;
    }
    at += 2;
    const std::size_t positions = positionBytes / 2;
    if (positions == 0) {
        return false;
    }
    out.covered = readU16(body, at + positionBytes - 2);

    // The reading is *rebuilt* by slicing the input at those positions: the
    // pinyin payload itself carries syllable ids, not letters.
    //
    // It is only kept when every piece is a plausible syllable. A one-letter
    // piece is an abbreviation (`yyds` → y|y|d|s), not a pronunciation, so such
    // a candidate ends up with no reading — and `usableCandidate` then refuses
    // it, which is correct: with no reading there is no way to tell whether the
    // word matches what was typed.
    std::string reading;
    std::size_t previous = 0;
    for (std::size_t index = 0; index < positions; ++index) {
        const auto end = readU16(body, at + index * 2);
        if (end <= previous || end > pinyin.size() || end - previous > 6) {
            reading.clear();
            break;
        }
        const std::string_view piece = pinyin.substr(previous, end - previous);
        const bool single = piece.size() == 1;
        const bool vowel = single && (piece == "a" || piece == "o" || piece == "e");
        if (single && !vowel) {
            reading.clear();
            break;
        }
        if (!reading.empty()) {
            reading.push_back('\'');
        }
        reading.append(piece);
        previous = end;
    }
    at += positionBytes + 1; // trailing flag byte
    out.reading = std::move(reading);
    return !out.text.empty();
}

} // namespace

std::string_view backendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::Auto: return "auto";
    case Backend::Baidu: return "baidu";
    case Backend::Sogou: return "sogou";
    case Backend::GoogleCn: break;
    }
    return "googlecn";
}

std::optional<Backend> backendFromString(std::string_view name) noexcept {
    if (name == "auto") {
        return Backend::Auto;
    }
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
    return {Backend::Auto, Backend::GoogleCn, Backend::Baidu, Backend::Sogou};
}

Backend resolveBackend(Backend backend, std::string_view pinyin) noexcept {
    if (backend != Backend::Auto) {
        return backend;
    }
    // Measured 2026-11 against both live endpoints:
    //   zhongguoren  Baidu: 中果人 (homophone)   Sogou: 中国人
    //   yyds         Baidu: 永远的神             Sogou: 永远都是
    //   zhrmghg      Baidu: 中华人民共和国       Sogou: none
    // So the split is by input shape, which is the one signal available before
    // sending anything.
    const auto segments = normalizeSegments(pinyin);
    if (!segments) {
        return Backend::Baidu;
    }
    return isWholePinyin(*segments) ? Backend::Sogou : Backend::Baidu;
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
    switch (resolveBackend(backend, pinyin)) {
    case Backend::Baidu:
        return std::string(kBaiduBase) + encoded;
    case Backend::Sogou:
        // The pinyin travels in the body; there is no URL that means anything
        // on its own.
        return {};
    case Backend::Auto:
    case Backend::GoogleCn:
        break;
    }
    return std::string(kGoogleCnBase) + encoded;
}

HttpRequest requestFor(Backend backend, std::string_view pinyin) {
    HttpRequest request;
    request.backend = resolveBackend(backend, pinyin);

    if (request.backend == Backend::Sogou) {
        // length | banner | length | pinyin | XOR of everything before it.
        //
        // The mobile client's framing, byte for byte: the server answers
        // anything else with an empty body, which is indistinguishable from
        // "no candidates" unless the format is exactly right.
        std::string body;
        body.reserve(pinyin.size() + 10);
        body.push_back(static_cast<char>(kSogouToken.size() + pinyin.size() + 3));
        for (const std::uint8_t byte : kSogouToken) {
            body.push_back(static_cast<char>(byte));
        }
        body.push_back(static_cast<char>(pinyin.size()));
        body.append(pinyin);
        std::uint8_t checksum = 0;
        for (const char character : body) {
            checksum ^= static_cast<std::uint8_t>(character);
        }
        body.push_back(static_cast<char>(checksum));

        request.url = std::string(kSogouBase);
        request.body = std::move(body);
        request.contentType = "application/octet-stream";
        return request;
    }

    request.url = requestUrl(request.backend, pinyin);
    return request;
}

std::vector<Proposal> parseCandidates(Backend backend, std::string_view pinyin,
                                      std::string_view body) {
    std::vector<Proposal> proposals;
    if (body.empty()) {
        return proposals;
    }

    if (backend == Backend::Baidu) {
        // {"status":"T","errno":"0","result":[[["你好",5,{"pinyin":"ni'hao",...}], ...]]}
        if (body.find("\"status\":\"T\"") == std::string_view::npos ||
            body.find("\"errno\":\"0\"") == std::string_view::npos) {
            return proposals;
        }
        std::size_t at = body.find("\"result\":[[[");
        if (at == std::string_view::npos) {
            return proposals;
        }
        at += 12; // strlen("\"result\":[[[")
        while (proposals.size() < 16) {
            Proposal proposal;
            if (!readBaiduRecord(body, at, proposal)) {
                break;
            }
            proposals.push_back(std::move(proposal));
            // `readBaiduRecord` leaves `at` on the `]` that closes this record.
            // What follows is `,["next"` or `]]]}`; skipping `]`, `,` and `[`
            // therefore lands on the next record's text, or on a `]` that ends
            // the list.
            while (at < body.size() && (body[at] == ']' || body[at] == ',' || body[at] == '[')) {
                ++at;
            }
            if (at >= body.size() || body[at] != '"') {
                break;
            }
        }
        return proposals;
    }

    if (backend == Backend::Sogou) {
        // 20-byte header, record count as u16 LE at offset 18.
        const auto count = readU16(body, 18);
        if (body.size() < 22 || count == 0 || count > 32) {
            return proposals;
        }
        std::size_t at = 20;
        for (std::size_t index = 0; index < count; ++index) {
            Proposal proposal;
            if (!readSogouRecord(pinyin, body, at, proposal)) {
                break;
            }
            proposals.push_back(std::move(proposal));
        }
        return proposals;
    }

    if (backend == Backend::GoogleCn) {
        // ["SUCCESS",[["nihao",["你好","拟好"],[],{...}]]]
        if (body.find("\"SUCCESS\"") == std::string_view::npos) {
            return proposals;
        }
        const std::size_t marker = body.find("\",[\"");
        if (marker == std::string_view::npos) {
            return proposals;
        }
        // `marker` is the quote that *closes* the echoed pinyin, so the reading
        // starts at the quote before it. Google sends no syllable boundaries;
        // the run is segmented later by the syllable table.
        std::string reading;
        if (marker > 0) {
            const std::size_t echoStart = body.rfind('"', marker - 1);
            if (echoStart != std::string_view::npos && echoStart + 1 < marker) {
                reading.assign(body.substr(echoStart + 1, marker - echoStart - 1));
            }
        }
        std::size_t at = marker + 4; // strlen("\",[\"")
        while (proposals.size() < 16 && at < body.size()) {
            const std::size_t end = body.find('"', at);
            if (end == std::string_view::npos || end == at) {
                break;
            }
            Proposal proposal;
            proposal.text.assign(body.substr(at, end - at));
            proposal.reading = reading;
            proposal.covered = static_cast<int>(reading.size());
            proposals.push_back(std::move(proposal));
            // Skip `"` and an optional `,`; a `]` ends the list.
            at = end + 1;
            if (at < body.size() && body[at] == ',') {
                ++at;
            }
            if (at >= body.size() || body[at] != '"') {
                break;
            }
        }
        return proposals;
    }

    return proposals;
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
