// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

/// A socket file left behind by a previous instance must not block the next one.
///
/// This is not hypothetical: Fcitx5 replacing another Fcitx5 (`fcitx5 -r`) left
/// the file in place, the new agent failed to bind with EADDRINUSE, and the
/// overlay could never connect again — the panels silently fell back to Fcitx5's
/// classic UI, which looks exactly like "the input method is broken".
void testRecoversFromStaleSocketFile() {
    const std::string path = uniquePath();

    // Bind and close without unlinking: what a killed process leaves.
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size());
        CHECK(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        ::close(fd);
    }
    CHECK(std::filesystem::exists(path));

    SceneServer server;
    std::string error;
    CHECK(server.listen(path, error));
    CHECK(error.empty());

    // A real client must reach it, i.e. the file now belongs to this server.
    const int client = connectClient(path);
    CHECK(client >= 0);
    if (client >= 0) {
        ::close(client);
    }
    server.close();
    std::filesystem::remove(path);
}

/// A *live* listener must win: two overlays on one socket would fight.
void testRefusesLiveSocket() {
    const std::string path = uniquePath();
    SceneServer first;
    std::string error;
    CHECK(first.listen(path, error));

    SceneServer second;
    std::string secondError;
    CHECK(!second.listen(path, secondError));
    CHECK(!secondError.empty());

    first.close();
    std::filesystem::remove(path);
}

/// The socket file is not the server: once somebody removes it, a server that
/// keeps polling is unreachable, and only an explicit re-listen brings the
/// panels back. This is the state Fcitx5 was left in when two agents raced.
void testOwnsSocketFileFollowsThePath() {
    const std::string path = uniquePath();
    SceneServer server;
    std::string error;
    CHECK(server.listen(path, error));
    CHECK(server.ownsSocketFile());

    CHECK(std::filesystem::remove(path));
    CHECK(!server.ownsSocketFile());
    // Still polling; there is simply nobody who can reach it any more.
    CHECK(server.listening());

    // Re-listening is how the agent recovers, and it has to work without a
    // restart: same object, same path, no lock of its own to fight.
    CHECK(server.listen(path, error));
    CHECK(error.empty());
    CHECK(server.ownsSocketFile());

    const int client = connectClient(path);
    CHECK(client >= 0);
    if (client >= 0) {
        ::close(client);
    }
    server.close();
    CHECK(!std::filesystem::exists(path));
    std::filesystem::remove(path + ".lock");
}

/// Two agents start at the same moment during an Fcitx5 handover. The loser
/// must fail instead of taking the socket file over, otherwise both report a
/// working socket and whichever dies first pulls the path out from under the
/// survivor.
void testOwnershipLockSurvivesAStolenPath() {
    const std::string path = uniquePath();
    SceneServer first;
    std::string error;
    CHECK(first.listen(path, error));

    // What the old racy code did: delete the live server's file behind its back.
    CHECK(std::filesystem::remove(path));

    SceneServer second;
    std::string secondError;
    CHECK(!second.listen(path, secondError));
    CHECK(!secondError.empty());
    CHECK(!second.ownsSocketFile());

    // The owner can rebuild; the stranger still cannot.
    CHECK(first.listen(path, error));
    CHECK(first.ownsSocketFile());
    CHECK(!second.listen(path, secondError));

    first.close();
    // A closed server releases the lock, so a handover works.
    CHECK(second.listen(path, secondError));
    CHECK(second.ownsSocketFile());
    second.close();
    std::filesystem::remove(path + ".lock");
}

/// Eight servers starting in the same millisecond, as happens when Fcitx5
/// replaces another Fcitx5: exactly one may win, and it must be the one holding
/// the path afterwards. Losing here is normal; losing *after* binding is what
/// left a live but unreachable socket behind.
void testConcurrentListenHasOneWinner() {
    const std::string path = uniquePath();
    constexpr int kRacers = 8;

    std::vector<std::unique_ptr<SceneServer>> racers;
    std::vector<std::string> errors(kRacers);
    std::vector<bool> results(kRacers, false);
    std::atomic<int> waiting{0};
    std::vector<std::thread> threads;
    threads.reserve(kRacers);
    for (int i = 0; i < kRacers; ++i) {
        racers.push_back(std::make_unique<SceneServer>());
        threads.emplace_back([&, i] {
            waiting.fetch_add(1);
            while (waiting.load() < kRacers) {
                std::this_thread::yield();
            }
            results[i] = racers[i]->listen(path, errors[i]);
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    int winners = 0;
    int owner = -1;
    for (int i = 0; i < kRacers; ++i) {
        if (results[i]) {
            ++winners;
            owner = i;
        }
    }
    CHECK(winners == 1);
    if (owner >= 0) {
        CHECK(racers[owner]->ownsSocketFile());
        const int client = connectClient(path);
        CHECK(client >= 0);
        if (client >= 0) {
            ::close(client);
        }
    }
    for (auto &racer : racers) {
        racer->close();
    }
    std::filesystem::remove(path + ".lock");
}

int main() {
    testBroadcastToClient();
    testRecoversFromStaleSocketFile();
    testRefusesLiveSocket();
    testClientCommandReachesHandler();
    testRefusesNonSocketPath();
    testOwnsSocketFileFollowsThePath();
    testOwnershipLockSurvivesAStolenPath();
    testConcurrentListenHasOneWinner();
    return superior::test::finish("socket tests");
}
