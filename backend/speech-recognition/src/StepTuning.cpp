// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/StepTuning.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include <unistd.h>

#include "superior/paths/Roots.hpp"

namespace superior::speech_recognition {

namespace {

/// Same layout as the log and the recogniser's stderr, so a user who finds one
/// file finds all of them.
std::filesystem::path stateRoot() {
    return ::superior::paths::Roots::fromEnvironment().state;
}

} // namespace

std::filesystem::path defaultTuningPath() {
    const std::filesystem::path root = stateRoot();
    if (root.empty()) {
        return {};
    }
    std::error_code error;
    std::filesystem::create_directories(root / "superior-ime", error);
    if (error) {
        return {};
    }
    return root / "superior-ime" / "streaming-step";
}

StepTuning readTuning(const std::filesystem::path &path) {
    StepTuning tuning;
    if (path.empty()) {
        return tuning;
    }
    std::ifstream file(path);
    if (!file) {
        return tuning;
    }
    // Flat `key value` lines: readable with `cat`, and a malformed line is
    // simply ignored instead of aborting the parse.
    std::string key;
    long long value = 0;
    while (file >> key >> value) {
        if (key == "strikes") {
            tuning.strikes = value < 0 ? 0 : static_cast<int>(std::min<long long>(value, kMaxStrikes));
        } else if (key == "cleanRuns") {
            tuning.cleanRuns = value < 0 ? 0 : static_cast<int>(std::min<long long>(value, 1000));
        }
    }
    return tuning;
}

bool writeTuning(const std::filesystem::path &path, const StepTuning &tuning) {
    if (path.empty()) {
        return false;
    }
    // Written to a sibling and renamed: the file is rewritten at the end of every
    // session, and a crash halfway through must not leave a step the next session
    // would read as "this machine needs 3000 ms".
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) {
            return false;
        }
        file << "strikes " << tuning.strikes << '\n';
        file << "cleanRuns " << tuning.cleanRuns << '\n';
        file.flush();
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace superior::speech_recognition
