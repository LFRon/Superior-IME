// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/runtime/Runtime.hpp"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>
#include <variant>

#include "superior/api/TextEdit.hpp"
#include "superior/core/KeyPolicy.hpp"
#include "superior/core/SceneInput.hpp"
#include "superior/diagnostics/Log.hpp"

namespace superior::runtime {

namespace diag = ::superior::diagnostics;

namespace {

/// UTF-8 encoding for the one character `KeyPress::text` carries.
std::string utf8FromCodePoint(char32_t code) {
    std::string out;
    if (code < 0x80U) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
    return out;
}

/// 0-based selection slot for a digit key, or -1. `text` is authoritative: a
/// shifted digit produces a symbol, not a digit, so it cannot select.
int digitSlot(const api::KeyPress &press) {
    if (press.text >= U'1' && press.text <= U'9') {
        return static_cast<int>(press.text - U'1');
    }
    if (press.text == U'0') {
        return 9;
    }
    return -1;
}

/// How long the language badge stays on screen after Shift flips 中/英 while
/// the toolbar is hidden.
///
/// It is a product decision, not a UI one: the runtime owns the clock and the
/// host only executes the resulting frame. Long enough to read a single glyph,
/// short enough not to linger over the next word.
constexpr std::uint64_t kLanguageIndicatorMs = 900;

bool sameComposition(const api::CompositionFact &left, const api::CompositionFact &right) {
    if (left.visible != right.visible || left.preedit != right.preedit ||
        left.highlighted != right.highlighted || left.pageIndex != right.pageIndex ||
        left.pageCount != right.pageCount || left.candidates.size() != right.candidates.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.candidates.size(); ++index) {
        if (left.candidates[index].text != right.candidates[index].text) {
            return false;
        }
    }
    return true;
}

} // namespace

struct Runtime::Session {
    explicit Session(api::SessionKey value) : key(value), scene(value) {}

    api::SessionKey key;
    core::ImeSession scene;
    core::Modes modes;
    std::unique_ptr<api::Composer> composer;
    api::HostFacts host;
    bool composerFailed = false;
    std::string composerError;
    /// Monotonic deadline for the language badge; 0 when it is not showing.
    /// The text itself lives in `scene`, so there is still one owner per fact.
    std::uint64_t indicatorUntilMs = 0;
};

Runtime::Runtime(Ports ports, api::ComposerFactory &pinyin, SpeechPort &speech, Options options)
    : ports_(ports), pinyin_(pinyin), speech_(speech), options_(std::move(options)) {
    if (options_.settingsPath.empty()) {
        options_.settingsPath = config::defaultSettingsPath();
    }
    reloadSettings();
}

Runtime::~Runtime() {
    for (auto &session : sessions_) {
        if (session->composer) {
            session->composer->reset();
        }
    }
    speech_.shutdown();
}

std::uint64_t Runtime::nowMs() const {
    return ports_.scheduler.nowMs();
}

Runtime::Session *Runtime::find(const api::SessionKey &key) {
    for (auto &session : sessions_) {
        if (session->key == key) {
            return session.get();
        }
    }
    return nullptr;
}

const Runtime::Session *Runtime::find(const api::SessionKey &key) const {
    for (const auto &session : sessions_) {
        if (session->key == key) {
            return session.get();
        }
    }
    return nullptr;
}

Runtime::Session &Runtime::ensure(const api::SessionKey &key, const api::HostFacts &host) {
    if (Session *existing = find(key)) {
        existing->host = host;
        return *existing;
    }
    auto session = std::make_unique<Session>(key);
    session->host = host;
    session->scene.setSkin(settings_.skinId.empty() ? options_.defaultSkinId : settings_.skinId, 1);
    createComposer(*session);
    sessions_.push_back(std::move(session));
    return *sessions_.back();
}

void Runtime::createComposer(Session &session) {
    if (session.composer) {
        return;
    }
    std::string error;
    auto composer = pinyin_.create(error);
    if (!composer) {
        session.composerFailed = true;
        session.composerError = error;
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("error", error)
            .field("program", session.host.program)
            << "pinyin composer unavailable; keys will be forwarded";
        ports_.lifecycle.degraded(api::DegradedReason::EngineUnavailable, error);
        return;
    }
    session.composer = std::move(composer);
    session.composer->setOptions(pipelineOptions_);
    applyModes(session);
    applyPreferences(session);
    ports_.lifecycle.recovered(api::DegradedReason::EngineUnavailable);
}

void Runtime::applyModes(Session &session) {
    if (!session.composer) {
        return;
    }
    session.composer->setOption("ascii_punct", session.modes.halfPunctuation());
    session.composer->setOption("full_shape", session.modes.fullShape());
    // The product owns the Chinese/English switch; librime must not flip it
    // behind the runtime's back (v2's "still English after switching back" bug).
    session.composer->setOption("ascii_mode", false);
}

void Runtime::applyPreferences(Session &session) {
    if (!session.composer) {
        return;
    }
    session.composer->setOption("emoji", settings_.emojiCandidates);
}

void Runtime::reloadSettings() {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kCheckInterval = std::chrono::milliseconds(200);
    if (settingsLoaded_ && now - settingsChecked_ < kCheckInterval) {
        return;
    }
    settingsChecked_ = now;

    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(options_.settingsPath, error);
    if (settingsLoaded_ && (error || stamp == settingsStamp_)) {
        return;
    }
    settingsStamp_ = stamp;
    settingsLoaded_ = true;

    const config::SettingsLoad loaded = config::loadSettings(options_.settingsPath);
    for (const auto &warning : loaded.warnings) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("settings", options_.settingsPath.string())
            .field("warning", warning)
            << "settings rejected part of the file";
    }
    settings_ = loaded.settings;
    if (settings_.skinId.empty()) {
        settings_.skinId = options_.defaultSkinId;
    }

    pipelineOptions_.cloudEnabled = settings_.cloudPinyinEnabled;
    pipelineOptions_.cloudMinLength = settings_.cloudPinyinMinLength;
    pipelineOptions_.cloudIndex = settings_.cloudPinyinIndex;
    pipelineOptions_.cloudBackend = settings_.cloudPinyinBackend;

    speech_.setEnabled(settings_.speechRecognitionEnabled);
    speech_.setHotkeyEnabled(settings_.speechRecognitionHotkey);
    speech_.setModelId(settings_.speechRecognitionModelId);

    for (auto &session : sessions_) {
        if (session->composer) {
            session->composer->setOptions(pipelineOptions_);
        }
        applyModes(*session);
        applyPreferences(*session);
        present(*session);
    }

    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("inlineEnglish", settings_.shiftToggle)
        .field("emoji", settings_.emojiCandidates)
        .field("cloud", settings_.cloudPinyinEnabled)
        .field("speech", settings_.speechRecognitionEnabled)
        << "runtime settings loaded";
}

void Runtime::focusIn(const api::SessionKey &key, const api::HostFacts &host) {
    Session &session = ensure(key, host);
    active_ = key;
    outbox_.setDestination(key);
    outbox_.flush();
    speech_.flushHeld();
    session.host = host;
    session.host.focused = true;
    present(session);
}

void Runtime::focusOut(const api::SessionKey &key) {
    if (Session *session = find(key)) {
        session->host.focused = false;
        // The badge belongs to the caret that just lost focus; leaving it would
        // make the next focus-in flash a stale language state.
        session->indicatorUntilMs = 0;
        session->scene.hideIndicator();
        presentHidden(key);
    }
    speech_.holdCommits();
    if (active_ == key) {
        active_ = {};
        // Only the focus that is actually leaving may clear the caret target.
        // A late `focusOut` for an old context must not erase the destination
        // that the new focus just installed.
        outbox_.setDestination({});
    }
}

void Runtime::destroy(const api::SessionKey &key) {
    if (outbox_.destination() == key) {
        outbox_.setDestination({});
    }
    if (active_ == key) {
        active_ = {};
        // The session that speech was typing into is going away. Keep a
        // transcript that arrives afterwards instead of dropping it into a
        // context that no longer exists.
        speech_.holdCommits();
    }
    const auto found = std::find_if(sessions_.begin(), sessions_.end(),
                                    [&key](const auto &session) { return session->key == key; });
    if (found == sessions_.end()) {
        return;
    }
    if ((*found)->composer) {
        (*found)->composer->reset();
    }
    presentHidden(key);
    sessions_.erase(found);
}

void Runtime::reset(const api::SessionKey &key) {
    Session *session = find(key);
    if (session == nullptr) {
        return;
    }
    if (session->composer) {
        session->composer->reset();
    }
    present(*session);
}

bool Runtime::key(const api::SessionKey &key, const api::KeyPress &press,
                  const api::HostFacts &host) {
    Session &session = ensure(key, host);
    session.host = host;

    // Releases never carry a product decision, but the engine may need them
    // (librime tracks modifier and key state). They go straight to the
    // capability, exactly as `架构设计.md` §5.1 describes.
    if (press.release) {
        if (session.composer && session.composer->frame().composing) {
            const api::KeyOutcome outcome = session.composer->processKey(press, nowMs());
            applyOutcome(session, outcome, core::WhenUnfocused::Drop);
            present(session);
            return outcome.consumed;
        }
        return false;
    }

    const bool composing = session.composer && session.composer->frame().composing;
    const core::KeyDecision decision = core::decideKey(core::KeyPolicyInput{
        .key = press,
        .composing = composing,
        .english = session.modes.english(),
    });

    switch (decision.intent) {
    case core::KeyIntent::Ignore:
    case core::KeyIntent::Forward:
        return false;
    case core::KeyIntent::Cancel:
        if (session.composer) {
            session.composer->reset();
        }
        present(session);
        return true;
    case core::KeyIntent::ToggleLanguage:
        setLanguage(session, false, "escape");
        return true;
    case core::KeyIntent::CommitInline: {
        if (press.text == 0) {
            return false;
        }
        applyText(session.key, api::TextEdit::commit(utf8FromCodePoint(press.text)),
                  core::WhenUnfocused::Drop);
        present(session);
        return true;
    }
    case core::KeyIntent::Engine:
        break;
    }

    if (!session.composer) {
        return false;
    }

    // Selection and paging are product rules, so they are handled here and not
    // by the engine or the host. They only apply while a composition is open;
    // without one, the same key belongs to the client or to the schema.
    if (!press.release && !press.control && !press.alt && !press.super) {
        const api::CompositionFact frame = session.composer->frame();
        if (frame.composing) {
            const std::uint64_t now = nowMs();
            if (press.code == api::KeyCode::Space) {
                if (frame.candidates.empty()) {
                    // Nothing to pick: the engine could not turn the rest of the
                    // input into candidates, so the composition is left holding
                    // text only ("weibrds" -> 为 + brds, or a bare "brds").
                    // Space then types what is being composed as it stands, the
                    // same text librime's own space key would commit.
                    std::string letters;
                    if (session.composer->commitLetters(letters) && !letters.empty()) {
                        outbox_.commit(session.key, letters, core::WhenUnfocused::Drop);
                    }
                    present(session);
                    return true;
                }
                const auto row = frame.highlighted >= 0
                                     ? static_cast<std::size_t>(frame.highlighted)
                                     : 0U;
                applyOutcome(session, session.composer->select(row, now),
                             core::WhenUnfocused::Drop);
                present(session);
                return true;
            }
            if (const int slot = digitSlot(press);
                slot >= 0 && static_cast<std::size_t>(slot) < frame.candidates.size()) {
                applyOutcome(session,
                             session.composer->select(static_cast<std::size_t>(slot), now),
                             core::WhenUnfocused::Drop);
                present(session);
                return true;
            }
            if (press.code == api::KeyCode::Up || press.code == api::KeyCode::Down) {
                if (session.composer->moveSelection(press.code == api::KeyCode::Up ? -1 : 1)) {
                    present(session);
                }
                return true;
            }
            if (const int step = core::pageStep(press.code); step != 0) {
                if (session.composer->page(step)) {
                    present(session);
                }
                return true;
            }
        }
    }

    const api::KeyOutcome outcome = session.composer->processKey(press, nowMs());
    applyOutcome(session, outcome, core::WhenUnfocused::Drop);
    present(session);
    return outcome.consumed;
}

bool Runtime::gesture(const api::SessionKey &key, api::Gesture gesture,
                      const api::HostFacts &host) {
    Session &session = ensure(key, host);
    session.host = host;
    const bool composing = session.composer && session.composer->frame().composing;

    const core::GestureIntent intent = core::decideGesture(core::GestureInput{
        .gesture = gesture,
        .english = session.modes.english(),
        .composing = composing,
        .inlineEnglishEnabled = settings_.shiftToggle,
        .shiftCommitPinyin = settings_.shiftCommitPinyin,
        .speechEnabled = settings_.speechRecognitionEnabled,
        .speechHotkeyEnabled = settings_.speechRecognitionHotkey,
        .speaking = speech_.busy(),
    });

    switch (intent) {
    case core::GestureIntent::None:
        return false;
    case core::GestureIntent::ToggleLanguage:
        setLanguage(session, !session.modes.english(), "gesture");
        return true;
    case core::GestureIntent::CommitAndSwitchToEnglish:
        commitLettersAndSwitch(session);
        return true;
    case core::GestureIntent::ToggleSpeech:
        speech_.toggleByHotkey();
        present(session);
        return true;
    case core::GestureIntent::CancelSpeech:
        speech_.cancel("escape");
        present(session);
        return true;
    }
    return false;
}

void Runtime::setLanguage(Session &session, bool english, const char *reason) {
    const core::Language target = english ? core::Language::English : core::Language::Chinese;
    if (!session.modes.setLanguage(target)) {
        return;
    }
    if (english) {
        // Drop the half-finished pinyin; otherwise its stale candidate list
        // keeps the panel visible while English letters commit directly.
        if (session.composer) {
            session.composer->reset();
        }
        outbox_.abandonComposition();
    }
    applyModes(session);
    if (!settings_.showStatusBar) {
        // With the toolbar hidden the badge is the only feedback the user gets
        // for a Shift toggle; with it visible the 中/英 chip already answers.
        session.indicatorUntilMs = nowMs() + kLanguageIndicatorMs;
        session.scene.showIndicator(english ? "A" : "拼");
    }
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("english", session.modes.english())
        .field("reason", std::string(reason))
        .field("program", session.host.program)
        << "inline input mode changed";
    present(session);
}

void Runtime::commitLettersAndSwitch(Session &session) {
    std::string letters;
    const bool hadLetters =
        session.composer && session.composer->commitLetters(letters) && !letters.empty();
    if (hadLetters) {
        outbox_.commit(session.key, letters, core::WhenUnfocused::Drop);
    }
    setLanguage(session, true, "shift tap: letters committed");
}

void Runtime::applyOutcome(Session &session, const api::KeyOutcome &outcome,
                           core::WhenUnfocused when) {
    if (!outcome.commit.empty()) {
        outbox_.commit(session.key, outcome.commit, when);
    }
}

void Runtime::applyText(const api::SessionKey &key, api::TextEdit edit,
                        core::WhenUnfocused when) {
    outbox_.apply(key, std::span<const api::TextEdit>(&edit, 1), when);
}

std::vector<api::StatusItem> Runtime::statusItems(const Session &session) const {
    std::vector<api::StatusItem> items = session.host.statusItems;

    api::StatusItem mode;
    mode.id = "inline-mode";
    mode.label = session.modes.english() ? "英" : "中";
    mode.active = session.modes.english();
    items.insert(items.begin(), std::move(mode));

    api::StatusItem punctuation;
    punctuation.id = "punctuation";
    punctuation.label = session.modes.halfPunctuation() ? "半角" : "全角";
    punctuation.active = !session.modes.halfPunctuation();
    punctuation.role = api::StatusRole::Punctuation;
    items.push_back(std::move(punctuation));

    if (speech_.enabled()) {
        api::StatusItem microphone;
        microphone.id = "speech";
        microphone.icon = "microphone";
        microphone.active = speech_.recording();
        microphone.role = api::StatusRole::Speech;
        items.insert(items.begin(), std::move(microphone));
    }
    return items;
}

void Runtime::publishStatus(Session &session) {
    if (ports_.status != nullptr) {
        const std::vector<api::StatusItem> items = statusItems(session);
        ports_.status->setStatusItems(items);
    }
}

void Runtime::present(Session &session) {
    api::CompositionFact composition;
    if (session.composer) {
        composition = session.composer->frame();
    }

    // A new composition takes the space next to the caret, and a toolbar that
    // became visible already shows the mode. Either way the badge loses its
    // reason to exist; the deadline is dropped so it cannot come back.
    if (composition.composing || settings_.showStatusBar) {
        session.indicatorUntilMs = 0;
        session.scene.hideIndicator();
    }

    api::HostFacts host = session.host;
    host.statusItems = statusItems(session);
    core::updateSession(composition, host, session.scene,
                        settings_.skinId.empty() ? options_.defaultSkinId : settings_.skinId, 1);
    session.scene.setSpeech(speech_.scene(
        settings_.skinId.empty() ? options_.defaultSkinId : settings_.skinId, 1));
    publishStatus(session);

    const api::UiSceneFrame frame = session.scene.snapshot(nextFrame_++);
    ports_.scene.present(frame);

    if (ports_.nativePanel != nullptr) {
        if (frame.panel.visible) {
            api::NativePanel panel;
            panel.visible = true;
            panel.preedit = frame.panel.preedit;
            panel.page = frame.panel.candidates;
            panel.pageIndex = frame.panel.candidates.pageIndex;
            panel.pageCount = frame.panel.candidates.pageCount;
            panel.vertical = true;
            ports_.nativePanel->show(session.key, panel);
        } else {
            ports_.nativePanel->hide(session.key);
        }
    }
}

void Runtime::presentHidden(const api::SessionKey &key) {
    api::UiSceneFrame frame;
    frame.frameId = nextFrame_++;
    frame.session = key;
    frame.panel.visible = false;
    frame.status.visible = false;
    ports_.scene.present(frame);
    if (ports_.nativePanel != nullptr) {
        ports_.nativePanel->hide(key);
    }
}

void Runtime::hostUpdate(const api::SessionKey &key, const api::HostFacts &host) {
    Session *session = find(key);
    if (session == nullptr) {
        return;
    }
    session->host = host;
    present(*session);
}

void Runtime::command(const api::UiCommand &command) {
    std::visit(
        [this](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, api::ActivateCandidate>) {
                handleCandidate(value);
            } else if constexpr (std::is_same_v<T, api::PageCandidates>) {
                handlePage(value);
            } else if constexpr (std::is_same_v<T, api::SelectIndex>) {
                handleSelectIndex(value);
            } else if constexpr (std::is_same_v<T, api::SetInputMode>) {
                if (!settings_.shiftToggle) {
                    return;
                }
                if (Session *session = find(value.session)) {
                    setLanguage(*session, value.mode == api::InputMode::English, "ui command");
                }
            } else if constexpr (std::is_same_v<T, api::ToggleStatusItem>) {
                handleToggleStatus(value.itemId);
            } else if constexpr (std::is_same_v<T, api::SetSkin>) {
                settings_.skinId = value.skinId.empty() ? options_.defaultSkinId : value.skinId;
                for (auto &session : sessions_) {
                    present(*session);
                }
            } else if constexpr (std::is_same_v<T, api::SetUiScale>) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("scale", value.scale, 2)
                    << "ui scale reported by the UI";
            } else if constexpr (std::is_same_v<T, api::SpeechStart>) {
                handleSpeechStart(value.session);
            } else if constexpr (std::is_same_v<T, api::SpeechStop>) {
                handleSpeechStop(value.session, value.commit);
            } else if constexpr (std::is_same_v<T, api::SpeechCancel>) {
                handleSpeechCancel();
            } else if constexpr (std::is_same_v<T, api::SpeechReload>) {
                handleSpeechReload();
            } else if constexpr (std::is_same_v<T, api::MovePanel> ||
                                 std::is_same_v<T, api::ResetPanelPosition>) {
                // The drag offset is presentation state owned by the UI shell;
                // the runtime does not duplicate it.
            } else if constexpr (std::is_same_v<T, api::RequestSettings>) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("page", value.page)
                    << "settings page requested";
            } else if constexpr (std::is_same_v<T, api::ShellReady>) {
                SUPERIOR_INFO(diag::categories::Fcitx5)
                    .field("shell", value.shellName)
                    .field("version", value.shellVersion)
                    << "ui shell connected";
            } else if constexpr (std::is_same_v<T, api::ShellLost>) {
                SUPERIOR_WARN(diag::categories::Fcitx5)
                    .field("reason", value.reason)
                    << "ui shell went away";
                for (auto &session : sessions_) {
                    present(*session);
                }
            }
        },
        command);
}

void Runtime::handleCandidate(const api::ActivateCandidate &command) {
    if (command.session != active_) {
        // A click from a panel that is no longer the focused context is stale.
        return;
    }
    Session *session = find(command.session);
    if (session == nullptr || !session->composer) {
        return;
    }
    const auto position = session->scene.candidates().positionOf(command.candidate);
    if (!position) {
        return;
    }
    applyOutcome(*session, session->composer->select(*position, nowMs()),
                 core::WhenUnfocused::Drop);
    present(*session);
}

void Runtime::handlePage(const api::PageCandidates &command) {
    if (command.session != active_) {
        return;
    }
    Session *session = find(command.session);
    if (session == nullptr || !session->composer) {
        return;
    }
    if (session->composer->page(command.delta)) {
        present(*session);
    }
}

void Runtime::handleSelectIndex(const api::SelectIndex &command) {
    if (command.session != active_) {
        return;
    }
    Session *session = find(command.session);
    if (session == nullptr || !session->composer || command.index < 0) {
        return;
    }
    applyOutcome(*session,
                 session->composer->select(static_cast<std::size_t>(command.index), nowMs()),
                 core::WhenUnfocused::Drop);
    present(*session);
}

void Runtime::handleToggleStatus(const std::string &itemId) {
    if (itemId == "speech") {
        speech_.toggle();
        if (Session *session = find(active_)) {
            present(*session);
        }
        return;
    }
    Session *session = find(active_);
    if (itemId == "inline-mode") {
        if (!settings_.shiftToggle || session == nullptr) {
            return;
        }
        setLanguage(*session, !session->modes.english(), "status bar");
        return;
    }
    if (itemId == "punctuation") {
        if (session == nullptr) {
            return;
        }
        if (session->modes.togglePunctuation()) {
            applyModes(*session);
            present(*session);
        }
        return;
    }
    if (ports_.status != nullptr) {
        ports_.status->activate(itemId);
    }
}

void Runtime::handleSpeechStart(const api::SessionKey &key) {
    if (key != active_ || find(key) == nullptr || !speech_.canStart()) {
        return;
    }
    speech_.start();
    if (Session *session = find(key)) {
        present(*session);
    }
}

void Runtime::handleSpeechStop(const api::SessionKey &key, bool commit) {
    if (key != active_ || find(key) == nullptr) {
        return;
    }
    if (commit) {
        speech_.stop();
    } else {
        speech_.cancel("stop without commit");
    }
    if (Session *session = find(key)) {
        present(*session);
    }
}

void Runtime::handleSpeechCancel() {
    speech_.cancel("discard button");
    if (Session *session = find(active_)) {
        present(*session);
    }
}

void Runtime::handleSpeechReload() {
    reloadSettings();
    speech_.reload();
    if (Session *session = find(active_)) {
        present(*session);
    }
}

void Runtime::tick() {
    reloadSettings();
    const std::uint64_t now = nowMs();

    bool activeChanged = false;
    for (auto &session : sessions_) {
        // Cloud work is per composition and shared by one worker. Ticking a
        // background window would let it consume the debounce and the request
        // slot that the focused window needs; it resumes on the next tick after
        // focus returns.
        if (!session->composer || session->key != active_) {
            continue;
        }
        const api::CompositionFact before = session->composer->frame();
        session->composer->tick(now);
        const api::CompositionFact after = session->composer->frame();
        if (!sameComposition(before, after)) {
            activeChanged = true;
        }
    }
    speech_.tick();

    // Speech level/partial updates arrive at ~20 Hz and are not part of the
    // composition; while a session is live the panel needs every tick.
    if (Session *session = find(active_)) {
        bool indicatorExpired = false;
        if (session->scene.indicatorVisible() && now >= session->indicatorUntilMs) {
            session->indicatorUntilMs = 0;
            session->scene.hideIndicator();
            indicatorExpired = true;
        }
        if (activeChanged || speech_.busy() || indicatorExpired) {
            present(*session);
        }
    }
}

bool Runtime::composing(const api::SessionKey &key) const {
    const Session *session = find(key);
    return session != nullptr && session->composer && session->composer->frame().composing;
}

bool Runtime::english(const api::SessionKey &key) const {
    const Session *session = find(key);
    return session != nullptr && session->modes.english();
}

} // namespace superior::runtime
