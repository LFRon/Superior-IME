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
// clone must be able to run `ctest` before `scripts/fetch-rime-data.sh`.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "superior/rime/LibrimePort.hpp"
#include "superior/rime/RimeEngine.hpp"
#include "superior/rime/RimePaths.hpp"

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

    superior::rime::LibrimePort probe;
    if (!probe.available()) {
        return skip("librime not available: " + probe.unavailableReason());
    }

    const superior::rime::Paths paths = superior::rime::Paths::fromEnvironment();
    if (!superior::rime::hasCompiledArtifacts(paths)) {
        return skip("no deployed schema under " + paths.userData.string());
    }

    int failures = 0;
    for (const Case &entry : cases) {
        superior::rime::LibrimePort port;
        superior::rime::RimeEngine engine(port);
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
                superior::rime::KeyInput{static_cast<std::uint32_t>(character), 0, false});
        }

        const superior::rime::Snapshot snapshot = engine.snapshot();
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

    if (failures != 0) {
        std::cerr << "golden pinyin: " << failures << " of " << cases.size() << " failed\n";
        return 1;
    }
    std::cout << "golden pinyin: " << cases.size() << " case(s) passed\n";
    return 0;
}
