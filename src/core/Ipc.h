// SPDX-License-Identifier: MIT
#pragma once

#include <QJsonObject>
#include <QLocalServer>
#include <QObject>

#include <functional>

namespace selsearch {

// $XDG_RUNTIME_DIR/selsearch.sock (or /tmp/selsearch-$UID.sock).
// Protocol: one JSON object per line, each answered by one JSON line.
//   -> {"cmd":"lens","mode":"region"}
//   <- {"ok":true}
QString ipcSocketPath();

class IpcServer : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonObject &)>;
    using Handler = std::function<void(const QJsonObject &request, const Reply &reply)>;

    explicit IpcServer(QObject *parent = nullptr);
    // Fails when another daemon already listens on the socket.
    bool listen(QString *error = nullptr);
    void setHandler(Handler handler) { m_handler = std::move(handler); }

private:
    void onNewConnection();
    QLocalServer m_server;
    Handler m_handler;
};

}  // namespace selsearch
