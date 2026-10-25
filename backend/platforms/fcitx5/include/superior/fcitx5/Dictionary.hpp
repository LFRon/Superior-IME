// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace superior::fcitx5 {

/// Where Fcitx5's pinyin addon keeps its dictionaries.
///
/// Superior-IME does **not** own a dictionary format. The engine is
/// `fcitx5-pinyin` + `libime`, so the only correct alignment is to consume the
/// exact same files in the exact same locations. Nothing is copied, converted
/// or duplicated at runtime.
///
/// Layout mirrored from `fcitx5-pinyin` 5.1.6 (`PinyinEngine::PinyinEngine`,
/// `loadExtraDict`, `save`):
///
///   system  : <datadir>/libime/sc.dict              (SystemDict)
///             <datadir>/libime/extb.dict            (built-in extension)
///             <pkgdatadir>/pinyin/chaizi.dict
///   user    : <pkgdata>/pinyin/user.dict            (UserDict)
///             <pkgdata>/pinyin/user.history         (user language model)
///   extra   : <pkgdata>/pinyin/dictionaries/*.dict  (Sogou imports land here)
///             <pkgdata>/pinyin/dictionaries/*.dict.disable
///   lm      : zh_CN.lm via libime's resolver
struct DictionaryLayout {
    std::vector<std::filesystem::path> dataRoots;
    std::vector<std::filesystem::path> pkgDataRoots;

    std::filesystem::path systemDict;
    std::filesystem::path extbDict;
    std::filesystem::path chaiziDict;

    std::filesystem::path userDict;
    std::filesystem::path userHistory;
    std::filesystem::path userDictDirectory;

    std::filesystem::path languageModel;

    std::vector<std::filesystem::path> extraDictionaries;
    std::vector<std::filesystem::path> disabledDictionaries;

    [[nodiscard]] bool hasSystemDict() const noexcept { return !systemDict.empty(); }
    [[nodiscard]] bool hasUserDict() const noexcept { return !userDict.empty(); }
    [[nodiscard]] bool usable() const noexcept { return hasSystemDict(); }
};

/// Resolves the layout from XDG variables, with the same precedence Fcitx5
/// uses (XDG_DATA_HOME, then XDG_DATA_DIRS, then /usr/local/share:/usr/share).
[[nodiscard]] DictionaryLayout detectDictionaryLayout();

/// Human-readable summary for `--diagnose` style output. Never includes
/// dictionary contents.
[[nodiscard]] std::string describeDictionaryLayout(const DictionaryLayout &layout);

// ------------------------------------------------------------------ importing

enum class ImportTool {
    Scel2Org5,
    Scel2Org,
};

struct ImportOptions {
    /// Directory that receives the generated `.dict`. Defaults to
    /// `<pkgdata>/pinyin/dictionaries`.
    std::filesystem::path targetDirectory;

    /// Overwrite an existing file with the same stem.
    bool overwrite = false;

    /// Reject inputs larger than this. Sogou cell dictionaries are small.
    std::uintmax_t maxInputBytes = 64U * 1024U * 1024U;
};

enum class ImportError {
    None,
    InputMissing,
    InputNotRegular,
    InputTooLarge,
    InputUnreadable,
    TargetUnwritable,
    ToolMissing,
    ConversionFailed,
    DictionaryBuildFailed,
    OutputEmpty,
    OutputTooLarge,
    RenameFailed,
};

struct ImportResult {
    ImportError error = ImportError::None;
    std::string detail;
    std::filesystem::path output;
    std::string converter;   // resolved tool name, for diagnostics
    std::uintmax_t textBytes = 0;
    std::uintmax_t dictBytes = 0;

    [[nodiscard]] bool ok() const noexcept { return error == ImportError::None; }
};

/// Converts a Sogou `.scel` cell dictionary into a libime binary `.dict`.
///
/// This replicates what `fcitx5-pinyin`'s dictionary manager does:
///
///     scel2org5 <input.scel> -o <tmp.txt>
///     libime_pinyindict <tmp.txt> <tmp.dict>
///     rename <tmp.dict> <target>/<stem>.dict
///
/// The tools are executed with `fork`/`execvp` and an explicit argv array:
/// no shell is involved, so a crafted file name cannot inject commands. Both
/// temporary files are created with `mkstemp` inside the target directory and
/// removed on every failure path.
[[nodiscard]] ImportResult importSogouScel(const std::filesystem::path &scelFile,
                                           const ImportOptions &options = {});

/// Path that `importSogouScel` would write for a given input.
[[nodiscard]] std::filesystem::path defaultImportTarget(const std::filesystem::path &scelFile,
                                                        const DictionaryLayout &layout);

/// Locates the converter, preferring the Fcitx5-era `scel2org5`.
[[nodiscard]] std::optional<std::pair<ImportTool, std::filesystem::path>>
findScelConverter();

} // namespace superior::fcitx5
