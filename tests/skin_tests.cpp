// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "superior/skin/Skin.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n';     \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const auto &actualValue = (actual);                                                        \
        const auto &expectedValue = (expected);                                                    \
        if (!(actualValue == expectedValue)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK_EQ failed: " #actual              \
                      << " != " #expected << '\n';                                                 \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

namespace fs = std::filesystem;
using superior::skin::Diagnostic;
using superior::skin::LoadOptions;
using superior::skin::LoadResult;
using superior::skin::Severity;

bool hasCode(const LoadResult &result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [code](const Diagnostic &diagnostic) { return diagnostic.code == code; });
}

bool hasCodeAt(const LoadResult &result, std::string_view code, std::string_view pointer) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [code, pointer](const Diagnostic &diagnostic) {
                           return diagnostic.code == code && diagnostic.pointer == pointer;
                       });
}

class TempPackage {
public:
    TempPackage() {
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("superior-skin-test-" + std::to_string(++counter));
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_ / "assets");
        write("assets/panel.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
    }

    ~TempPackage() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    TempPackage(const TempPackage &) = delete;
    TempPackage &operator=(const TempPackage &) = delete;

    void write(std::string_view relative, std::string_view contents) const {
        const fs::path path = root_ / fs::path(relative);
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    [[nodiscard]] const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

std::string minimalSkin(std::string_view body = {}) {
    std::string json = R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": {
            "styles": {
                "h1": {
                    "background": { "image": "assets/panel.svg" },
                    "candidates": { "flow": "horizontal", "maxVisible": 5 }
                }
            }
        })";
    if (!body.empty()) {
        json.append(",\n");
        json.append(body);
    }
    json.append("\n}\n");
    return json;
}

void testDefaultSkinLoads() {
    const LoadResult result =
        superior::skin::loadSkin(fs::path(SUPERIOR_SOURCE_DIR) / "skins" / "default", LoadOptions{});
    CHECK(result.ok());
    CHECK(result.spec.has_value());
    if (result.spec) {
        CHECK_EQ(result.spec->id, std::string("org.superior.default"));
        CHECK_EQ(result.spec->schema, std::string("superior.skin/1"));
        CHECK_EQ(result.spec->panelStyles.size(), std::size_t(2));
        CHECK_EQ(result.spec->status.items.size(), std::size_t(5));
        CHECK(!result.spec->hash.empty());
        CHECK(result.spec->hash.size() == 16);
        CHECK(!result.spec->assets.empty());
    }
}

void testUnsupportedSchema() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/99",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": { "styles": { "h1": {} } }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCodeAt(result, superior::skin::codes::SchemaUnsupported, "/schema"));
}

void testUnknownKeyWarningAndStrict() {
    TempPackage package;
    package.write("skin.json", minimalSkin(R"("futureKey": { "ignored": true })"));

    const LoadResult lenient = superior::skin::loadSkin(package.root());
    CHECK(lenient.ok());
    CHECK(hasCode(lenient, superior::skin::codes::FieldUnknown));

    LoadOptions strict;
    strict.strict = true;
    const LoadResult strictResult = superior::skin::loadSkin(package.root(), strict);
    CHECK(!strictResult.ok());
    CHECK(hasCodeAt(strictResult, superior::skin::codes::FieldUnknown, "/futureKey"));
}

void testInvalidColor() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": {
            "styles": {
                "h1": { "backgroundColor": "#GGGGGG" }
            }
        }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCodeAt(result, superior::skin::codes::ColorInvalid,
                    "/panel/styles/h1/backgroundColor"));
}

void testTokenCycle() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "tokens": { "cycle.a": "$cycle.b", "cycle.b": "$cycle.a" },
        "panel": {
            "styles": {
                "h1": { "backgroundColor": "$cycle.a" }
            }
        }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCode(result, superior::skin::codes::TokenCycle));
}

void testUnknownToken() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": {
            "styles": {
                "h1": { "backgroundColor": "$nope.missing" }
            }
        }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCodeAt(result, superior::skin::codes::TokenUnknown,
                    "/panel/styles/h1/backgroundColor"));
}

void testPathTraversalRejected() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": {
            "styles": {
                "h1": { "background": { "image": "../outside.svg" } }
            }
        }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCode(result, superior::skin::codes::AssetPathInvalid));
}

void testMissingAsset() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": {
            "styles": {
                "h1": { "background": { "image": { "image": "assets/missing.png" } } }
            }
        }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCodeAt(result, superior::skin::codes::AssetMissing,
                    "/panel/styles/h1/background/image/image"));
}

void testVariantMustReferenceKnownStyle() {
    TempPackage package;
    package.write("skin.json", R"({
        "schema": "superior.skin/1",
        "id": "org.test.skin",
        "name": "Test",
        "version": "1.0.0",
        "panel": {
            "variants": [ { "when": { "orientation": "horizontal" }, "use": "nope" } ],
            "styles": { "h1": {} }
        }
    })");
    const LoadResult result = superior::skin::loadSkin(package.root());
    CHECK(!result.ok());
    CHECK(hasCodeAt(result, superior::skin::codes::VariantStyleUnknown,
                    "/panel/variants/0/use"));
}

void testHashIsStableAndContentSensitive() {
    TempPackage package;
    package.write("skin.json", minimalSkin());
    const LoadResult first = superior::skin::loadSkin(package.root());
    const LoadResult second = superior::skin::loadSkin(package.root());
    CHECK(first.ok());
    CHECK(second.ok());
    if (first.spec && second.spec) {
        CHECK_EQ(first.spec->hash, second.spec->hash);
    }

    package.write("assets/panel.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" id=\"changed\"/>");
    const LoadResult changed = superior::skin::loadSkin(package.root());
    CHECK(changed.ok());
    if (first.spec && changed.spec) {
        CHECK(first.spec->hash != changed.spec->hash);
    }
}

void testUnknownKeysDoNotChangeHash() {
    TempPackage plain;
    plain.write("skin.json", minimalSkin());

    TempPackage annotated;
    annotated.write("skin.json", minimalSkin(R"("x-future": { "enabled": true })"));

    const LoadResult first = superior::skin::loadSkin(plain.root());
    const LoadResult second = superior::skin::loadSkin(annotated.root());
    CHECK(first.ok());
    CHECK(second.ok());
    if (first.spec && second.spec) {
        CHECK_EQ(first.spec->hash, second.spec->hash);
    }
}

void testScanSkins() {
    const auto summaries = superior::skin::scanSkins(fs::path(SUPERIOR_SOURCE_DIR) / "skins");
    CHECK(!summaries.empty());
    const auto found = std::find_if(summaries.begin(), summaries.end(),
                                    [](const auto &summary) {
                                        return summary.id == "org.superior.default";
                                    });
    CHECK(found != summaries.end());
    if (found != summaries.end()) {
        CHECK(found->valid);
        CHECK_EQ(found->version, std::string("0.1.0"));
    }
}

void testCanonicalJsonIsDeterministic() {
    const LoadResult result =
        superior::skin::loadSkin(fs::path(SUPERIOR_SOURCE_DIR) / "skins" / "default");
    CHECK(result.spec.has_value());
    if (result.spec) {
        const std::string first = superior::skin::toCanonicalJson(*result.spec);
        const std::string second = superior::skin::toCanonicalJson(*result.spec);
        CHECK_EQ(first, second);
        CHECK(first.find("\"schema\":\"superior.skin/1\"") != std::string::npos);
    }
}

} // namespace

int main() {
    testDefaultSkinLoads();
    testUnsupportedSchema();
    testUnknownKeyWarningAndStrict();
    testInvalidColor();
    testTokenCycle();
    testUnknownToken();
    testPathTraversalRejected();
    testMissingAsset();
    testVariantMustReferenceKnownStyle();
    testHashIsStableAndContentSensitive();
    testUnknownKeysDoNotChangeHash();
    testScanSkins();
    testCanonicalJsonIsDeterministic();

    if (g_failures == 0) {
        std::cout << "skin tests: " << g_checks << " checks passed\n";
        return 0;
    }
    std::cerr << "skin tests: " << g_failures << " failure(s) out of " << g_checks << " checks\n";
    return 1;
}
