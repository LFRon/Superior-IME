// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/config/StatusBarState.hpp"

#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "superior/paths/Roots.hpp"

namespace superior::config {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view kSchema = "superior.statusbar/1";

} // namespace

fs::path defaultStatusBarStatePath() {
    const fs::path configHome = ::superior::paths::Roots::fromEnvironment().config;
    if (configHome.empty()) {
        return {};
    }
    return configHome / "superior-ime" / "statusbar.json";
}

StatusBarLoad loadStatusBarState(const fs::path &path) {
    StatusBarLoad result;
    std::error_code error;
    if (path.empty() || !fs::is_regular_file(path, error)) {
        return result;
    }
    result.existed = true;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.warnings.emplace_back("cannot open status bar state");
        return result;
    }

    Json document;
    try {
        document = Json::parse(std::string{std::istreambuf_iterator<char>(stream),
                                          std::istreambuf_iterator<char>()});
    } catch (const std::exception &exception) {
        result.warnings.emplace_back(std::string("cannot parse status bar state: ") +
                                     exception.what());
        return result;
    }

    const std::string schema = document.value("schema", std::string{});
    if (schema != kSchema) {
        result.warnings.push_back("unknown status bar state schema '" + schema + "', ignoring");
        return result;
    }

    StatusBarState &state = result.state;
    state.placement = document.value("placement", state.placement);
    // Coordinates are only meaningful together with a placement, so require
    // both to be present and integral.
    if (document.contains("x") && document.contains("y") && document["x"].is_number_integer() &&
        document["y"].is_number_integer()) {
        state.x = document["x"].get<int>();
        state.y = document["y"].get<int>();
        state.hasCustomPosition = true;
    } else {
        result.warnings.emplace_back("status bar state has no usable coordinates");
    }
    return result;
}

bool saveStatusBarState(const fs::path &path, const StatusBarState &state, std::string &error) {
    if (path.empty()) {
        error = "no status bar state path (HOME is unset)";
        return false;
    }

    Json document;
    document["schema"] = std::string(kSchema);
    document["x"] = state.x;
    document["y"] = state.y;
    document["placement"] = state.placement;

    std::error_code code;
    fs::create_directories(path.parent_path(), code);
    if (code) {
        error = "cannot create '" + path.parent_path().string() + "': " + code.message();
        return false;
    }

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot write '" + temporary.string() + "'";
            return false;
        }
        const std::string text = document.dump(2) + "\n";
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            error = "short write on '" + temporary.string() + "'";
            return false;
        }
    }
    fs::rename(temporary, path, code);
    if (code) {
        error = "cannot replace '" + path.string() + "': " + code.message();
        return false;
    }
    return true;
}

bool clearStatusBarState(const fs::path &path, std::string &error) {
    if (path.empty()) {
        return true;
    }
    std::error_code code;
    fs::remove(path, code);
    if (code) {
        error = "cannot remove '" + path.string() + "': " + code.message();
        return false;
    }
    return true;
}

} // namespace superior::config
