// SPDX-License-Identifier: GPL-3.0-or-later
#include "RimeInputMethod.h"

#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "superior/core/KeyPolicy.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/pinyin/Layout.hpp"

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;

namespace core = superior::core;

namespace pinyin = superior::pinyin;

namespace {

/// Candidate list that reports *librime's* pagination, not its own.
///
/// `CommonCandidateList` only knows the items it was given, which is always
/// exactly one page: librime hands out the current page only. Without these
/// overrides the page indicator would always read "1/1" and wheel paging would
/// silently do nothing.
class RimeCandidateList final : public fcitx::CommonCandidateList {
public:
    using Pager = std::function<void(bool forward)>;

    void setPagination(int pageIndex, int pageCount, bool hasPrevious, bool hasNext) {
        pageIndex_ = pageIndex;
        pageCount_ = pageCount;
        hasPrevious_ = hasPrevious;
        hasNext_ = hasNext;
    }

    void setPager(Pager pager) { pager_ = std::move(pager); }

    [[nodiscard]] int totalPages() const override { return pageCount_; }
    [[nodiscard]] int currentPage() const override { return pageIndex_; }
    [[nodiscard]] bool hasPrev() const override { return hasPrevious_; }
    [[nodiscard]] bool hasNext() const override { return hasNext_; }

    // Wheel paging arrives through these two methods. The local list only holds
    // one page, so they have to forward to librime instead of moving locally.
    void prev() override { dispatch(false); }
    void next() override { dispatch(true); }

private:
    void dispatch(bool forward) {
        // Copy first: the paginator rebuilds the panel, which may drop the last
        // reference to this list while `dispatch` is still on the stack.
        auto pager = pager_;
        if (pager) {
            pager(forward);
        }
    }

    /// 0-based, as required by `PageableCandidateList`.
    int pageIndex_ = 0;
    /// Number of pages, or 0 when librime could not tell (it only reports
    /// "this is the last page"). The UI then omits the indicator instead of
    /// showing a fabricated total.
    int pageCount_ = 0;
    bool hasPrevious_ = false;
    bool hasNext_ = false;
    Pager pager_;
};

/// A candidate that routes selection back into librime.
///
/// The stored index is the candidate's row on the page librime currently shows,
/// not an absolute list index: the agent may insert a learned word or a cloud
/// candidate above it, and those rows must not shift which word this one
/// commits. `LibrimePort` turns the row into an absolute index when selecting.
class RimeCandidateWord final : public fcitx::CandidateWord {
public:
    using Selector = std::function<void(fcitx::InputContext *, std::size_t)>;

    RimeCandidateWord(fcitx::Text text, fcitx::Text comment, std::size_t rimeIndex,
                      Selector selector)
        : fcitx::CandidateWord(std::move(text)), rimeIndex_(rimeIndex),
          selector_(std::move(selector)) {
        if (!comment.empty()) {
            setComment(std::move(comment));
        }
    }

    void select(fcitx::InputContext *inputContext) const override {
        // Copy first: selection rebuilds the panel list, which may destroy this
        // word while its own method is still running.
        auto selector = selector_;
        if (selector) {
            selector(inputContext, rimeIndex_);
        }
    }

private:
    std::size_t rimeIndex_;
    Selector selector_;
};

/// librime uses the X11 modifier layout, which Fcitx5's `KeyState` shares, so
/// masking is enough — no lookup table.
fcitx::KeyStates rimeModifierMask() {
    return fcitx::KeyStates{fcitx::KeyState::Shift, fcitx::KeyState::CapsLock,
                            fcitx::KeyState::Ctrl, fcitx::KeyState::Alt,
                            fcitx::KeyState::Super, fcitx::KeyState::Super2};
}

/// 0-based selection slot for a digit key, or -1.
///
/// `check(sym)` guarantees no Ctrl/Alt/Super: those are shortcuts, not candidate
/// picks.
/// Keys that librime's `ascii_composer` uses to toggle its own ascii_mode.
///
/// The agent implements the Sogou-style Shift tap, so the engine must not
/// switch behind its back. `default.custom.yaml` sets these bindings to `noop`;
/// this list is the runtime guard for a data directory that has not been
/// redeployed yet.
bool isAsciiToggleKey(fcitx::KeySym sym) {
    return sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R || sym == FcitxKey_Caps_Lock;
}

int selectionSlot(const fcitx::Key &key) {
    const fcitx::KeySym sym = key.sym();
    int slot = -1;
    if (sym >= FcitxKey_1 && sym <= FcitxKey_9) {
        slot = static_cast<int>(sym - FcitxKey_1);
    } else if (sym == FcitxKey_0) {
        slot = 9;
    }
    if (slot < 0 || !key.check(sym)) {
        return -1;
    }
    return slot;
}

} // namespace

RimeInputMethod::RimeInputMethod(fcitx::AddonManager *manager)
    : manager_(manager), instance_(manager != nullptr ? manager->instance() : nullptr) {
    diag::configureFromEnvironment();

    // Migrate before touching the data. This is the only place that renames the
    // directory: library-level path resolution stays side-effect free, so a CLI
    // or a unit test can never move the data out from under a running Fcitx5.
    const pinyin::MigrationResult migration = pinyin::migrateLegacyLayoutFromEnvironment();
    if (migration.migrated) {
        SUPERIOR_INFO(diag::categories::Pinyin)
            .field("root", migration.root.string())
            << "pinyin data migrated at startup";
    }

    paths_ = pinyin::Paths::fromEnvironment();

    // Probe without initializing: the addon must stay loadable on a machine
    // where librime is missing, so the log explains the failure instead of
    // Fcitx5 reporting a broken addon.
    {
        pinyin::LibrimePort probe;
        libraryReady_ = probe.available();
        if (!libraryReady_) {
            startError_ = probe.unavailableReason();
        } else {
            SUPERIOR_INFO(diag::categories::Pinyin)
                .field("version", probe.libraryVersion())
                .field("shared", paths_.sharedData.string())
                .field("user", paths_.userData.string())
                .field("artifacts", pinyin::hasCompiledArtifacts(paths_))
                << "rime input method addon starting";
        }
    }

    if (!libraryReady_) {
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("error", startError_)
            << "librime is not available; the Rime input method cannot be used";
    }

    if (instance_ != nullptr && libraryReady_) {
        punctuationAction_.setCheckable(true);
        punctuationAction_.setChecked(!modes_.halfPunctuation());
        punctuationAction_.setShortText("全角");
        punctuationAction_.setLongText("切换标点全角/半角");
        punctuationAction_.setIcon("fcitx-punc-active");
        punctuationAction_.connect<fcitx::SimpleAction::Activated>(
            [this](fcitx::InputContext *inputContext) { togglePunctuation(inputContext); });
        punctuationRegistered_ =
            instance_->userInterfaceManager().registerAction("punctuation", &punctuationAction_);
        if (!punctuationRegistered_) {
            SUPERIOR_WARN(diag::categories::Pinyin)
                << "status action 'punctuation' is already registered; "
                   "leaving punctuation to the other addon";
        }
    }

    if (instance_ != nullptr) {
        // `InputMethodEngine` has no per-context destruction hook; the instance
        // event is the only way to close a session when a window goes away.
        destroyedHandler_ = instance_->watchEvent(
            fcitx::EventType::InputContextDestroyed, fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                forget(static_cast<fcitx::InputContextEvent &>(event).inputContext());
            });
    }
}

RimeInputMethod::~RimeInputMethod() {
    if (instance_ != nullptr && punctuationRegistered_) {
        instance_->userInterfaceManager().unregisterAction(&punctuationAction_);
    }
    destroyedHandler_.reset();
    for (auto &[context, session] : sessions_) {
        (void)context;
        session->engine.stop();
    }
    sessions_.clear();
}

bool RimeInputMethod::engineAvailable() const {
    return libraryReady_;
}

RimeInputMethod::Session *RimeInputMethod::sessionFor(fcitx::InputContext *inputContext) {
    if (inputContext == nullptr || !libraryReady_) {
        // Reported once in the constructor. Retrying dlopen on every key would
        // spam the log and never succeed.
        return nullptr;
    }
    const auto it = sessions_.find(inputContext);
    if (it != sessions_.end()) {
        return it->second.get();
    }

    auto session = std::make_unique<Session>();
    std::string error;
    if (!session->engine.start(paths_, error)) {
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("program", inputContext->program())
            .field("error", error)
            << "cannot open a rime session for this input context";
        return nullptr;
    }
    if (session->engine.needsDeploy()) {
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("shared", paths_.sharedData.string())
            .field("user", paths_.userData.string())
            << "rime has no compiled schema yet; run scripts/fetch-rime-data.sh, then restart";
    }

    Session *raw = session.get();
    sessions_.emplace(inputContext, std::move(session));
    return raw;
}

void RimeInputMethod::forget(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    if (it == sessions_.end()) {
        return;
    }
    it->second->engine.stop();
    sessions_.erase(it);
    SUPERIOR_DEBUG(diag::categories::Pinyin)
        .field("remaining", static_cast<std::int64_t>(sessions_.size()))
        << "rime session closed";
}

void RimeInputMethod::activate(const fcitx::InputMethodEntry &entry,
                               fcitx::InputContextEvent &event) {
    (void)entry;
    auto *inputContext = event.inputContext();
    if (inputContext == nullptr) {
        return;
    }
    // Opening lazily on activation keeps windows that never use this input
    // method from allocating a session at all.
    auto *session = sessionFor(inputContext);
    inputContext->inputPanel().reset();
    if (session != nullptr) {
        attachPunctuation(inputContext, *session);
        updatePanel(inputContext, *session);
    }
}

void RimeInputMethod::deactivate(const fcitx::InputMethodEntry &entry,
                                 fcitx::InputContextEvent &event) {
    (void)entry;
    auto *inputContext = event.inputContext();
    if (inputContext == nullptr) {
        return;
    }
    inputContext->inputPanel().reset();
    forget(inputContext);
}

void RimeInputMethod::reset(const fcitx::InputMethodEntry &entry,
                            fcitx::InputContextEvent &event) {
    (void)entry;
    auto *inputContext = event.inputContext();
    if (inputContext == nullptr) {
        return;
    }
    // Reopening is the only reliable way to clear librime's composition through
    // the C API; `clear_composition` leaves parts of the state behind.
    forget(inputContext);
    auto *session = sessionFor(inputContext);
    inputContext->inputPanel().reset();
    if (session != nullptr) {
        attachPunctuation(inputContext, *session);
        updatePanel(inputContext, *session);
    }
}

void RimeInputMethod::keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &event) {
    (void)entry;
    auto *inputContext = event.inputContext();
    if (inputContext == nullptr) {
        return;
    }
    auto *session = sessionFor(inputContext);
    if (session == nullptr) {
        return;
    }

    // Selection keys are routed through the visible list, not through librime.
    // Cloud pinyin and learned words are rows the engine has never seen; letting
    // librime resolve "2" would commit a different word than the one shown.
    if (handleSelectionKey(inputContext, event)) {
        event.filterAndAccept();
        return;
    }

    // Sogou-style paging. `+` reaches us as Shift+`=` with the `plus` keysym,
    // which the schema has no binding for, so librime would commit it as
    // punctuation and end the composition. Paging through the list keeps this
    // identical to a wheel scroll: same engine call, same panel refresh.
    if (const int step = core::pageStep(static_cast<std::uint32_t>(event.key().sym()));
        step != 0 && !event.isRelease()) {
        const fcitx::KeyStates states = event.key().states();
        const bool shortcut =
            states.testAny(fcitx::KeyStates{fcitx::KeyState::Ctrl, fcitx::KeyState::Alt,
                                            fcitx::KeyState::Super});
        auto list = std::dynamic_pointer_cast<RimeCandidateList>(
            inputContext->inputPanel().candidateList());
        if (!shortcut && list && list->size() > 0) {
            SUPERIOR_TRACE(diag::categories::Pinyin)
                .field("pageStep", static_cast<std::int64_t>(step))
                .field("page", static_cast<std::int64_t>(list->currentPage()))
                << "paging the visible list";
            if (step > 0) {
                list->next();
            } else {
                list->prev();
            }
            event.filterAndAccept();
            return;
        }
    }

    const bool asciiBefore = session->engine.asciiMode();
    pinyin::KeyInput key;
    key.sym = static_cast<std::uint32_t>(event.key().sym());
    key.states = static_cast<std::uint32_t>(event.key().states() & rimeModifierMask());
    key.release = event.isRelease();

    const pinyin::RimeEngine::KeyResult result = session->engine.processKey(key);

    // Defense for an old deployment: with the shipped `default.custom.yaml`
    // librime never toggles, but a data directory deployed before that patch
    // would still flip ascii_mode on the same Shift release the agent uses.
    // Leaving it on makes every later key bypass pinyin, which looks like
    // "switch back to Chinese still types English".
    if (isAsciiToggleKey(event.key().sym()) && session->engine.asciiMode()) {
        session->engine.setAsciiMode(false);
        SUPERIOR_DEBUG(diag::categories::Pinyin)
            .field("sym", static_cast<std::int64_t>(event.key().sym()))
            << "librime tried to switch ascii_mode; kept it off (the agent owns the switch)";
    }

    SUPERIOR_TRACE(diag::categories::Pinyin)
        .field("sym", static_cast<std::int64_t>(key.sym))
        .field("release", key.release)
        .field("consumed", result.consumed)
        .field("commit", static_cast<std::int64_t>(result.commit.size()))
        .field("asciiBefore", asciiBefore)
        .field("asciiAfter", session->engine.asciiMode())
        << "key processed by librime";
    // Consume before committing: the commit re-enters the UI, and a key that is
    // both consumed and productive must not also reach the client.
    if (result.consumed) {
        event.filterAndAccept();
    }
    if (!result.commit.empty()) {
        commit(inputContext, result.commit);
    }
    updatePanel(inputContext, *session);
}

bool RimeInputMethod::handleSelectionKey(fcitx::InputContext *inputContext,
                                         fcitx::KeyEvent &event) {
    if (event.isRelease()) {
        return false;
    }
    const auto candidateList = inputContext->inputPanel().candidateList();
    auto list = std::dynamic_pointer_cast<RimeCandidateList>(candidateList);
    if (!list || list->size() <= 0) {
        return false;
    }

    const fcitx::Key key = event.key();
    SUPERIOR_TRACE(diag::categories::Pinyin)
        .field("sym", static_cast<std::int64_t>(key.sym()))
        .field("rows", static_cast<std::int64_t>(list->size()))
        .field("cursor", static_cast<std::int64_t>(list->cursorIndex()))
        << "selection key on the visible list";
    if (key.check(FcitxKey_space)) {
        int cursor = list->cursorIndex();
        if (cursor < 0 || cursor >= list->size()) {
            cursor = 0;
        }
        list->candidate(cursor).select(inputContext);
        return true;
    }
    if (key.check(FcitxKey_Up) || key.check(FcitxKey_Down)) {
        const int delta = key.check(FcitxKey_Up) ? -1 : 1;
        int cursor = list->cursorIndex();
        if (cursor < 0) {
            cursor = 0;
        }
        const int next = std::clamp(cursor + delta, 0, list->size() - 1);
        if (next != cursor) {
            list->setCursorIndex(next);
            inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return true;
    }
    const int slot = selectionSlot(key);
    if (slot >= 0 && slot < list->size()) {
        SUPERIOR_TRACE(diag::categories::Pinyin)
            .field("slot", static_cast<std::int64_t>(slot))
            .field("text", list->candidate(slot).text().toString())
            << "selecting the visible row";
        list->candidate(slot).select(inputContext);
        return true;
    }
    if (slot >= 0) {
        SUPERIOR_TRACE(diag::categories::Pinyin)
            .field("slot", static_cast<std::int64_t>(slot))
            .field("rows", static_cast<std::int64_t>(list->size()))
            << "selection slot is outside this page";
    }
    return false;
}

void RimeInputMethod::commit(fcitx::InputContext *inputContext, const std::string &text) {
    if (text.empty() || inputContext == nullptr) {
        return;
    }
    inputContext->commitString(text);
    inputContext->updatePreedit();
}

void RimeInputMethod::updatePanel(fcitx::InputContext *inputContext, Session &session) {
    session.lastSnapshot = session.engine.snapshot();
    const auto &snapshot = session.lastSnapshot;

    auto &panel = inputContext->inputPanel();
    if (snapshot.composition.preedit.empty() && snapshot.menu.candidates.empty()) {
        if (session.listBuilt || panel.candidateList() || !panel.preedit().toString().empty()) {
            panel.reset();
            session.listBuilt = false;
            session.builtTexts.clear();
            inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return;
    }

    fcitx::Text preedit(snapshot.composition.preedit);
    preedit.setCursor(static_cast<int>(snapshot.composition.cursor));
    panel.setPreedit(preedit);

    const int pageIndex = std::max(0, snapshot.menu.pageIndex - 1);

    // Keep the list object when librime's candidate set did not change. That is
    // what makes Up/Down navigation and an inserted cloud row compatible:
    // moving the cursor must not rebuild the list and lose the row.
    bool sameCandidates = session.builtTexts.size() == snapshot.menu.candidates.size();
    for (std::size_t index = 0; sameCandidates && index < session.builtTexts.size(); ++index) {
        sameCandidates = session.builtTexts[index] == snapshot.menu.candidates[index].text;
    }
    if (session.listBuilt && sameCandidates) {
        if (auto current = std::dynamic_pointer_cast<RimeCandidateList>(panel.candidateList())) {
            current->setPagination(pageIndex, snapshot.menu.pageCount,
                                   snapshot.menu.hasPrevious, snapshot.menu.hasNext);
            inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            return;
        }
    }

    auto list = std::make_unique<RimeCandidateList>();
    list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
    list->setPagination(pageIndex, snapshot.menu.pageCount, snapshot.menu.hasPrevious,
                        snapshot.menu.hasNext);
    if (!snapshot.menu.candidates.empty()) {
        list->setPageSize(static_cast<int>(snapshot.menu.candidates.size()));
    }
    // Wheel paging has to change librime's page, not the local one.
    list->setPager([this, inputContext](bool forward) {
        const auto it = sessions_.find(inputContext);
        if (it == sessions_.end()) {
            return;
        }
        // `RimeEngine::page` takes "backward", the list reports "forward".
        if (!it->second->engine.page(!forward)) {
            return;
        }
        updatePanel(inputContext, *it->second);
    });

    for (std::size_t index = 0; index < snapshot.menu.candidates.size(); ++index) {
        const auto &candidate = snapshot.menu.candidates[index];
        list->append<RimeCandidateWord>(
            fcitx::Text(candidate.text), fcitx::Text(candidate.comment), index,
            [this](fcitx::InputContext *context, std::size_t rimeIndex) {
                const auto it = sessions_.find(context);
                if (it == sessions_.end()) {
                    return;
                }
                Session &target = *it->second;
                SUPERIOR_DEBUG(diag::categories::Pinyin)
                    .field("rimeIndex", static_cast<std::int64_t>(rimeIndex))
                    .field("program", context->program())
                    << "candidate selected from the visible list";
                if (!target.engine.selectCandidate(rimeIndex)) {
                    return;
                }
                std::string committed;
                if (target.engine.takeCommit(committed)) {
                    commit(context, committed);
                }
                updatePanel(context, target);
            });
    }
    if (snapshot.menu.highlighted >= 0 && !snapshot.menu.candidates.empty()) {
        list->setCursorIndex(std::min(snapshot.menu.highlighted,
                                      static_cast<int>(snapshot.menu.candidates.size()) - 1));
    }

    session.builtTexts.clear();
    session.builtTexts.reserve(snapshot.menu.candidates.size());
    for (const auto &candidate : snapshot.menu.candidates) {
        session.builtTexts.push_back(candidate.text);
    }
    session.listBuilt = true;

    panel.setCandidateList(std::move(list));
    inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void RimeInputMethod::attachPunctuation(fcitx::InputContext *inputContext, Session &session) {
    if (!punctuationRegistered_) {
        return;
    }
    // A window that takes focus later must agree with the toolbar, so the
    // session is *told* the current mode instead of being asked for its default.
    applyModes(session);
    inputContext->statusArea().addAction(fcitx::StatusGroup::InputMethod, &punctuationAction_);
}

void RimeInputMethod::applyModes(Session &session) {
    // Only the switches this addon owns: language belongs to the agent, and
    // pushing `ascii_mode` from here would be a second writer again.
    session.engine.setOption("ascii_punct", modes_.halfPunctuation());
    session.engine.setOption("full_shape", modes_.fullShape());
}

void RimeInputMethod::refreshPunctuationAction() {
    const bool half = modes_.halfPunctuation();
    // `isChecked` drives the glyph: full width is a filled circle.
    punctuationAction_.setChecked(!half);
    punctuationAction_.setShortText(half ? "半角" : "全角");
    punctuationAction_.setIcon(half ? "fcitx-punc-inactive" : "fcitx-punc-active");
}

void RimeInputMethod::togglePunctuation(fcitx::InputContext *inputContext) {
    SUPERIOR_DEBUG(diag::categories::Pinyin)
        .field("program", inputContext != nullptr ? inputContext->program() : std::string{})
        .field("sessions", static_cast<std::int64_t>(sessions_.size()))
        << "punctuation action activated";
    const auto it = sessions_.find(inputContext);
    if (it == sessions_.end()) {
        SUPERIOR_DEBUG(diag::categories::Pinyin)
            .field("program", inputContext != nullptr ? inputContext->program() : std::string{})
            << "punctuation action ignored: no session for this context";
        return;
    }

    modes_.togglePunctuation();
    const bool half = modes_.halfPunctuation();
    // Every open session has to receive the new value; a window that takes
    // focus later would otherwise keep the punctuation of the previous window.
    for (auto &[context, session] : sessions_) {
        (void)context;
        applyModes(*session);
    }
    refreshPunctuationAction();

    inputContext->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
    SUPERIOR_INFO(diag::categories::Pinyin)
        .field("halfWidth", half)
        .field("program", inputContext->program())
        << "punctuation width changed";
}

std::string RimeInputMethod::subMode(const fcitx::InputMethodEntry &entry,
                                     fcitx::InputContext &inputContext) {
    (void)entry;
    const auto it = sessions_.find(&inputContext);
    if (it == sessions_.end()) {
        return {};
    }
    return it->second->engine.asciiMode() ? "en" : "zh";
}

std::string RimeInputMethod::subModeLabelImpl(const fcitx::InputMethodEntry &entry,
                                              fcitx::InputContext &inputContext) {
    (void)entry;
    const auto it = sessions_.find(&inputContext);
    if (it == sessions_.end()) {
        return {};
    }
    return it->second->engine.asciiMode() ? "英" : "中";
}

} // namespace superior::fcitx5

namespace {

class RimeInputMethodFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new superior::fcitx5::RimeInputMethod(manager);
    }
};

} // namespace

FCITX_ADDON_FACTORY_V2(superiorpinyin, RimeInputMethodFactory);
