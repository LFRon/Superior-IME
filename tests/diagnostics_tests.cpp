// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/diagnostics/Log.hpp"

namespace {

namespace diag = superior::diagnostics;

void testLevelFiltering() {
    std::vector<diag::Record> records;
    diag::setSink([&records](const diag::Record &record) { records.push_back(record); });
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Warning);

    SUPERIOR_DEBUG(diag::categories::Core) << "hidden";
    SUPERIOR_WARN(diag::categories::Core) << "shown";

    CHECK(records.size() == 1);
    CHECK(records.front().message == "shown");
    CHECK(records.front().level == diag::Level::Warning);
}

void testCategoryPrefixOverride() {
    std::vector<diag::Record> records;
    diag::setSink([&records](const diag::Record &record) { records.push_back(record); });
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Warning);

    diag::setCategoryLevel("superior.protocol", diag::Level::Trace);

    SUPERIOR_TRACE("superior.protocol.socket") << "child";
    SUPERIOR_TRACE("superior.skin") << "other";
    SUPERIOR_TRACE("superior.protocolX") << "not a child";

    CHECK(records.size() == 1);
    CHECK(records.front().category == "superior.protocol.socket");
}

void testFieldsAndRedaction() {
    std::vector<diag::Record> records;
    diag::setSink([&records](const diag::Record &record) { records.push_back(record); });
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Trace);

    const std::string secret = "用户输入 ni hao";
    SUPERIOR_INFO(diag::categories::Core)
            .field("count", static_cast<std::uint64_t>(3))
            .field("ratio", 0.5, 1)
            .field("flag", true)
            .redacted("preedit", secret)
        << "composed";

    CHECK(records.size() == 1);
    const std::string &message = records.front().message;
    CHECK(message.find("composed") != std::string::npos);
    CHECK(message.find("count=3") != std::string::npos);
    CHECK(message.find("ratio=0.5") != std::string::npos);
    CHECK(message.find("flag=true") != std::string::npos);
    CHECK(message.find(diag::redact(secret)) != std::string::npos);
    CHECK(message.find("用户输入") == std::string::npos);
}

void testEscaping() {
    const std::string escaped = diag::escapeField("a\nb\tc\"d\\e");
    CHECK(escaped == "a\\nb\\tc\\\"d\\\\e");

    const std::string control = diag::escapeField(std::string(1, '\x01'));
    CHECK(control == "\\x01");

    const std::string longValue(400, 'x');
    const std::string truncated = diag::escapeField(longValue, 10);
    CHECK(truncated.size() == 13);
    CHECK(truncated.substr(0, 10) == std::string(10, 'x'));
    CHECK(truncated.substr(10) == "...");
}

void testDisabledCategoryBuildsNothing() {
    std::vector<diag::Record> records;
    diag::setSink([&records](const diag::Record &record) { records.push_back(record); });
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Off);

    diag::Log log(diag::Level::Error, diag::categories::Core, __FILE__, __LINE__);
    CHECK(!static_cast<bool>(log));
    SUPERIOR_ERROR(diag::categories::Core) << "never";
    CHECK(records.empty());
}

void testSinkFailureIsContained() {
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Trace);
    diag::setSink([](const diag::Record &) { throw std::runtime_error("sink exploded"); });

    // Must not throw or terminate the process.
    SUPERIOR_ERROR(diag::categories::Core) << "still alive";
    CHECK(true);
}

void testFormatRecordShape() {
    diag::Record record;
    record.level = diag::Level::Warning;
    record.category = "superior.skin";
    record.file = "/very/long/path/Skin.cpp";
    record.line = 42;
    record.message = "hello";
    record.wallTimeMs = 0;

    const std::string line = diag::formatRecord(record);
    CHECK(line.find("[W]") != std::string::npos);
    CHECK(line.find("superior.skin") != std::string::npos);
    CHECK(line.find("Skin.cpp:42") != std::string::npos);
    CHECK(line.find("hello") != std::string::npos);
    CHECK(line.find("1970-01-01T00:00:00.000Z") == 0);
}

void testLevelParsing() {
    CHECK(diag::levelFromString("trace") == diag::Level::Trace);
    CHECK(diag::levelFromString("warn") == diag::Level::Warning);
    CHECK(diag::levelFromString("off") == diag::Level::Off);
    CHECK(diag::levelFromString("nonsense") == diag::Level::Info);
    CHECK(diag::levelName(diag::Level::Error) == "error");
}

void testPublishRawIgnoresCategoryFilter() {
    std::vector<diag::Record> records;
    diag::setFallbackSink([&records](const diag::Record &record) { records.push_back(record); });
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Off);

    diag::Record record;
    record.level = diag::Level::Debug;
    record.category = "qt.default";
    record.message = "from Qt";
    diag::publishRaw(record);

    CHECK(records.size() == 1);
    CHECK(records.front().category == "qt.default");
}

/// A string literal must log as text. Before the `const char*` overload existed
/// it matched the `bool` overload, so `.field("reason", "too-short")` printed
/// `reason=true` and every diagnostic using a literal was useless.
void testFieldStringLiteral() {
    std::vector<diag::Record> records;
    diag::setSink([&records](const diag::Record &record) { records.push_back(record); });
    // Earlier tests lowered the level; reset so an INFO record is not filtered.
    diag::clearCategoryLevels();
    diag::setMinLevel(diag::Level::Trace);

    SUPERIOR_INFO("test.literal").field("reason", "too-short").field("count", static_cast<std::int64_t>(3)) << "msg";

    CHECK(records.size() == 1);
    if (records.empty()) {
        return;
    }
    const std::string &message = records.front().message;
    CHECK(message.find("reason=too-short") != std::string::npos);
    CHECK(message.find("reason=true") == std::string::npos);
    CHECK(message.find("count=3") != std::string::npos);
}

} // namespace

int main() {
    testLevelFiltering();
    testCategoryPrefixOverride();
    testFieldsAndRedaction();
    testEscaping();
    testDisabledCategoryBuildsNothing();
    testSinkFailureIsContained();
    testFormatRecordShape();
    testLevelParsing();
    testPublishRawIgnoresCategoryFilter();

    diag::setSink(nullptr);
    diag::setFallbackSink(nullptr);
    testFieldStringLiteral();
    return superior::test::finish("diagnostics tests");
}
