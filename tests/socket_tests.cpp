// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/TestSupport.hpp"
#include "superior/protocol/SceneServer.hpp"
#include "superior/protocol/Wire.hpp"

namespace {

using superior::protocol::DecodeError;
using superior::protocol::MessageType;
using superior::protocol::SceneServer;

std::string uniquePath() {
    static int counter = 0;
    return (std::filesystem::temp_directory_path() /
            ("superior-socket-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(++counter) + ".sock"))
        .string();
}

int connectClient(const std::string &path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

std::string receiveFrame(int fd, int timeoutMs = 2000) {
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd entry{fd, POLLIN, 0};
        const int result = ::poll(&entry, 1, 50);
        if (result > 0 && (entry.revents & POLLIN) != 0) {
            char chunk[4096];
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return {};
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            std::size_t consumed = 0;
            superior::protocol::WireMessage message;
            DecodeError error;
            if (superior::protocol::tryDecodeFrame(buffer, consumed, message, error) ==
                superior::protocol::FrameStatus::Complete) {
                return buffer.substr(0, consumed);
            }
        }
    }
    return {};
}

void testBroadcastToClient() {
    const std::string path = uniquePath();
    SceneServer server;
    std::string error;
    CHECK(server.listen(path, error));
    CHECK(server.listening());

    const int client = connectClient(path);
    CHECK(client >= 0);
    CHECK(waitFor([&] { return server.clientCount() == 1; }));

    const std::string payload = "scene-1";
    server.broadcast(superior::protocol::encodeFrame(MessageType::Scene, payload));

    const std::string frame = receiveFrame(client);
    CHECK(!frame.empty());

    DecodeError decodeError;
    const auto message = superior::protocol::decodeFrame(frame, decodeError);
    CHECK(message.has_value());
    CHECK(decodeError.ok());
    if (message) {
        CHECK(message->type == MessageType::Scene);
        CHECK(message->payload == payload);
    }

    ::close(client);
    CHECK(waitFor([&] { return server.clientCount() == 0; }));
    server.close();
    CHECK(!std::filesystem::exists(path));
}

void testClientCommandReachesHandler() {
    const std::string path = uniquePath();
    SceneServer server;
    std::string error;
    CHECK(server.listen(path, error));

    std::mutex mutex;
    std::string received;
    server.setCommandHandler([&](std::string bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        received += bytes;
    });

    const int client = connectClient(path);
    CHECK(client >= 0);
    CHECK(waitFor([&] { return server.clientCount() == 1; }));

    const std::string frame = superior::protocol::encodeFrame(MessageType::Command, "cmd");
    CHECK(::send(client, frame.data(), frame.size(), MSG_NOSIGNAL) ==
          static_cast<ssize_t>(frame.size()));

    CHECK(waitFor([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return received == frame;
    }));

    ::close(client);
    server.close();
}

void testRefusesNonSocketPath() {
    const std::string path = uniquePath();
    {
        std::FILE *file = std::fopen(path.c_str(), "w");
        CHECK(file != nullptr);
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    SceneServer server;
    std::string error;
    CHECK(!server.listen(path, error));
    CHECK(!error.empty());
    CHECK(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

} // namespace

int main() {
    testBroadcastToClient();
    testClientCommandReachesHandler();
    testRefusesNonSocketPath();
    return superior::test::finish("socket tests");
}
