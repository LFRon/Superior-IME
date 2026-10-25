// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/diagnostics/Log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace superior::diagnostics {

namespace {

constexpr std::size_t kMaxMessageBytes = 8U * 1024U;
constexpr std::size_t kMaxFieldBytes = 256;
constexpr std::size_t kMaxCategoryBytes = 64;

/// Intentionally leaked: logging must stay usable during static destruction
/// and after Qt tears down at exit. A destroyed mutex would be worse than a
/// process-lifetime allocation.
struct State {
    std::mutex mutex;
    Sink sink;
    Sink fallback;
    Level minLevel = Level::Info;
    std::map<std::string, Level> categoryLevels;
};

State &state() {
    static State *instance = [] {
        auto *created = new State();
        // A component that never calls configureFromEnvironment() (for example
        // a library used by a long-running host) must still produce output.
        Sink stderrSink = [](const Record &record) {
            const std::string line = formatRecord(record);
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        };
        created->sink = stderrSink;
        created->fallback = std::move(stderrSink);
        return created;
    }();
    return *instance;
}

std::atomic<std::uint64_t> g_sequence{1};
std::atomic<bool> g_envConfigured{false};

Level clampLevel(Level level) noexcept {
    if (level < Level::Trace) {
        return Level::Trace;
    }
    if (level > Level::Off) {
        return Level::Off;
    }
    return level;
}

Sink sinkCopy(bool fallback) {
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    const Sink &sink = fallback ? shared.fallback : shared.sink;
    return sink;
}

std::int64_t nowWallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t position = text.find(separator, start);
        const std::size_t end = position == std::string_view::npos ? text.size() : position;
        parts.push_back(trim(text.substr(start, end - start)));
        if (position == std::string_view::npos) {
            break;
        }
        start = position + 1;
    }
    return parts;
}

std::string sanitizeCategory(std::string_view category) {
    std::string out;
    out.reserve(std::min(category.size(), kMaxCategoryBytes));
    for (char c : category) {
        if (out.size() >= kMaxCategoryBytes) {
            break;
        }
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(safe ? c : '_');
    }
    if (out.empty()) {
        out = "superior";
    }
    return out;
}

std::string expandHome(std::string_view path) {
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return std::string(home) + std::string(path.substr(1));
        }
    }
    return std::string(path);
}

std::string formatTimestamp(std::int64_t wallTimeMs) {
    const std::time_t seconds = static_cast<std::time_t>(wallTimeMs / 1000);
    const std::int64_t millis = wallTimeMs % 1000;
    std::tm broken{};
    if (::gmtime_r(&seconds, &broken) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }

    std::array<char, 32> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &broken) == 0) {
        return "1970-01-01T00:00:00.000Z";
    }

    std::array<char, 8> fraction{};
    std::snprintf(fraction.data(), fraction.size(), ".%03dZ",
                  static_cast<int>(millis < 0 ? 0 : millis));
    return std::string(buffer.data()) + fraction.data();
}

char levelMarker(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return 'T';
    case Level::Debug:
        return 'D';
    case Level::Info:
        return 'I';
    case Level::Warning:
        return 'W';
    case Level::Error:
        return 'E';
    case Level::Off:
        break;
    }
    return '?';
}

std::string basenameOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

void writeToStderr(const Record &record) {
    const std::string line = formatRecord(record);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

struct FileState {
    explicit FileState(std::string filePath, std::size_t limitBytes)
        : path(std::move(filePath)), maxBytes(limitBytes) {}

    std::mutex mutex;
    std::string path;
    std::size_t maxBytes = 0;
    std::ofstream stream;
    std::size_t written = 0;

    bool open() {
        stream.open(path, std::ios::binary | std::ios::app);
        if (!stream) {
            return false;
        }
        stream.seekp(0, std::ios::end);
        const auto position = stream.tellp();
        written = position > 0 ? static_cast<std::size_t>(position) : 0;
        return true;
    }

    void rotate() {
        stream.close();
        const std::string previous = path + ".1";
        std::remove(previous.c_str());
        std::rename(path.c_str(), previous.c_str());
        stream.open(path, std::ios::binary | std::ios::trunc);
        written = 0;
    }
};

} // namespace

// ---------------------------------------------------------------- primitives

std::string_view levelName(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "trace";
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warning:
        return "warning";
    case Level::Error:
        return "error";
    case Level::Off:
        return "off";
    }
    return "info";
}

Level levelFromString(std::string_view name) noexcept {
    if (name == "trace" || name == "t") {
        return Level::Trace;
    }
    if (name == "debug" || name == "d") {
        return Level::Debug;
    }
    if (name == "info" || name == "i") {
        return Level::Info;
    }
    if (name == "warning" || name == "warn" || name == "w") {
        return Level::Warning;
    }
    if (name == "error" || name == "err" || name == "e") {
        return Level::Error;
    }
    if (name == "off" || name == "none") {
        return Level::Off;
    }
    return Level::Info;
}

std::string escapeField(std::string_view value, std::size_t limit) {
    std::string out;
    out.reserve(std::min(value.size(), limit) + 16);
    for (char raw : value) {
        if (out.size() >= limit) {
            out.append("...");
            break;
        }
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
        case '\\':
            out.append("\\\\");
            break;
        case '"':
            out.append("\\\"");
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
            if (byte < 0x20 || byte == 0x7F) {
                std::array<char, 8> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "\\x%02x", byte);
                out.append(buffer.data());
            } else {
                out.push_back(raw);
            }
            break;
        }
    }
    return out;
}

std::string redact(std::string_view value) {
    return "<redacted len=" + std::to_string(value.size()) + ">";
}

// -------------------------------------------------------------------- config

Sink defaultStderrSink() {
    return [](const Record &record) { writeToStderr(record); };
}

Sink fileSink(std::string path, std::size_t maxBytes) {
    path = expandHome(path);
    auto state = std::make_shared<FileState>(path, maxBytes);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->open()) {
            writeToStderr(Record{Level::Error, std::string(categories::Diagnostics), __FILE__,
                                 __LINE__,
                                 "cannot open log file '" + escapeField(path) + "', using stderr",
                                 g_sequence.fetch_add(1), nowWallMs()});
            return defaultStderrSink();
        }
    }
    return [state](const Record &record) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->stream) {
            return;
        }
        const std::string line = formatRecord(record);
        if (state->maxBytes > 0 && state->written + line.size() + 1 > state->maxBytes) {
            state->rotate();
            if (!state->stream) {
                return;
            }
        }
        state->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
        state->stream.put('\n');
        state->stream.flush();
        state->written += line.size() + 1;
    };
}

void setSink(Sink sink) {
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.sink = sink ? std::move(sink) : defaultStderrSink();
}

void setFallbackSink(Sink sink) {
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.fallback = sink ? std::move(sink) : defaultStderrSink();
}

void setMinLevel(Level level) {
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.minLevel = clampLevel(level);
}

void setCategoryLevel(std::string_view category, Level level) {
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.categoryLevels[sanitizeCategory(category)] = clampLevel(level);
}

void clearCategoryLevels() {
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.categoryLevels.clear();
}

bool enabled(Level level, std::string_view category) noexcept {
    if (level == Level::Off) {
        return false;
    }
    State &shared = state();
    std::lock_guard<std::mutex> lock(shared.mutex);
    Level threshold = shared.minLevel;
    if (!shared.categoryLevels.empty()) {
        const std::string name = sanitizeCategory(category);
        const auto exact = shared.categoryLevels.find(name);
        if (exact != shared.categoryLevels.end()) {
            threshold = exact->second;
        } else {
            // Longest matching prefix wins, so "superior.protocol=trace"
            // also raises "superior.protocol.socket".
            std::size_t bestLength = 0;
            for (const auto &[key, value] : shared.categoryLevels) {
                if (name.size() > key.size() && name.compare(0, key.size(), key) == 0 &&
                    name[key.size()] == '.' && key.size() > bestLength) {
                    bestLength = key.size();
                    threshold = value;
                }
            }
        }
    }
    return static_cast<int>(level) >= static_cast<int>(threshold);
}

void configureFromEnvironment() {
    if (g_envConfigured.exchange(true)) {
        return;
    }

    if (const char *level = std::getenv("SUPERIOR_LOG_LEVEL"); level != nullptr) {
        setMinLevel(levelFromString(trim(level)));
    }

    if (const char *rules = std::getenv("SUPERIOR_LOG"); rules != nullptr) {
        for (const std::string &rule : split(rules, ',')) {
            if (rule.empty()) {
                continue;
            }
            const std::size_t equals = rule.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            const std::string name = trim(std::string_view(rule).substr(0, equals));
            const std::string value = trim(std::string_view(rule).substr(equals + 1));
            if (!name.empty()) {
                setCategoryLevel(name, levelFromString(value));
            }
        }
    }

    Sink sink = defaultStderrSink();
    if (const char *file = std::getenv("SUPERIOR_LOG_FILE"); file != nullptr && file[0] != '\0') {
        sink = fileSink(file);
    }
    setSink(sink);
    setFallbackSink(sink);

    Level reported = Level::Info;
    {
        State &shared = state();
        std::lock_guard<std::mutex> lock(shared.mutex);
        reported = shared.minLevel;
    }
    SUPERIOR_INFO(categories::Diagnostics)
        << "logging configured, minLevel=" << levelName(reported);
}

std::string formatRecord(const Record &record) {
    std::string line;
    line.reserve(record.message.size() + 96);
    line.append(formatTimestamp(record.wallTimeMs));
    line.append(" [");
    line.push_back(levelMarker(record.level));
    line.append("] ");
    line.append(sanitizeCategory(record.category));
    line.push_back(' ');
    line.append(basenameOf(record.file));
    line.push_back(':');
    line.append(std::to_string(record.line));
    line.push_back(' ');
    line.append(escapeField(record.message, kMaxMessageBytes));
    return line;
}

void publish(Record record) {
    const Level level = clampLevel(record.level);
    if (!enabled(level, record.category)) {
        return;
    }
    record.level = level;
    record.sequence = g_sequence.fetch_add(1);
    record.wallTimeMs = nowWallMs();
    const Sink sink = sinkCopy(false);
    if (!sink) {
        return;
    }
    try {
        sink(record);
    } catch (...) {
        // A failing sink must never propagate into business logic.
    }
}

void publishRaw(Record record) {
    record.sequence = g_sequence.fetch_add(1);
    record.wallTimeMs = nowWallMs();
    Sink sink = sinkCopy(true);
    if (!sink) {
        sink = defaultStderrSink();
    }
    try {
        sink(record);
    } catch (...) {
    }
}

// ------------------------------------------------------------------- builder

Log::Log(Level level, std::string_view category, const char *file, int line)
    : active_(enabled(level, category)), level_(clampLevel(level)),
      category_(sanitizeCategory(category)), file_(file), line_(line) {}

Log::Log(Log &&other) noexcept
    : active_(other.active_), level_(other.level_), category_(std::move(other.category_)),
      file_(other.file_), line_(other.line_), stream_(std::move(other.stream_)),
      fields_(std::move(other.fields_)) {
    other.active_ = false;
}

Log::~Log() {
    if (!active_) {
        return;
    }
    std::string message = stream_.str();
    appendFields(message);
    Record record;
    record.level = level_;
    record.category = category_;
    record.file = file_ != nullptr ? file_ : "";
    record.line = line_;
    record.message = std::move(message);
    publish(std::move(record));
}

std::ostream &Log::stream() {
    return stream_;
}

Log &Log::field(std::string_view key, std::string_view value) {
    if (!fields_.empty()) {
        fields_.push_back(' ');
    }
    fields_.append(escapeField(key, 64));
    fields_.push_back('=');
    fields_.append(escapeField(value, kMaxFieldBytes));
    return *this;
}

Log &Log::field(std::string_view key, const char *value) {
    return field(key, value != nullptr ? std::string_view(value) : std::string_view{});
}

Log &Log::field(std::string_view key, std::int64_t value) {
    return field(key, std::string_view(std::to_string(value)));
}

Log &Log::field(std::string_view key, std::uint64_t value) {
    return field(key, std::string_view(std::to_string(value)));
}

Log &Log::field(std::string_view key, double value, int precision) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return field(key, std::string_view(out.str()));
}

Log &Log::field(std::string_view key, bool value) {
    return field(key, value ? std::string_view("true") : std::string_view("false"));
}

Log &Log::redacted(std::string_view key, std::string_view secret) {
    return field(key, std::string_view(redact(secret)));
}

void Log::appendFields(std::string &message) {
    if (fields_.empty()) {
        return;
    }
    if (!message.empty()) {
        message.push_back(' ');
    }
    message.append(fields_);
}

} // namespace superior::diagnostics
