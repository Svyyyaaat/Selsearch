// SPDX-License-Identifier: MIT
#include "Ipc.h"

#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>

#include <unistd.h>

namespace selsearch {

QString ipcSocketPath()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty())
        return runtime + QStringLiteral("/selsearch.sock");
    return QStringLiteral("/tmp/selsearch-%1.sock").arg(getuid());
}

IpcServer::IpcServer(QObject *parent)
    : QObject(parent)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);
}

bool IpcServer::listen(QString *error)
{
    const QString path = ipcSocketPath();
    if (QFile::exists(path)) {
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(300)) {
            if (error)
                *error = QStringLiteral("another selsearchd is already running");
            return false;
        }
        QLocalServer::removeServer(path);  // stale socket from a crash
    }
    if (!m_server.listen(path)) {
        if (error)
            *error = m_server.errorString();
        return false;
    }
    return true;
}

void IpcServer::onNewConnection()
{
    while (QLocalSocket *sock = m_server.nextPendingConnection()) {
        connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] {
            while (sock->canReadLine()) {
                const QByteArray line = sock->readLine().trimmed();
                if (line.isEmpty())
                    continue;
                QJsonParseError err;
                const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
                QPointer<QLocalSocket> guard(sock);
                Reply reply = [guard](const QJsonObject &obj) {
                    if (!guard)
                        return;
                    guard->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
                    guard->flush();
                };
                if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                    reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("invalid JSON")}});
                    continue;
                }
                if (m_handler)
                    m_handler(doc.object(), reply);
                else
                    reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("not ready")}});
            }
        });
    }
}

}  // namespace selsearch
