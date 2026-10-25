// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "superior/diagnostics/Log.hpp"
#include "superior/theme/ClassicUiTheme.hpp"
#include "superior/skin/Skin.hpp"

namespace {

void usage(std::ostream &out) {
    out << "usage: skin-lint <package-dir> [options]\n"
           "\n"
           "  --strict            treat warnings as errors\n"
           "  --json              emit diagnostics as JSON\n"
           "  --print-spec        print the normalized canonical spec\n"
           "  --no-hash           skip hashing asset contents\n"
           "  --export-theme DIR  export as an fcitx5 classicui theme\n"
           "  --theme-style NAME  panel style to project (default: h1, then v1)\n"
           "  --theme-overwrite   allow replacing an existing theme.conf\n"
           "\n"
           "exit codes: 0 clean, 1 diagnostics, 2 usage or I/O failure\n";
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

void printJson(const std::filesystem::path &path, const superior::skin::LoadResult &result) {
    std::cout << "{\n  \"path\": \"" << jsonEscape(path.string()) << "\",\n";
    std::cout << "  \"ok\": " << (result.ok() ? "true" : "false") << ",\n";
    std::cout << "  \"hash\": \""
              << (result.spec ? jsonEscape(result.spec->hash) : std::string()) << "\",\n";
    std::cout << "  \"diagnostics\": [";
    bool first = true;
    for (const auto &diagnostic : result.diagnostics) {
        if (!first) {
            std::cout << ',';
        }
        first = false;
        std::cout << "\n    {\"severity\": \"" << superior::skin::toString(diagnostic.severity)
                  << "\", \"pointer\": \"" << jsonEscape(diagnostic.pointer) << "\", \"code\": \""
                  << jsonEscape(diagnostic.code) << "\", \"message\": \""
                  << jsonEscape(diagnostic.message) << "\"}";
    }
    if (!first) {
        std::cout << '\n';
    }
    std::cout << "  ]\n}\n";
}

} // namespace

int main(int argc, char **argv) {
    superior::diagnostics::configureFromEnvironment();

    std::filesystem::path path;
    bool strict = false;
    bool json = false;
    bool printSpec = false;
    bool noHash = false;
    std::filesystem::path themeDir;
    std::string themeStyle;
    bool themeOverwrite = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--strict") {
            strict = true;
        } else if (argument == "--json") {
            json = true;
        } else if (argument == "--print-spec") {
            printSpec = true;
        } else if (argument == "--no-hash") {
            noHash = true;
        } else if (argument == "--export-theme" && i + 1 < argc) {
            themeDir = argv[++i];
        } else if (argument == "--theme-style" && i + 1 < argc) {
            themeStyle = argv[++i];
        } else if (argument == "--theme-overwrite") {
            themeOverwrite = true;
        } else if (argument == "-h" || argument == "--help") {
            usage(std::cout);
            return 0;
        } else if (argument.starts_with('-')) {
            std::cerr << "skin-lint: unknown option '" << argument << "'\n";
            usage(std::cerr);
            return 2;
        } else if (path.empty()) {
            path = argument;
        } else {
            std::cerr << "skin-lint: unexpected extra argument '" << argument << "'\n";
            return 2;
        }
    }

    if (path.empty()) {
        usage(std::cerr);
        return 2;
    }

    superior::skin::LoadOptions options;
    options.strict = strict;
    options.hashAssets = !noHash;

    const auto result = superior::skin::loadSkin(path, options);

    if (json) {
        printJson(path, result);
    } else {
        for (const auto &diagnostic : result.diagnostics) {
            std::cout << superior::skin::format(diagnostic) << '\n';
        }
        std::cout << path.string() << ": " << result.summary() << '\n';
    }

    if (printSpec && result.spec) {
        std::cout << superior::skin::toCanonicalJson(*result.spec) << '\n';
    }

    if (!themeDir.empty()) {
        if (!result.spec) {
            std::cerr << "skin-lint: cannot export a theme from an invalid skin\n";
            return 1;
        }
        superior::theme::ThemeExportOptions themeOptions;
        themeOptions.styleId = themeStyle;
        themeOptions.overwrite = themeOverwrite;
        const auto exported =
            superior::theme::exportClassicUiTheme(*result.spec, themeDir, themeOptions);
        if (!exported.ok) {
            std::cerr << "skin-lint: theme export failed: " << exported.detail << '\n';
            return 1;
        }
        std::cout << "theme exported: " << exported.directory.string()
                  << " (style " << exported.styleId << ")\n";
    }

    if (result.ok()) {
        return 0;
    }
    return 1;
}
