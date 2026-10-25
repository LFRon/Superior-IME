// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "superior/rime/RimePort.hpp"

namespace superior::rime {

/// `RimePort` backed by the real librime shared library.
///
/// The header deliberately mentions no librime type: `rime_api.h` lives only in
/// the `.cpp`, so nothing that includes this header can accidentally depend on
/// the C API or on librime being installed.
class LibrimePort final : public RimePort {
public:
    LibrimePort();
    ~LibrimePort() override;

    LibrimePort(const LibrimePort &) = delete;
    LibrimePort &operator=(const LibrimePort &) = delete;

    /// False when `rime_get_api` could not be resolved — the caller reports
    /// "engine unavailable" instead of failing to start the whole addon.
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::string unavailableReason() const;

    bool initialize(const Paths &paths, std::string &error) override;
    void finalize() override;
    bool deploy(std::string &error) override;

    bool createSession(std::string &error) override;
    void destroySession() override;

    bool processKey(std::uint32_t sym, std::uint32_t mask) override;
    bool takeCommit(std::string &out) override;

    [[nodiscard]] Composition composition() override;
    [[nodiscard]] Menu menu() override;

    bool selectCandidate(std::size_t index) override;
    bool changePage(bool backward) override;

    bool setOption(std::string_view name, bool value) override;
    [[nodiscard]] bool getOption(std::string_view name) override;

    [[nodiscard]] bool asciiMode() override;
    [[nodiscard]] std::string schemaId() override;

    /// Version reported by the loaded library, for the diagnostics page.
    [[nodiscard]] std::string libraryVersion() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace superior::rime
