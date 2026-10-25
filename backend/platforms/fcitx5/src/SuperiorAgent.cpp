// SPDX-License-Identifier: GPL-3.0-or-later
#include "SuperiorAgent.h"

#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx/action.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "superior/diagnostics/Log.hpp"
#include "superior/config/Settings.hpp"
#include "superior/fcitx5/CandidateActions.hpp"
#include "superior/fcitx5/InlineText.hpp"
#include "superior/fcitx5/PanelMapper.hpp"
#include "superior/fcitx5/PreeditSource.hpp"
#include "superior/fcitx5/PinyinAssets.hpp"
#include "superior/protocol/Wire.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;

namespace {
constexpr int kMinPageSize = 1;
constexpr int kMaxPageSize = 32;

std::size_t byteCursor(const fcitx::Text &text) {
    const int cursor = text.cursor();
    if (cursor < 0) {
        return text.toString().size();
    }
    return static_cast<std::size_t>(cursor);
}
} // namespace

SuperiorAgent::SuperiorAgent(fcitx::AddonManager *manager)
    : manager_(manager), instance_(manager != nullptr ? manager->instance() : nullptr) {
    diag::configureFromEnvironment();

    if (instance_ == nullptr) {
        SUPERIOR_ERROR(diag::categories::Fcitx5)
            << "no Fcitx instance; agent disabled";
        return;
    }

    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("version", std::string_view("0.1.0"))
        .field("epoch", static_cast<std::uint64_t>(epoch_))
        << "superior agent starting (observer only)";

    // Read-only probe: never parses or modifies the engine dictionaries, only
    // reports whether the data Fcitx5 needs is present.
    const PinyinAssets assets = detectPinyinAssets();
    if (!assets.usable()) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            << "pinyin assets are incomplete; text input still works but "
               "candidates may be missing";
    }

    std::string error;
    const std::string socketPath = protocol::SceneServer::defaultSocketPath();
    if (!server_.listen(socketPath, error)) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("path", socketPath)
            .field("error", error)
            << "cannot listen for the UI; the overlay will not update";
    } else {
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("path", socketPath)
            << "UI socket ready";
    }
    server_.setCommandHandler([this](std::string bytes) { handleCommand(std::move(bytes)); });

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFlushUI, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto &flush = static_cast<fcitx::InputContextFlushUIEvent &>(event);
            if (flush.component() == fcitx::UserInterfaceComponent::InputPanel) {
                refresh(flush.inputContext());
            }
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusIn, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                << "input context focused";
            reloadSettings();
            ensureOverlay();
            if (!isOurs(inputContext)) {
                if (activeContext_ != nullptr) {
                    activeContext_ = nullptr;
                    broadcastHidden();
                }
                return;
            }
            refresh(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusOut, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                << "input context lost focus";
            if (!isOurs(inputContext)) {
                return;
            }
            if (auto *session = findSession(inputContext)) {
                session->hide();
                broadcast(inputContext);
            }
            // Drop physical key state: a Shift held while focus moves must not
            // make the next tap look like a modifier use.
            if (auto it = shiftTaps_.find(inputContext); it != shiftTaps_.end()) {
                it->second.reset();
            }
        }));

    // Installed as early as possible: the first InputPanel draw must already be
    // routed to us, otherwise Classic UI paints one frame that it never hides
    // afterwards (its own update path is then bypassed).
    reloadSettings();
    ensureOverlay();

    // Fcitx5 may already have a focused client when the addon loads (Fcitx5
    // restarted while an application kept focus). No FocusIn event will follow
    // in that case, so the toolbar would stay invisible until the user types.
    // Paint once from the most recent context after the UI had time to connect.
    startupTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 600000, 0,
        [this](fcitx::EventSourceTime *, std::uint64_t) {
            if (auto *inputContext = instance_->mostRecentInputContext();
                inputContext != nullptr && isOurs(inputContext)) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("program", inputContext->program())
                    << "initial paint from the most recent input context";
                refresh(inputContext);
            }
            return false;
        });

    // Poll the settings file so a switch in the settings window takes effect
    // without restarting Fcitx5 or refocusing a window.
    settingsTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000, 0,
        [this](fcitx::EventSourceTime *source, std::uint64_t) {
            reloadSettings();
            source->setNextInterval(500000);
            source->setOneShot();
            return true;
        });

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::PreInputMethod,
        [this](fcitx::Event &event) {
            static_cast<void>(handleKeyEvent(static_cast<fcitx::KeyEvent &>(event)));
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextInputMethodActivated,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            syncPanelOwnership(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextInputMethodDeactivated,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            inputContext->inputPanel().setCustomInputPanelCallback({});
            releasePreeditOwnership(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextCursorRectChanged,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            const auto &rect = inputContext->cursorRect();
            SUPERIOR_TRACE(diag::categories::Fcitx5)
                .field("x", static_cast<std::int64_t>(rect.left()))
                .field("y", static_cast<std::int64_t>(rect.top()))
                .field("w", static_cast<std::int64_t>(rect.width()))
                .field("h", static_cast<std::int64_t>(rect.height()))
                << "cursor rect changed";
            refresh(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextSwitchInputMethod,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                << "input method switched";
            syncPanelOwnership(inputContext);
            refresh(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextDestroyed, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5) << "input context destroyed";
            inputContext->inputPanel().setCustomInputPanelCallback({});
            // The context is going away; dropping the entry is enough, its
            // capability flags die with it.
            preeditOwnership_.erase(inputContext);
            shiftTaps_.erase(inputContext);
            if (activeContext_ == inputContext) {
                activeContext_ = nullptr;
            }
            forget(inputContext);
        }));
}

SuperiorAgent::~SuperiorAgent() {
    settingsTimer_.reset();
    startupTimer_.reset();
    reaperStop_.store(true, std::memory_order_relaxed);
    for (auto &reaper : reapers_) {
        if (reaper.joinable()) {
            reaper.join();
        }
    }
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
        << "superior agent stopping";
    handlers_.clear();
    server_.close();
}

core::ImeSession *SuperiorAgent::findSession(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    return it == sessions_.end() ? nullptr : it->second.get();
}

core::ImeSession &SuperiorAgent::sessionFor(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    if (it != sessions_.end()) {
        return *it->second;
    }
    auto session = std::make_unique<core::ImeSession>(api::SessionKey{epoch_, nextContext_++});
    core::ImeSession &reference = *session;
    sessions_.emplace(inputContext, std::move(session));
    return reference;
}

PanelFacts SuperiorAgent::collect(fcitx::InputContext *inputContext) {
    PanelFacts facts;
    facts.program = inputContext->program();
    facts.scale = inputContext->scaleFactor();
    if (const auto *entry = instance_->inputMethodEntry(inputContext)) {
        facts.inputMethod = entry->uniqueName();
    }

    const auto &cursor = inputContext->cursorRect();
    facts.cursorRect = api::Rect{static_cast<double>(cursor.left()),
                                 static_cast<double>(cursor.top()),
                                 static_cast<double>(cursor.width()),
                                 static_cast<double>(cursor.height())};
    facts.cursorValid = inputContext->hasFocus();

    const auto &panel = inputContext->inputPanel();
    // The composition row is drawn by us, but which field carries the text
    // depends on the engine and on the client's capabilities: pinyin writes
    // clientPreedit for "show actual pinyin" mode, which the application would
    // render inline in its own style. Taking the panel preedit first and
    // falling back to the client preedit keeps one look everywhere.
    const auto &panelPreedit = panel.preedit();
    const auto &clientPreedit = panel.clientPreedit();
    const PreeditView preedit = choosePreedit(panelPreedit.toString(), byteCursor(panelPreedit),
                                              clientPreedit.toString(),
                                              byteCursor(clientPreedit));
    facts.preedit = preedit.text;
    facts.preeditCursor = preedit.cursor;
    facts.preeditFromClient = preedit.fromClient;

    if (auto candidateList = panel.candidateList()) {
        const int size = candidateList->size();
        facts.candidates.reserve(static_cast<std::size_t>(std::max(0, size)));
        for (int index = 0; index < size; ++index) {
            const auto &word = candidateList->candidate(index);
            CandidateEntry entry;
            entry.id = static_cast<api::CandidateId>(index + 1);
            entry.label = candidateList->label(index).toString();
            entry.text = word.text().toString();
            entry.comment = word.comment().toString();
            facts.candidates.push_back(std::move(entry));
        }
        facts.cursorIndex = candidateList->cursorIndex();
        facts.vertical = candidateList->layoutHint() == fcitx::CandidateLayoutHint::Vertical;

        if (auto *pageable = candidateList->toPageable()) {
            // `size()` is the number of candidates in the *current* page, not
            // the total. Deriving a page size from it produced one item per
            // page; the page itself is already the slice we should display.
            if (size > 0) {
                facts.pageSize = std::clamp(size, kMinPageSize, kMaxPageSize);
            }
            facts.pageCount = pageable->totalPages();
            const int current = pageable->currentPage();
            facts.pageIndex = current >= 0 ? current + 1 : 1;
        }
    }

    for (auto *action : inputContext->statusArea().allActions()) {
        if (action == nullptr || action->name().empty()) {
            continue;
        }
        api::StatusItem item;
        item.id = action->name();
        item.label = action->shortText(inputContext);
        item.icon = action->icon(inputContext);
        item.active = action->isChecked(inputContext);
        facts.statusItems.push_back(std::move(item));
    }

    const bool english = englishMode_;
    api::StatusItem modeItem;
    modeItem.id = "inline-mode";
    modeItem.label = english ? "英" : "中";
    modeItem.active = english;
    facts.statusItems.insert(facts.statusItems.begin(), std::move(modeItem));

    facts.composing = !facts.preedit.empty() || !facts.candidates.empty();

    // Never log the composition itself: log shape only.
    SUPERIOR_TRACE(diag::categories::Fcitx5)
        .field("program", facts.program)
        .field("im", facts.inputMethod)
        .redacted("preedit", facts.preedit)
        .field("preeditSource", facts.preeditFromClient ? "client" : "panel")
        .field("candidates", static_cast<std::uint64_t>(facts.candidates.size()))
        .field("pageSize", static_cast<std::int64_t>(facts.pageSize))
        .field("cursorIndex", static_cast<std::int64_t>(facts.cursorIndex))
        .field("vertical", facts.vertical)
        .field("statusItems", static_cast<std::uint64_t>(facts.statusItems.size()))
        .field("scale", facts.scale, 2)
        << "collected panel facts";
    return facts;
}

void SuperiorAgent::reloadSettings() {
    const std::filesystem::path path = config::defaultSettingsPath();
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    if (settingsLoaded_ && (error || stamp == settingsStamp_)) {
        return;
    }
    settingsStamp_ = stamp;
    settingsLoaded_ = true;

    const auto loaded = config::loadSettings(path);
    for (const auto &warning : loaded.warnings) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("settings", path.string())
            .field("warning", warning)
            << "settings rejected part of the file";
    }

    const bool previousInlineEnglish = inlineEnglishEnabled_;
    inlineEnglishEnabled_ = loaded.settings.shiftToggle;
    if (!loaded.settings.skinId.empty()) {
        skinId_ = loaded.settings.skinId;
    }
    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("inlineEnglish", inlineEnglishEnabled_)
        .field("settings", path.string())
        << "settings loaded";

    if (previousInlineEnglish != inlineEnglishEnabled_) {
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("inlineEnglish", inlineEnglishEnabled_)
            << "inline English setting changed";
    }

    // Turning the feature off must also leave English mode; otherwise letters
    // would keep being committed directly even though the user disabled it.
    if (!inlineEnglishEnabled_ && englishMode_) {
        setEnglishMode(activeContext_, false, "settings");
    }
}

void SuperiorAgent::setEnglishMode(fcitx::InputContext *inputContext, bool english,
                                   const char *reason) {
    if (englishMode_ == english) {
        return;
    }
    englishMode_ = english;

    if (english && inputContext != nullptr && instance_ != nullptr) {
        // Drop the half-finished pinyin composition, otherwise its stale
        // candidate list would keep the panel visible while English letters
        // are committed directly.
        instance_->resetCompose(inputContext);
        inputContext->inputPanel().reset();
        inputContext->updatePreedit();
    }

    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("english", englishMode_)
        .field("reason", std::string(reason))
        .field("program", inputContext != nullptr ? inputContext->program() : std::string{})
        << "inline input mode changed";

    if (inputContext != nullptr) {
        refresh(inputContext);
    }
}

void SuperiorAgent::ensureOverlay() {
    if (server_.clientCount() > 0) {
        overlaySpawned_ = true;
        return;
    }

    if (const char *disabled = std::getenv("SUPERIOR_UI_AUTOSTART");
        disabled != nullptr && std::string_view(disabled) == "0") {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (overlaySpawned_ && now - lastOverlayAttempt_ < std::chrono::seconds(3)) {
        return;
    }
    lastOverlayAttempt_ = now;

    // Order matters: the per-user install keeps the executable under
    // ~/.local/share/superior-ime/bin (only a tiny wrapper sits in
    // ~/.local/bin), a system install puts it in /usr/bin, and the PATH lookup
    // is the last resort for hand-built trees.
    std::vector<std::string> candidates;
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        candidates.push_back(std::string(home) + "/.local/share/superior-ime/bin/superior_ui");
        candidates.push_back(std::string(home) + "/.local/bin/superior_ui");
    }
    candidates.emplace_back("/usr/bin/superior_ui");
    candidates.emplace_back("superior_ui");

    for (const auto &binary : candidates) {
        pid_t pid = -1;
        std::string program = binary;
        std::vector<char *> argv{program.data(), nullptr};
        if (::posix_spawnp(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) == 0) {
            overlaySpawned_ = true;
            // Reap the child so a short-lived overlay does not stay a zombie
            // for the lifetime of Fcitx5.
            std::thread([pid] {
                int status = 0;
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }).detach();
            SUPERIOR_INFO(diag::categories::Fcitx5)
                .field("binary", binary)
                << "started the Superior overlay";
            return;
        }
    }
    SUPERIOR_WARN(diag::categories::Fcitx5)
        << "cannot start the overlay; run superior_ui manually or set SUPERIOR_UI_AUTOSTART=0";
}

void SuperiorAgent::takePreeditOwnership(fcitx::InputContext *inputContext) {
    PreeditOwnership &state = preeditOwnership_[inputContext];
    if (state.owned) {
        return;
    }
    const auto flags = inputContext->capabilityFlags();
    // Remember only whether the client could display preedit at all, so
    // releasing restores exactly what the frontend announced instead of
    // clobbering flags someone else changed in the meantime (Password, Disable).
    state.clientShowedPreedit = flags.test(fcitx::CapabilityFlag::Preedit);
    if (!state.clientShowedPreedit) {
        // Nothing to take away: the client never drew inline preedit, so the
        // engine already fills the panel preedit.
        state.owned = true;
        return;
    }

    // Dropping the capability makes engines put the composition into
    // `InputPanel::preedit` (which we draw) instead of `clientPreedit` (which
    // the application draws inline in its own font and colour). This is how the
    // Sogou Linux build behaves, and it is why the panel looked different in
    // VSCode: VSCode renders client preedit itself.
    inputContext->setCapabilityFlags(flags.unset(fcitx::CapabilityFlag::Preedit));
    state.owned = true;
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("program", inputContext->program())
        .field("im", instance_->inputMethod(inputContext))
        << "drawing the composition ourselves; client preedit disabled for this context";
}

void SuperiorAgent::releasePreeditOwnership(fcitx::InputContext *inputContext) {
    const auto it = preeditOwnership_.find(inputContext);
    if (it == preeditOwnership_.end()) {
        return;
    }
    if (it->second.owned && it->second.clientShowedPreedit) {
        // Add the capability back rather than restoring a snapshot: other flags
        // may have changed while we owned the panel.
        inputContext->setCapabilityFlags(inputContext->capabilityFlags() |
                                         fcitx::CapabilityFlag::Preedit);
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("program", inputContext->program())
            << "releasing the composition back to the client";
    }
    preeditOwnership_.erase(it);
}

void SuperiorAgent::syncPanelOwnership(fcitx::InputContext *inputContext) {
    if (instance_ == nullptr || inputContext == nullptr) {
        return;
    }

    const std::string inputMethod = instance_->inputMethod(inputContext);
    const bool ours = inputMethod == "super-ime";
    const bool hasUi = server_.clientCount() > 0;
    auto &panel = inputContext->inputPanel();

    if (!ours) {
        // Another input method took over this context: hand the composition
        // back so Rime & friends keep their own client-side preedit.
        releasePreeditOwnership(inputContext);
        if (panel.customInputPanelCallback()) {
            panel.setCustomInputPanelCallback({});
            SUPERIOR_INFO(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                .field("im", inputMethod)
                << "releasing the input panel back to Fcitx5's UI";
        }
        return;
    }

    if (!hasUi) {
        // No overlay to draw for us: fall back to Classic UI instead of leaving
        // the user with no candidates at all, and hand the composition back to
        // the client so nothing disappears.
        releasePreeditOwnership(inputContext);
        if (panel.customInputPanelCallback()) {
            panel.setCustomInputPanelCallback({});
            inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return;
    }

    takePreeditOwnership(inputContext);
    if (!panel.customInputPanelCallback()) {
        // Replaces Classic UI for this context only. Fcitx5 still routes
        // update batching through the same path, so the callback simply
        // forwards the latest state to our UI process.
        panel.setCustomInputPanelCallback(
            [this](fcitx::InputContext *context) { refresh(context); });
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("program", inputContext->program())
            .field("im", inputMethod)
            << "taking over the input panel for this context";
    }
}

bool SuperiorAgent::handleKeyEvent(fcitx::KeyEvent &event) {
    if (instance_ == nullptr) {
        return false;
    }
    auto *inputContext = event.inputContext();
    if (inputContext == nullptr || instance_->inputMethod(inputContext) != "super-ime") {
        return false;
    }

    // Take over (or release) the panel before the engine produces anything for
    // this key, so the very first candidate frame already belongs to us.
    syncPanelOwnership(inputContext);

    ShiftTapDetector &shiftTap = shiftTaps_[inputContext];
    const fcitx::KeySym sym = event.key().sym();
    const bool release = event.isRelease();
    const auto now = std::chrono::steady_clock::now();

    if (sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R) {
        const auto shiftKey = sym == FcitxKey_Shift_L ? ShiftTapDetector::ShiftKey::Left
                                                      : ShiftTapDetector::ShiftKey::Right;
        const auto result =
            release ? shiftTap.shiftReleased(shiftKey, now) : shiftTap.shiftPressed(shiftKey, now);
        if (result == ShiftTapDetector::Result::Toggle) {
            if (inlineEnglishEnabled_) {
                setEnglishMode(inputContext, !englishMode_, "shift tap");
            } else {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("program", inputContext->program())
                    << "shift tap ignored: inline English is disabled in settings";
            }
        }
        // Never filter Shift: the application still needs modifier state for
        // selection and shortcuts. A tap only toggles our inline mode.
        return false;
    }

    if (!release) {
        // Any real key while Shift is held means "modifier", not "tap".
        shiftTap.otherKeyPressed();
    }

    if (release || !englishMode_) {
        return false;
    }

    // Shortcuts must reach the application: only plain typing (optionally with
    // Shift for capitals) is turned into committed text.
    const auto states = event.key().states();
    const bool plainTyping = !states.test(fcitx::KeyState::Ctrl) &&
                             !states.test(fcitx::KeyState::Alt) &&
                             !states.test(fcitx::KeyState::Super);
    if (!plainTyping) {
        return false;
    }

    if (sym == FcitxKey_Escape) {
        setEnglishMode(inputContext, false, "escape");
        event.filterAndAccept();
        return true;
    }
    // Editing and navigation keys are never consumed in English mode.
    //
    // Inline text is committed straight to the client, so the client already
    // owns the characters: its own Backspace/Delete/arrow handling is correct,
    // and forwarding the key keeps undo, selection and IME-less widgets intact.
    // Deleting here via deleteSurroundingText broke Backspace on every client
    // that does not implement surrounding text (most of them), because the key
    // was consumed and nothing was removed.
    if (sym == FcitxKey_BackSpace || sym == FcitxKey_Delete) {
        return false;
    }
    if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter || sym == FcitxKey_Tab) {
        // Leave navigation keys to the application.
        return false;
    }

    if (const auto text = inlineTextFor(static_cast<std::uint32_t>(sym),
                                        event.key().states().test(fcitx::KeyState::Shift));
        text && !text->empty()) {
        inputContext->commitString(*text);
        event.filterAndAccept();
        return true;
    }
    return false;
}

void SuperiorAgent::refresh(fcitx::InputContext *inputContext) {
    if (instance_ == nullptr || inputContext == nullptr || refreshing_) {
        return;
    }

    // Never touch contexts that belong to another input method. Rime and the
    // rest keep their own candidates and their own Fcitx5 theme.
    if (!isOurs(inputContext)) {
        if (activeContext_ != nullptr) {
            activeContext_ = nullptr;
            broadcastHidden();
        }
        return;
    }
    activeContext_ = inputContext;

    refreshing_ = true;
    syncPanelOwnership(inputContext);

    core::ImeSession &session = sessionFor(inputContext);
    PanelMapper::apply(collect(inputContext), session, skinId_);

    if (server_.clientCount() > 0) {
        broadcast(inputContext);
    } else {
        // Nothing to draw with: hand the panel back to Fcitx5 instead of
        // leaving the user without candidates.
        inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }
    refreshing_ = false;
}

bool SuperiorAgent::isOurs(fcitx::InputContext *inputContext) const {
    return instance_ != nullptr && inputContext != nullptr &&
           instance_->inputMethod(inputContext) == "super-ime";
}

void SuperiorAgent::broadcastHidden() {
    if (server_.clientCount() == 0) {
        return;
    }
    api::UiSceneFrame frame;
    frame.frameId = nextFrame_++;
    frame.session = api::SessionKey{epoch_, 0};
    frame.panel.visible = false;
    frame.status.visible = false;
    server_.broadcast(protocol::encodeFrame(protocol::MessageType::Scene,
                                            protocol::encodeScene(frame)));
    SUPERIOR_DEBUG(diag::categories::Fcitx5) << "hiding our windows: another input method is active";
}

void SuperiorAgent::broadcast(fcitx::InputContext *inputContext) {
    const std::size_t clients = server_.clientCount();
    if (clients == 0) {
        SUPERIOR_TRACE(diag::categories::Fcitx5)
            .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
            << "no UI connected, frame dropped";
        return;
    }
    auto *session = findSession(inputContext);
    if (session == nullptr) {
        return;
    }
    const api::UiSceneFrame frame = session->snapshot(nextFrame_++);
    const std::string payload = protocol::encodeScene(frame);
    SUPERIOR_TRACE(diag::categories::Fcitx5)
        .field("frame", static_cast<std::uint64_t>(frame.frameId))
        .field("clients", static_cast<std::uint64_t>(clients))
        .field("bytes", static_cast<std::uint64_t>(payload.size()))
        .field("visible", frame.panel.visible)
        << "broadcasting scene";
    server_.broadcast(protocol::encodeFrame(protocol::MessageType::Scene, payload));
}

void SuperiorAgent::forget(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    if (it == sessions_.end()) {
        return;
    }
    it->second->hide();
    it->second->setStatus({});
    broadcast(inputContext);
    sessions_.erase(it);
    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
        << "session forgotten";
}

void SuperiorAgent::handleCommand(std::string bytes) {
    std::size_t consumed = 0;
    protocol::WireMessage message;
    protocol::DecodeError error;
    if (protocol::tryDecodeFrame(bytes, consumed, message, error) !=
        protocol::FrameStatus::Complete) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("code", error.code)
            .field("detail", error.detail)
            .field("bytes", static_cast<std::uint64_t>(bytes.size()))
            << "malformed frame from UI";
        return;
    }
    if (message.type != protocol::MessageType::Command) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("type", std::string(protocol::messageTypeName(message.type)))
            << "ignoring non-command message";
        return;
    }
    const auto command = protocol::decodeCommand(message.payload, error);
    if (!command) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("code", error.code)
            .field("detail", error.detail)
            << "malformed command from UI";
        return;
    }
    applyCommand(*command);
}

void SuperiorAgent::applyCommand(const api::UiCommand &command) {
    // Commands are semantic: the UI never sends geometry or engine internals.
    // Anything not listed here is ignored on purpose, so an older UI cannot
    // corrupt a newer agent.
    const auto handle = [this](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, api::ToggleStatusItem>) {
            if (value.itemId != "inline-mode") {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("item", value.itemId)
                    << "status item click ignored";
                return;
            }
            if (!inlineEnglishEnabled_) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    << "status bar toggle ignored: inline English is disabled in settings";
                return;
            }
            setEnglishMode(activeContext_, !englishMode_, "status bar");
        } else if constexpr (std::is_same_v<T, api::SetInputMode>) {
            if (!inlineEnglishEnabled_) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    << "set input mode ignored: inline English is disabled in settings";
                return;
            }
            setEnglishMode(activeContext_, value.mode == api::InputMode::English, "ui command");
        } else if constexpr (std::is_same_v<T, api::ActivateCandidate>) {
            activateCandidate(value);
        } else if constexpr (std::is_same_v<T, api::PageCandidates>) {
            pageCandidates(value);
        } else if constexpr (std::is_same_v<T, api::SetUiScale>) {
            // Scaling is applied by the UI process, which also owns the
            // settings file. Logged so a wrong value is visible in a trace.
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("scale", value.scale, 2)
                << "ui scale reported by the UI";
        } else {
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                << "command not handled by the agent";
        }
    };
    std::visit(handle, command);
}

void SuperiorAgent::activateCandidate(const api::ActivateCandidate &command) {
    auto *inputContext = contextFor(command.session);
    if (inputContext == nullptr) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("candidate", static_cast<std::int64_t>(command.candidate))
            << "candidate activation ignored: session is no longer active";
        return;
    }

    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        // The list changed between painting and clicking. Dropping the click is
        // correct: guessing would commit a word the user never saw.
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("candidate", static_cast<std::int64_t>(command.candidate))
            << "candidate activation ignored: no candidate list";
        return;
    }

    const auto index = candidateIndexFor(command.candidate, candidateList->size());
    if (!index) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("candidate", static_cast<std::int64_t>(command.candidate))
            .field("size", static_cast<std::int64_t>(candidateList->size()))
            << "candidate activation ignored: id outside the current page";
        return;
    }

    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("candidate", static_cast<std::int64_t>(command.candidate))
        .field("index", static_cast<std::int64_t>(*index))
        .field("program", inputContext->program())
        << "candidate activated from the UI";
    candidateList->candidate(*index).select(inputContext);
    refresh(inputContext);
}

void SuperiorAgent::pageCandidates(const api::PageCandidates &command) {
    auto *inputContext = contextFor(command.session);
    if (inputContext == nullptr) {
        return;
    }
    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    auto *pageable = candidateList->toPageable();
    if (pageable == nullptr) {
        // A single-page list: scrolling must do nothing, not wrap silently.
        return;
    }

    // One wheel notch is one page, matching every other paging key
    // (Tab/Shift+Tab, +/-) rather than scrolling a single candidate.
    bool moved = false;
    if (command.delta > 0) {
        if (pageable->hasNext()) {
            pageable->next();
            moved = true;
        }
    } else if (command.delta < 0) {
        if (pageable->hasPrev()) {
            pageable->prev();
            moved = true;
        }
    }
    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("delta", static_cast<std::int64_t>(command.delta))
        .field("moved", moved)
        .field("program", inputContext->program())
        << "candidate page requested";
    if (moved) {
        refresh(inputContext);
    }
}

fcitx::InputContext *SuperiorAgent::contextFor(const api::SessionKey &session) {
    // Only the session the UI is currently being fed for may act: a click on a
    // window that already lost focus must not type into the new one.
    if (activeContext_ == nullptr || session.epoch != epoch_) {
        return nullptr;
    }
    const auto it = sessions_.find(activeContext_);
    if (it == sessions_.end() || session.context != it->second->key().context) {
        return nullptr;
    }
    return activeContext_;
}

} // namespace superior::fcitx5

namespace {

class SuperiorAgentFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new superior::fcitx5::SuperiorAgent(manager);
    }
};

} // namespace

FCITX_ADDON_FACTORY_V2(superioragent, SuperiorAgentFactory);
