// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/rime/RimeEngine.hpp"

#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::rime {

namespace diag = ::superior::diagnostics;

RimeEngine::RimeEngine(RimePort &port) : port_(port) {}

bool RimeEngine::start(const Paths &paths, std::string &error) {
    if (sessionOpen_) {
        return true;
    }
    if (!port_.initialize(paths, error)) {
        SUPERIOR_ERROR(diag::categories::Rime)
            .field("shared", paths.sharedData.string())
            .field("user", paths.userData.string())
            .field("error", error)
            << "librime initialization failed";
        return false;
    }

    if (!port_.createSession(error)) {
        SUPERIOR_ERROR(diag::categories::Rime).field("error", error) << "cannot open a session";
        port_.finalize();
        return false;
    }
    sessionOpen_ = true;
    needsDeploy_ = port_.schemaId().empty();

    // There is exactly one Chinese/English switch, and it lives in the
    // platform layer. Starting a session in ASCII mode would make typing look
    // broken with no visible cause.
    port_.setOption("ascii_mode", false);
    refreshAsciiMode();
    SUPERIOR_INFO(diag::categories::Rime)
        .field("schema", port_.schemaId())
        .field("needsDeploy", needsDeploy_)
        .field("shared", paths.sharedData.string())
        .field("user", paths.userData.string())
        << "rime engine ready";
    return true;
}

void RimeEngine::stop() {
    if (!sessionOpen_) {
        return;
    }
    port_.destroySession();
    port_.finalize();
    sessionOpen_ = false;
    SUPERIOR_INFO(diag::categories::Rime) << "rime engine stopped";
}

bool RimeEngine::deploy(std::string &error) {
    if (!sessionOpen_) {
        error = "engine is not running";
        return false;
    }
    // A deployment replaces every compiled artifact; the open session would keep
    // pointing at the old ones, so it is closed and reopened around the call.
    port_.destroySession();
    sessionOpen_ = false;

    const bool ok = port_.deploy(error);

    if (!sessionOpen_) {
        std::string reopenError;
        if (port_.createSession(reopenError)) {
            sessionOpen_ = true;
            needsDeploy_ = port_.schemaId().empty();
            refreshAsciiMode();
        } else if (ok) {
            // Deployment succeeded but the session could not come back; report it
            // as a failure so the caller does not silently lose input.
            error = reopenError;
            return false;
        }
    }
    return ok;
}

RimeEngine::KeyResult RimeEngine::processKey(const KeyInput &key) {
    KeyResult result;
    if (!sessionOpen_) {
        return result;
    }

    // librime expects X11 modifier bits plus its own release bit.
    std::uint32_t mask = key.states;
    if (key.release) {
        mask |= kReleaseMask;
    }
    result.consumed = port_.processKey(key.sym, mask);

    // Drain the commit once, whatever the consumed state: a key can both be
    // consumed and produce text (space selecting the highlighted candidate).
    std::string commit;
    if (port_.takeCommit(commit) && !commit.empty()) {
        result.commit = std::move(commit);
    }

    refreshAsciiMode();
    return result;
}

Snapshot RimeEngine::snapshot() {
    Snapshot snapshot;
    if (!sessionOpen_) {
        return snapshot;
    }
    snapshot.composition = port_.composition();
    snapshot.menu = port_.menu();
    snapshot.asciiMode = ascii_;
    return snapshot;
}

bool RimeEngine::selectCandidate(std::size_t index) {
    if (!sessionOpen_) {
        return false;
    }
    return port_.selectCandidate(index);
}

bool RimeEngine::page(bool backward) {
    if (!sessionOpen_) {
        return false;
    }
    return port_.changePage(backward);
}

bool RimeEngine::takeCommit(std::string &out) {
    out.clear();
    if (!sessionOpen_) {
        return false;
    }
    const bool drained = port_.takeCommit(out);
    refreshAsciiMode();
    return drained;
}

bool RimeEngine::setAsciiMode(bool ascii) {
    if (!sessionOpen_) {
        return false;
    }
    if (!port_.setOption("ascii_mode", ascii)) {
        return false;
    }
    ascii_ = ascii;
    return true;
}

bool RimeEngine::asciiMode() {
    refreshAsciiMode();
    return ascii_;
}

bool RimeEngine::setOption(std::string_view name, bool value) {
    if (!sessionOpen_) {
        return false;
    }
    return port_.setOption(name, value);
}

bool RimeEngine::getOption(std::string_view name) {
    if (!sessionOpen_) {
        return false;
    }
    return port_.getOption(name);
}

int RimeEngine::indexForId(std::uint32_t id, int count) noexcept {
    if (id == 0 || count <= 0) {
        return -1;
    }
    const auto index = static_cast<int>(id) - 1;
    if (index < 0 || index >= count) {
        return -1;
    }
    return index;
}

void RimeEngine::refreshAsciiMode() {
    if (!sessionOpen_) {
        ascii_ = false;
        return;
    }
    ascii_ = port_.asciiMode();
}

} // namespace superior::rime
