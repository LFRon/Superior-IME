// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpeechProcess.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "superior/diagnostics/Log.hpp"

extern char **environ;

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;
namespace fs = std::filesystem;

namespace {

fs::path dataHome() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return fs::path(xdg);
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local" / "share";
    }
    return {};
}

bool executable(const fs::path &path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

fs::path findOnPath(std::string_view name) {
    const char *path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    std::string_view rest(path);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto entry = rest.substr(0, colon);
        if (!entry.empty()) {
            const fs::path candidate = fs::path(entry) / name;
            if (executable(candidate)) {
                return candidate;
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(colon + 1);
    }
    return {};
}

} // namespace

SpeechProcess::SpeechProcess(fcitx::Instance *instance) : instance_(instance) {}

SpeechProcess::~SpeechProcess() {
    stop();
}

std::string SpeechProcess::resolveBinary() {
    if (const char *override = std::getenv("SUPERIOR_SPEECH_BINARY");
        override != nullptr && override[0] != '\0') {
        return override;
    }
    const fs::path home = dataHome();
    if (!home.empty()) {
        const fs::path managed = home / "superior-ime" / "bin" / "superior_speech_recognition";
        if (executable(managed)) {
            return managed.string();
        }
    }
    const fs::path onPath = findOnPath("superior_speech_recognition");
    return onPath.empty() ? std::string{} : onPath.string();
}

namespace {

/// Where the recogniser's stderr goes: `<state>/superior-ime/speech-recognition.log`.
///
/// The capability process cannot log its own failures through our logger (its
/// stderr is a plain pipe), and the engine writes its reasons to stderr, so this
/// file is the only place a model-load failure can be read afterwards.
std::string speechLogPath() {
    const char *state = std::getenv("XDG_STATE_HOME");
    std::filesystem::path root;
    if (state != nullptr && state[0] != '\0') {
        root = std::filesystem::path(state) / "superior-ime";
    } else if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        root = std::filesystem::path(home) / ".local" / "state" / "superior-ime";
    } else {
        return {};
    }
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        return {};
    }
    return (root / "speech-recognition.log").string();
}

} // namespace

bool SpeechProcess::start(LineHandler handler, std::string &error) {
    if (running()) {
        handler_ = std::move(handler);
        return true;
    }

    const std::string binary = resolveBinary();
    if (binary.empty()) {
        error = "superior_speech_recognition is not installed";
        return false;
    }

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
        error = std::string("cannot create pipes: ") + std::strerror(errno);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    // The child's stderr goes to our own log file, not to Fcitx5's log and not to
    // /dev/null. Swallowing it made a real model-load failure undebuggable: the
    // panel said "check the log" and the log had nothing in it.
    const std::string speechLog = speechLogPath();
    if (!speechLog.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, speechLog.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0600);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);

    std::vector<std::string> storage{binary};
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int spawnError =
        posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(inPipe[0]);
    ::close(outPipe[1]);

    if (spawnError != 0) {
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        error = std::string("cannot start ") + binary + ": " + std::strerror(spawnError);
        return false;
    }

    pid_ = pid;
    fd_ = outPipe[0];
    stdinFd_ = inPipe[1];
    buffer_.clear();
    handler_ = std::move(handler);

    io_ = instance_->eventLoop().addIOEvent(
        fd_, fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) { return handleReadable(); });

    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("binary", binary)
        .field("pid", static_cast<std::int64_t>(pid_))
        << "speech recognition process started";
    return true;
}

void SpeechProcess::stop() {
    io_.reset();
    if (stdinFd_ >= 0) {
        ::close(stdinFd_);
        stdinFd_ = -1;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        SUPERIOR_INFO(diag::categories::SpeechRecognition) << "speech recognition process stopped";
        pid_ = -1;
    }
}

bool SpeechProcess::send(std::string_view line) {
    if (stdinFd_ < 0) {
        return false;
    }
    std::string payload(line);
    payload.push_back('\n');
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t count = ::write(stdinFd_, payload.data() + written, payload.size() - written);
        if (count <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

bool SpeechProcess::handleReadable() {
    char chunk[1024];
    const ssize_t count = ::read(fd_, chunk, sizeof(chunk));
    if (count <= 0) {
        // EOF: the child died. `pid_` has to go with it, otherwise `running()` keeps
        // answering true and `ensureSpeechProcess()` never restarts anything: every
        // later `Start` was written into a closed pipe and silently dropped, so the
        // input method stayed mute until Fcitx5 was restarted. Reaping here also
        // keeps a zombie from hanging around.
        io_.reset();
        if (stdinFd_ >= 0) {
            ::close(stdinFd_);
            stdinFd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            ::waitpid(pid_, &status, WNOHANG);
            pid_ = -1;
        }
        fd_ = -1;
        buffer_.clear();
        return false;
    }
    buffer_.append(chunk, static_cast<std::size_t>(count));
    std::size_t newline = 0;
    while ((newline = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && handler_) {
            handler_(line);
        }
    }
    return true;
}

} // namespace superior::fcitx5
