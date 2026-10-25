// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "superior/diagnostics/Log.hpp"
#include "superior/fcitx5/Dictionary.hpp"
#include "superior/fcitx5/PinyinAssets.hpp"
#include "superior/fcitx5/RimeDict.hpp"

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
           "  rime <file.yaml>... --name NAME [--out DIR] [--overwrite]\n"
           "       [--no-calibrate] [--limit N] [--weight-boost F]\n"
           "                                 convert a Rime dictionary (rime-ice,\n"
           "                                 雾凇, …) into a libime .dict\n"
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

/// Reads a whole file. Returns false and fills `error` on failure.
bool readWholeFile(const fs::path &path, std::string &out, std::string &error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open '" + path.string() + "'";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (!stream.eof() && stream.fail()) {
        error = "cannot read '" + path.string() + "'";
        return false;
    }
    return true;
}

/// Rime dictionaries are split across files (base / ext / tencent …). They are
/// concatenated into one text dictionary so libime sees a single extra dict.
int importRime(const std::vector<fs::path> &inputs, const std::string &name,
               const fs::path &targetDirectory, bool overwrite, bool calibrate,
               std::size_t limit, double weightBoost) {
    using namespace superior::fcitx5;

    RimeConvertOptions options;
    options.calibrateWeights = calibrate;
    options.maxEntries = limit;
    options.weightBoost = weightBoost;
    if (weightBoost > 1.0) {
        // A boosted import is meant to win against the language model, so the
        // default ceiling has to move with it.
        options.maxWeight = 2000000000;
    }

    std::string combined;
    std::size_t totalKept = 0;
    RimeConvertStats total;
    for (const auto &input : inputs) {
        std::string yaml;
        std::string error;
        if (!readWholeFile(input, yaml, error)) {
            std::cerr << "superior-dict: " << error << '\n';
            return 1;
        }
        RimeConvertStats stats;
        const std::string converted = convertRimeDictionary(yaml, options, stats);
        // The limit applies across files, so once it is reached the remaining
        // inputs are skipped rather than silently producing a second dictionary.
        options.maxEntries = limit == 0 ? 0 : (limit > totalKept ? limit - totalKept : 0);
        if (limit != 0 && totalKept >= limit) {
            break;
        }
        combined += converted;
        totalKept += stats.kept;
        total.notHanzi += stats.notHanzi;
        total.badPinyin += stats.badPinyin;
        total.badWeight += stats.badWeight;
        total.duplicate += stats.duplicate;
        std::cout << "read      : " << input.string() << '\n'
                  << "  kept    : " << stats.kept << '\n'
                  << "  skipped : " << stats.skipped() << " (not-hanzi " << stats.notHanzi
                  << ", bad-pinyin " << stats.badPinyin << ", bad-weight " << stats.badWeight
                  << ", duplicate " << stats.duplicate << ")\n";
    }

    if (combined.empty()) {
        std::cerr << "superior-dict: no entries converted; is this a Rime dictionary "
                     "(with a `...` line before the entries)?\n";
        return 1;
    }

    ImportOptions importOptions;
    importOptions.overwrite = overwrite;
    if (!targetDirectory.empty()) {
        importOptions.targetDirectory = targetDirectory;
    }
    const ImportResult result = importTextDictionary(combined, name, importOptions);
    if (!result.ok()) {
        std::cerr << "import failed: " << result.detail << '\n';
        return 1;
    }

    std::cout << "\nentries   : " << totalKept << '\n'
              << "weights   : " << (calibrate ? "log2-calibrated" : "raw")
              << (weightBoost != 1.0 ? " x" + std::to_string(weightBoost) : std::string{})
              << '\n'
              << "text bytes: " << result.textBytes << '\n'
              << "dict bytes: " << result.dictBytes << '\n'
              << "installed : " << result.output.string() << '\n'
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

    if (command == "rime") {
        std::vector<fs::path> inputs;
        std::string name;
        fs::path targetDirectory;
        bool overwrite = false;
        bool calibrate = true;
        double weightBoost = 1.0;
        std::size_t limit = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--name" && i + 1 < argc) {
                name = argv[++i];
            } else if (flag == "--out" && i + 1 < argc) {
                targetDirectory = argv[++i];
            } else if (flag == "--overwrite") {
                overwrite = true;
            } else if (flag == "--no-calibrate") {
                calibrate = false;
            } else if (flag == "--weight-boost" && i + 1 < argc) {
                weightBoost = std::stod(argv[++i]);
            } else if (flag == "--limit" && i + 1 < argc) {
                limit = static_cast<std::size_t>(std::stoul(argv[++i]));
            } else if (!flag.empty() && flag.front() == '-') {
                std::cerr << "superior-dict: unexpected argument '" << flag << "'\n";
                return 2;
            } else {
                inputs.emplace_back(flag);
            }
        }
        if (inputs.empty() || name.empty()) {
            std::cerr << "superior-dict: rime needs at least one .yaml and --name\n";
            return 2;
        }
        return importRime(inputs, name, targetDirectory, overwrite, calibrate, limit,
                          weightBoost);
    }

    std::cerr << "superior-dict: unknown command '" << command << "'\n";
    usage(std::cerr);
    return 2;
}
