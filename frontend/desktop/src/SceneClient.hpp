// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QLocalSocket>
#include <QTimer>

#include <functional>
#include <string>

#include "superior/api/Commands.hpp"
#include "superior/api/Scene.hpp"

namespace superior::ui {

/// Connects to the agent's scene socket and reassembles framed messages.
///
/// Runs on the GUI thread; no worker thread is needed because scenes are small
/// and low frequency (typing rate, not frame rate).
class SceneClient {
public:
    using Handler = std::function<void(const api::UiSceneFrame &)>;

    explicit SceneClient(QString socketPath);

    void setHandler(Handler handler);
    void start();
    void stop();

    /// Sends a semantic command. Frames are small and the socket is local, so
    /// this writes synchronously and drops the command when nothing is
    /// connected instead of queueing stale intent.
    void send(const api::UiCommand &command);

    [[nodiscard]] bool connected() const;

private:
    void connectNow();
    void onReadyRead();
    void scheduleReconnect();

    QString socketPath_;
    QLocalSocket socket_;
    QTimer reconnectTimer_;
    std::string buffer_;
    Handler handler_;
    bool running_ = false;
};

} // namespace superior::ui
