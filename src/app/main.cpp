// SPDX-License-Identifier: MIT
//
// selsearchd: resident daemon. Keeps overlay windows and models warm so the
// hotkey shows UI within a frame. Controlled through `selsearch` (Unix socket,
// or a named pipe on Windows) and, on Windows, by its own global hotkeys.

#include "App.h"

#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>

#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <QApplication>  // QSystemTrayIcon needs a QApplication

#include <windows.h>
#else
#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            // A GUI program has no console of its own; print to the terminal's.
            // (Only here: a daemon attached to a terminal would die with it.)
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                std::freopen("CONOUT$", "w", stdout);
            std::printf("selsearchd %s\n", SELSEARCH_VERSION);
            std::fflush(stdout);
            return 0;
        }
    }
#endif
    // The lens overlay needs per-window Wayland layer surfaces; the launcher too.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    // Never let a stray QML error dialog or session manager grab us.
    qunsetenv("SESSION_MANAGER");
    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication::setDesktopFileName(QStringLiteral("selsearch"));
#ifdef Q_OS_WIN
    // Qt 6 makes the process Per-Monitor-V2 DPI aware by default, which the
    // capture and the window list rely on (physical pixels per monitor).
    QApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif
    QGuiApplication::setApplicationName(QStringLiteral("selsearch"));
    QGuiApplication::setApplicationVersion(QStringLiteral(SELSEARCH_VERSION));
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

#ifdef Q_OS_WIN
    // No signals on Windows. Ctrl+C / Ctrl+Break reach us only when a console
    // is attached; the handler runs on its own thread.
    SetConsoleCtrlHandler(
        [](DWORD) -> BOOL {
            QMetaObject::invokeMethod(qApp, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
            return TRUE;
        },
        TRUE);
#else
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
#endif

    selsearch::App selsearchApp;
    QString error;
    if (!selsearchApp.init(&error)) {
        std::fprintf(stderr, "selsearchd: %s\n", qPrintable(error));
        return 1;
    }
    return app.exec();
}
