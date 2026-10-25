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
using superior::pinyin::locatePinyinRoot;
using superior::pinyin::migrateLegacyLayout;

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
    // Only the legacy directory: keep reading it until a migration is asked for.
    CHECK(decideLayout(false, true) == LayoutDecision::UseLegacy);
}

/// Reading must never move files: a CLI or a test binary resolving the path
/// must not be able to yank the data out from under a running Fcitx5.
void testLocateIsSideEffectFree() {
    const fs::path root = fs::temp_directory_path() / "superior-layout-test-pure";
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path legacy = makeTree(root, "rime");

    const auto resolution = locatePinyinRoot(root);
    CHECK(resolution.decision == LayoutDecision::UseLegacy);
    CHECK(resolution.root == legacy);
    CHECK(fs::exists(legacy / "shared" / "schema.yaml"));
    fs::remove_all(root, error);
}

void testMigratesLegacyTree() {
    const fs::path root = fs::temp_directory_path() / "superior-layout-test-migrate";
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path legacy = makeTree(root, "rime");

    const auto resolution = migrateLegacyLayout(root);
    CHECK(resolution.migrated);
    CHECK(resolution.root == root / "superior-ime" / "pinyin");
    CHECK(fs::exists(resolution.root / "shared" / "schema.yaml"));
    CHECK(!fs::exists(legacy));

    // Idempotent: the second call finds the new directory and does nothing.
    const auto again = migrateLegacyLayout(root);
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

    const auto resolution = migrateLegacyLayout(root);
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

    const auto migrated = migrateLegacyLayout(root);
    CHECK(!migrated.migrated);
    CHECK(migrated.root == root / "superior-ime" / "pinyin");
    CHECK(!fs::exists(migrated.root));

    const auto located = locatePinyinRoot(root);
    CHECK(located.decision == LayoutDecision::UseNew);
    CHECK(located.root == migrated.root);
    fs::remove_all(root, error);
}

void testEmptyRootStaysEmpty() {
    const auto located = locatePinyinRoot({});
    CHECK(located.root.empty());
    const auto migrated = migrateLegacyLayout({});
    CHECK(migrated.root.empty());
    CHECK(!migrated.migrated);
}

} // namespace

int main() {
    testDecision();
    testLocateIsSideEffectFree();
    testMigratesLegacyTree();
    testKeepsBothWithoutMerging();
    testFreshInstallUsesNewPath();
    testEmptyRootStaysEmpty();
    return superior::test::finish("pinyin layout tests");
}
