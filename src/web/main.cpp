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

#include <LayerShellQt/Window>

#include <cstdio>
#include <unistd.h>

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
        std::fputs(QJsonDocument(o).toJson(QJsonDocument::Compact).append('\n').constData(), stdout);
        std::fflush(stdout);
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
    if (auto *layer = LayerShellQt::Window::get(window)) {
        layer->setLayer(LayerShellQt::Window::LayerTop);
        layer->setAnchors(LayerShellQt::Window::AnchorTop);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layer->setScope(QStringLiteral("selsearch-results"));
        layer->setScreenConfiguration(LayerShellQt::Window::ScreenFromCompositor);  // the focused output
        layer->setMargins(QMargins(0, 72, 0, 0));
    }

    // stdin control channel
    QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
    QByteArray buffer;
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        char chunk[4096];
        const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
        if (n <= 0) {  // daemon went away
            QCoreApplication::quit();
            return;
        }
        buffer.append(chunk, n);
        int nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(buffer.left(nl)).object();
            buffer.remove(0, nl + 1);
            const QString cmd = o.value(QStringLiteral("cmd")).toString();
            if (cmd == QLatin1String("open")) {
                Q_EMIT bridge.openRequested(o.value(QStringLiteral("url")).toString(), o.value(QStringLiteral("title")).toString());
                window->show();
                window->requestActivate();
            } else if (cmd == QLatin1String("hide")) {
                Q_EMIT bridge.hideRequested();
            } else if (cmd == QLatin1String("quit")) {
                QCoreApplication::quit();
            }
        }
    });
    return app.exec();
}

#include "main.moc"
