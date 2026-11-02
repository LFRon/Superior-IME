// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/api/Composer.hpp"
#include "superior/api/Key.hpp"
#include "superior/pinyin/CloudService.hpp"
#include "superior/pinyin/LearningStore.hpp"
#include "superior/pinyin/RimeComposer.hpp"

namespace {

using superior::api::KeyCode;
using superior::api::KeyPress;
using superior::api::PipelineOptions;
using superior::pinyin::Candidate;
using superior::pinyin::CloudService;
using superior::pinyin::Composition;
using superior::pinyin::LearningStore;
using superior::pinyin::Menu;
using superior::pinyin::Paths;
using superior::pinyin::RimeComposer;
using superior::pinyin::RimePort;
using superior::pinyin::Snapshot;

/// A scripted engine: enough to test the product layer (selection, learning,
/// candidate promotion, commit-letters) without librime or a schema.
class FakeRimePort final : public RimePort {
public:
    bool initialize(const Paths &, std::string &) override { return true; }
    void finalize() override {}
    bool deploy(std::string &) override { return true; }
    bool createSession(std::string &) override {
        open_ = true;
        return true;
    }
    void destroySession() override { open_ = false; }
    void clearComposition() override {
        preedit_.clear();
        candidates_.clear();
        highlighted_ = -1;
    }

    bool processKey(std::uint32_t sym, std::uint32_t) override {
        if (!open_) {
            return false;
        }
        if (sym >= static_cast<std::uint32_t>('a') && sym <= static_cast<std::uint32_t>('z')) {
            preedit_.push_back(static_cast<char>(sym));
            rebuild();
            return true;
        }
        return false;
    }

    bool takeCommit(std::string &out) override {
        if (pending_.empty()) {
            return false;
        }
        out = pending_;
        pending_.clear();
        return true;
    }

    Composition composition() override {
        Composition composition;
        composition.preedit = preedit_;
        composition.cursor = preedit_.size();
        return composition;
    }

    Menu menu() override {
        Menu menu;
        menu.pageIndex = 1;
        menu.pageCount = 1;
        menu.highlighted = highlighted_;
        for (const auto &text : candidates_) {
            Candidate candidate;
            candidate.text = text;
            menu.candidates.push_back(std::move(candidate));
        }
        return menu;
    }

    bool selectCandidate(std::size_t index) override {
        if (index >= candidates_.size()) {
            return false;
        }
        pending_ = candidates_[index];
        preedit_.clear();
        candidates_.clear();
        highlighted_ = -1;
        return true;
    }

    bool changePage(bool) override { return false; }
    bool setOption(std::string_view, bool) override { return true; }
    bool getOption(std::string_view) override { return false; }
    bool asciiMode() override { return false; }
    std::string schemaId() override { return "test"; }

private:
    void rebuild() {
        if (preedit_ == "wo") {
            candidates_ = {"我", "握", "窝"};
            highlighted_ = 0;
        } else {
            candidates_ = {"候" + preedit_};
            highlighted_ = 0;
        }
    }

    bool open_ = false;
    std::string preedit_;
    std::vector<std::string> candidates_;
    int highlighted_ = -1;
    std::string pending_;
};

KeyPress letter(char value) {
    KeyPress key;
    key.code = KeyCode::Other;
    key.text = static_cast<char32_t>(static_cast<unsigned char>(value));
    return key;
}

std::filesystem::path freshPath() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "superior-rime-composer-tests";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir / "learning.json";
}

Paths testPaths() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "superior-rime-composer-paths";
    Paths paths;
    paths.sharedData = root / "shared";
    paths.userData = root / "user";
    return paths;
}

void testSelectLearnsFromTheFirstPick() {
    const auto learningPath = freshPath();
    LearningStore learning(learningPath);
    learning.load();
    CloudService cloud;
    PipelineOptions options;
    Paths paths = testPaths();
    std::string error;

    RimeComposer composer(std::make_unique<FakeRimePort>(), paths, learning, cloud, options,
                          error);
    CHECK(composer.available());

    composer.processKey(letter('w'), 1000);
    composer.processKey(letter('o'), 1000);
    CHECK(composer.frame().preedit == "wo");
    CHECK(composer.frame().candidates.size() == 3);
    CHECK(composer.frame().candidates[0].text == "我");

    const auto outcome = composer.select(1, 2000);
    CHECK(outcome.consumed);
    CHECK(outcome.commit == "握");
    CHECK(!composer.frame().composing);
    CHECK(composer.frame().preedit.empty());
    CHECK(learning.preferred("wo") == std::optional<std::string>("握"));

    // The next `wo` must offer 握 first, from the very first pick.
    composer.reset();
    composer.processKey(letter('w'), 3000);
    composer.processKey(letter('o'), 3000);
    CHECK(composer.frame().candidates.size() == 3);
    CHECK(composer.frame().candidates[0].text == "握");
    CHECK(composer.frame().candidates[0].source ==
          superior::api::CandidateFact::Source::Engine);
    CHECK(composer.frame().candidates[0].engineRow == 1U);
}

void testCommitLettersDropsTheComposition() {
    const auto learningPath = freshPath();
    LearningStore learning(learningPath);
    learning.load();
    CloudService cloud;
    PipelineOptions options;
    Paths paths = testPaths();
    std::string error;
    RimeComposer composer(std::make_unique<FakeRimePort>(), paths, learning, cloud, options,
                          error);

    composer.processKey(letter('n'), 1000);
    composer.processKey(letter('i'), 1000);
    std::string letters;
    CHECK(composer.commitLetters(letters));
    CHECK(letters == "ni");
    CHECK(!composer.frame().visible);
    CHECK(!composer.frame().composing);
}

} // namespace

int main() {
    testSelectLearnsFromTheFirstPick();
    testCommitLettersDropsTheComposition();
    return superior::test::finish("rime-composer");
}
