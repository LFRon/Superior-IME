// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>

class QThread;

namespace superior::ui {

/// One model download, executed on a worker thread.
///
/// The transfer is *not* on the UI thread, and that is the whole point. Qt builds
/// its default TLS configuration synchronously inside
/// `QNetworkAccessManager::get()`, and building it walks the CA certificate
/// directory with `realpath()` per entry. On a loaded disk that takes tens of
/// seconds — 40+ s at 100% CPU was measured here — and on the UI thread that is a
/// frozen input method, which is exactly the bug this class removes.
///
/// Plain callbacks instead of Qt signals: the rest of this UI has no `Q_OBJECT`
/// classes, and adding moc for one downloader would be a build-system change with
/// no user-visible benefit.
class ModelDownloader : public QObject {
public:
    enum class Status {
        Ok,
        Failed,
        Cancelled,
    };

    /// Bytes so far; invoked on the UI thread.
    using ProgressHandler = std::function<void(qint64 received, qint64 total)>;
    /// Invoked on the UI thread once per transfer, including after `cancel()`.
    using FinishedHandler = std::function<void(Status status)>;

    explicit ModelDownloader(QObject *parent = nullptr);
    ~ModelDownloader() override;

    void setProgressHandler(ProgressHandler handler);
    void setFinishedHandler(FinishedHandler handler);

    /// Starts a transfer into `partPath` (created or truncated).
    void start(const QUrl &url, const QString &partPath);
    /// Asks the worker to abort. `finished` still follows.
    void cancel();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    void createWorker();
    void destroyWorker();
    /// Runs the finished handler on the UI thread.
    void report(Status status);

    QThread *thread_ = nullptr;
    /// Reinterpreted `Transfer *`; only dereferenced on the worker thread.
    void *transfer_ = nullptr;
    /// Lives in the worker thread; functors posted to it run there.
    QObject *context_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelRequested_{false};
    ProgressHandler progress_;
    FinishedHandler finished_;
};

} // namespace superior::ui
