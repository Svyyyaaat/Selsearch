// SPDX-License-Identifier: MIT
//
// selsearch-web: the results panel. A separate process so QtWebEngine
// (Chromium) only costs memory while results are on screen; selsearchd starts
// it on demand and stops it after it has been hidden for a while.
//
// Control protocol: JSON lines on stdin.
//   {"cmd":"open","url":"https://...","title":"..."}
//   {"cmd":"hide"} / {"cmd":"quit"}
// Events on stdout: {"event":"hidden"} {"event":"openExternal","url":...}

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QtWebEngineQuick>

#include "app/WindowPlacement.h"

#include <cstdio>

#ifdef Q_OS_WIN
#include <thread>
#include <windows.h>
#else
#include <unistd.h>
#endif

class Bridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool persistent READ persistent CONSTANT)
    Q_PROPERTY(QString storagePath READ storagePath CONSTANT)
public:
    explicit Bridge(bool persistent)
        : m_persistent(persistent)
    {
    }
    bool persistent() const { return m_persistent; }
    QString storagePath() const
    {
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/selsearch/web");
    }
    Q_INVOKABLE void emitEvent(const QString &event, const QString &url = {})
    {
        QJsonObject o{{QStringLiteral("event"), event}};
        if (!url.isEmpty())
            o.insert(QStringLiteral("url"), url);
        const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact).append('\n');
#ifdef Q_OS_WIN
        // A GUI-subsystem program: write to the pipe handle QProcess gave us.
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.constData(), DWORD(line.size()), &written, nullptr);
#else
        std::fputs(line.constData(), stdout);
        std::fflush(stdout);
#endif
    }

Q_SIGNALS:
    void openRequested(const QString &url, const QString &title);
    void hideRequested();

private:
    bool m_persistent;
};

int main(int argc, char **argv)
{
    QtWebEngineQuick::initialize();
    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication::setDesktopFileName(QStringLiteral("selsearch"));
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("selsearch-web"));
    QGuiApplication::setQuitOnLastWindowClosed(false);

    const bool persistent = app.arguments().contains(QStringLiteral("--persistent"));
    Bridge bridge(persistent);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    engine.load(QUrl(QStringLiteral("qrc:/qml/web/WebPanel.qml")));
    auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window)
        return 1;
    selsearch::setupResultsWindow(window);

    // stdin control channel
    QByteArray buffer;
    auto onInput = [&](const QByteArray &chunk) {
        if (chunk.isEmpty()) {  // daemon went away
            QCoreApplication::quit();
            return;
        }
        buffer.append(chunk);
        int nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(buffer.left(nl)).object();
            buffer.remove(0, nl + 1);
            const QString cmd = o.value(QStringLiteral("cmd")).toString();
            if (cmd == QLatin1String("open")) {
                Q_EMIT bridge.openRequested(o.value(QStringLiteral("url")).toString(), o.value(QStringLiteral("title")).toString());
                selsearch::placeResultsWindow(window, QPoint(o.value(QStringLiteral("x")).toInt(), o.value(QStringLiteral("y")).toInt()));
                window->show();
                selsearch::activateWindow(window);
            } else if (cmd == QLatin1String("hide")) {
                Q_EMIT bridge.hideRequested();
            } else if (cmd == QLatin1String("quit")) {
                QCoreApplication::quit();
            }
        }
    };
#ifdef Q_OS_WIN
    // Pipes cannot be watched with QSocketNotifier on Windows: read on a
    // thread and hand each chunk to the GUI thread.
    std::thread([&app, onInput] {
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        char chunk[4096];
        for (;;) {
            DWORD n = 0;
            const bool ok = ReadFile(in, chunk, sizeof chunk, &n, nullptr) && n > 0;
            const QByteArray data = ok ? QByteArray(chunk, int(n)) : QByteArray();
            QMetaObject::invokeMethod(&app, [onInput, data] { onInput(data); }, Qt::QueuedConnection);
            if (!ok)
                return;
        }
    }).detach();
#else
    QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        char chunk[4096];
        const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
        onInput(n > 0 ? QByteArray(chunk, int(n)) : QByteArray());
    });
#endif
    return app.exec();
}

#include "main.moc"
