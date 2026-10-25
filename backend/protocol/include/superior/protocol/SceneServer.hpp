// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace superior::protocol {

/// Broadcasts already-framed messages to connected UI shells.
///
/// The server runs its own poll thread so a non-Qt host (the Fcitx5 agent) can
/// use it without adopting Qt or the Fcitx event loop. `broadcast()` is
/// thread-safe; the command handler runs on the poll thread and must marshal to
/// its owner before touching engine state.
class SceneServer {
public:
    SceneServer();
    ~SceneServer();

    SceneServer(const SceneServer &) = delete;
    SceneServer &operator=(const SceneServer &) = delete;

    /// `$XDG_RUNTIME_DIR/superior-ime.sock`, falling back to
    /// `/tmp/superior-ime-$UID.sock`.
    [[nodiscard]] static std::string defaultSocketPath();

    /// Binds and starts the poll thread. Refuses to replace a non-socket file.
    ///
    /// Ownership of the path is arbitrated with an exclusive lock on
    /// `<socketPath>.lock`, so two processes that start at the same time cannot
    /// both end up bound (one to the real path, one to an inode nobody can
    /// reach). Call it again with the same path to rebuild a socket that was
    /// stolen or removed underneath us.
    bool listen(std::string socketPath, std::string &error);

    void setCommandHandler(std::function<void(std::string)> handler);

    /// Queues one frame for every connected client.
    void broadcast(std::string frame);

    void close();

    [[nodiscard]] std::size_t clientCount() const;
    [[nodiscard]] std::string socketPath() const;
    [[nodiscard]] bool listening() const;

    /// True while the socket file at `socketPath()` still belongs to this
    /// server. A server can keep listening on a file somebody else unlinked —
    /// then no client can ever reach it again, which looks like "the panel
    /// stopped appearing" rather than like a socket problem.
    [[nodiscard]] bool ownsSocketFile() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace superior::protocol
