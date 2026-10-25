// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superior::config {

/// Minimal INI editor that preserves the original file as much as possible.
///
/// Fcitx5 configuration files are hand-written INI with comments and localized
/// captions. Rewriting them from a parsed model would delete those comments, so
/// this class keeps every original line and only replaces the value of the key
/// it is asked to change, appending new keys at the end of their section.
class IniFile {
public:
    static IniFile parse(std::string_view text);

    [[nodiscard]] std::string serialize() const;

    [[nodiscard]] std::optional<std::string> get(std::string_view section,
                                                 std::string_view key) const;

    /// Returns true when the file changed.
    bool set(std::string_view section, std::string_view key, std::string_view value);

    /// Removes every occurrence of `key` inside `section`.
    bool remove(std::string_view section, std::string_view key);

    /// Removes every key whose name starts with `prefix` inside `section`.
    bool removeWithPrefix(std::string_view section, std::string_view prefix);

    [[nodiscard]] std::vector<std::string> keys(std::string_view section) const;

private:
    struct Line {
        enum class Kind { Blank, Comment, Section, Entry, Other };

        Kind kind = Kind::Other;
        std::string raw;
        std::string section;
        std::string key;
        std::string value;
    };

    [[nodiscard]] bool setImpl(std::string_view section, std::string_view key,
                               std::string_view value);
    [[nodiscard]] std::vector<std::size_t> findEntries(std::string_view section,
                                                       std::string_view key) const;
    [[nodiscard]] std::size_t sectionEnd(std::string_view section) const;

    std::vector<Line> lines_;
    bool dirty_ = false;
};

} // namespace superior::config
