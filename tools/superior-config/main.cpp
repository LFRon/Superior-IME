// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "superior/config/Fcitx5Config.hpp"
#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/cloudpinyin/CloudState.hpp"
#include "superior/cloudpinyin/Fetcher.hpp"
#include "superior/config/Settings.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/theme/ClassicUiTheme.hpp"
#include "superior/skin/Skin.hpp"

namespace {

namespace fs = std::filesystem;
namespace diag = superior::diagnostics;
using namespace superior;

void usage(std::ostream &out) {
    out << "usage: superior-config <command> [options]\n"
           "\n"
           "  status                       show the Fcitx5 theme state\n"
           "  theme install [SKIN]         export SKIN as a classicui theme and select it\n"
           "  theme select NAME            select an already installed theme\n"
           "  cloud probe [--backend NAME] [PINYIN]\n"
           "                               ask the cloud provider once and report\n"
           "\n"
           "Inline Chinese/English switching is handled inside the input method and\n"
           "is not a global Fcitx5 hotkey; see docs/input-panel.md.\n"
           "\n"
           "options:\n"
           "  --skin-dir DIR               where skins live (default: <dataHome>/superior-ime/skins)\n"
           "\n"
           "After changing the theme or trigger keys, restart Fcitx5:\n"
           "  fcitx5-remote -e && fcitx5 -d\n";
}

std::vector<fs::path> skinRoots(const config::Paths &paths, const fs::path &override) {
    if (!override.empty()) {
        return {override};
    }
    return config::skinRoots(paths.dataHome, paths.dataDirs);
}

/// Resolves a skin reference to a package directory.
///
/// Accepted forms, in order: an explicit package directory, a directory below
/// `root`, or a skin id matched by scanning `root`. Directory names and skin ids
/// are independent (`skins/default` holds id `org.superior.default`), so a plain
/// path join by id would be wrong.
fs::path resolveSkinPackage(const std::vector<fs::path> &roots, const std::string &reference) {
    std::error_code ec;
    if (!reference.empty()) {
        const fs::path direct(reference);
        if (fs::is_regular_file(direct / "skin.json", ec)) {
            return direct;
        }
    }

    for (const auto &root : roots) {
        if (!reference.empty()) {
            const fs::path underRoot = root / reference;
            if (fs::is_regular_file(underRoot / "skin.json", ec)) {
                return underRoot;
            }
        }
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        for (const auto &entry : fs::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_directory()) {
                continue;
            }
            const auto loaded = skin::loadSkin(entry.path());
            if (!loaded.spec) {
                continue;
            }
            if (reference.empty() || loaded.spec->id == reference) {
                return entry.path();
            }
        }
    }
    return {};
}

int report(const config::Paths &paths, const config::Settings &settings) {
    const auto status = config::inspect(paths, settings.themeName);

    std::cout << "theme active      : "
              << (status.activeTheme.empty() ? "(default)" : status.activeTheme) << '\n';
    std::cout << "theme installed   : " << (status.superiorThemeInstalled ? "yes" : "no") << '\n';
    std::cout << "trigger keys      :";
    for (const auto &key : status.triggerKeys) {
        std::cout << ' ' << key;
    }
    std::cout << '\n';
    std::cout << "available themes  :";
    for (const auto &theme : status.availableThemes) {
        std::cout << ' ' << theme;
    }
    std::cout << '\n';
    std::cout << "settings file     : " << config::defaultSettingsPath().string() << '\n';
    return 0;
}

int handleStatus(const config::Paths &paths) {
    const auto loaded = config::loadSettings(config::defaultSettingsPath());
    for (const auto &warning : loaded.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }
    return report(paths, loaded.settings);
}

int handleApply(const config::Paths &paths) {
    const auto loaded = config::loadSettings(config::defaultSettingsPath());
    for (const auto &warning : loaded.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }

    const auto themeOutcome = config::setClassicUiTheme(paths, loaded.settings.themeName);
    if (!themeOutcome.ok) {
        std::cerr << "theme: " << themeOutcome.detail << '\n';
        return 1;
    }
    std::cout << "theme: " << themeOutcome.detail << '\n';
    if (!themeOutcome.backup.empty()) {
        std::cout << "theme backup: " << themeOutcome.backup.string() << '\n';
    }

    return report(paths, loaded.settings);
}

int handleThemeInstall(const config::Paths &paths, const fs::path &skinOverride,
                       const std::string &skinArgument) {
    const auto loaded = config::loadSettings(config::defaultSettingsPath());
    const std::string skinId = skinArgument.empty() ? loaded.settings.skinId : skinArgument;

    const auto roots = skinRoots(paths, skinOverride);
    const fs::path package = resolveSkinPackage(roots, skinId);
    if (package.empty()) {
        std::cerr << "cannot find skin '" << skinId << "' in the configured skin roots\n";
        return 1;
    }
    const auto skin = skin::loadSkin(package);
    if (!skin.ok()) {
        std::cerr << "cannot load skin '" << package.string() << "': " << skin.summary() << '\n';
        return 1;
    }

    const std::string themeName = skin.spec->id;
    const fs::path staging = fs::temp_directory_path() / ("superior-theme-" + themeName);
    std::error_code ec;
    fs::remove_all(staging, ec);

    theme::ThemeExportOptions options;
    options.overwrite = true;
    const auto exported = theme::exportClassicUiTheme(*skin.spec, staging, options);
    if (!exported.ok) {
        std::cerr << "theme export failed: " << exported.detail << '\n';
        return 1;
    }

    const auto installed = config::installThemeDir(paths, staging, themeName);
    fs::remove_all(staging, ec);
    if (!installed.ok) {
        std::cerr << "theme install failed: " << installed.detail << '\n';
        return 1;
    }
    std::cout << installed.detail << '\n';

    const auto selected = config::setClassicUiTheme(paths, themeName);
    if (!selected.ok) {
        std::cerr << "theme select failed: " << selected.detail << '\n';
        return 1;
    }
    std::cout << selected.detail << '\n';
    return report(paths, loaded.settings);
}

int handleThemeSelect(const config::Paths &paths, const std::string &name) {
    const auto loaded = config::loadSettings(config::defaultSettingsPath());
    const auto selected = config::setClassicUiTheme(paths, name);
    if (!selected.ok) {
        std::cerr << selected.detail << '\n';
        return 1;
    }
    std::cout << selected.detail << '\n';
    return report(paths, loaded.settings);
}

/// One synchronous probe: build the URL, run one transfer, report what came
/// back. Used to answer "is cloud pinyin usable on this network right now?"
/// without having to type into an application and guess.
int handleCloudProbe(const std::string &backendName, const std::string &pinyin) {
    const auto backend = cloudpinyin::backendFromString(backendName);
    if (!backend) {
        std::cerr << "superior-config: unknown cloud backend '" << backendName << "'\n";
        return 2;
    }
    const auto normalized = cloudpinyin::normalizePinyin(pinyin);
    if (!normalized) {
        std::cerr << "superior-config: '" << pinyin << "' is not pinyin (a-z only)\n";
        return 2;
    }

    std::cout << "backend   : " << cloudpinyin::backendName(*backend) << '\n';
    std::cout << "query     : " << *normalized << '\n';
    const std::string url = cloudpinyin::requestUrl(*backend, *normalized);
    if (url.empty()) {
        std::cout << "endpoint  : (none) — this backend has no public endpoint\n";
        return 1;
    }
    std::cout << "endpoint  : " << url << '\n';

    cloudpinyin::Fetcher fetcher(5000);
    if (!fetcher.submit(*normalized, *backend)) {
        std::cout << "result    : cannot start the request\n";
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto results = fetcher.poll();
        if (results.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const auto &result = results.front();
        std::cout << "http      : " << result.httpCode << '\n';
        std::cout << "network   : " << (result.networkError ? "unreachable" : "ok") << '\n';
        if (result.ok && !result.text.empty()) {
            std::cout << "candidate : " << result.text << '\n';
            std::cout << "verdict   : available\n";
            return 0;
        }
        std::cout << "verdict   : no candidate (provider has no answer for this input)\n";
        return 1;
    }
    std::cout << "verdict   : timed out\n";
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    diag::configureFromEnvironment();

    std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.empty()) {
        usage(std::cerr);
        return 2;
    }
    if (arguments.front() == "-h" || arguments.front() == "--help") {
        usage(std::cout);
        return 0;
    }

    fs::path skinOverride;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == "--skin-dir" && i + 1 < arguments.size()) {
            skinOverride = arguments[++i];
        } else {
            positional.push_back(arguments[i]);
        }
    }
    if (positional.empty()) {
        usage(std::cerr);
        return 2;
    }

    const config::Paths paths = config::Paths::fromEnvironment();
    const std::string &command = positional.front();

    if (command == "status") {
        return handleStatus(paths);
    }
    if (command == "apply") {
        return handleApply(paths);
    }
    if (command == "cloud") {
        std::string backend = "googlecn";
        std::string pinyin = "nihao";
        for (std::size_t i = 1; i < positional.size(); ++i) {
            if (positional[i] == "--backend" && i + 1 < positional.size()) {
                backend = positional[++i];
            } else {
                pinyin = positional[i];
            }
        }
        return handleCloudProbe(backend, pinyin);
    }
    if (command == "theme") {
        if (positional.size() < 2) {
            std::cerr << "superior-config: theme needs 'install' or 'select'\n";
            return 2;
        }
        const std::string &action = positional[1];
        if (action == "install") {
            return handleThemeInstall(paths, skinOverride,
                                      positional.size() > 2 ? positional[2] : std::string{});
        }
        if (action == "select") {
            if (positional.size() < 3) {
                std::cerr << "superior-config: theme select needs a name\n";
                return 2;
            }
            return handleThemeSelect(paths, positional[2]);
        }
        std::cerr << "superior-config: unknown theme action '" << action << "'\n";
        return 2;
    }
    std::cerr << "superior-config: unknown command '" << command << "'\n";
    usage(std::cerr);
    return 2;
}
