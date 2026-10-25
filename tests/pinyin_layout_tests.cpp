// SPDX-License-Identifier: GPL-3.0-or-later
//
// The data-layout migration, tested at both levels:
// - the decision is a pure function (no filesystem);
// - the resolution actually renames a temporary tree, and never loses data when
//   it cannot.

#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"
#include "superior/pinyin/Layout.hpp"

namespace {

namespace fs = std::filesystem;
using superior::pinyin::decideLayout;
using superior::pinyin::LayoutDecision;
using superior::pinyin::resolvePinyinRoot;

/// Creates `<root>/superior-ime/<name>/shared/schema.yaml`.
fs::path makeTree(const fs::path &root, const std::string &name) {
    const fs::path dir = root / "superior-ime" / name;
    fs::create_directories(dir / "shared");
    fs::create_directories(dir / "user");
    std::ofstream(dir / "shared" / "schema.yaml") << "name: test\n";
    return dir;
}

void testDecision() {
    // Fresh install: use the new directory (nobody has data yet).
    CHECK(decideLayout(false, false) == LayoutDecision::UseNew);
    // Already migrated.
    CHECK(decideLayout(true, false) == LayoutDecision::UseNew);
    // Half-finished migration: the new directory wins, never merge.
    CHECK(decideLayout(true, true) == LayoutDecision::UseNew);
    // Only the legacy directory: rename it.
    CHECK(decideLayout(false, true) == LayoutDecision::RenameLegacy);
}

void testMigratesLegacyTree() {
    const fs::path root = fs::temp_directory_path() / "superior-layout-test-migrate";
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path legacy = makeTree(root, "rime");

    const auto resolution = resolvePinyinRoot(root);
    CHECK(resolution.migrated);
    CHECK(resolution.root == root / "superior-ime" / "pinyin");
    CHECK(fs::exists(resolution.root / "shared" / "schema.yaml"));
    CHECK(!fs::exists(legacy));

    // Idempotent: the second call finds the new directory and does nothing.
    const auto again = resolvePinyinRoot(root);
    CHECK(!again.migrated);
    CHECK(again.root == resolution.root);
    fs::remove_all(root, error);
}

void testKeepsBothWithoutMerging() {
    const fs::path root = fs::temp_directory_path() / "superior-layout-test-both";
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path legacy = makeTree(root, "rime");
    const fs::path current = makeTree(root, "pinyin");
    std::ofstream(current / "shared" / "new.yaml") << "new: true\n";

    const auto resolution = resolvePinyinRoot(root);
    CHECK(!resolution.migrated);
    CHECK(resolution.root == current);
    // The legacy tree is left exactly as it was, for the user to inspect.
    CHECK(fs::exists(legacy / "shared" / "schema.yaml"));
    CHECK(fs::exists(current / "shared" / "new.yaml"));
    fs::remove_all(root, error);
}

void testFreshInstallUsesNewPath() {
    const fs::path root = fs::temp_directory_path() / "superior-layout-test-fresh";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    const auto resolution = resolvePinyinRoot(root);
    CHECK(!resolution.migrated);
    CHECK(resolution.root == root / "superior-ime" / "pinyin");
    CHECK(!fs::exists(resolution.root));
    fs::remove_all(root, error);
}

void testEmptyRootStaysEmpty() {
    const auto resolution = resolvePinyinRoot({});
    CHECK(resolution.root.empty());
    CHECK(!resolution.migrated);
}

} // namespace

int main() {
    testDecision();
    testMigratesLegacyTree();
    testKeepsBothWithoutMerging();
    testFreshInstallUsesNewPath();
    testEmptyRootStaysEmpty();
    return superior::test::finish("pinyin layout tests");
}
