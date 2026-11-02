// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/cloudpinyin/Match.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace superior::cloudpinyin {

namespace {

/// Every syllable a keyboard can produce, `ü` written as `v`.
///
/// Used for one question only: is this input made of whole syllables? A cloud
/// provider is then chosen by shape (see `resolveBackend`), which is a routing
/// decision, not a conversion — the engine owns segmentation for real. That is
/// why the table can be a plain list with no tone or frequency data.
constexpr std::string_view kSyllables =
    "a ai an ang ao "
    "ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu "
    "ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng chi chong chou chu "
    "chua chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo "
    "da dai dan dang dao de dei deng di dia dian diao die ding diu dong dou du duan dui dun duo "
    "e ei en eng er "
    "fa fan fang fei fen feng fo fou fu "
    "ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun guo "
    "ha hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui hun huo "
    "ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun "
    "ka kai kan kang kao ke ken keng kong kou ku kua kuai kuan kuang kui kun kuo "
    "la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu lo long lou lu luan "
    "lue lun luo lv lve "
    "ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu "
    "na nai nan nang nao ne nei nen neng ni nian niang niao nie nin ning niu nong nou nu nuan nue "
    "nuo nv nve "
    "o ou "
    "pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu "
    "qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun "
    "ran rang rao re ren reng ri rong rou ru rua ruan rui run ruo "
    "sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng shi shou shu shua "
    "shuai shuan shuang shui shun shuo si song sou su suan sui sun suo "
    "ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tua tuan tui tun tuo "
    "wa wai wan wang wei wen weng wo wu "
    "xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun "
    "ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun "
    "za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhei zhen zheng zhi zhong "
    "zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo ";

/// Longest syllable above (`zhuang`, `chuang`, `shuang`).
constexpr std::size_t kMaxSyllable = 6;

/// The table as a sorted vector, built once.
///
/// Sorted here instead of written sorted: a 410-entry literal is easy to
/// mis-order, and `binary_search` on a wrong order is silently wrong. Building
/// it is nanoseconds and happens once.
const std::vector<std::string_view> &syllableTable() {
    static const std::vector<std::string_view> table = [] {
        std::vector<std::string_view> words;
        std::size_t start = 0;
        while (start < kSyllables.size()) {
            const std::size_t end = kSyllables.find(' ', start);
            const std::size_t stop = end == std::string_view::npos ? kSyllables.size() : end;
            if (stop > start) {
                words.push_back(kSyllables.substr(start, stop - start));
            }
            start = stop + 1;
        }
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
        return words;
    }();
    return table;
}

bool isSyllable(std::string_view candidate) noexcept {
    const auto &table = syllableTable();
    return std::binary_search(table.begin(), table.end(), candidate);
}

/// Word-break: can `text` be split into whole syllables, end to end?
bool isWholeRun(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    std::vector<char> reachable(text.size() + 1, 0);
    reachable[0] = 1;
    for (std::size_t start = 0; start < text.size(); ++start) {
        if (reachable[start] == 0) {
            continue;
        }
        const std::size_t longest = std::min(kMaxSyllable, text.size() - start);
        for (std::size_t length = 1; length <= longest; ++length) {
            if (isSyllable(text.substr(start, length))) {
                reachable[start + length] = 1;
            }
        }
    }
    return reachable[text.size()] != 0;
}

bool startsWithVowel(std::string_view syllable) noexcept {
    if (syllable.empty()) {
        return false;
    }
    return syllable.front() == 'a' || syllable.front() == 'o' || syllable.front() == 'e';
}

/// One segmentation of `text` into syllables, or empty when there is none.
///
/// The predecessors are tracked so the *chosen* split can be returned: a caller
/// needs the boundaries, not just "yes, it splits".
std::vector<std::string_view> segmentRun(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    std::vector<int> from(text.size() + 1, -1);
    from[0] = 0;
    for (std::size_t start = 0; start < text.size(); ++start) {
        if (from[start] < 0) {
            continue;
        }
        const std::size_t longest = std::min(kMaxSyllable, text.size() - start);
        for (std::size_t length = 1; length <= longest; ++length) {
            if (from[start + length] < 0 && isSyllable(text.substr(start, length))) {
                from[start + length] = static_cast<int>(start);
            }
        }
    }
    if (from[text.size()] < 0) {
        return {};
    }
    std::vector<std::string_view> syllables;
    for (std::size_t end = text.size(); end > 0;) {
        const auto start = static_cast<std::size_t>(from[end]);
        syllables.push_back(text.substr(start, end - start));
        end = start;
    }
    std::reverse(syllables.begin(), syllables.end());
    return syllables;
}

/// Syllables of a reading, or an empty vector when the reading is malformed.
std::vector<std::string_view> splitReading(std::string_view reading) {
    std::vector<std::string_view> syllables;
    std::size_t start = 0;
    while (start <= reading.size()) {
        const std::size_t end = reading.find('\'', start);
        const std::size_t stop = end == std::string_view::npos ? reading.size() : end;
        const std::string_view syllable = reading.substr(start, stop - start);
        if (syllable.empty()) {
            return {};
        }
        syllables.push_back(syllable);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return syllables;
}

/// First byte length of the UTF-8 sequence starting at `index`.
std::size_t sequenceLength(std::string_view text, std::size_t index) noexcept {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
    }
    return std::min(length, text.size() - index);
}

/// Code point at `index`, and its byte length. Invalid bytes decode to 0xFFFD.
std::pair<char32_t, std::size_t> decode(std::string_view text, std::size_t index) noexcept {
    const auto lead = static_cast<unsigned char>(text[index]);
    const std::size_t length = sequenceLength(text, index);
    if (length == 1) {
        return {lead, 1};
    }
    char32_t value = 0;
    switch (length) {
    case 2: value = lead & 0x1FU; break;
    case 3: value = lead & 0x0FU; break;
    default: value = lead & 0x07U; break;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto byte = static_cast<unsigned char>(text[index + offset]);
        if ((byte & 0xC0U) != 0x80U) {
            return {0xFFFD, offset};
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    return {value, length};
}

bool isIdeograph(char32_t value) noexcept {
    return value == 0x3007 || (value >= 0x3400 && value <= 0x4DBF) ||
           (value >= 0x4E00 && value <= 0x9FFF) || (value >= 0xF900 && value <= 0xFAFF) ||
           (value >= 0x20000 && value <= 0x323AF);
}

} // namespace

std::vector<std::string_view> readingSyllables(std::string_view reading) {
    if (reading.empty()) {
        return {};
    }
    if (reading.find('\'') != std::string_view::npos) {
        return splitReading(reading);
    }
    // No boundaries given: segment it ourselves, or refuse the candidate. A
    // reading that cannot be segmented cannot be counted, and a candidate whose
    // character count is unverifiable is not worth the space it would take.
    return segmentRun(reading);
}

std::optional<std::string> normalizeSegments(std::string_view preedit) {
    std::string out;
    out.reserve(preedit.size());
    for (const char character : preedit) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z') {
            out.push_back(static_cast<char>(byte - 'A' + 'a'));
            continue;
        }
        if (byte >= 'a' && byte <= 'z') {
            out.push_back(character);
            continue;
        }
        if (character == '\'' || character == ' ') {
            // Collapse runs and drop a leading separator: `ni  hao` and `'nihao`
            // describe the same typing.
            if (!out.empty() && out.back() != '\'') {
                out.push_back('\'');
            }
            continue;
        }
        // Digits, punctuation, CJK: not a pinyin composition.
        return std::nullopt;
    }
    while (!out.empty() && out.back() == '\'') {
        out.pop_back();
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

bool isWholePinyin(std::string_view segments) {
    if (segments.empty()) {
        return false;
    }
    // With separators the boundaries are known, so each part has to be a
    // syllable outright. Without them, word-break the whole run.
    if (segments.find('\'') == std::string_view::npos) {
        return isWholeRun(segments);
    }
    std::size_t start = 0;
    while (start <= segments.size()) {
        const std::size_t end = segments.find('\'', start);
        const std::size_t stop = end == std::string_view::npos ? segments.size() : end;
        if (!isSyllable(segments.substr(start, stop - start))) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

ReadingMatch alignReading(std::string_view segments, std::string_view reading) noexcept {
    if (segments.empty() || reading.empty()) {
        return ReadingMatch::None;
    }
    const std::vector<std::string_view> syllables = readingSyllables(reading);
    if (syllables.empty()) {
        return ReadingMatch::None;
    }

    const std::size_t count = syllables.size();
    const std::size_t length = segments.size();
    // memo[i * (count + 1) + j]: 0 = unknown, else ReadingMatch + 1.
    std::vector<char> memo((length + 1) * (count + 1), 0);

    // The input is tiny (a composition, not a document), so recursion depth is
    // bounded by the syllable count and the memo removes the branching.
    const auto align = [&](auto &&self, std::size_t at, std::size_t syllable) -> ReadingMatch {
        if (syllable == count) {
            return at == length ? ReadingMatch::Full : ReadingMatch::None;
        }
        if (at < length && segments[at] == '\'') {
            if (at == 0 || syllable == 0) {
                return ReadingMatch::None;
            }
            ++at;
        }
        const std::string_view current = syllables[syllable];
        const bool separated = at > 0 && segments[at - 1] == '\'';
        // A vowel-initial syllable after the first has to be separated: without
        // that rule "deepseek" reads as de'e'pi'se'e'ka and an English word gets
        // a Chinese cloud candidate.
        if (syllable > 0 && !separated && startsWithVowel(current)) {
            return ReadingMatch::None;
        }
        char &seen = memo[at * (count + 1) + syllable];
        if (seen != 0) {
            return static_cast<ReadingMatch>(seen - 1);
        }
        ReadingMatch best = ReadingMatch::None;
        const std::size_t longest = std::min(current.size(), length - at);
        for (std::size_t used = longest; used > 0 && best != ReadingMatch::Full; --used) {
            if (segments.substr(at, used) != current.substr(0, used)) {
                continue;
            }
            const ReadingMatch rest = self(self, at + used, syllable + 1);
            if (rest == ReadingMatch::None) {
                continue;
            }
            const ReadingMatch here =
                used == current.size() && rest == ReadingMatch::Full ? ReadingMatch::Full
                                                                     : ReadingMatch::Abbreviated;
            if (here == ReadingMatch::Full || best == ReadingMatch::None) {
                best = here;
            }
        }
        seen = static_cast<char>(static_cast<int>(best) + 1);
        return best;
    };
    return align(align, 0, 0);
}

std::size_t utf8Length(std::string_view text) noexcept {
    std::size_t characters = 0;
    for (std::size_t index = 0; index < text.size();) {
        index += decode(text, index).second;
        ++characters;
    }
    return characters;
}

bool isIdeographSequence(std::string_view text) noexcept {
    std::size_t characters = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto [value, length] = decode(text, index);
        if (!isIdeograph(value)) {
            return false;
        }
        index += length;
        ++characters;
    }
    return characters >= 2;
}

std::optional<std::string> usableCandidate(std::string_view segments,
                                           const Proposal &proposal) {
    if (proposal.text.empty() || proposal.reading.empty() || segments.empty()) {
        return std::nullopt;
    }
    // Bounds for the alignment below: a composition is a word or a phrase, and a
    // provider reading cannot be longer than what it spells. Without a cap a
    // hostile or broken answer decides how much memory the memo table takes.
    if (segments.size() > 200 || proposal.reading.size() > 256) {
        return std::nullopt;
    }
    // Strip the separators: a provider counts letters, not boundary marks.
    const std::string letters = [&segments] {
        std::string out;
        out.reserve(segments.size());
        for (const char character : segments) {
            if (character != '\'') {
                out.push_back(character);
            }
        }
        return out;
    }();
    if (proposal.covered < static_cast<int>(letters.size())) {
        // Covers only part of what was typed (`jintianqhh` → 今天): the rest of
        // the composition would be left unexplained.
        return std::nullopt;
    }
    if (alignReading(segments, proposal.reading) == ReadingMatch::None) {
        return std::nullopt;
    }
    if (!isIdeographSequence(proposal.text)) {
        // Mixed scripts and single characters cannot map one syllable per
        // character, and a single character is always in the local dictionary.
        return std::nullopt;
    }
    const auto syllables = readingSyllables(proposal.reading).size();
    if (utf8Length(proposal.text) != syllables) {
        return std::nullopt;
    }
    return proposal.text;
}

std::optional<std::string> pickCandidate(std::string_view segments,
                                         std::span<const Proposal> proposals) {
    for (const Proposal &proposal : proposals) {
        if (auto text = usableCandidate(segments, proposal)) {
            return text;
        }
    }
    return std::nullopt;
}

} // namespace superior::cloudpinyin
