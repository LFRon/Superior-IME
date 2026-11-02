// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace superior::api {

/// The recogniser child, as the speech controller sees it.
///
/// Only four things ever happen to it: start it, ask whether it is alive, write
/// one command line, stop it. `SpeechProcess` is the Fcitx5/Linux
/// implementation; a Windows or Android host implements the same four methods
/// around `CreateProcess` / `Runtime.exec`. The controller never learns how the
/// process was spawned.
class SpeechCapability {
public:
    using LineHandler = std::function<void(std::string_view)>;

    virtual ~SpeechCapability() = default;

    [[nodiscard]] virtual bool running() const = 0;
    virtual bool start(LineHandler handler, std::string &error) = 0;
    virtual bool send(std::string_view line) = 0;
    virtual void stop() = 0;
};

} // namespace superior::api
