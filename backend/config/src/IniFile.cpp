// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/config/IniFile.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace superior::config {

namespace {

std::string_view trimView(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string trimCopy(std::string_view text) {
    return std::string(trimView(text));
}

} // namespace

IniFile IniFile::parse(std::string_view text) {
    IniFile file;
    std::string currentSection;
    std::size_t start = 0;

    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string raw(text.substr(start, end - start));
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }

        Line line;
        line.raw = raw;
        const std::string_view trimmed = trimView(raw);

        if (trimmed.empty()) {
            line.kind = Line::Kind::Blank;
        } else if (trimmed.front() == '#' || trimmed.front() == ';') {
            line.kind = Line::Kind::Comment;
        } else if (trimmed.front() == '[' && trimmed.back() == ']') {
            line.kind = Line::Kind::Section;
            line.section = std::string(trimView(trimmed.substr(1, trimmed.size() - 2)));
            currentSection = line.section;
        } else {
            const std::size_t equals = trimmed.find('=');
            if (equals == std::string_view::npos) {
                line.kind = Line::Kind::Other;
            } else {
                line.kind = Line::Kind::Entry;
                line.section = currentSection;
                line.key = std::string(trimView(trimmed.substr(0, equals)));
                line.value = trimCopy(trimmed.substr(equals + 1));
            }
        }

        file.lines_.push_back(std::move(line));
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return file;
}

std::string IniFile::serialize() const {
    std::string out;
    for (const auto &line : lines_) {
        if (line.kind == Line::Kind::Entry) {
            if (line.section.empty()) {
                out.append(line.key).append("=").append(line.value);
            } else {
                out.append(line.key).append("=").append(line.value);
            }
        } else {
            out.append(line.raw);
        }
        out.push_back('\n');
    }
    return out;
}

std::optional<std::string> IniFile::get(std::string_view section, std::string_view key) const {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
        if (it->kind == Line::Kind::Entry && it->section == section && it->key == key) {
            return it->value;
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> IniFile::findEntries(std::string_view section,
                                              std::string_view key) const {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const auto &line = lines_[i];
        if (line.kind == Line::Kind::Entry && line.section == section && line.key == key) {
            found.push_back(i);
        }
    }
    return found;
}

std::size_t IniFile::sectionEnd(std::string_view section) const {
    bool inside = false;
    std::size_t lastContent = 0;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const auto &line = lines_[i];
        if (line.kind == Line::Kind::Section) {
            if (inside) {
                return lastContent + 1;
            }
            inside = line.section == section;
        }
        if (inside && line.kind != Line::Kind::Blank) {
            lastContent = i;
        }
    }
    return inside ? lastContent + 1 : lines_.size();
}

bool IniFile::setImpl(std::string_view section, std::string_view key, std::string_view value) {
    const auto existing = findEntries(section, key);
    if (!existing.empty()) {
        bool changed = false;
        for (std::size_t index : existing) {
            if (lines_[index].value != value) {
                lines_[index].value = std::string(value);
                changed = true;
            }
        }
        if (changed) {
            dirty_ = true;
        }
        return changed;
    }

    const std::string sectionName(section);
    const std::size_t insertAt = sectionEnd(sectionName);

    // Insert at the end of the section, before any trailing blank separator.
    std::size_t position = insertAt;
    while (position > 0 && lines_[position - 1].kind == Line::Kind::Blank) {
        --position;
    }

    Line line;
    line.kind = Line::Kind::Entry;
    line.section = sectionName;
    line.key = std::string(key);
    line.value = std::string(value);
    line.raw = line.key + "=" + line.value;

    if (position >= lines_.size() && sectionName.empty()) {
        lines_.push_back(std::move(line));
    } else {
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(position), std::move(line));
    }
    dirty_ = true;
    return true;
}

bool IniFile::set(std::string_view section, std::string_view key, std::string_view value) {
    return setImpl(section, key, value);
}

bool IniFile::remove(std::string_view section, std::string_view key) {
    const auto existing = findEntries(section, key);
    if (existing.empty()) {
        return false;
    }
    for (auto it = existing.rbegin(); it != existing.rend(); ++it) {
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    dirty_ = true;
    return true;
}

bool IniFile::removeWithPrefix(std::string_view section, std::string_view prefix) {
    bool removed = false;
    for (std::size_t i = lines_.size(); i > 0; --i) {
        const auto &line = lines_[i - 1];
        if (line.kind == Line::Kind::Entry && line.section == section &&
            line.key.rfind(prefix, 0) == 0) {
            lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(i - 1));
            removed = true;
        }
    }
    if (removed) {
        dirty_ = true;
    }
    return removed;
}

std::vector<std::string> IniFile::keys(std::string_view section) const {
    std::vector<std::string> result;
    for (const auto &line : lines_) {
        if (line.kind == Line::Kind::Entry && line.section == section) {
            result.push_back(line.key);
        }
    }
    return result;
}

} // namespace superior::config
