// SPDX-License-Identifier: GPL-3.0-or-later
#include "ModelDownload.hpp"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>

#include <memory>

namespace superior::ui {

namespace {

/// Touched only on the worker thread: it may block, and it must not touch a
/// widget.
struct Transfer {
    QNetworkAccessManager *manager = nullptr;
    QNetworkReply *reply = nullptr;
    std::unique_ptr<QFile> file;
};

} // namespace

ModelDownloader::ModelDownloader(QObject *parent) : QObject(parent) {}

ModelDownloader::~ModelDownloader() {
    destroyWorker();
}

void ModelDownloader::setProgressHandler(ProgressHandler handler) {
    progress_ = std::move(handler);
}

void ModelDownloader::setFinishedHandler(FinishedHandler handler) {
    finished_ = std::move(handler);
}

void ModelDownloader::createWorker() {
    if (thread_ != nullptr) {
        return;
    }
    thread_ = new QThread(this);
    // A plain QObject as the thread's owner: `invokeMethod` with a functor and a
    // queued connection runs it in this object's thread, no moc required.
    context_ = new QObject();
    context_->moveToThread(thread_);
    QObject::connect(thread_, &QThread::finished, context_, &QObject::deleteLater);
    thread_->start();
}

void ModelDownloader::start(const QUrl &url, const QString &partPath) {
    if (running_.load()) {
        return;
    }
    createWorker();
    running_.store(true);
    cancelRequested_.store(false);

    ModelDownloader *self = this;
    QMetaObject::invokeMethod(
        context_,
        [this, self, url, partPath] {
            auto transfer = std::make_unique<Transfer>();
            transfer->file = std::make_unique<QFile>(partPath);
            if (!transfer->file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                self->running_.store(false);
                QMetaObject::invokeMethod(
                    self, [self] { self->report(Status::Failed); }, Qt::QueuedConnection);
                return;
            }

            // Constructed here, in the worker thread, on purpose.
            transfer->manager = new QNetworkAccessManager(context_);
            QNetworkRequest request{url};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
            transfer->reply = transfer->manager->get(request);
            Transfer *current = transfer.release();
            transfer_ = current;
            if (cancelRequested_.load()) {
                // The user clicked "取消" while this request was still being set up
                // (TLS handshake, redirect chain — seconds on a slow mirror). The
                // queued abort from `cancel()` found no reply yet and did nothing,
                // so the transfer used to run to completion before reporting
                // "cancelled". Aborting here is the fix.
                current->reply->abort();
            }
            QObject::connect(current->reply, &QNetworkReply::readyRead, context_,
                             [current] { current->file->write(current->reply->readAll()); });
            QObject::connect(current->reply, &QNetworkReply::downloadProgress, context_,
                             [self](qint64 received, qint64 total) {
                                 QMetaObject::invokeMethod(
                                     self,
                                     [self, received, total] {
                                         if (self->progress_) {
                                             self->progress_(received, total);
                                         }
                                     },
                                     Qt::QueuedConnection);
                             });
            QObject::connect(current->reply, &QNetworkReply::finished, context_,
                             [this, self, current] {
                                 const QNetworkReply::NetworkError error = current->reply->error();
                                 current->reply->deleteLater();
                                 current->reply = nullptr;
                                 if (current->file) {
                                     current->file->flush();
                                     current->file->close();
                                     current->file.reset();
                                 }
                                 const bool cancelled = self->cancelRequested_.load() ||
                                                        error ==
                                                            QNetworkReply::OperationCanceledError;
                                 const Status status = cancelled ? Status::Cancelled
                                                        : error == QNetworkReply::NoError
                                                            ? Status::Ok
                                                            : Status::Failed;
                                 self->running_.store(false);
                                 if (current->manager != nullptr) {
                                     current->manager->deleteLater();
                                     current->manager = nullptr;
                                 }
                                 delete current;
                                 self->transfer_ = nullptr;
                                 QMetaObject::invokeMethod(
                                     self, [self, status] { self->report(status); },
                                     Qt::QueuedConnection);
                             });
        },
        Qt::QueuedConnection);
}

void ModelDownloader::report(Status status) {
    if (finished_) {
        finished_(status);
    }
}

void ModelDownloader::cancel() {
    if (context_ == nullptr) {
        return;
    }
    cancelRequested_.store(true);
    // Aborting from the worker thread keeps the reply's state single-threaded.
    QMetaObject::invokeMethod(
        context_,
        [this] {
            auto *transfer = static_cast<Transfer *>(transfer_);
            if (transfer != nullptr && transfer->reply != nullptr) {
                transfer->reply->abort();
            }
        },
        Qt::QueuedConnection);
}

void ModelDownloader::destroyWorker() {
    if (thread_ == nullptr) {
        return;
    }
    cancel();
    thread_->quit();
    thread_->wait();
    context_ = nullptr;
    delete thread_;
    thread_ = nullptr;
}

} // namespace superior::ui
