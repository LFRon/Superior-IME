// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superior::config {

/// Filesystem locations used to configure Fcitx5.
///
/// This module knows INI syntax and XDG paths, but **no Fcitx5 headers**, so a
/// Qt frontend may link it without depending on the platform adapter.
struct Paths {
    std::filesystem::path configHome;
    std::filesystem::path dataHome;
    std::vector<std::filesystem::path> dataDirs;

    [[nodiscard]] static Paths fromEnvironment();

    [[nodiscard]] std::filesystem::path fcitxConfigDir() const;
    [[nodiscard]] std::filesystem::path classicUiConf() const;
    [[nodiscard]] std::filesystem::path pinyinConf() const;
    [[nodiscard]] std::filesystem::path globalConf() const;
    [[nodiscard]] std::filesystem::path userProfile() const;
    [[nodiscard]] std::filesystem::path userThemeDir() const;
};

struct FcitxStatus {
    std::string activeTheme;
    bool darkThemeSelected = false;
    std::vector<std::string> triggerKeys;
    std::vector<std::string> availableThemes;
    bool superiorThemeInstalled = false;
};

[[nodiscard]] FcitxStatus inspect(const Paths &paths, std::string_view superiorThemeName);

struct ApplyOutcome {
    bool ok = false;
    bool changed = false;
    std::string detail;
    std::filesystem::path backup;

    [[nodiscard]] bool healthy() const noexcept { return ok; }
};

/// Selects `themeName` in `classicui.conf` (Theme, DarkTheme, UseDarkTheme).
///
/// A one-time backup `classicui.conf.bak-superior` is written before the first
/// change. Comments and unrelated keys are preserved.
[[nodiscard]] ApplyOutcome setClassicUiTheme(const Paths &paths, std::string_view themeName);

/// `[Fuzzy] Inner` from `conf/pinyin.conf`, or nullopt when unset.
///
/// libime's "inner segment" lets one syllable be split into two: `xian` may match
/// `xi'an`, and — the reason this exists — `bian` may match `bi'an`. That is why
/// `yubianyi` could come out as the four-character 御笔安逸 instead of a
/// three-syllable word. It is on by default upstream.
[[nodiscard]] std::optional<bool> readInnerSegmentation(const Paths &paths);

/// Writes `[Fuzzy] Inner`. `InnerShort` (qie -> qi'e) is deliberately left
/// alone: it only affects very short input, and turning it off would lose 企鹅
/// for `qie`.
///
/// This edits the shared pinyin engine config, so the stock "拼音" entry is
/// affected too — both entries are backed by `Addon=pinyin`.
[[nodiscard]] ApplyOutcome setInnerSegmentation(const Paths &paths, bool enabled);

/// The fuzzy-pinyin pairs that correspond to real typing mistakes:
/// `z/zh`, `c/ch`, `s/sh`, `n/l`, `an/ang`, `en/eng`, `in/ing`.
///
/// This is libime's own typo tolerance. It is the only mechanism that actually
/// works: a dictionary row whose pinyin is not a legal syllable is rejected by
/// `libime_pinyindict`, so Rime-style `derive` spellings (`hzi` for `zhi`)
/// cannot be imported.
inline constexpr std::string_view kCommonFuzzyKeys[] = {"Z_ZH", "C_CH",   "S_SH",  "L_N",
                                                       "AN_ANG", "EN_ENG", "IN_ING"};

/// True when **all** of `kCommonFuzzyKeys` are enabled in `[Fuzzy]`.
[[nodiscard]] bool readCommonFuzzy(const Paths &paths);

/// Enables or disables the whole `kCommonFuzzyKeys` set.
[[nodiscard]] ApplyOutcome setCommonFuzzy(const Paths &paths, bool enabled);

/// Copies an exported theme directory into the user theme directory.
[[nodiscard]] ApplyOutcome installThemeDir(const Paths &paths,
                                           const std::filesystem::path &sourceDirectory,
                                           std::string_view themeName);

} // namespace superior::config
