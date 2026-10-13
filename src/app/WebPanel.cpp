// SPDX-License-Identifier: MIT
#include "WebPanel.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace selsearch {

static QString helperPath()
{
    const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/selsearch-web");
    if (QFileInfo(local).isExecutable())
        return local;
    return QStandardPaths::findExecutable(QStringLiteral("selsearch-web"));
}

WebPanel::WebPanel(QObject *parent)
    : QObject(parent)
{
    m_proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &WebPanel::onStdout);
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleMs);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        if (m_proc.state() == QProcess::Running) {
            send(R"({"cmd":"quit"})");
            m_proc.closeWriteChannel();
            if (!m_proc.waitForFinished(2000))
                m_proc.kill();
        }
    });
}

WebPanel::~WebPanel()
{
    if (m_proc.state() == QProcess::Running) {
        m_proc.closeWriteChannel();
        if (!m_proc.waitForFinished(1000))
            m_proc.kill();
    }
}

bool WebPanel::isAvailable()
{
    return !helperPath().isEmpty();
}

void WebPanel::open(const QUrl &url, const QString &title, const QPoint &onScreen, bool persistentCookies)
{
    m_idle.stop();
    if (m_proc.state() == QProcess::Running && persistentCookies != m_persistent) {
        m_proc.kill();
        m_proc.waitForFinished(1000);
    }
    if (m_proc.state() != QProcess::Running) {
        m_persistent = persistentCookies;
        QStringList args;
        if (persistentCookies)
            args << QStringLiteral("--persistent");
        m_proc.start(helperPath(), args);
        m_proc.waitForStarted(2000);
    }
    const QJsonObject o{{QStringLiteral("cmd"), QStringLiteral("open")},
                        {QStringLiteral("url"), url.toString()},
                        {QStringLiteral("title"), title},
                        {QStringLiteral("x"), onScreen.x()},
                        {QStringLiteral("y"), onScreen.y()}};
    send(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void WebPanel::hide()
{
    if (m_proc.state() == QProcess::Running)
        send(R"({"cmd":"hide"})");
}

void WebPanel::send(const QByteArray &json)
{
    m_proc.write(json + '\n');
}

void WebPanel::onStdout()
{
    m_buffer += m_proc.readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QJsonObject o = QJsonDocument::fromJson(m_buffer.left(nl)).object();
        m_buffer.remove(0, nl + 1);
        const QString ev = o.value(QStringLiteral("event")).toString();
        if (ev == QLatin1String("hidden"))
            m_idle.start();
        else if (ev == QLatin1String("openExternal"))
            Q_EMIT openExternal(QUrl(o.value(QStringLiteral("url")).toString()));
    }
}

}  // namespace selsearch
