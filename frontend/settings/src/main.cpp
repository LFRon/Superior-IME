// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QPalette>

#include "SettingsWindow.hpp"
#include "superior/config/Fcitx5Config.hpp"
#include "superior/config/Settings.hpp"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("superior-settings"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setStyle(QStringLiteral("Fusion"));

    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(21, 24, 28));
    palette.setColor(QPalette::Base, QColor(30, 33, 38));
    palette.setColor(QPalette::Text, QColor(232, 234, 237));
    palette.setColor(QPalette::WindowText, QColor(232, 234, 237));
    palette.setColor(QPalette::Highlight, QColor(42, 111, 219));
    palette.setColor(QPalette::Button, QColor(36, 40, 47));
    QApplication::setPalette(palette);

    superior::ui::SettingsWindow window(superior::config::Paths::fromEnvironment());
    window.show();
    return QApplication::exec();
}
