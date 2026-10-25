// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/protocol/SceneServer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "superior/diagnostics/Log.hpp"

namespace superior::protocol {

namespace diag = ::superior::diagnostics;

namespace {
constexpr std::size_t kMaxClientBuffer = 1024U * 1024U;
constexpr std::size_t kMaxQueuedFrames = 256;

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class SocketGuard {
public:
    explicit SocketGuard(int fd) : fd_(fd) {}
    ~SocketGuard() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    SocketGuard(const SocketGuard &) = delete;
    SocketGuard &operator=(const SocketGuard &) = delete;

    [[nodiscard]] int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

void closeFd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}
} // namespace

struct SceneServer::Impl {
    struct Client {
        int fd = -1;
        std::string outbox;
    };

    std::string path;
    dev_t pathDevice = 0;
    ino_t pathInode = 0;
    int listenFd = -1;
    int lockFd = -1;
    int wakeRead = -1;
    int wakeWrite = -1;

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> ready{false};

    mutable std::mutex clientsMutex;
    std::vector<Client> clients;

    std::mutex queueMutex;
    std::vector<std::string> queued;

    std::mutex handlerMutex;
    std::function<void(std::string)> handler;

    [[nodiscard]] std::size_t clientCount() const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return clients.size();
    }

    void wake() {
        if (wakeWrite >= 0) {
            const char byte = 1;
            [[maybe_unused]] const ssize_t written = ::write(wakeWrite, &byte, 1);
        }
    }

    void drainWake() {
        char buffer[64];
        while (::read(wakeRead, buffer, sizeof(buffer)) > 0) {
        }
    }

    void dropClient(std::size_t index) {
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (index < clients.size()) {
            SUPERIOR_DEBUG(diag::categories::Socket)
                .field("clients", static_cast<std::uint64_t>(clients.size() - 1))
                << "UI client disconnected";
            ::close(clients[index].fd);
            clients[index].fd = -1;
            clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    void acceptClient() {
        while (true) {
            const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) {
                return;
            }
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
                credentials.uid != ::getuid()) {
                ::close(fd);
                continue;
            }
            std::lock_guard<std::mutex> lock(clientsMutex);
            if (clients.size() >= 8) {
                SUPERIOR_WARN(diag::categories::Socket)
                    .field("pid", static_cast<std::int64_t>(credentials.pid))
                    << "rejecting client: connection limit reached";
                ::close(fd);
                continue;
            }
            clients.push_back(Client{fd, {}});
            SUPERIOR_DEBUG(diag::categories::Socket)
                .field("pid", static_cast<std::int64_t>(credentials.pid))
                .field("clients", static_cast<std::uint64_t>(clients.size()))
                << "UI client connected";
        }
    }

    void flushQueued() {
        std::vector<std::string> frames;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            frames.swap(queued);
        }
        if (frames.empty()) {
            return;
        }
        SUPERIOR_TRACE(diag::categories::Socket)
            .field("frames", static_cast<std::uint64_t>(frames.size()))
            .field("bytes", static_cast<std::uint64_t>(frames.front().size()))
            << "flushing queued frames";
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto &client : clients) {
            for (const auto &frame : frames) {
                client.outbox.append(frame);
            }
            if (client.outbox.size() > kMaxClientBuffer) {
                client.outbox.clear();
            }
        }
    }

    void serviceClient(std::size_t index, short revents) {
        Client *client = nullptr;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            if (index >= clients.size()) {
                return;
            }
            client = &clients[index];
        }

        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            dropClient(index);
            return;
        }

        if ((revents & POLLIN) != 0) {
            char buffer[4096];
            const ssize_t received = ::recv(client->fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                dropClient(index);
                return;
            }
            std::function<void(std::string)> callback;
            {
                std::lock_guard<std::mutex> lock(handlerMutex);
                callback = handler;
            }
            if (callback) {
                callback(std::string(buffer, static_cast<std::size_t>(received)));
            }
        }

        if ((revents & POLLOUT) != 0 && !client->outbox.empty()) {
            const ssize_t sent =
                ::send(client->fd, client->outbox.data(), client->outbox.size(), MSG_NOSIGNAL);
            if (sent > 0) {
                client->outbox.erase(0, static_cast<std::size_t>(sent));
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                dropClient(index);
            }
        }
    }

    void run() {
        running = true;
        ready = true;
        std::vector<pollfd> fds;
        while (running) {
            fds.clear();
            fds.push_back(pollfd{listenFd, POLLIN, 0});
            fds.push_back(pollfd{wakeRead, POLLIN, 0});

            std::size_t clientCount = 0;
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                clientCount = clients.size();
                for (std::size_t i = 0; i < clients.size(); ++i) {
                    short events = POLLIN;
                    if (!clients[i].outbox.empty()) {
                        events |= POLLOUT;
                    }
                    fds.push_back(pollfd{clients[i].fd, events, 0});
                }
            }

            const int result = ::poll(fds.data(), fds.size(), 1000);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                drainWake();
            }
            if ((fds[0].revents & POLLIN) != 0) {
                acceptClient();
            }

            flushQueued();

            // Service clients back-to-front so erase() cannot shift an index we
            // have not visited yet.
            for (std::size_t i = clientCount; i > 0; --i) {
                const std::size_t index = i - 1;
                if (index + 2 >= fds.size()) {
                    continue;
                }
                if (fds[index + 2].revents != 0) {
                    serviceClient(index, fds[index + 2].revents);
                }
            }
        }
        ready = false;
    }

    /// Releases the ownership lock. Only the kernel-side lock matters here: the
    /// file itself is left in place, an empty 0600 file in the runtime
    /// directory that the next server reuses.
    void releaseLock() {
        closeFd(lockFd);
    }

    /// True while the socket file at `path` is still the one we bound.
    [[nodiscard]] bool ownsPath() const {
        if (path.empty()) {
            return false;
        }
        struct stat status {};
        return ::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) &&
               status.st_dev == pathDevice && status.st_ino == pathInode;
    }

    void stop(bool releaseOwnership = true) {
        if (!running.exchange(false)) {
            // Thread may not have started yet.
        }
        wake();
        if (thread.joinable()) {
            thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (auto &client : clients) {
                if (client.fd >= 0) {
                    ::close(client.fd);
                }
            }
            clients.clear();
        }
        closeFd(listenFd);
        closeFd(wakeRead);
        closeFd(wakeWrite);

        // Only remove the socket we created. Fcitx5 may reload this addon while
        // another instance has already bound the same path; unlinking blindly
        // would delete the live socket and every UI would see "server not
        // found" until the next restart.
        if (!path.empty()) {
            struct stat status {};
            if (::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) &&
                status.st_dev == pathDevice && status.st_ino == pathInode) {
                ::unlink(path.c_str());
            }
            path.clear();
        }
        if (releaseOwnership) {
            releaseLock();
        }
    }
};

SceneServer::SceneServer() : impl_(std::make_unique<Impl>()) {}

SceneServer::~SceneServer() {
    close();
}

std::string SceneServer::defaultSocketPath() {
    const char *runtime = ::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') {
        return std::string(runtime) + "/superior-ime.sock";
    }
    return "/tmp/superior-ime-" + std::to_string(::getuid()) + ".sock";
}

namespace {

/// True when something actually accepts connections on `path`.
///
/// A socket *file* proves nothing: the listener may be gone. Only a successful
/// connect tells the two apart, which is what makes it safe to unlink the file.
bool socketIsServed(const std::string &path) {
    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    const bool served =
        ::connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    ::close(probe);
    return served;
}

} // namespace

bool SceneServer::listen(std::string socketPath, std::string &error) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath.size() + 1 >= sizeof(address.sun_path)) {
        error = "socket path is too long";
        return false;
    }

    // Arbitration first, teardown second. Two agents do start at the same
    // moment: `fcitx5 -r` brings the new process up while the old one is still
    // unloading the addon. The probe below cannot see a server that has bound
    // its file but not called listen() yet, so the loser used to remove the
    // winner's file and bind a second socket: both reported success, the older
    // one kept an inode nobody could reach, and the panels never came back.
    // With the lock, the loser fails here and retries once the winner is gone.
    const bool alreadyOwned = impl_->lockFd >= 0;
    if (!alreadyOwned) {
        const std::string lockPath = socketPath + ".lock";
        // O_NOFOLLOW: the fallback path for a session without XDG_RUNTIME_DIR
        // lives in /tmp, where a planted symlink must not redirect the lock.
        const int lockFd =
            ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lockFd < 0) {
            error = std::string("open(lock): ") + std::strerror(errno);
            return false;
        }
        if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            const int saved = errno;
            ::close(lockFd);
            error = saved == EWOULDBLOCK
                        ? "another Superior overlay server is already listening"
                        : std::string("flock(): ") + std::strerror(saved);
            return false;
        }
        impl_->lockFd = lockFd;
    }

    // The path is ours now, so the previous binding can go. `false` keeps the
    // lock held: that is what lets a stolen or removed socket be rebuilt
    // without ever reopening the window another agent could race into.
    impl_->stop(false);

    // A failed attempt serves nothing — the previous binding is already gone —
    // so the lock goes back too. Holding it would keep the next agent out while
    // this one has no socket to offer.
    const auto fail = [&](std::string message) {
        error = std::move(message);
        impl_->releaseLock();
        return false;
    };

    struct stat status {};
    if (::lstat(socketPath.c_str(), &status) == 0) {
        if (!S_ISSOCK(status.st_mode)) {
            return fail("path exists and is not a socket: " + socketPath);
        }
        // Only a *dead* socket may be removed: a listener we do not know about
        // (another build, a simulator) is not ours to steal.
        if (socketIsServed(socketPath)) {
            return fail("another Superior overlay server is already listening");
        }
        ::unlink(socketPath.c_str());
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return fail(std::string("socket(): ") + std::strerror(errno));
    }
    SocketGuard guard(fd);

    if (!setNonBlocking(fd)) {
        return fail("cannot make the listening socket non-blocking");
    }

    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size());

    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        // A previous instance can leave the socket file behind — Fcitx5 taking
        // over from another Fcitx5 (`fcitx5 -r`), a crash, or a session that
        // ended while our addon was still loaded. Binding then fails with
        // EADDRINUSE even though nobody is listening, and without this the
        // overlay could never connect for the rest of the session: typing still
        // worked, but every panel fell back to Fcitx5's classic UI, which looks
        // exactly like "the input method is broken".
        if (errno != EADDRINUSE) {
            return fail(std::string("bind(): ") + std::strerror(errno));
        }
        if (socketIsServed(socketPath)) {
            return fail("another Superior overlay server is already listening");
        }
        if (::unlink(socketPath.c_str()) != 0 && errno != ENOENT) {
            return fail(std::string("unlink(stale socket): ") + std::strerror(errno));
        }
        SUPERIOR_WARN(diag::categories::Socket)
            .field("path", socketPath)
            << "removed a stale socket left by a previous instance";
        if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            return fail(std::string("bind(): ") + std::strerror(errno));
        }
    }

    // `bind()` is what creates the file, and it refuses an existing path, so
    // from here the socket on disk is ours. The inode is remembered for `stop()`
    // and `ownsPath()`, which must not remove somebody else's file later.
    struct stat bound {};
    if (::lstat(socketPath.c_str(), &bound) != 0) {
        const std::string message = std::string("lstat(): ") + std::strerror(errno);
        ::unlink(socketPath.c_str());
        return fail(message);
    }

    if (::chmod(socketPath.c_str(), 0600) != 0) {
        const std::string message = std::string("chmod(): ") + std::strerror(errno);
        ::unlink(socketPath.c_str());
        return fail(message);
    }

    if (::listen(fd, 4) != 0) {
        const std::string message = std::string("listen(): ") + std::strerror(errno);
        ::unlink(socketPath.c_str());
        return fail(message);
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
        const std::string message = std::string("pipe2(): ") + std::strerror(errno);
        ::unlink(socketPath.c_str());
        return fail(message);
    }

    impl_->listenFd = guard.release();
    impl_->pathDevice = bound.st_dev;
    impl_->pathInode = bound.st_ino;
    impl_->wakeRead = pipeFds[0];
    impl_->wakeWrite = pipeFds[1];
    impl_->path = std::move(socketPath);
    impl_->ready = false;

    SUPERIOR_INFO(diag::categories::Socket).field("path", impl_->path) << "scene server listening";

    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    for (int i = 0; i < 100 && !impl_->ready; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!impl_->ready) {
        error = "server thread did not start";
        close();
        return false;
    }
    return true;
}

void SceneServer::setCommandHandler(std::function<void(std::string)> handler) {
    std::lock_guard<std::mutex> lock(impl_->handlerMutex);
    impl_->handler = std::move(handler);
}

void SceneServer::broadcast(std::string frame) {
    if (!impl_->running) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        if (impl_->queued.size() >= kMaxQueuedFrames) {
            SUPERIOR_WARN(diag::categories::Socket)
                .field("dropped", static_cast<std::uint64_t>(1))
                .field("queued", static_cast<std::uint64_t>(impl_->queued.size()))
                << "frame queue full, dropping the oldest frame";
            impl_->queued.erase(impl_->queued.begin());
        }
        impl_->queued.push_back(std::move(frame));
    }
    impl_->wake();
}

void SceneServer::close() {
    if (impl_->listenFd >= 0) {
        SUPERIOR_INFO(diag::categories::Socket)
            .field("path", impl_->path)
            << "scene server closing";
    }
    impl_->stop();
}

std::size_t SceneServer::clientCount() const {
    return impl_->clientCount();
}

std::string SceneServer::socketPath() const {
    return impl_->path;
}

bool SceneServer::listening() const {
    return impl_->listenFd >= 0;
}

bool SceneServer::ownsSocketFile() const {
    return impl_->listenFd >= 0 && impl_->ownsPath();
}

} // namespace superior::protocol
