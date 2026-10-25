// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "superior/pinyin/Types.hpp"

namespace superior::pinyin {

/// Everything `RimeEngine` needs from librime, as an interface.
///
/// The engine logic is written against this port instead of calling `RimeApi`
/// directly, for two reasons:
/// - the whole state machine (key handling, commit draining, pagination,
///   deployment bookkeeping) can be unit tested with a fake port, without a
///   deployed schema or even librime installed;
/// - a second backend (for Windows TSF, or a bundled librime with a different
///   API layout) only has to implement this interface.
class RimePort {
public:
    virtual ~RimePort() = default;

    /// Sets `user_data_dir` / `shared_data_dir` and starts the library.
    virtual bool initialize(const Paths &paths, std::string &error) = 0;
    virtual void finalize() = 0;

    /// Compiles schemas and dictionaries. Blocking; call from a worker or accept
    /// the pause on first run. Returns false with `error` filled on failure.
    virtual bool deploy(std::string &error) = 0;

    virtual bool createSession(std::string &error) = 0;
    virtual void destroySession() = 0;

    /// Returns true when the engine consumed the key.
    virtual bool processKey(std::uint32_t sym, std::uint32_t mask) = 0;

    /// Removes and returns the pending commit string, if any.
    ///
    /// librime accumulates commits in the session; draining them is the
    /// caller's job, and doing it more than once per key would duplicate text.
    virtual bool takeCommit(std::string &out) = 0;

    [[nodiscard]] virtual Composition composition() = 0;
    [[nodiscard]] virtual Menu menu() = 0;

    /// Commits the candidate at row `index` of the **currently visible page**.
    ///
    /// The row is relative, like the numbers the UI shows: after paging, row 0
    /// must be the first word of that page, not of the first one. Ports whose
    /// engine wants an absolute index do the conversion themselves.
    virtual bool selectCandidate(std::size_t index) = 0;
    virtual bool changePage(bool backward) = 0;

    virtual bool setOption(std::string_view name, bool value) = 0;
    [[nodiscard]] virtual bool getOption(std::string_view name) = 0;

    [[nodiscard]] virtual bool asciiMode() = 0;
    [[nodiscard]] virtual std::string schemaId() = 0;
};

} // namespace superior::pinyin
