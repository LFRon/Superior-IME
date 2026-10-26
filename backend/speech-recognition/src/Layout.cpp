// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Layout.hpp"

#include <cstdlib>
#include <system_error>

#include <unistd.h>

#include "superior/paths/Roots.hpp"
#include "superior/speech_recognition/Models.hpp"

namespace superior::speech_recognition {

namespace fs = std::filesystem;

namespace {

fs::path dataHome() {
    return ::superior::paths::Roots::fromEnvironment().data;
}

fs::path runtimeHome() {
    const fs::path runtime = ::superior::paths::Roots::fromEnvironment().runtime;
    return runtime.empty() ? fs::temp_directory_path() : runtime;
}

fs::path cacheHome() {
    return ::superior::paths::Roots::fromEnvironment().cache;
}

/// The file name the engine itself uses for its Silero weights.
constexpr std::string_view kVadModelFile = "ggml-silero-v6.2.0.bin";

bool isExecutableFile(const fs::path &path) {
    std::error_code error;
    if (path.empty() || !fs::is_regular_file(path, error)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

/// Looks up a bare program name in `PATH`.
fs::path findOnPath(std::string_view name) {
    const char *path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    std::string_view rest(path);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto entry = rest.substr(0, colon);
        if (!entry.empty()) {
            const fs::path candidate = fs::path(entry) / name;
            if (isExecutableFile(candidate)) {
                return candidate;
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(colon + 1);
    }
    return {};
}

} // namespace

fs::path dataRoot() {
    if (const char *override = std::getenv("SUPERIOR_SPEECH_DIR");
        override != nullptr && override[0] != '\0') {
        return override;
    }
    const fs::path home = dataHome();
    if (home.empty()) {
        return {};
    }
    return home / "superior-ime" / "speech-recognition";
}

fs::path engineDirectory() {
    const fs::path root = dataRoot();
    return root.empty() ? fs::path{} : root / "engine";
}

fs::path enginePath() {
    const fs::path dir = engineDirectory();
    return dir.empty() ? fs::path{} : dir / "crispasr";
}

fs::path modelsRoot() {
    const fs::path root = dataRoot();
    return root.empty() ? fs::path{} : root / "models";
}

fs::path modelPath(std::string_view id) {
    const ModelEntry *entry = findModel(id);
    const fs::path root = modelsRoot();
    if (entry == nullptr || root.empty()) {
        return {};
    }
    return root / entry->id / entry->file;
}

fs::path punctuationModelPath() {
    const fs::path root = modelsRoot();
    if (root.empty()) {
        return {};
    }
    const fs::path candidate = root / "punctuation" / "fireredpunc-q4_k.gguf";
    std::error_code error;
    if (!fs::is_regular_file(candidate, error)) {
        return {};
    }
    return candidate;
}

fs::path vadModelPath() {
    std::error_code error;
    if (const char *override = std::getenv("SUPERIOR_ASR_VAD_MODEL");
        override != nullptr && override[0] != '\0') {
        const fs::path path(override);
        return fs::is_regular_file(path, error) ? path : fs::path{};
    }
    // The managed copy first: this is where `install-speech-engine.sh` puts it.
    const fs::path root = modelsRoot();
    if (!root.empty()) {
        const fs::path managed = root / "vad" / kVadModelFile;
        if (fs::is_regular_file(managed, error)) {
            return managed;
        }
    }
    // Second chance: an engine that ran with `--vad` before keeps the weights in
    // its own cache. Reusing that copy gives an existing install gating without a
    // second download, and — unlike passing `--vad` alone — never starts one.
    const fs::path cache = cacheHome();
    if (!cache.empty()) {
        const fs::path cached = cache / "crispasr" / kVadModelFile;
        if (fs::is_regular_file(cached, error)) {
            return cached;
        }
    }
    return {};
}

bool customEngineConfigured() {
    const char *custom = std::getenv("SUPERIOR_ASR_CMD");
    return custom != nullptr && custom[0] != '\0';
}

std::string resolveEngine() {
    // An explicit override wins: that is how a distribution package, a test or a
    // user with a hand-built crispasr points us at the right binary.
    if (const char *override = std::getenv("SUPERIOR_ASR_ENGINE");
        override != nullptr && override[0] != '\0') {
        return override;
    }
    const fs::path managed = enginePath();
    if (isExecutableFile(managed)) {
        return managed.string();
    }
    const fs::path onPath = findOnPath("crispasr");
    return onPath.empty() ? std::string{} : onPath.string();
}

bool enginePresent() {
    // A fully custom command line counts as an engine: it is how a test, a
    // distribution or a user with a different recogniser plugs in.
    return customEngineConfigured() || !resolveEngine().empty();
}

bool modelPresent(std::string_view id) {
    // The same check the downloader applies, so "present" cannot mean "a file
    // exists with the right name" — it means the model can actually be loaded.
    const ModelEntry *entry = findModel(id);
    if (entry == nullptr) {
        return false;
    }
    return checkModelFile(modelPath(id), entry->bytes) == ModelFileCheck::Ok;
}

fs::path temporaryAudioPath() {
    const fs::path dir = runtimeHome() / "superior-ime" / "speech-recognition";
    std::error_code error;
    fs::create_directories(dir, error);
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, error);
    return dir / ("capture-" + std::to_string(::getpid()) + ".wav");
}

} // namespace superior::speech_recognition
