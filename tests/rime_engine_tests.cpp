// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/rime/RimeEngine.hpp"
#include "superior/rime/RimePaths.hpp"

namespace {

namespace fs = std::filesystem;
using namespace superior::rime;

/// Records every call so the tests assert on behaviour, not on librime.
class FakePort final : public RimePort {
public:
    bool initialize(const Paths &paths, std::string &error) override {
        if (failInitialize) {
            error = "fake init failure";
            return false;
        }
        lastPaths = paths;
        initialized = true;
        return true;
    }
    void finalize() override { initialized = false; }

    bool deploy(std::string &error) override {
        ++deployCount;
        if (failDeploy) {
            error = "fake deploy failure";
            return false;
        }
        schema = "wanxiang";
        return true;
    }

    bool createSession(std::string &error) override {
        ++sessionCount;
        if (failSession) {
            error = "fake session failure";
            return false;
        }
        open = true;
        return true;
    }
    void destroySession() override { open = false; }

    bool processKey(std::uint32_t sym, std::uint32_t mask) override {
        keys.push_back({sym, mask});
        if (consumeNext) {
            consumeNext = false;
            return true;
        }
        return false;
    }

    bool takeCommit(std::string &out) override {
        if (pendingCommit.empty()) {
            return false;
        }
        out = pendingCommit;
        pendingCommit.clear();
        ++commitDrains;
        return true;
    }

    Composition composition() override { return pendingComposition; }
    Menu menu() override { return pendingMenu; }

    bool selectCandidate(std::size_t index) override {
        selected = static_cast<int>(index);
        return true;
    }
    bool changePage(bool backward) override {
        pages.push_back(backward);
        return true;
    }

    bool setOption(std::string_view name, bool value) override {
        options.emplace_back(std::string(name), value);
        if (name == "ascii_mode") {
            ascii = value;
        }
        return true;
    }
    bool getOption(std::string_view name) override {
        return name == "ascii_mode" && ascii;
    }

    bool asciiMode() override { return ascii; }
    std::string schemaId() override { return schema; }

    struct Key {
        std::uint32_t sym;
        std::uint32_t mask;
    };

    Paths lastPaths;
    std::vector<Key> keys;
    std::vector<bool> pages;
    std::vector<std::pair<std::string, bool>> options;
    Composition pendingComposition;
    Menu pendingMenu;
    std::string pendingCommit;
    std::string schema;
    int selected = -1;
    int deployCount = 0;
    int sessionCount = 0;
    int commitDrains = 0;
    bool initialized = false;
    bool open = false;
    bool ascii = false;
    bool consumeNext = false;
    bool failInitialize = false;
    bool failSession = false;
    bool failDeploy = false;
};

Paths testPaths() {
    Paths paths;
    paths.sharedData = "/tmp/superior-rime-test/shared";
    paths.userData = "/tmp/superior-rime-test/user";
    return paths;
}

void testStartOpensSession() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));
    CHECK(error.empty());
    CHECK(engine.running());
    CHECK(port.initialized);
    CHECK(port.open);
    CHECK(port.sessionCount == 1);
    // The fake reports a schema, so no deployment is pending.
    CHECK(!engine.needsDeploy());
    engine.stop();
    CHECK(!engine.running());
    CHECK(!port.open);
}

void testStartReportsMissingSchema() {
    FakePort port;
    port.schema.clear();
    RimeEngine engine(port);
    std::string error;
    // A session without a deployed schema is the first-run state: the engine
    // must still start (so the UI can explain) but flag the deployment.
    CHECK(engine.start(testPaths(), error));
    CHECK(engine.needsDeploy());
}

void testInitializeFailureIsReported() {
    FakePort port;
    port.failInitialize = true;
    RimeEngine engine(port);
    std::string error;
    CHECK(!engine.start(testPaths(), error));
    CHECK(error == "fake init failure");
    CHECK(!engine.running());
}

void testSessionFailureFinalizes() {
    FakePort port;
    port.failSession = true;
    RimeEngine engine(port);
    std::string error;
    CHECK(!engine.start(testPaths(), error));
    CHECK(!engine.running());
    // librime was initialized; leaving it that way would keep its data dirs
    // locked for the next start.
    CHECK(!port.initialized);
}

void testReleaseAddsReleaseMask() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    KeyInput press;
    press.sym = 0x61; // 'a'
    press.states = 0;
    engine.processKey(press);

    KeyInput release = press;
    release.release = true;
    engine.processKey(release);

    CHECK(port.keys.size() == 2);
    CHECK(port.keys[0].mask == 0U);
    // X11 Shift is 1<<0; the release bit is librime's 1<<30.
    CHECK(port.keys[1].mask == kReleaseMask);
}

void testModifierBitsPassThrough() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    KeyInput key;
    key.sym = 0x20; // space
    key.states = 0x1U | 0x4U; // Shift | Control
    engine.processKey(key);

    CHECK(port.keys.size() == 1);
    CHECK(port.keys[0].mask == (0x1U | 0x4U));
}

void testCommitDrainedOnce() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    port.pendingCommit = "你好";
    KeyInput key;
    key.sym = 0x20;
    auto result = engine.processKey(key);
    CHECK(result.commit == "你好");
    CHECK(port.commitDrains == 1);

    // The next key must not replay the same commit.
    auto second = engine.processKey(key);
    CHECK(second.commit.empty());
    CHECK(port.commitDrains == 1);
}

void testConsumedFlagFollowsPort() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    KeyInput key;
    key.sym = 0x61;
    CHECK(!engine.processKey(key).consumed);
    port.consumeNext = true;
    CHECK(engine.processKey(key).consumed);
}

void testStartForcesChineseMode() {
    FakePort port;
    port.schema = "wanxiang";
    // A previous session may have left ascii_mode on (librime's own Switcher
    // remembers options). Starting in English would make typing look broken,
    // because the only Chinese/English switch is the agent's.
    port.ascii = true;
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));
    CHECK(!engine.asciiMode());
    CHECK(!port.ascii);
}

void testAsciiModeOption() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    CHECK(!engine.asciiMode());
    CHECK(engine.setAsciiMode(true));
    CHECK(engine.asciiMode());
    CHECK(!port.options.empty());
    CHECK(port.options.back().first == "ascii_mode");
    CHECK(port.options.back().second);

    // The snapshot mirrors it, because the UI decides on the composition row
    // from this flag.
    CHECK(engine.snapshot().asciiMode);
}

void testSnapshotCarriesCompositionAndMenu() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    port.pendingComposition.preedit = "nihao";
    port.pendingComposition.cursor = 5;
    port.pendingMenu.candidates = {Candidate{"你好", "ni hao"}, Candidate{"拟好", ""}};
    port.pendingMenu.pageIndex = 1;
    port.pendingMenu.pageCount = 1;
    port.pendingMenu.highlighted = 0;

    const auto snapshot = engine.snapshot();
    CHECK(snapshot.composition.preedit == "nihao");
    CHECK(snapshot.composition.cursor == 5);
    CHECK(snapshot.menu.candidates.size() == 2);
    CHECK(snapshot.menu.candidates.front().text == "你好");
    CHECK(snapshot.menu.candidates.front().comment == "ni hao");
    CHECK(snapshot.menu.highlighted == 0);
}

void testDeployReopensSession() {
    FakePort port;
    port.schema.clear();
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));
    CHECK(engine.needsDeploy());
    CHECK(port.sessionCount == 1);

    CHECK(engine.deploy(error));
    CHECK(port.deployCount == 1);
    // The session pointed at the old artifacts, so it has to be reopened.
    CHECK(port.sessionCount == 2);
    CHECK(engine.running());
}

void testDeployFailureIsReported() {
    FakePort port;
    port.schema.clear();
    port.failDeploy = true;
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));
    CHECK(!engine.deploy(error));
    CHECK(error == "fake deploy failure");
    // A failed deployment must not leave the engine dead: the old session is
    // reopened so typing still works with whatever was already built.
    CHECK(engine.running());
}

void testCandidateIndexFromId() {
    // Ids are 1-based positions on the current page; 0 and out-of-range are
    // rejected so a stale click cannot select a different word.
    CHECK(RimeEngine::indexForId(1, 7) == 0);
    CHECK(RimeEngine::indexForId(7, 7) == 6);
    CHECK(RimeEngine::indexForId(8, 7) == -1);
    CHECK(RimeEngine::indexForId(0, 7) == -1);
    CHECK(RimeEngine::indexForId(1, 0) == -1);
    CHECK(RimeEngine::indexForId(1, -3) == -1);
}

void testSelectAndPageForwardToPort() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    CHECK(engine.selectCandidate(2));
    CHECK(port.selected == 2);
    CHECK(engine.page(true));
    CHECK(engine.page(false));
    CHECK(port.pages.size() == 2);
    CHECK(port.pages[0]);
    CHECK(!port.pages[1]);
}

void testTakeCommitAfterSelection() {
    FakePort port;
    port.schema = "wanxiang";
    RimeEngine engine(port);
    std::string error;
    CHECK(engine.start(testPaths(), error));

    std::string text = "stale";
    CHECK(!engine.takeCommit(text));
    CHECK(text.empty());

    port.pendingCommit = "原神";
    CHECK(engine.takeCommit(text));
    CHECK(text == "原神");
    CHECK(port.commitDrains == 1);

    // Draining twice must not repeat the text: the click path and the key path
    // share this function, and a duplicate commit would type the word twice.
    CHECK(!engine.takeCommit(text));
    CHECK(text.empty());
    CHECK(port.commitDrains == 1);
}

void testOperationsBeforeStartAreNoOps() {
    FakePort port;
    RimeEngine engine(port);
    KeyInput key;
    CHECK(!engine.processKey(key).consumed);
    CHECK(engine.snapshot().menu.candidates.empty());
    CHECK(!engine.selectCandidate(0));
    CHECK(!engine.page(true));
    CHECK(!engine.setAsciiMode(true));
    CHECK(!engine.asciiMode());
    CHECK(!engine.running());
}

void testPathsAreIsolatedFromFcitx() {
    // The whole point of owning the engine: our data never lands in the user's
    // existing Rime directory.
    const auto paths = Paths::withRoot("/home/user/.local/share");
    CHECK(paths.sharedData == fs::path("/home/user/.local/share/superior-ime/rime/shared"));
    CHECK(paths.userData == fs::path("/home/user/.local/share/superior-ime/rime/user"));
    const std::string user = paths.userData.string();
    CHECK(user.find("fcitx5") == std::string::npos);
    CHECK(user.find("superior-ime") != std::string::npos);
}

void testMissingRootLeavesPathsEmpty() {
    const auto paths = Paths::withRoot({});
    CHECK(paths.sharedData.empty());
    CHECK(paths.userData.empty());
    // An unresolved path must never be treated as "deployed".
    CHECK(!hasCompiledArtifacts(paths));
}

void testArtifactDetection() {
    const fs::path base = fs::temp_directory_path() / "superior-rime-artifacts";
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base, error);

    const auto paths = Paths::withRoot(base);
    fs::create_directories(paths.userData, error);
    CHECK(!hasCompiledArtifacts(paths));

    // A `.prism.bin` is what librime writes after a successful build.
    { std::ofstream stream(paths.userData / "wanxiang.prism.bin"); stream << "x"; }
    CHECK(hasCompiledArtifacts(paths));

    // Current librime writes into `<user>/build`; both layouts must count as
    // deployed, otherwise every session reports "needs deploy" forever.
    fs::remove(paths.userData / "wanxiang.prism.bin", error);
    CHECK(!hasCompiledArtifacts(paths));
    fs::create_directories(paths.userData / "build", error);
    { std::ofstream stream(paths.userData / "build" / "wanxiang.table.bin"); stream << "x"; }
    CHECK(hasCompiledArtifacts(paths));

    fs::remove_all(base, error);
}

} // namespace

int main() {
    testStartOpensSession();
    testStartReportsMissingSchema();
    testInitializeFailureIsReported();
    testSessionFailureFinalizes();
    testReleaseAddsReleaseMask();
    testModifierBitsPassThrough();
    testCommitDrainedOnce();
    testConsumedFlagFollowsPort();
    testStartForcesChineseMode();
    testAsciiModeOption();
    testSnapshotCarriesCompositionAndMenu();
    testDeployReopensSession();
    testDeployFailureIsReported();
    testCandidateIndexFromId();
    testSelectAndPageForwardToPort();
    testTakeCommitAfterSelection();
    testOperationsBeforeStartAreNoOps();
    testPathsAreIsolatedFromFcitx();
    testMissingRootLeavesPathsEmpty();
    testArtifactDetection();
    return superior::test::finish("rime engine tests");
}
