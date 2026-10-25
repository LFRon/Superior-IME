// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <string>
#include <string_view>

#include "superior/diagnostics/Log.hpp"
#include "superior/fcitx5/Dictionary.hpp"
#include "superior/fcitx5/PinyinAssets.hpp"

namespace {

namespace fs = std::filesystem;
namespace diag = superior::diagnostics;

void usage(std::ostream &out) {
    out << "usage: superior-dict <command> [options]\n"
           "\n"
           "  diagnose                       show the dictionaries Fcitx5 would load\n"
           "  import <file.scel> [--out DIR] [--overwrite]\n"
           "                                 convert a Sogou cell dictionary into a\n"
           "                                 libime .dict using the same tools and\n"
           "                                 layout as fcitx5-pinyin\n"
           "\n"
           "Environment:\n"
           "  SUPERIOR_LOG_LEVEL, SUPERIOR_LOG, SUPERIOR_LOG_FILE\n"
           "\n"
           "exit codes: 0 success, 1 failed, 2 usage\n";
}

int diagnose() {
    using namespace superior::fcitx5;
    const DictionaryLayout layout = detectDictionaryLayout();
    std::cout << describeDictionaryLayout(layout);

    // Reuse the layout we just resolved: avoids a second filesystem walk and a
    // duplicated debug line.
    const PinyinAssets assets = detectPinyinAssets(layout);
    std::cout << "engine usable   : " << (assets.usable() ? "yes" : "no") << '\n';

    if (const auto converter = findScelConverter(); converter) {
        std::cout << "scel converter  : " << converter->second.string() << '\n';
    } else {
        std::cout << "scel converter  : missing (install fcitx-tools)\n";
    }
    return assets.usable() ? 0 : 1;
}

int importScel(const fs::path &input, const fs::path &targetDirectory, bool overwrite) {
    using namespace superior::fcitx5;

    ImportOptions options;
    options.overwrite = overwrite;
    if (!targetDirectory.empty()) {
        options.targetDirectory = targetDirectory;
    }

    const ImportResult result = importSogouScel(input, options);
    if (!result.ok()) {
        std::cerr << "import failed: " << result.detail << '\n';
        return 1;
    }

    std::cout << "imported: " << result.output.string() << '\n'
              << "  converter : " << result.converter << '\n'
              << "  text bytes: " << result.textBytes << '\n'
              << "  dict bytes: " << result.dictBytes << '\n'
              << "\nRestart Fcitx5 to load the new dictionary:\n"
              << "  fcitx5-remote -e && fcitx5 -d\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    diag::configureFromEnvironment();

    if (argc < 2) {
        usage(std::cerr);
        return 2;
    }

    const std::string_view command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        usage(std::cout);
        return 0;
    }

    if (command == "diagnose") {
        return diagnose();
    }

    if (command == "import") {
        if (argc < 3) {
            std::cerr << "superior-dict: import needs a .scel file\n";
            return 2;
        }
        fs::path input = argv[2];
        fs::path targetDirectory;
        bool overwrite = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--overwrite") {
                overwrite = true;
            } else if (flag == "--out" && i + 1 < argc) {
                targetDirectory = argv[++i];
            } else {
                std::cerr << "superior-dict: unexpected argument '" << flag << "'\n";
                return 2;
            }
        }
        return importScel(input, targetDirectory, overwrite);
    }

    std::cerr << "superior-dict: unknown command '" << command << "'\n";
    usage(std::cerr);
    return 2;
}
