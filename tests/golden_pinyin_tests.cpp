// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden pinyin regression test.
//
// The decision-layer tests prove *behaviour* without any data. This one proves
// the *data*: typing a real pinyin sequence into a real librime session must
// still put the expected word first. Without it, a 万象 update or a schema
// patch can silently change what every user sees and nothing fails.
//
// It is deliberately tolerant about the environment: no librime, no deployed
// schema, or an unreadable golden file all mean "skipped", because a fresh
// clone must be able to run `ctest` before `scripts/fetch-pinyin-data.sh`.
//
// The session runs against a **private** user directory that contains only the
// compiled artifacts. The real one also holds the user's `userdb`, and learned
// weights legitimately reorder candidates ("原神" vs "原身"); a regression test
// must fail on data changes, not on what the developer happened to type last.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>
#include <vector>

#include "superior/pinyin/LibrimePort.hpp"
#include "superior/pinyin/RimeEngine.hpp"
#include "superior/pinyin/Layout.hpp"

namespace pinyin = superior::pinyin;

namespace {

namespace fs = std::filesystem;

struct Case {
    std::string pinyin;
    std::string expected;
};

std::vector<Case> loadCases(const fs::path &path) {
    std::vector<Case> cases;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream fields(line);
        Case entry;
        if (!(fields >> entry.pinyin >> entry.expected)) {
            continue;
        }
        cases.push_back(std::move(entry));
    }
    return cases;
}

/// Builds a throw-away user directory that mirrors only what the schema needs.
///
/// Copying `build/` costs ~67 MB once per run; that is the price of a
/// deterministic result and it is still far cheaper than a deploy.
std::filesystem::path makeIsolatedUserDir(const std::filesystem::path &source,
                                         std::string &error) {
    std::error_code code;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(code) /
        ("superior-golden-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root, code);
    if (code) {
        error = code.message();
        return {};
    }

    const auto build = source / "build";
    if (std::filesystem::is_directory(build, code)) {
        std::filesystem::copy(build, root / "build",
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              code);
        if (code) {
            error = code.message();
            return root;
        }
    }
    for (const char *name : {"installation.yaml", "user.yaml", "default.custom.yaml"}) {
        const auto file = source / name;
        if (std::filesystem::is_regular_file(file, code)) {
            std::filesystem::copy_file(file, root / name,
                                       std::filesystem::copy_options::overwrite_existing, code);
            code.clear();
        }
    }
    return root;
}

int skip(const std::string &reason) {
    std::cout << "golden pinyin: skipped (" << reason << ")\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        return skip("no golden file argument");
    }
    const fs::path goldenPath = argv[1];
    std::error_code error;
    if (!fs::is_regular_file(goldenPath, error)) {
        return skip("golden file not found: " + goldenPath.string());
    }

    const auto cases = loadCases(goldenPath);
    if (cases.empty()) {
        return skip("no cases in " + goldenPath.string());
    }

    superior::pinyin::LibrimePort probe;
    if (!probe.available()) {
        return skip("librime not available: " + probe.unavailableReason());
    }

    const superior::pinyin::Paths deployed = superior::pinyin::Paths::fromEnvironment();
    if (!superior::pinyin::hasCompiledArtifacts(deployed)) {
        return skip("no deployed schema under " + deployed.userData.string());
    }

    std::string isolationError;
    const std::filesystem::path isolatedUser = makeIsolatedUserDir(deployed.userData,
                                                                   isolationError);
    if (isolatedUser.empty() || !isolationError.empty()) {
        return skip("cannot isolate the user directory: " + isolationError);
    }

    superior::pinyin::Paths paths = deployed;
    paths.userData = isolatedUser;

    int failures = 0;
    for (const Case &entry : cases) {
        superior::pinyin::LibrimePort port;
        superior::pinyin::RimeEngine engine(port);
        std::string message;
        if (!engine.start(paths, message)) {
            std::cerr << "golden pinyin: cannot start the engine: " << message << '\n';
            return 1;
        }
        if (engine.needsDeploy()) {
            engine.stop();
            return skip("schema needs deployment");
        }

        for (const char character : entry.pinyin) {
            if (character < 'a' || character > 'z') {
                std::cerr << "golden pinyin: '" << entry.pinyin << "' is not plain a-z\n";
                engine.stop();
                return 2;
            }
            // ASCII keysyms are the character codes themselves.
            engine.processKey(
                superior::pinyin::KeyInput{static_cast<std::uint32_t>(character), 0, false});
        }

        const superior::pinyin::Snapshot snapshot = engine.snapshot();
        const std::string actual =
            snapshot.menu.candidates.empty() ? std::string{} : snapshot.menu.candidates.front().text;
        if (actual != entry.expected) {
            std::cerr << "golden pinyin: FAIL " << entry.pinyin << " expected '" << entry.expected
                      << "' got '" << actual << "'\n";
            ++failures;
        } else {
            std::cout << "golden pinyin: ok " << entry.pinyin << " -> " << actual << '\n';
        }
        engine.stop();
    }

    std::error_code cleanup;
    std::filesystem::remove_all(isolatedUser, cleanup);

    if (failures != 0) {
        std::cerr << "golden pinyin: " << failures << " of " << cases.size() << " failed\n";
        return 1;
    }
    std::cout << "golden pinyin: " << cases.size() << " case(s) passed\n";
    return 0;
}
