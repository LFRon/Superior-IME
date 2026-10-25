// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/PinyinAssets.hpp"

#include "superior/diagnostics/Log.hpp"
#include "superior/fcitx5/Dictionary.hpp"

namespace superior::fcitx5 {

namespace diag = superior::diagnostics;

PinyinAssets detectPinyinAssets() {
    // Single source of truth: the layout mirrors fcitx5-pinyin's own lookup.
    return detectPinyinAssets(detectDictionaryLayout());
}

PinyinAssets detectPinyinAssets(const DictionaryLayout &layout) {
    PinyinAssets assets;
    assets.systemDict = layout.hasSystemDict();
    assets.systemDictPath = layout.systemDict.string();
    assets.languageModel = !layout.languageModel.empty();
    assets.languageModelPath = layout.languageModel.string();
    assets.userDictionary = layout.hasUserDict();
    assets.userDictionaryPath = layout.userDict.string();

    for (const auto &root : layout.dataRoots) {
        if (!layout.languageModel.empty()) {
            break;
        }
        std::error_code error;
        if (std::filesystem::is_regular_file(root / "libime" / "zh_CN.lm.predict", error)) {
            assets.predictionModel = true;
        }
    }

    if (assets.usable()) {
        SUPERIOR_INFO(diag::categories::Pinyin)
            .field("dict", assets.systemDictPath)
            .field("lm", assets.languageModelPath)
            .field("userDict", assets.userDictionary ? assets.userDictionaryPath
                                                    : std::string("absent"))
            << "libime dictionary available";
    } else {
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("dict", assets.systemDict ? assets.systemDictPath : std::string("missing"))
            .field("lm", assets.languageModel ? assets.languageModelPath : std::string("missing"))
            << "libime dictionary incomplete; pinyin candidates may be empty. "
               "Install libime-data and libime-data-language-model.";
    }

    return assets;
}

} // namespace superior::fcitx5
