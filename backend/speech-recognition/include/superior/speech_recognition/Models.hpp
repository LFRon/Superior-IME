// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace superior::speech_recognition {

/// One downloadable recognition model.
///
/// The catalog is the single source of truth shared by the panel, the settings
/// page and (later) the first-run guide: "what can be downloaded" must not be
/// duplicated, or the downloader and the loader will disagree about file names.
struct ModelEntry {
    std::string id;          // "qwen3-asr-0.6b" — also the directory name
    std::string displayName; // "Qwen3-ASR-0.6B"
    std::string repo;        // HuggingFace repo id
    std::string file;        // file inside the repo and on disk
    std::uint64_t bytes = 0;
    std::string sha256;      // empty = not pinned yet
    std::string backend;     // crispasr backend name
    std::string languages;
};

/// Ordered: the recommended model first.
[[nodiscard]] inline const std::vector<ModelEntry> &modelCatalog() {
    static const std::vector<ModelEntry> catalog = {
        {"qwen3-asr-0.6b", "Qwen3-ASR-0.6B（推荐）", "cstr/qwen3-asr-0.6b-GGUF",
         "qwen3-asr-0.6b-q4_k.gguf", 631000000, "", "qwen3", "zh,en,…"},
        {"qwen3-asr-1.7b", "Qwen3-ASR-1.7B（高精度）", "cstr/qwen3-asr-1.7b-GGUF",
         "qwen3-asr-1.7b-q4_k.gguf", 1150000000, "", "qwen3", "zh,en,…"},
    };
    return catalog;
}

[[nodiscard]] inline const ModelEntry *findModel(std::string_view id) {
    for (const auto &entry : modelCatalog()) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const ModelEntry &defaultModel() {
    return modelCatalog().front();
}

/// Outcome of looking at a downloaded model file.
enum class ModelFileCheck {
    /// A GGUF container of plausible size.
    Ok,
    /// Missing or unreadable.
    Missing,
    /// Downloaded content is not a GGUF file — usually an error page saved under
    /// a `.gguf` name, which a mirror returns with HTTP 200 surprisingly often.
    NotGguf,
    /// Truncated. The catalogue's byte count is approximate, so only a gross
    /// shortfall is treated as truncation.
    TooSmall,
};

/// Checks a file without loading it: four magic bytes plus a size floor.
[[nodiscard]] ModelFileCheck checkModelFile(const std::filesystem::path &path,
                                            std::uint64_t expectedBytes);

} // namespace superior::speech_recognition
