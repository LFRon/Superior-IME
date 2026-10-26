// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/linux/AudioCapture.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace superior::linux {

namespace {

bool existsInPath(std::string_view name) {
    const char *path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }
    std::string_view rest(path);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto entry = rest.substr(0, colon);
        if (!entry.empty()) {
            const std::string candidate = std::string(entry) + "/" + std::string(name);
            if (::access(candidate.c_str(), X_OK) == 0) {
                return true;
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(colon + 1);
    }
    return false;
}

/// Splits on spaces, honouring double quotes: one shell-quoting habit covers
/// both the capture override and the recogniser override.
std::vector<std::string> splitLine(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (const char ch : line) {
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (ch == ' ' && !quoted) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

/// Runs a command and returns its first line of stdout, trimmed.
///
/// `pactl` is the one tool every sound server on Linux accepts (PipeWire keeps a
/// pulse-compatible layer), it is what the desktop's own mixer uses, and the
/// commands below are read-only. A machine without it simply has no default to
/// remember, which the empty return value expresses.
std::string runAndReadLine(const char *command) {
    FILE *pipe = ::popen(command, "r");
    if (pipe == nullptr) {
        return {};
    }
    std::string line;
    char buffer[256];
    if (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line = buffer;
    }
    ::pclose(pipe);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

} // namespace

CaptureCommand buildCaptureCommand(std::string_view tool, int rate, int channels,
                                   std::string_view target) {
    const std::string rateText = std::to_string(rate);
    const std::string channelsText = std::to_string(channels);
    if (tool == "parec") {
        // PulseAudio; also works through pipewire-pulse.
        CaptureCommand command{"parec",
                               {"--format=s16le", "--rate=" + rateText,
                                "--channels=" + channelsText, "--raw"}};
        if (!target.empty()) {
            command.args.insert(command.args.begin(), "--device=" + std::string(target));
        }
        return command;
    }
    if (tool == "arecord") {
        // ALSA fallback, for machines without a sound server. arecord wants an
        // ALSA device name, which is not what the sound server reports, so it
        // deliberately stays on the system default.
        return CaptureCommand{"arecord",
                              {"-q", "-t", "raw", "-f", "S16_LE", "-r", rateText, "-c",
                               channelsText}};
    }
    // PipeWire default. "-" means stdout, which pw-cat keeps headerless.
    CaptureCommand command{"pw-record",
                           {"--format=s16", "--rate=" + rateText, "--channels=" + channelsText,
                            "-"}};
    if (!target.empty()) {
        command.args.insert(command.args.begin(), "--target=" + std::string(target));
    }
    return command;
}

bool captureToolPresent() {
    // A configured command is itself the answer: the user may be recording with
    // something we do not know about, and none of the three tools.
    const char *custom = std::getenv("SUPERIOR_CAPTURE_CMD");
    if (custom != nullptr && custom[0] != '\0') {
        return true;
    }
    return existsInPath("pw-record") || existsInPath("parec") || existsInPath("arecord");
}

bool shouldRestoreDefaultSource(const std::string &remembered, const std::string &current,
                                const std::vector<std::string> &available) {
    if (remembered.empty() || remembered == current) {
        return false;
    }
    return std::find(available.begin(), available.end(), remembered) != available.end();
}

LinuxAudioSource::~LinuxAudioSource() { stop(); }

bool LinuxAudioSource::available() const { return captureToolPresent(); }

std::string LinuxAudioSource::defaultDevice() const {
    std::string name = runAndReadLine("pactl get-default-source 2>/dev/null");
    if (name.empty()) {
        // Very old pulse servers predate `get-default-source`.
        name = runAndReadLine("pactl info 2>/dev/null | sed -n 's/^Default Source: //p'");
    }
    return name;
}

std::vector<std::string> LinuxAudioSource::availableDevices() const {
    // `short` output: index, name, driver, spec, state.
    FILE *pipe = ::popen("pactl list short sources 2>/dev/null", "r");
    std::vector<std::string> names;
    if (pipe == nullptr) {
        return names;
    }
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        const std::size_t pos = line.find('\t');
        if (pos == std::string::npos) {
            continue;
        }
        const std::size_t start = pos + 1;
        const std::size_t end = line.find('\t', start);
        if (end == std::string::npos) {
            continue;
        }
        names.push_back(line.substr(start, end - start));
    }
    ::pclose(pipe);
    return names;
}

bool LinuxAudioSource::selectDefault(const std::string &name) {
    if (name.empty()) {
        return false;
    }
    // Only ever called with a name that came from the sound server itself, so
    // quoting is about spaces, not about escaping.
    std::string command = "pactl set-default-source '";
    for (const char ch : name) {
        if (ch == '\'') {
            command += "'\\''";
        } else {
            command.push_back(ch);
        }
    }
    command += "' >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

bool LinuxAudioSource::restoreDefault(const std::string &device) {
    if (device.empty()) {
        return false;
    }
    const std::string current = defaultDevice();
    // Never invent a device and never override a deliberate change: only put back
    // a name the sound server still lists. See the trade-off note in
    // `AudioCapture.hpp`.
    if (!shouldRestoreDefaultSource(device, current, availableDevices())) {
        return false;
    }
    return selectDefault(device);
}

bool LinuxAudioSource::start(const api::AudioRequest &request, api::AudioSink &sink,
                             std::string &error) {
    if (running_) {
        stop();
    }
    sink_ = &sink;

    CaptureCommand command;
    if (const char *custom = std::getenv("SUPERIOR_CAPTURE_CMD");
        custom != nullptr && custom[0] != '\0') {
        const auto tokens = splitLine(custom);
        if (tokens.empty()) {
            error = "SUPERIOR_CAPTURE_CMD is empty";
            return false;
        }
        command.program = tokens.front();
        command.args.assign(tokens.begin() + 1, tokens.end());
    } else if (existsInPath("pw-record")) {
        command = buildCaptureCommand("pw-record", request.rate, request.channels, request.device);
    } else if (existsInPath("parec")) {
        command = buildCaptureCommand("parec", request.rate, request.channels, request.device);
    } else if (existsInPath("arecord")) {
        command = buildCaptureCommand("arecord", request.rate, request.channels, request.device);
    } else {
        error = "no capture tool found (tried pw-record, parec, arecord)";
        return false;
    }

    int pipeFd[2] = {-1, -1};
    if (::pipe(pipeFd) != 0) {
        error = std::string("cannot create a pipe: ") + std::strerror(errno);
        return false;
    }

    std::vector<std::string> argvStorage;
    argvStorage.push_back(command.program);
    argvStorage.insert(argvStorage.end(), command.args.begin(), command.args.end());
    std::vector<char *> argv;
    argv.reserve(argvStorage.size() + 1);
    for (auto &item : argvStorage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        error = std::string("cannot fork: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        ::dup2(pipeFd[1], STDOUT_FILENO);
        // Capture tools are chatty; keep their stderr away from our protocol.
        const int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDERR_FILENO);
        }
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        ::execvp(command.program.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(pipeFd[1]);
    pid_ = pid;
    fd_ = pipeFd[0];
    running_ = true;
    endedReported_ = false;

    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void LinuxAudioSource::readLoop() {
    // 100 ms at 16 kHz. Small enough that `stop()` is responsive, large enough
    // that the per-block overhead disappears.
    std::vector<std::int16_t> chunk(1600);
    while (running_) {
        // A blocking `read` cannot be interrupted by closing the fd — that is
        // undefined behaviour, and in practice it does not wake the thread. Poll
        // instead, so a stop request is honoured within one tick and the parent
        // never closes an fd another thread is using.
        pollfd ready{fd_, POLLIN, 0};
        const int result = ::poll(&ready, 1, 50);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (result == 0) {
            continue;
        }
        const ssize_t bytes = ::read(fd_, chunk.data(), chunk.size() * sizeof(std::int16_t));
        if (bytes <= 0) {
            break;
        }
        const auto samples = static_cast<std::size_t>(bytes) / sizeof(std::int16_t);
        if (sink_ != nullptr) {
            sink_->onAudio(std::span<const std::int16_t>(chunk.data(), samples));
        }
    }
    // The tool exited on its own (device busy, unplugged, killed). Say so:
    // without it the session would keep "recording" silence until the no-speech
    // timeout. A deliberate `stop()` clears `endedReported_` first, because
    // "the user asked" is not a failure worth reporting.
    if (!endedReported_ && sink_ != nullptr) {
        sink_->onAudioEnded();
    }
}

void LinuxAudioSource::stop() {
    endedReported_ = true;
    running_ = false;
    if (pid_ > 0) {
        // SIGTERM first so the tool closes the stream cleanly; the reader then
        // sees EOF.
        ::kill(pid_, SIGTERM);
    }
    // Joining, not detaching: `onAudio` must never run after `stop` returns.
    if (reader_.joinable()) {
        reader_.join();
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0) {
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    sink_ = nullptr;
}

} // namespace superior::linux
