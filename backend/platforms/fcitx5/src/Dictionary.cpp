// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/Dictionary.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "superior/diagnostics/Log.hpp"

extern char **environ;

namespace superior::fcitx5 {

namespace {

namespace fs = std::filesystem;
namespace diag = superior::diagnostics;

constexpr std::uintmax_t kMaxTextBytes = 256U * 1024U * 1024U;
constexpr std::uintmax_t kMaxDictBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaxCapturedOutput = 4096;

std::vector<fs::path> splitPathList(const char *raw) {
    std::vector<fs::path> paths;
    if (raw == nullptr) {
        return paths;
    }
    std::string text(raw);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t position = text.find(':', start);
        const std::size_t end = position == std::string::npos ? text.size() : position;
        if (end > start) {
            paths.emplace_back(text.substr(start, end - start));
        }
        if (position == std::string::npos) {
            break;
        }
        start = position + 1;
    }
    return paths;
}

bool isRegularFile(const fs::path &path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return fs::is_regular_file(path, error);
}

void appendUnique(std::vector<fs::path> &paths, fs::path value) {
    if (value.empty()) {
        return;
    }
    if (std::find(paths.begin(), paths.end(), value) == paths.end()) {
        paths.push_back(std::move(value));
    }
}

std::vector<fs::path> dataRoots() {
    std::vector<fs::path> roots;
    for (const auto &path : splitPathList(std::getenv("XDG_DATA_DIRS"))) {
        appendUnique(roots, path);
    }
    if (roots.empty()) {
        appendUnique(roots, "/usr/local/share");
        appendUnique(roots, "/usr/share");
    }
    return roots;
}

fs::path userDataHome() {
    if (const char *home = std::getenv("XDG_DATA_HOME"); home != nullptr && home[0] != '\0') {
        return home;
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local" / "share";
    }
    return {};
}

std::vector<fs::path> libimeModelDirs() {
    std::vector<fs::path> dirs;
    if (const char *override = std::getenv("SUPERIOR_LIBIME_LM_DIR");
        override != nullptr && override[0] != '\0') {
        appendUnique(dirs, override);
    }
    for (const char *name :
         {"/usr/lib/x86_64-linux-gnu/libime", "/usr/lib/aarch64-linux-gnu/libime",
          "/usr/lib64/libime", "/usr/lib/libime", "/usr/local/lib/libime"}) {
        appendUnique(dirs, name);
    }
    for (const auto &root : dataRoots()) {
        appendUnique(dirs, root / "libime");
    }
    return dirs;
}

std::string quoteForLog(const fs::path &path) {
    return path.string();
}

struct ToolRun {
    int exitCode = -1;
    bool started = false;
    std::string captured;
};

/// Runs one tool with an explicit argv vector.
///
/// `posix_spawn` is used instead of `system()`/`popen()`: there is no shell
/// parsing, so no argument can be reinterpreted as a command. stderr is piped
/// and truncated, only for diagnostics.
ToolRun runTool(const std::vector<std::string> &argv) {
    ToolRun result;
    if (argv.empty()) {
        return result;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe2(pipeFds, O_CLOEXEC) != 0) {
        result.captured = std::string("pipe2: ") + std::strerror(errno);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);

    std::vector<char *> raw;
    raw.reserve(argv.size() + 1);
    for (const auto &argument : argv) {
        raw.push_back(const_cast<char *>(argument.c_str()));
    }
    raw.push_back(nullptr);

    pid_t pid = -1;
    const int spawnResult =
        ::posix_spawnp(&pid, raw.front(), &actions, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipeFds[1]);

    if (spawnResult != 0) {
        ::close(pipeFds[0]);
        result.captured = std::string("spawn: ") + std::strerror(spawnResult);
        return result;
    }
    result.started = true;

    std::array<char, 512> buffer{};
    while (result.captured.size() < kMaxCapturedOutput) {
        const ssize_t received = ::read(pipeFds[0], buffer.data(), buffer.size());
        if (received <= 0) {
            break;
        }
        const std::size_t room = kMaxCapturedOutput - result.captured.size();
        result.captured.append(buffer.data(),
                               std::min(static_cast<std::size_t>(received), room));
    }
    ::close(pipeFds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }

    while (!result.captured.empty() &&
           (result.captured.back() == '\n' || result.captured.back() == '\r')) {
        result.captured.pop_back();
    }
    return result;
}

std::string tail(const std::string &text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    return "..." + text.substr(text.size() - limit);
}

class TempFile {
public:
    TempFile(fs::path directory, std::string suffix) {
        std::error_code error;
        fs::create_directories(directory, error);
        std::string pattern = (directory / ("superior-dict-XXXXXX" + suffix)).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        fd_ = ::mkstemp(writable.data());
        if (fd_ >= 0) {
            const int flags = ::fcntl(fd_, F_GETFD);
            if (flags >= 0) {
                ::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
            }
            path_ = fs::path(writable.data());
        }
    }

    ~TempFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        std::error_code error;
        if (!path_.empty()) {
            fs::remove(path_, error);
        }
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] const fs::path &path() const noexcept { return path_; }

    /// mkstemp already created the file; the converter opens it by path.
    void closeFd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    fs::path path_;
};

} // namespace

DictionaryLayout detectDictionaryLayout() {
    DictionaryLayout layout;
    layout.dataRoots = dataRoots();

    const fs::path dataHome = userDataHome();
    if (!dataHome.empty()) {
        appendUnique(layout.pkgDataRoots, dataHome / "fcitx5");
    }
    for (const auto &root : layout.dataRoots) {
        appendUnique(layout.pkgDataRoots, root / "fcitx5");
    }

    for (const auto &root : layout.dataRoots) {
        if (layout.systemDict.empty()) {
            const fs::path candidate = root / "libime" / "sc.dict";
            if (isRegularFile(candidate)) {
                layout.systemDict = candidate;
            }
        }
        if (layout.extbDict.empty()) {
            const fs::path candidate = root / "libime" / "extb.dict";
            if (isRegularFile(candidate)) {
                layout.extbDict = candidate;
            }
        }
    }
    for (const auto &root : layout.pkgDataRoots) {
        if (layout.chaiziDict.empty()) {
            const fs::path candidate = root / "pinyin" / "chaizi.dict";
            if (isRegularFile(candidate)) {
                layout.chaiziDict = candidate;
            }
        }
    }

    for (const auto &root : layout.pkgDataRoots) {
        if (layout.userDict.empty()) {
            const fs::path candidate = root / "pinyin" / "user.dict";
            if (isRegularFile(candidate)) {
                layout.userDict = candidate;
            }
        }
        if (layout.userHistory.empty()) {
            const fs::path candidate = root / "pinyin" / "user.history";
            if (isRegularFile(candidate)) {
                layout.userHistory = candidate;
            }
        }
        if (layout.userDictDirectory.empty()) {
            const fs::path candidate = root / "pinyin" / "dictionaries";
            std::error_code error;
            if (fs::is_directory(candidate, error)) {
                layout.userDictDirectory = candidate;
            }
        }
    }

    for (const auto &root : libimeModelDirs()) {
        const fs::path candidate = root / "zh_CN.lm";
        if (isRegularFile(candidate)) {
            layout.languageModel = candidate;
            break;
        }
    }

    if (!layout.userDictDirectory.empty()) {
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(layout.userDictDirectory, error)) {
            if (error) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".dict") == 0) {
                layout.extraDictionaries.push_back(entry.path());
            } else if (name.size() > 13 &&
                       name.compare(name.size() - 13, 13, ".dict.disable") == 0) {
                layout.disabledDictionaries.push_back(entry.path());
            }
        }
        std::sort(layout.extraDictionaries.begin(), layout.extraDictionaries.end());
        std::sort(layout.disabledDictionaries.begin(), layout.disabledDictionaries.end());
    }

    SUPERIOR_DEBUG(diag::categories::Pinyin)
        .field("systemDict", layout.systemDict.empty() ? std::string("missing")
                                                       : layout.systemDict.string())
        .field("userDict", layout.userDict.empty() ? std::string("absent")
                                                   : layout.userDict.string())
        .field("languageModel", layout.languageModel.empty() ? std::string("missing")
                                                             : layout.languageModel.string())
        .field("extra", static_cast<std::uint64_t>(layout.extraDictionaries.size()))
        << "dictionary layout resolved";

    return layout;
}

std::string describeDictionaryLayout(const DictionaryLayout &layout) {
    const auto line = [](const char *label, const fs::path &path, const char *fallback) {
        std::string out = label;
        out.append(path.empty() ? fallback : path.string());
        out.push_back('\n');
        return out;
    };

    std::string out;
    out += line("system dict     : ", layout.systemDict, "(missing)");
    out += line("extension dict  : ", layout.extbDict, "(missing)");
    out += line("chaizi dict     : ", layout.chaiziDict, "(missing)");
    out += line("user dict       : ", layout.userDict, "(not created yet)");
    out += line("user history    : ", layout.userHistory, "(not created yet)");
    out += line("extra dict dir  : ", layout.userDictDirectory, "(missing)");
    out += line("language model  : ", layout.languageModel, "(missing)");
    out += "extra dicts     : " + std::to_string(layout.extraDictionaries.size()) + "\n";
    out += "disabled dicts  : " + std::to_string(layout.disabledDictionaries.size()) + "\n";
    return out;
}

std::optional<std::pair<ImportTool, std::filesystem::path>> findScelConverter() {
    const std::array<std::pair<ImportTool, const char *>, 2> candidates{{
        {ImportTool::Scel2Org5, "scel2org5"},
        {ImportTool::Scel2Org, "scel2org"},
    }};

    const char *pathEnv = std::getenv("PATH");
    std::vector<fs::path> searchDirs = splitPathList(pathEnv);
    if (searchDirs.empty()) {
        searchDirs.emplace_back("/usr/local/bin");
        searchDirs.emplace_back("/usr/bin");
        searchDirs.emplace_back("/bin");
    }

    for (const auto &[tool, name] : candidates) {
        for (const auto &directory : searchDirs) {
            const fs::path candidate = directory / name;
            if (isRegularFile(candidate) && ::access(candidate.c_str(), X_OK) == 0) {
                return std::make_pair(tool, candidate);
            }
        }
    }
    return std::nullopt;
}

fs::path defaultImportTarget(const fs::path &scelFile, const DictionaryLayout &layout) {
    if (layout.userDictDirectory.empty()) {
        return {};
    }
    return layout.userDictDirectory / (scelFile.stem().string() + ".dict");
}

ImportResult importSogouScel(const fs::path &scelFile, const ImportOptions &options) {
    ImportResult result;

    std::error_code error;
    if (!fs::exists(scelFile, error)) {
        result.error = ImportError::InputMissing;
        result.detail = "input does not exist: " + quoteForLog(scelFile);
        return result;
    }
    if (!fs::is_regular_file(scelFile, error)) {
        result.error = ImportError::InputNotRegular;
        result.detail = "input is not a regular file: " + quoteForLog(scelFile);
        return result;
    }

    const auto inputBytes = fs::file_size(scelFile, error);
    if (error) {
        result.error = ImportError::InputUnreadable;
        result.detail = "cannot stat input: " + error.message();
        return result;
    }
    if (inputBytes > options.maxInputBytes) {
        result.error = ImportError::InputTooLarge;
        result.detail = "input is larger than the configured limit";
        return result;
    }

    const auto converter = findScelConverter();
    if (!converter) {
        result.error = ImportError::ToolMissing;
        result.detail =
            "scel2org5 not found in PATH. Install the Fcitx dictionary tools "
            "(Debian/Ubuntu: fcitx-tools, or the scel2org package from your distribution).";
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("input", scelFile.filename().string())
            .field("bytes", static_cast<std::uint64_t>(inputBytes))
            << result.detail;
        return result;
    }
    result.converter = converter->second.filename().string();

    fs::path targetDirectory = options.targetDirectory;
    if (targetDirectory.empty()) {
        const DictionaryLayout layout = detectDictionaryLayout();
        targetDirectory = layout.userDictDirectory;
        if (targetDirectory.empty()) {
            const fs::path dataHome = userDataHome();
            if (dataHome.empty()) {
                result.error = ImportError::TargetUnwritable;
                result.detail = "cannot determine a user data directory (HOME unset)";
                return result;
            }
            targetDirectory = dataHome / "fcitx5" / "pinyin" / "dictionaries";
        }
    }

    fs::create_directories(targetDirectory, error);
    if (error) {
        result.error = ImportError::TargetUnwritable;
        result.detail = "cannot create target directory: " + error.message();
        return result;
    }
    if (::access(targetDirectory.c_str(), W_OK) != 0) {
        result.error = ImportError::TargetUnwritable;
        result.detail = "target directory is not writable: " + quoteForLog(targetDirectory);
        return result;
    }

    const fs::path finalPath = targetDirectory / (scelFile.stem().string() + ".dict");
    if (fs::exists(finalPath, error) && !options.overwrite) {
        result.error = ImportError::RenameFailed;
        result.detail = "target already exists: " + quoteForLog(finalPath);
        return result;
    }

    SUPERIOR_INFO(diag::categories::Pinyin)
        .field("input", scelFile.filename().string())
        .field("bytes", static_cast<std::uint64_t>(inputBytes))
        .field("tool", result.converter)
        .field("target", finalPath.filename().string())
        << "importing Sogou cell dictionary";

    TempFile textFile(targetDirectory, ".txt");
    if (!textFile.valid()) {
        result.error = ImportError::TargetUnwritable;
        result.detail = std::string("cannot create a temporary file: ") + std::strerror(errno);
        return result;
    }
    textFile.closeFd();

    const std::vector<std::string> convertArgv{
        converter->second.string(), scelFile.string(), "-o", textFile.path().string()};
    const ToolRun convert = runTool(convertArgv);
    if (!convert.started || convert.exitCode != 0) {
        result.error = ImportError::ConversionFailed;
        result.detail = "scel2org failed";
        if (!convert.captured.empty()) {
            result.detail += ": " + tail(convert.captured, 512);
        }
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("exit", static_cast<std::int64_t>(convert.exitCode))
            .field("output", tail(convert.captured, 512))
            << "scel2org failed";
        return result;
    }

    const auto textBytes = fs::file_size(textFile.path(), error);
    if (error || textBytes == 0) {
        result.error = ImportError::OutputEmpty;
        result.detail = "converter produced an empty text dictionary";
        return result;
    }
    if (textBytes > kMaxTextBytes) {
        result.error = ImportError::OutputTooLarge;
        result.detail = "converted text dictionary exceeds the size limit";
        return result;
    }
    result.textBytes = textBytes;

    TempFile dictFile(targetDirectory, ".dict");
    if (!dictFile.valid()) {
        result.error = ImportError::TargetUnwritable;
        result.detail = std::string("cannot create a temporary file: ") + std::strerror(errno);
        return result;
    }
    dictFile.closeFd();

    const std::vector<std::string> buildArgv{"libime_pinyindict", textFile.path().string(),
                                             dictFile.path().string()};
    const ToolRun build = runTool(buildArgv);
    if (!build.started || build.exitCode != 0) {
        result.error = ImportError::DictionaryBuildFailed;
        result.detail = "libime_pinyindict failed";
        if (!build.captured.empty()) {
            result.detail += ": " + tail(build.captured, 512);
        }
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("exit", static_cast<std::int64_t>(build.exitCode))
            .field("output", tail(build.captured, 512))
            << "libime_pinyindict failed";
        return result;
    }

    const auto dictBytes = fs::file_size(dictFile.path(), error);
    if (error || dictBytes == 0) {
        result.error = ImportError::OutputEmpty;
        result.detail = "libime_pinyindict produced an empty dictionary";
        return result;
    }
    if (dictBytes > kMaxDictBytes) {
        result.error = ImportError::OutputTooLarge;
        result.detail = "binary dictionary exceeds the size limit";
        return result;
    }
    result.dictBytes = dictBytes;

    fs::rename(dictFile.path(), finalPath, error);
    if (error) {
        result.error = ImportError::RenameFailed;
        result.detail = "cannot move the dictionary into place: " + error.message();
        return result;
    }

    result.output = finalPath;
    SUPERIOR_INFO(diag::categories::Pinyin)
        .field("entries", static_cast<std::uint64_t>(0))
        .field("textBytes", static_cast<std::uint64_t>(result.textBytes))
        .field("dictBytes", static_cast<std::uint64_t>(result.dictBytes))
        .field("output", finalPath.filename().string())
        << "Sogou dictionary imported; restart Fcitx5 to load it";
    return result;
}

} // namespace superior::fcitx5
