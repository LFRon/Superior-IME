// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/LibrimePort.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rime_api.h"
#include "superior/pinyin/Layout.hpp"

namespace superior::pinyin {

namespace {

/// librime is loaded at runtime, not linked.
///
/// Two consequences, both wanted:
/// - the addon still loads on a machine without librime, so the UI and the
///   diagnostic path keep working instead of the whole input method failing;
/// - `backend/rime` has **no build-time dependency** on librime, so it stays
///   testable and portable.
constexpr const char *kCandidates[] = {"librime.so.1", "librime.so", "librime.dylib"};

using GetApiFn = RimeApi *(*)();

/// Process-wide state of librime.
///
/// `setup`/`initialize` are global, not per session, but the Fcitx5 addon opens
/// one session per input context. Reference counting makes the first port
/// initialize the library and the last one finalize it, so closing a window
/// never pulls the schema out from under the other windows.
struct LibraryState {
    std::mutex mutex;
    int references = 0;
    /// librime keeps the pointers from `RimeTraits`; these strings must outlive
    /// the library, not just the `initialize` call.
    std::string appName;
    std::string sharedData;
    std::string userData;
};

LibraryState &libraryState() {
    static LibraryState state;
    return state;
}

/// Converts a UTF-8 character offset (what librime reports) to a byte offset
/// (what our scene contract uses).
std::size_t charOffsetToByte(std::string_view text, int charOffset) {
    if (charOffset <= 0) {
        return 0;
    }
    std::size_t bytes = 0;
    int chars = 0;
    while (bytes < text.size() && chars < charOffset) {
        const auto lead = static_cast<unsigned char>(text[bytes]);
        if ((lead & 0xC0U) != 0x80U) {
            ++chars;
        }
        ++bytes;
    }
    return bytes;
}

} // namespace

struct LibrimePort::Impl {
    void *library = nullptr;
    RimeApi *api = nullptr;
    RimeSessionId session = 0;
    /// True while this port holds one reference on the process-wide library.
    bool initialized = false;
    std::string loadError;
};

LibrimePort::LibrimePort() : impl_(std::make_unique<Impl>()) {
    for (const char *name : kCandidates) {
        impl_->library = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (impl_->library != nullptr) {
            break;
        }
    }
    if (impl_->library == nullptr) {
        const char *error = ::dlerror();
        impl_->loadError = error != nullptr ? error : "cannot load librime";
        return;
    }

    auto *symbol = ::dlsym(impl_->library, "rime_get_api");
    if (symbol == nullptr) {
        impl_->loadError = "librime has no rime_get_api symbol";
        ::dlclose(impl_->library);
        impl_->library = nullptr;
        return;
    }
    // The C API is a versioned function table: the returned pointer is stable for
    // the lifetime of the library, so it is cached here.
    impl_->api = reinterpret_cast<GetApiFn>(symbol)();
    if (impl_->api == nullptr) {
        impl_->loadError = "rime_get_api returned null";
    }
}

LibrimePort::~LibrimePort() {
    destroySession();
    finalize();
    if (impl_ != nullptr && impl_->library != nullptr) {
        ::dlclose(impl_->library);
        impl_->library = nullptr;
    }
}

bool LibrimePort::available() const noexcept {
    return impl_ != nullptr && impl_->api != nullptr;
}

std::string LibrimePort::unavailableReason() const {
    return impl_ != nullptr ? impl_->loadError : std::string("not initialized");
}

std::string LibrimePort::libraryVersion() const {
    if (!available() || impl_->api->get_version == nullptr) {
        return {};
    }
    return impl_->api->get_version();
}

bool LibrimePort::initialize(const Paths &paths, std::string &error) {
    if (!available()) {
        error = unavailableReason();
        return false;
    }
    if (impl_->initialized) {
        return true;
    }
    if (paths.sharedData.empty() || paths.userData.empty()) {
        error = "rime data directories are not resolved (HOME is unset)";
        return false;
    }

    LibraryState &state = libraryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.references == 0) {
        state.appName = paths.appName;
        state.sharedData = paths.sharedData.string();
        state.userData = paths.userData.string();

        RimeTraits traits{};
        RIME_STRUCT_INIT(RimeTraits, traits);
        traits.app_name = state.appName.c_str();
        traits.shared_data_dir = state.sharedData.c_str();
        traits.user_data_dir = state.userData.c_str();
        // Keep librime's own logging quiet: its output goes to a file we do not
        // manage, and our diagnostics already cover what matters.
        traits.min_log_level = 2;

        if (impl_->api->setup != nullptr) {
            impl_->api->setup(&traits);
        }
        impl_->api->initialize(&traits);
    }
    ++state.references;
    impl_->initialized = true;
    return true;
}

void LibrimePort::finalize() {
    if (impl_ == nullptr || impl_->api == nullptr || !impl_->initialized) {
        return;
    }

    LibraryState &state = libraryState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.references > 0) {
        --state.references;
    }
    if (state.references == 0 && impl_->api->finalize != nullptr) {
        impl_->api->finalize();
    }
    impl_->initialized = false;
}

bool LibrimePort::deploy(std::string &error) {
    if (!available() || !impl_->initialized) {
        error = available() ? "rime is not initialized" : unavailableReason();
        return false;
    }
    const RimeApi *api = impl_->api;
    if (api->start_maintenance == nullptr || api->join_maintenance_thread == nullptr) {
        error = "this librime build cannot deploy";
        return false;
    }

    // Full check: the first run has no artifacts at all, and an incremental
    // check would skip the schemas that were never built.
    if (!api->start_maintenance(/*full_check=*/True)) {
        error = "start_maintenance failed";
        return false;
    }
    // Blocks until every schema is compiled. Deployment can take minutes on the
    // first run; the caller is responsible for not doing this on the UI thread.
    api->join_maintenance_thread();
    return true;
}

bool LibrimePort::createSession(std::string &error) {
    if (!available() || !impl_->initialized) {
        error = available() ? "rime is not initialized" : unavailableReason();
        return false;
    }
    if (impl_->session != 0) {
        return true;
    }
    impl_->session = impl_->api->create_session();
    if (impl_->session == 0) {
        error = "create_session failed (no schema deployed?)";
        return false;
    }
    return true;
}

void LibrimePort::destroySession() {
    if (impl_ == nullptr || impl_->api == nullptr || impl_->session == 0) {
        return;
    }
    impl_->api->destroy_session(impl_->session);
    impl_->session = 0;
}

bool LibrimePort::processKey(std::uint32_t sym, std::uint32_t mask) {
    if (!available() || impl_->session == 0) {
        return false;
    }
    return impl_->api->process_key(
               impl_->session, static_cast<int>(sym), static_cast<int>(mask)) != 0;
}

bool LibrimePort::takeCommit(std::string &out) {
    out.clear();
    if (!available() || impl_->session == 0) {
        return false;
    }
    RimeCommit commit{};
    RIME_STRUCT_INIT(RimeCommit, commit);
    if (!impl_->api->get_commit(impl_->session, &commit)) {
        return false;
    }
    if (commit.text != nullptr) {
        out = commit.text;
    }
    impl_->api->free_commit(&commit);
    return !out.empty();
}

Composition LibrimePort::composition() {
    Composition result;
    if (!available() || impl_->session == 0) {
        return result;
    }
    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (!impl_->api->get_context(impl_->session, &context)) {
        return result;
    }
    if (context.composition.preedit != nullptr) {
        result.preedit = context.composition.preedit;
    }
    result.cursor = charOffsetToByte(result.preedit, context.composition.cursor_pos);
    impl_->api->free_context(&context);
    return result;
}

Menu LibrimePort::menu() {
    Menu result;
    if (!available() || impl_->session == 0) {
        return result;
    }
    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (!impl_->api->get_context(impl_->session, &context)) {
        return result;
    }

    const RimeMenu &menu = context.menu;
    // librime is 0-based, the contract is 1-based.
    result.pageIndex = menu.page_no + 1;
    result.highlighted = menu.highlighted_candidate_index;
    result.hasNext = menu.is_last_page == 0;
    result.hasPrevious = menu.page_no > 0;
    // `RimeMenu` does not report a total page count, only "is this the last one".
    // Inventing a number would make the UI show "3/3" on a list of ten pages, so
    // an unknown count is reported as 0 and the UI simply omits the indicator.
    result.pageCount = menu.is_last_page != 0 ? result.pageIndex : 0;

    result.candidates.reserve(static_cast<std::size_t>(std::max(0, menu.num_candidates)));
    for (int index = 0; index < menu.num_candidates; ++index) {
        const RimeCandidate &candidate = menu.candidates[index];
        Candidate entry;
        if (candidate.text != nullptr) {
            entry.text = candidate.text;
        }
        if (candidate.comment != nullptr) {
            entry.comment = candidate.comment;
        }
        result.candidates.push_back(std::move(entry));
    }
    impl_->api->free_context(&context);
    return result;
}

bool LibrimePort::selectCandidate(std::size_t index) {
    if (!available() || impl_->session == 0) {
        return false;
    }
    return impl_->api->select_candidate(impl_->session, index) != 0;
}

bool LibrimePort::changePage(bool backward) {
    if (!available() || impl_->session == 0) {
        return false;
    }
    // `change_page` from the C API is a no-op in librime 1.17: it returns false
    // and the menu does not move. The engine's own Page_Up/Page_Down handling
    // works, and going through it also honours schema key bindings, so the wheel
    // synthesizes the same key the keyboard would send.
    constexpr std::uint32_t kPageUp = 0xFF55U;
    constexpr std::uint32_t kPageDown = 0xFF56U;
    return processKey(backward ? kPageUp : kPageDown, 0);
}

bool LibrimePort::setOption(std::string_view name, bool value) {
    if (!available() || impl_->session == 0 || impl_->api->set_option == nullptr) {
        return false;
    }
    const std::string key(name);
    // `set_option` returns void in the C API: it either applies or logs.
    impl_->api->set_option(impl_->session, key.c_str(), value ? True : False);
    return true;
}

bool LibrimePort::getOption(std::string_view name) {
    if (!available() || impl_->session == 0 || impl_->api->get_option == nullptr) {
        return false;
    }
    const std::string key(name);
    return impl_->api->get_option(impl_->session, key.c_str()) != 0;
}

bool LibrimePort::asciiMode() {
    return getOption("ascii_mode");
}

std::string LibrimePort::schemaId() {
    if (!available() || impl_->session == 0) {
        return {};
    }
    RimeStatus status{};
    RIME_STRUCT_INIT(RimeStatus, status);
    if (!impl_->api->get_status(impl_->session, &status)) {
        return {};
    }
    std::string id;
    if (status.schema_id != nullptr) {
        id = status.schema_id;
    }
    impl_->api->free_status(&status);
    return id;
}

} // namespace superior::pinyin
