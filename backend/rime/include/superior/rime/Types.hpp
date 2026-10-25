// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace superior::rime {

/// Where librime keeps its data.
///
/// Deliberately **not** Fcitx5's Rime directory: this engine owns its own
/// schemas and its own user dictionary, so installing 先！进！ never touches a
/// user's existing Rime setup.
struct Paths {
    /// Read-only schemas and dictionaries we ship or deploy.
    std::filesystem::path sharedData;
    /// Writable: `userdb`, build artifacts, `*.bin`.
    std::filesystem::path userData;
    /// Short name reported to librime; shows up in its logs and file names.
    std::string appName = "superior-ime";

    /// Resolves `$XDG_DATA_HOME/superior-ime/rime/{shared,user}`.
    [[nodiscard]] static Paths fromEnvironment();
    [[nodiscard]] static Paths withRoot(const std::filesystem::path &dataHome,
                                        std::string appName = "superior-ime");
};

/// One key press, already translated out of Fcitx5 types.
struct KeyInput {
    /// X11 keysym; librime speaks the same values.
    std::uint32_t sym = 0;
    /// X11 modifier bits (Shift = 1<<0, Lock = 1<<1, Control = 1<<2, Mod1 = 1<<3,
    /// and so on). librime uses the same layout, so no translation table is
    /// needed — but it stays a `uint32_t` here so this module depends on no
    /// toolkit at all.
    std::uint32_t states = 0;
    bool release = false;
};

/// librime's "this is a key release" bit (`RimeReleaseMask`).
///
/// Not exposed by `rime_api.h`, only by librime's C++ headers, so it is pinned
/// here with its origin documented.
inline constexpr std::uint32_t kReleaseMask = 1U << 30U;

/// One candidate as librime reported it.
struct Candidate {
    std::string text;
    /// Pinyin hint, weight, or whatever the schema put in `comment`.
    std::string comment;
};

struct Composition {
    std::string preedit;
    /// Byte offset into `preedit`.
    std::size_t cursor = 0;
};

struct Menu {
    std::vector<Candidate> candidates;
    /// 1-based, as required by the scene contract.
    int pageIndex = 1;
    int pageCount = 1;
    /// Index into `candidates` of the highlighted entry, or -1.
    int highlighted = -1;
    bool hasPrevious = false;
    bool hasNext = false;
};

/// Everything the UI needs for one frame.
struct Snapshot {
    Composition composition;
    Menu menu;
    /// True while the engine is in ASCII/English mode. The UI shows the
    /// composition row only when this is false.
    bool asciiMode = false;
};

/// Progress of an in-process deployment.
///
/// Deployment compiles schemas and dictionaries and can take minutes on the
/// first run, so the agent has to report it instead of blocking.
struct DeployState {
    enum class Phase {
        Idle,
        Running,
        Done,
        Failed,
    };

    Phase phase = Phase::Idle;
    /// 0..100 when the deployer reports it; -1 when unknown.
    int progress = -1;
    std::string detail;
};

} // namespace superior::rime
