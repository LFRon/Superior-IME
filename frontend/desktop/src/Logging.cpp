// SPDX-License-Identifier: GPL-3.0-or-later
#include "Logging.hpp"

#include <QByteArray>
#include <QLoggingCategory>
#include <QMessageLogger>
#include <QString>

#include <cstdlib>
#include <cstring>
#include <string>

#include "superior/diagnostics/Log.hpp"

namespace superior::ui {

namespace {

namespace diag = superior::diagnostics;

/// Record currently travelling Superior -> Qt. While set, the Qt handler knows
/// the message is ours and must be re-emitted with its original category
/// instead of being re-categorized as `qt.*`.
thread_local const diag::Record *g_forwarded = nullptr;

diag::Level fromQtType(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return diag::Level::Debug;
    case QtInfoMsg:
        return diag::Level::Info;
    case QtWarningMsg:
        return diag::Level::Warning;
    case QtCriticalMsg:
    case QtFatalMsg:
        return diag::Level::Error;
    }
    return diag::Level::Info;
}

const char *categoryOf(const QMessageLogContext &context) {
    return context.category != nullptr ? context.category : "default";
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext &context,
                      const QString &message) {
    // Case 1: the message is one of our records that we forwarded to Qt.
    // Re-emit the original record so category, level and file survive.
    if (g_forwarded != nullptr) {
        diag::publishRaw(*g_forwarded);
        return;
    }

    // Case 2: a genuine Qt message. Convert it into our format.
    diag::Record record;
    record.level = fromQtType(type);
    // Qt categories already look like "qt.qpa.xcb"; only prefix the default one.
    const std::string name = categoryOf(context);
    record.category = name.rfind("qt.", 0) == 0 ? name : "qt." + name;
    record.file = context.file != nullptr ? context.file : "";
    record.line = context.line;
    record.message = message.toStdString();

    // publishRaw uses the fallback sink (stderr or file), never the Qt
    // forwarder, so this cannot recurse.
    diag::publishRaw(std::move(record));
}

bool envTruthy(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "off") != 0;
}

void forwardToQt(const diag::Record &record) {
    const QByteArray category(record.category.c_str());
    const QByteArray text(record.message.c_str());
    // Four-argument form: the third parameter is the *function* name, the
    // fourth is the category. Passing the category third would silently fall
    // back to Qt's default category and break QT_LOGGING_RULES filtering.
    const QMessageLogger logger(record.file.c_str(), record.line, category.constData(),
                                category.constData());

    g_forwarded = &record;
    if (record.level >= diag::Level::Warning) {
        logger.warning().noquote() << text.constData();
    } else if (record.level == diag::Level::Info) {
        logger.info().noquote() << text.constData();
    } else {
        logger.debug().noquote() << text.constData();
    }
    g_forwarded = nullptr;
}

} // namespace

void installQtLogging() {
    // The overlay is launched by the agent with stderr on /dev/null; without this
    // the UI side of a "nothing happened" report leaves no trace either.
    static_cast<void>(diag::ensureLogFile("ui"));
    diag::configureFromEnvironment();

    // Qt captures qFatal/abort paths too; forwarding them keeps a single file.
    qInstallMessageHandler(&qtMessageHandler);

    if (!envTruthy("SUPERIOR_LOG_VIA_QT")) {
        return;
    }

    // Forwarded records use our category names as Qt categories. Without a
    // default rule Qt would filter them out (debug categories are off by
    // default). User-provided QT_LOGGING_RULES still takes precedence.
    if (std::getenv("QT_LOGGING_RULES") == nullptr) {
        QLoggingCategory::setFilterRules(QStringLiteral("superior.*=true"));
    }
    diag::setSink(forwardToQt);
    qInfo("superior-ui: forwarding Superior records through Qt logging");
}

} // namespace superior::ui
