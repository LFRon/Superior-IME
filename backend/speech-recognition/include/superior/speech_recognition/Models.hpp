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
/// Picking between them is a decision about *how the decoder works*, not only
/// about language, because that decides whether an ordinary laptop can keep up:
///
///   - Paraformer-zh is **non-autoregressive** (~220M): one forward pass through
///     the encoder produces the whole transcript, so the per-step cost is a
///     single encoder pass instead of an encoder pass plus a token-by-token
///     decoder loop. That loop was the dominant cost with FireRed (12 s of speech
///     cost 226 CPU core-seconds here), which is why Paraformer is the default.
///   - FireRedASR2-AED is autoregressive (1.1B) and markedly more accurate on
///     Mandarin, dialects included. Kept for machines with cycles to spare.
///   - Qwen3-ASR-1.7B is the English choice.
///
/// The catalog is the single source of truth shared by the panel, the settings
/// page and (later) the first-run guide: "what can be downloaded" must not be
/// duplicated, or the downloader and the loader will disagree about file names.
struct ModelEntry {
    std::string id;          // "paraformer-zh" — also the directory name
    std::string displayName; // "Paraformer-zh"
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
        // The default: light enough for a laptop that is also running a browser
        // and an IDE, and fast enough to keep up in real time.
        //
        // Upstream is Alibaba's `funasr/paraformer-zh` (Apache-2.0, ~220M,
        // Mandarin + English): https://huggingface.co/funasr/paraformer-zh — it
        // ships PyTorch weights only. `repo` below is its GGUF conversion for the
        // crispasr `paraformer` backend, which is what we can actually load; that
        // conversion carries the FunASR model licence (not Apache-2.0).
        {"paraformer-zh", "Paraformer-zh（推荐·中文·轻量）", "cstr/paraformer-zh-GGUF",
         "paraformer-zh-q4_k.gguf", 128687104, "", "paraformer", "zh,en"},
        {"firered-asr2-aed", "FireRedASR2-AED（高精度·中文）", "cstr/firered-asr2-aed-GGUF",
         "firered-asr2-aed-q4_k.gguf", 963000000, "", "firered-asr", "zh,en,粤语等"},
        {"qwen3-asr-1.7b", "Qwen3-ASR-1.7B（推荐·英语）", "cstr/qwen3-asr-1.7b-GGUF",
         "qwen3-asr-1.7b-q4_k.gguf", 1491000000, "", "qwen3", "en,zh,…"},
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

/// How a stored `speechRecognitionModelId` is turned into a model that exists.
///
/// Two things go wrong with a stored id over time, and both are invisible on
/// screen (the panel would simply highlight nothing):
///
///   - the id belongs to a model that is no longer in the catalog, because it was
///     dropped (Qwen3-ASR-0.6B) or renamed. Fall back to the default.
///   - the id is the *old default*, which the user never chose — it was written by
///     an earlier version. Settings files carry `settingsVersion`, so this is only
///     migrated once; after that a user who really wants FireRed keeps it.
struct ModelChoice {
    std::string id;
    /// The stored id was the pre-v2 default and has been replaced.
    bool migrated = false;
    /// The stored id is not in the catalog at all.
    bool unknown = false;
};

/// `settingsVersion` is the version of the file the id came from (1 = before
/// `settingsVersion` existed).
[[nodiscard]] inline ModelChoice resolveStoredModel(std::string_view stored,
                                                    int settingsVersion) {
    if (stored.empty()) {
        return {std::string(defaultModel().id), false, false};
    }
    if (findModel(stored) == nullptr) {
        return {std::string(defaultModel().id), false, true};
    }
    // The v1 default. Replacing it is the whole point of the bump to v2, and
    // `saveSettings` stamps every file it writes with the current version, so a
    // deliberate choice made afterwards is respected.
    constexpr std::string_view kLegacyDefault = "firered-asr2-aed";
    if (settingsVersion < 2 && stored == kLegacyDefault &&
        defaultModel().id != std::string(kLegacyDefault)) {
        return {std::string(defaultModel().id), true, false};
    }
    return {std::string(stored), false, false};
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
