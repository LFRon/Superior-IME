// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "superior/fcitx5/Dictionary.hpp"

namespace superior::fcitx5 {

/// Read-only inventory of the pinyin data that Fcitx5/libime would use.
///
/// Superior-IME deliberately does not ship or parse the dictionary format: the
/// engine belongs to Fcitx5 + libime. This helper only reports what is present
/// so startup problems ("engine works but candidates are empty") are diagnosable
/// from the log instead of guesswork.
struct PinyinAssets {
    bool systemDict = false;
    bool languageModel = false;
    bool predictionModel = false;
    bool userDictionary = false;

    std::string systemDictPath;
    std::string languageModelPath;
    std::string userDictionaryPath;

    [[nodiscard]] bool usable() const noexcept { return systemDict && languageModel; }
};

/// Probes the standard libime locations.
PinyinAssets detectPinyinAssets();

/// Reuses an already-resolved layout, avoiding duplicate filesystem walks.
PinyinAssets detectPinyinAssets(const DictionaryLayout &layout);

} // namespace superior::fcitx5
