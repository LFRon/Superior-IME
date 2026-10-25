// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneClient.hpp"

#include <QByteArray>

#include <utility>

#include "superior/diagnostics/Log.hpp"
#include "superior/protocol/Wire.hpp"

namespace superior::ui {

namespace diag = ::superior::diagnostics;

SceneClient::SceneClient(QString socketPath) : socketPath_(std::move(socketPath)) {
    QObject::connect(&socket_, &QLocalSocket::connected, [this]() {
        buffer_.clear();
        SUPERIOR_INFO(diag::categories::Ui)
            .field("socket", socketPath_.toStdString())
            << "connected to the agent";
    });
    QObject::connect(&socket_, &QLocalSocket::readyRead, [this]() { onReadyRead(); });
    QObject::connect(&socket_, &QLocalSocket::disconnected, [this]() {
        SUPERIOR_WARN(diag::categories::Ui) << "agent disconnected; will retry";
        scheduleReconnect();
    });
    QObject::connect(&socket_, &QLocalSocket::errorOccurred,
                     [this](QLocalSocket::LocalSocketError socketError) {
                         SUPERIOR_DEBUG(diag::categories::Ui)
                             .field("error", static_cast<std::int64_t>(socketError))
                             << "socket error; will retry";
                         scheduleReconnect();
                     });

    reconnectTimer_.setSingleShot(true);
    reconnectTimer_.setInterval(1000);
    QObject::connect(&reconnectTimer_, &QTimer::timeout, [this]() { connectNow(); });
}

void SceneClient::setHandler(Handler handler) {
    handler_ = std::move(handler);
}

void SceneClient::start() {
    running_ = true;
    SUPERIOR_DEBUG(diag::categories::Ui)
        .field("socket", socketPath_.toStdString())
        << "starting scene client";
    connectNow();
}

void SceneClient::stop() {
    running_ = false;
    reconnectTimer_.stop();
    socket_.abort();
}

void SceneClient::send(const api::UiCommand &command) {
    if (!connected()) {
        SUPERIOR_DEBUG(diag::categories::Ui) << "command dropped: no agent connection";
        return;
    }
    const std::string payload = protocol::encodeCommand(command);
    const std::string frame =
        protocol::encodeFrame(protocol::MessageType::Command, payload);
    const QByteArray bytes(frame.data(), static_cast<qsizetype>(frame.size()));
    if (socket_.write(bytes) != bytes.size()) {
        SUPERIOR_WARN(diag::categories::Ui) << "short write while sending a command";
        return;
    }
    socket_.flush();
    SUPERIOR_DEBUG(diag::categories::Ui)
        .field("bytes", static_cast<std::int64_t>(bytes.size()))
        << "command sent";
}

bool SceneClient::connected() const {
    return socket_.state() == QLocalSocket::ConnectedState;
}

void SceneClient::connectNow() {
    if (!running_ || socket_.state() != QLocalSocket::UnconnectedState) {
        return;
    }
    socket_.connectToServer(socketPath_);
}

void SceneClient::scheduleReconnect() {
    buffer_.clear();
    if (running_ && !reconnectTimer_.isActive()) {
        reconnectTimer_.start();
    }
}

void SceneClient::onReadyRead() {
    const QByteArray chunk = socket_.readAll();
    buffer_.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));

    while (!buffer_.empty()) {
        std::size_t consumed = 0;
        protocol::WireMessage message;
        protocol::DecodeError error;
        const auto status =
            protocol::tryDecodeFrame(buffer_, consumed, message, error);
        if (status == protocol::FrameStatus::NeedMore) {
            return;
        }
        if (status == protocol::FrameStatus::Invalid) {
            SUPERIOR_WARN(diag::categories::Ui)
                .field("code", error.code)
                .field("detail", error.detail)
                << "invalid frame from the agent; reconnecting";
            buffer_.clear();
            socket_.abort();
            return;
        }
        buffer_.erase(0, consumed);
        if (message.type != protocol::MessageType::Scene || !handler_) {
            continue;
        }
        const auto frame = protocol::decodeScene(message.payload, error);
        if (!frame) {
            SUPERIOR_WARN(diag::categories::Ui)
                .field("code", error.code)
                .field("detail", error.detail)
                << "cannot decode scene";
            continue;
        }
        SUPERIOR_TRACE(diag::categories::Ui)
            .field("frame", static_cast<std::uint64_t>(frame->frameId))
            .field("visible", frame->panel.visible)
            << "scene received";
        handler_(*frame);
    }
}

} // namespace superior::ui
