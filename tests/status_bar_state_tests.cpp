// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"
#include "superior/config/StatusBarState.hpp"

namespace {

namespace fs = std::filesystem;
using superior::config::StatusBarState;

fs::path temporaryDirectory() {
    const fs::path base = fs::temp_directory_path() / "superior-statusbar-tests";
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base, error);
    return base;
}

void writeFile(const fs::path &path, const std::string &text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

void testMissingFileIsNotAnError() {
    const fs::path directory = temporaryDirectory();
    const auto loaded = superior::config::loadStatusBarState(directory / "missing.json");
    CHECK(!loaded.existed);
    CHECK(!loaded.state.hasCustomPosition);
    CHECK(loaded.warnings.empty());
}

void testRoundTrip() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "statusbar.json";

    StatusBarState state;
    state.hasCustomPosition = true;
    state.x = 1234;
    state.y = -56;
    state.placement = "topLeft";

    std::string error;
    CHECK(superior::config::saveStatusBarState(path, state, error));
    CHECK(error.empty());

    const auto loaded = superior::config::loadStatusBarState(path);
    CHECK(loaded.existed);
    CHECK(loaded.state.hasCustomPosition);
    CHECK(loaded.state.x == 1234);
    CHECK(loaded.state.y == -56);
    CHECK(loaded.state.placement == "topLeft");
    CHECK(loaded.warnings.empty());
}

void testSaveCreatesParentDirectory() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "nested" / "deeper" / "statusbar.json";
    StatusBarState state;
    state.hasCustomPosition = true;
    state.x = 10;
    state.y = 20;
    std::string error;
    CHECK(superior::config::saveStatusBarState(path, state, error));
    CHECK(fs::is_regular_file(path));
}

void testUnknownSchemaIsIgnored() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "statusbar.json";
    writeFile(path, R"({"schema":"superior.statusbar/99","x":5,"y":6,"placement":"bottomLeft"})");

    const auto loaded = superior::config::loadStatusBarState(path);
    CHECK(loaded.existed);
    CHECK(!loaded.state.hasCustomPosition);
    CHECK(!loaded.warnings.empty());
}

void testMalformedJsonIsIgnored() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "statusbar.json";
    writeFile(path, "{not json");

    const auto loaded = superior::config::loadStatusBarState(path);
    CHECK(!loaded.state.hasCustomPosition);
    CHECK(!loaded.warnings.empty());
}

void testMissingCoordinatesIsNotCustomPosition() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "statusbar.json";
    writeFile(path, R"({"schema":"superior.statusbar/1","placement":"bottomRight"})");

    const auto loaded = superior::config::loadStatusBarState(path);
    CHECK(!loaded.state.hasCustomPosition);
    CHECK(!loaded.warnings.empty());
}

void testNonIntegerCoordinatesRejected() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "statusbar.json";
    writeFile(path, R"({"schema":"superior.statusbar/1","x":"10","y":20})");

    const auto loaded = superior::config::loadStatusBarState(path);
    CHECK(!loaded.state.hasCustomPosition);
}

void testClear() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "statusbar.json";
    StatusBarState state;
    state.hasCustomPosition = true;
    std::string error;
    CHECK(superior::config::saveStatusBarState(path, state, error));
    CHECK(fs::exists(path));

    CHECK(superior::config::clearStatusBarState(path, error));
    CHECK(!fs::exists(path));

    // Clearing a missing file is not an error: callers do it unconditionally.
    CHECK(superior::config::clearStatusBarState(path, error));
}

void testDefaultPathHonoursXdgConfigHome() {
    const char *previous = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = previous != nullptr ? std::string(previous) : std::string{};

    setenv("XDG_CONFIG_HOME", "/tmp/xdg-config-home-test", 1);
    const fs::path path = superior::config::defaultStatusBarStatePath();
    CHECK(path == fs::path("/tmp/xdg-config-home-test/superior-ime/statusbar.json"));

    if (saved.empty()) {
        unsetenv("XDG_CONFIG_HOME");
    } else {
        setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    }
}

} // namespace

int main() {
    testMissingFileIsNotAnError();
    testRoundTrip();
    testSaveCreatesParentDirectory();
    testUnknownSchemaIsIgnored();
    testMalformedJsonIsIgnored();
    testMissingCoordinatesIsNotCustomPosition();
    testNonIntegerCoordinatesRejected();
    testClear();
    testDefaultPathHonoursXdgConfigHome();
    return superior::test::finish("status bar state tests");
}
