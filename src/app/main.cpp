// SPDX-License-Identifier: MIT
//
// selsearchd: resident daemon. Keeps overlay windows and models warm so the
// hotkey shows UI within a frame. Controlled through `selsearch` (Unix socket).

#include "App.h"

#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSocketNotifier>

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

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

    // Quit cleanly on SIGINT/SIGTERM (systemd stop) via a self-pipe; only
    // async-signal-safe work happens in the handler itself.
    static int signalPipe[2];
    if (::pipe2(signalPipe, O_CLOEXEC | O_NONBLOCK) == 0) {
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] { QCoreApplication::quit(); });
        struct sigaction sa = {};
        sa.sa_handler = [](int) {
            const char c = 1;
            [[maybe_unused]] auto r = ::write(signalPipe[1], &c, 1);
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    selsearch::App selsearchApp;
    QString error;
    if (!selsearchApp.init(&error)) {
        std::fprintf(stderr, "selsearchd: %s\n", qPrintable(error));
        return 1;
    }
    return app.exec();
}
