// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace superior::ui {

/// Wires Qt's logging facilities into the unified Superior log.
///
/// Two directions are supported:
///
/// 1. Qt -> Superior (always): `qDebug`, `qCWarning` and every Qt category are
///    converted into `superior::diagnostics` records with the category prefixed
///    by `qt.`. `QT_LOGGING_RULES` still decides what Qt emits, so existing Qt
///    debugging keeps working unchanged.
///
/// 2. Superior -> Qt (opt-in): when `SUPERIOR_LOG_VIA_QT=1`, our records are
///    handed to `QMessageLogger`, so `QT_LOGGING_RULES` can filter them too.
///    A thread-local guard prevents the two directions from recursing.
void installQtLogging();

} // namespace superior::ui
