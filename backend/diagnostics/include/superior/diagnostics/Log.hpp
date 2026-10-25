// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

/// Unified logging entry point for every Superior-IME component.
///
/// Design notes:
/// - This module has no Qt, Fcitx5 or platform dependency, so the same calls
///   work inside the Fcitx5 agent, CLI tools and the Qt shell.
/// - The Qt shell installs a sink that forwards records to QMessageLogger, so
///   `QT_LOGGING_RULES`, `qCDebug` output and our records share one pipeline.
/// - There is no printf-style formatting anywhere: values go through
///   `operator<<`. No format string can be attacker controlled.
///
/// Security rules for call sites (see docs/logging.md):
/// - never log preedit, candidate or committed text; use `redacted()`;
/// - never log environment dumps, tokens, or file contents;
/// - treat every path and identifier as untrusted: the sink escapes control
///   characters and truncates long values.
namespace superior::diagnostics {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Off = 5,
};

struct Record {
    Level level = Level::Info;
    std::string category;
    std::string file;
    int line = 0;
    std::string message;
    std::uint64_t sequence = 0;
    std::int64_t wallTimeMs = 0;
};

using Sink = std::function<void(const Record &)>;

// ---------------------------------------------------------------- namespaces

namespace categories {

inline constexpr std::string_view Diagnostics = "superior.diagnostics";
inline constexpr std::string_view Skin = "superior.skin";
inline constexpr std::string_view Core = "superior.core";
inline constexpr std::string_view Runtime = "superior.runtime";
inline constexpr std::string_view Protocol = "superior.protocol";
inline constexpr std::string_view Socket = "superior.protocol.socket";
inline constexpr std::string_view Fcitx5 = "superior.fcitx5";
inline constexpr std::string_view Cloud = "superior.cloud";
inline constexpr std::string_view Pinyin = "superior.pinyin";
inline constexpr std::string_view SpeechRecognition = "superior.speech-recognition";
inline constexpr std::string_view Ui = "superior.ui";
inline constexpr std::string_view Tool = "superior.tool";

} // namespace categories

// ---------------------------------------------------------------- primitives

[[nodiscard]] std::string_view levelName(Level level) noexcept;
[[nodiscard]] Level levelFromString(std::string_view name) noexcept;

/// Escapes control characters, quotes and backslashes, then truncates to
/// `limit` bytes. Used for every structured field.
[[nodiscard]] std::string escapeField(std::string_view value, std::size_t limit = 256);

/// Replaces text with its byte length only. Use for anything the user typed.
[[nodiscard]] std::string redact(std::string_view value);

// -------------------------------------------------------------------- config

/// Points `SUPERIOR_LOG_FILE` at `<state>/superior-ime/<name>.log` unless the
/// caller already chose a file. Returns the path it settled on (empty when no
/// state directory can be resolved).
///
/// Why this exists: both processes are normally started by the desktop with
/// stderr going to /dev/null. Logging to stderr then means there is *no* record
/// of anything — a "语音输入没反应" report had nothing to diagnose, twice.
[[nodiscard]] std::string ensureLogFile(std::string_view name);

/// Installs the primary sink. `nullptr` restores the default stderr sink.
/// Any exception thrown by the sink is swallowed: logging must never take the
/// process down.
void setSink(Sink sink);

/// Sink used for records that originate from the Qt message handler. Kept
/// separate so a Qt sink cannot recurse into itself.
void setFallbackSink(Sink sink);

[[nodiscard]] Sink defaultStderrSink();
[[nodiscard]] Sink fileSink(std::string path, std::size_t maxBytes = 8U * 1024U * 1024U);

void setMinLevel(Level level);
void setCategoryLevel(std::string_view category, Level level);
void clearCategoryLevels();

[[nodiscard]] bool enabled(Level level, std::string_view category) noexcept;

/// Reads `SUPERIOR_LOG_LEVEL`, `SUPERIOR_LOG` and `SUPERIOR_LOG_FILE`.
///
/// Example:
///   SUPERIOR_LOG_LEVEL=info
///   SUPERIOR_LOG="superior.protocol=trace,superior.protocol.socket=debug"
///   SUPERIOR_LOG_FILE=~/.cache/superior-ime/log.txt
void configureFromEnvironment();

/// Single-line, stable format used by the default sinks.
[[nodiscard]] std::string formatRecord(const Record &record);

/// Emits a record through the normal filters and sink.
void publish(Record record);

/// Emits a record using the fallback sink and no category filtering. Used by
/// the Qt message handler, which has already applied Qt's own filtering.
void publishRaw(Record record);

// ------------------------------------------------------------------- builder

/// RAII log statement. Constructed only when the category is enabled, so
/// disabled call sites never build strings.
class Log {
public:
    Log(Level level, std::string_view category, const char *file, int line);
    ~Log();

    Log(const Log &) = delete;
    Log &operator=(const Log &) = delete;
    Log(Log &&other) noexcept;
    Log &operator=(Log &&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return active_; }

    std::ostream &stream();
    Log &field(std::string_view key, std::string_view value);
    /// Without this overload a string literal picks the `bool` one (a literal is
    /// an array decaying to a pointer, which converts to bool far too eagerly),
    /// so `.field("reason", "too-short")` logged `reason=true`.
    Log &field(std::string_view key, const char *value);
    Log &field(std::string_view key, std::int64_t value);
    Log &field(std::string_view key, std::uint64_t value);
    Log &field(std::string_view key, double value, int precision = 3);
    Log &field(std::string_view key, bool value);

    /// Logs `key=<redacted len=N>` without ever touching the content.
    Log &redacted(std::string_view key, std::string_view secret);

private:
    void appendFields(std::string &message);

    bool active_ = false;
    Level level_ = Level::Info;
    std::string category_;
    const char *file_ = nullptr;
    int line_ = 0;
    std::ostringstream stream_;
    std::string fields_;
};

template <typename T>
Log &operator<<(Log &log, const T &value) {
    log.stream() << value;
    return log;
}

} // namespace superior::diagnostics

/// Usage: `SUPERIOR_LOG(categories::Skin, Level::Info) << "loaded " << id;`
#define SUPERIOR_LOG(category, level)                                                              \
    if (auto superior_log_ = ::superior::diagnostics::Log((level), (category), __FILE__, __LINE__)) \
    superior_log_

#define SUPERIOR_TRACE(category) SUPERIOR_LOG(category, ::superior::diagnostics::Level::Trace)
#define SUPERIOR_DEBUG(category) SUPERIOR_LOG(category, ::superior::diagnostics::Level::Debug)
#define SUPERIOR_INFO(category) SUPERIOR_LOG(category, ::superior::diagnostics::Level::Info)
#define SUPERIOR_WARN(category) SUPERIOR_LOG(category, ::superior::diagnostics::Level::Warning)
#define SUPERIOR_ERROR(category) SUPERIOR_LOG(category, ::superior::diagnostics::Level::Error)
