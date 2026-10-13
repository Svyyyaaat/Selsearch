// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QPoint>
#include <QProcess>
#include <QTimer>
#include <QUrl>

namespace selsearch {

// Drives the selsearch-web helper process: started on first use, reused while
// alive, and stopped after `kIdleMs` hidden so Chromium's memory is returned.
class WebPanel : public QObject {
    Q_OBJECT
public:
    explicit WebPanel(QObject *parent = nullptr);
    ~WebPanel() override;

    static bool isAvailable();  // selsearch-web is installed next to us
    void open(const QUrl &url, const QString &title, const QPoint &onScreen, bool persistentCookies);
    void hide();

Q_SIGNALS:
    void openExternal(const QUrl &url);

private:
    void send(const QByteArray &json);
    void onStdout();

    static constexpr int kIdleMs = 120'000;
    QProcess m_proc;
    QTimer m_idle;
    bool m_persistent = false;
    QByteArray m_buffer;
};

}  // namespace selsearch
