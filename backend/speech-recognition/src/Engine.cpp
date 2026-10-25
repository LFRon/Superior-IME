// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Engine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "superior/speech_recognition/Layout.hpp"
#include "superior/speech_recognition/Models.hpp"

namespace superior::speech_recognition {

namespace {

std::string substitute(std::string_view pattern, const std::string &key,
                       const std::string &value) {
    std::string result(pattern);
    const std::string token = "{" + key + "}";
    std::size_t position = 0;
    while ((position = result.find(token, position)) != std::string::npos) {
        result.replace(position, token.size(), value);
        position += value.size();
    }
    return result;
}

std::string trimmed(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

/// Exact flag match in an option list, so `--vad` does not match `--vad-model`.
bool mentionsFlag(std::string_view text, std::string_view flag) {
    const auto isSpace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };
    for (std::size_t pos = text.find(flag); pos != std::string_view::npos;
         pos = text.find(flag, pos + 1)) {
        const std::size_t end = pos + flag.size();
        const bool leftOk = pos == 0 || isSpace(text[pos - 1]);
        const bool rightOk = end >= text.size() || isSpace(text[end]);
        if (leftOk && rightOk) {
            return true;
        }
    }
    return false;
}

/// Runs `program --help` and reports whether its output mentions `flag`.
///
/// A help run loads no model and exits in milliseconds, and the child is killed
/// after a deadline so a wedged binary cannot hang a dictation session.
bool probeEngineHelp(const std::string &program, std::string_view flag) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl(program.c_str(), program.c_str(), "--help", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);

    constexpr std::size_t kMaxHelpBytes = 1 << 16;
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool timedOut = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        pollfd ready{fds[0], POLLIN, 0};
        const int result = ::poll(&ready, 1, static_cast<int>(left));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (result == 0) {
            timedOut = true;
            break;
        }
        char buffer[4096];
        const ssize_t bytes = ::read(fds[0], buffer, sizeof(buffer));
        if (bytes <= 0) {
            break;
        }
        // Keep draining past the cap instead of leaving the child blocked on a
        // full pipe: the flag we are looking for is in the first few KB anyway.
        if (output.size() < kMaxHelpBytes) {
            output.append(buffer, static_cast<std::size_t>(bytes));
        }
    }
    ::close(fds[0]);
    if (timedOut) {
        ::kill(pid, SIGKILL);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return mentionsFlag(output, flag);
}

/// Does this engine understand `--vad`?
///
/// Asked once per engine path: the answer cannot change while we run, and the
/// probe costs one short-lived process instead of every session.
bool engineSupportsVad(const std::string &program) {
    static std::mutex mutex;
    static std::map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto found = cache.find(program); found != cache.end()) {
            return found->second;
        }
    }
    const bool supported = probeEngineHelp(program, "--vad");
    std::lock_guard<std::mutex> lock(mutex);
    cache[program] = supported;
    return supported;
}

/// Adds the arguments that switch speech gating on, or nothing at all.
///
/// Both halves are required, and neither is a given: the engine has to know the
/// flag (an unknown option makes it exit before it loads a model), and the
/// weights have to be on disk. `--vad` on its own makes the engine download them
/// from huggingface.co, which is exactly what a user behind a mirror cannot do —
/// and the failure mode is not "no gating" but "every recording is silence", i.e.
/// dictation that types nothing. See `vadModelPath()`.
void appendVadArgs(EngineCommand &command) {
    if (!engineSupportsVad(command.program)) {
        return;
    }
    const std::filesystem::path weights = vadModelPath();
    if (weights.empty()) {
        return;
    }
    command.args.push_back("--vad");
    command.args.push_back("--vad-model");
    command.args.push_back(weights.string());
}

} // namespace

std::vector<std::string> splitCommandLine(std::string_view line) {
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

std::optional<EngineCommand> buildEngineCommand(const EngineConfig &config,
                                                const std::filesystem::path &wav,
                                                std::string &error) {
    if (config.modelPath.empty()) {
        error = "no model path";
        return std::nullopt;
    }
    const std::string threads = std::to_string(config.threads > 0 ? config.threads : 4);
    // The model decides the backend; guessing "qwen3" for a FireRed GGUF would
    // fail at load time with a message about tensors, not about the mix-up.
    const std::string backend =
        config.backend.empty() ? std::string(defaultModel().backend) : config.backend;

    if (const char *custom = std::getenv("SUPERIOR_ASR_CMD");
        custom != nullptr && custom[0] != '\0') {
        std::string line(custom);
        line = substitute(line, "model", config.modelPath);
        line = substitute(line, "wav", wav.string());
        line = substitute(line, "threads", threads);
        line = substitute(line, "language", config.language);
        line = substitute(line, "backend", backend);
        const auto tokens = splitCommandLine(line);
        if (tokens.empty()) {
            error = "SUPERIOR_ASR_CMD is empty";
            return std::nullopt;
        }
        EngineCommand command;
        command.program = tokens.front();
        command.args.assign(tokens.begin() + 1, tokens.end());
        return command;
    }

    if (config.enginePath.empty()) {
        error = "no recogniser engine";
        return std::nullopt;
    }

    EngineCommand command;
    command.program = config.enginePath;
    command.args = {"--backend", backend, "-m", config.modelPath, "-f", wav.string(),
                    "-t", threads, "--no-timestamps"};
    // Same speech gating as the streaming form: a recording that is mostly
    // silence is exactly what makes the model invent a transcript.
    appendVadArgs(command);
    // Same `off` rule as the streaming form: never let the engine fetch a second
    // model on its own.
    command.args.push_back("--punc-model");
    command.args.push_back(config.puncPath.empty() ? "off" : config.puncPath);
    if (!config.language.empty() && config.language != "auto") {
        command.args.push_back("-l");
        command.args.push_back(config.language);
    }
    return command;
}

std::string cleanTranscript(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size();) {
        const char ch = raw[index];
        // `<sil>`, `<unk>`, `<blank>`, `<s>`, `</s>` … anything in angle brackets
        // is a vocabulary artefact, never dictated text.
        if (ch == '<') {
            const std::size_t close = raw.find('>', index);
            if (close != std::string_view::npos && close - index <= 12) {
                index = close + 1;
                continue;
            }
        }
        // `[sil]`, `[noise]` and friends use the same convention.
        if (ch == '[') {
            const std::size_t close = raw.find(']', index);
            if (close != std::string_view::npos && close - index <= 12) {
                index = close + 1;
                continue;
            }
        }
        out.push_back(ch);
        ++index;
    }

    // Collapse the whitespace the removals leave behind.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool lastSpace = true;
    for (const char ch : out) {
        const bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        if (space) {
            if (!lastSpace) {
                collapsed.push_back(' ');
            }
            lastSpace = true;
            continue;
        }
        collapsed.push_back(ch);
        lastSpace = false;
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    while (!collapsed.empty() && collapsed.front() == ' ') {
        collapsed.erase(collapsed.begin());
    }

    // A result that is only punctuation says nothing: committing "?" into the
    // user's document is worse than showing "没有听清".
    const std::string_view punctuation = "?!.,;:。？！、，；：…～~\"'-";
    if (collapsed.find_first_not_of(punctuation) == std::string::npos) {
        return {};
    }
    return collapsed;
}

std::optional<EngineCommand> buildStreamingEngineCommand(const EngineConfig &config,
                                                         std::string &error, int stepMs,
                                                         int windowMs) {
    if (config.modelPath.empty()) {
        error = "no model path";
        return std::nullopt;
    }
    const std::string threads = std::to_string(config.threads > 0 ? config.threads : 4);
    const std::string backend =
        config.backend.empty() ? std::string(defaultModel().backend) : config.backend;

    if (const char *custom = std::getenv("SUPERIOR_ASR_STREAM_CMD");
        custom != nullptr && custom[0] != '\0') {
        std::string line(custom);
        line = substitute(line, "model", config.modelPath);
        line = substitute(line, "threads", threads);
        line = substitute(line, "language", config.language);
        line = substitute(line, "backend", backend);
        const auto tokens = splitCommandLine(line);
        if (tokens.empty()) {
            error = "SUPERIOR_ASR_STREAM_CMD is empty";
            return std::nullopt;
        }
        EngineCommand command;
        command.program = tokens.front();
        command.args.assign(tokens.begin() + 1, tokens.end());
        return command;
    }

    if (config.enginePath.empty()) {
        error = "no recogniser engine";
        return std::nullopt;
    }
    EngineCommand command;
    command.program = config.enginePath;
    // `--stream-final-on-silence-ms` is the segment boundary the user perceives:
    // pause briefly and the sentence becomes real text, keep talking and it stays
    // a draft. 800 ms is the engine default and matches speech rhythm.
    command.args = {"--backend", backend, "-m", config.modelPath, "--stream", "--stream-json",
                    "-t", threads, "--stream-step", std::to_string(stepMs),
                    "--stream-length", std::to_string(windowMs),
                    "--stream-final-on-silence-ms", "800"};
    // The same span in seconds, so a finished utterance is re-decoded over exactly
    // the audio the streaming pass saw. Left at the engine default it would cap at
    // 60 s, i.e. silently truncate a longer session's final transcript.
    const int windowSeconds = (windowMs + 999) / 1000;
    command.args.push_back("--stream-utterance-max-sec");
    command.args.push_back(std::to_string(windowSeconds));
    command.args.push_back("--punc-model");
    command.args.push_back(config.puncPath.empty() ? "off" : config.puncPath);
    // Silence is not neutral audio for this model. Every step re-decodes the whole
    // window, and with only room tone in it Paraformer still emits a fluent
    // transcript ("没有没有没有…", "eeee零零零…") that we would then type into the
    // user's document. VAD trims each utterance to the speech it contains and
    // reports `silence` instead, which is the difference between dictation and a
    // stream of nonsense. Skipped when the engine predates the flag, or when the
    // weights are not on disk yet.
    appendVadArgs(command);
    if (!config.language.empty() && config.language != "auto") {
        command.args.push_back("-l");
        command.args.push_back(config.language);
    }
    return command;
}

std::optional<std::string> runEngine(const EngineCommand &command, std::string &error) {
    if (command.program.empty()) {
        error = "no recogniser engine";
        return std::nullopt;
    }

    int pipeFd[2] = {-1, -1};
    if (::pipe(pipeFd) != 0) {
        error = std::string("cannot create a pipe: ") + std::strerror(errno);
        return std::nullopt;
    }

    std::vector<std::string> argvStorage;
    argvStorage.reserve(command.args.size() + 2);
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
        return std::nullopt;
    }
    if (pid == 0) {
        // Child: stdout goes to the pipe, stderr is dropped so engine chatter
        // never reaches the agent's log as if it were our own.
        ::dup2(pipeFd[1], STDOUT_FILENO);
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        ::execvp(command.program.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(pipeFd[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    ssize_t count = 0;
    while ((count = ::read(pipeFd[0], buffer.data(), buffer.size())) > 0) {
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipeFd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            error = "the recogniser engine is not installed";
        } else {
            error = "the recogniser engine failed";
        }
        return std::nullopt;
    }

    const std::string text = cleanTranscript(trimmed(output));
    if (text.empty()) {
        error = "no speech recognized";
        return std::nullopt;
    }
    return text;
}

} // namespace superior::speech_recognition
