// SPDX-License-Identifier: MIT
//
// selsearchd: resident daemon. Keeps overlay windows and models warm so the
// hotkey shows UI within a frame. Controlled through `selsearch` (Unix socket).

#include "App.h"

#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>

#include <csignal>
#include <cstdio>

int main(int argc, char **argv)
{
    // The lens overlay needs per-window Wayland layer surfaces; the launcher too.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    // Never let a stray QML error dialog or session manager grab us.
    qunsetenv("SESSION_MANAGER");
    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication::setDesktopFileName(QStringLiteral("selsearch"));
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("selsearch"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SELSEARCH_VERSION));
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    std::signal(SIGINT, [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });

    selsearch::App selsearchApp;
    QString error;
    if (!selsearchApp.init(&error)) {
        std::fprintf(stderr, "selsearchd: %s\n", qPrintable(error));
        return 1;
    }
    return app.exec();
}
