// SPDX-License-Identifier: MIT
#pragma once

// Windows-only parts of the daemon: there is no systemd and no compositor
// config, so Selsearch shows a tray icon and can register itself to start
// with Windows.

#include <QObject>

#include <memory>

class QMenu;
class QSystemTrayIcon;

namespace selsearch {

class WindowsTray : public QObject {
    Q_OBJECT
public:
    explicit WindowsTray(QObject *parent = nullptr);
    ~WindowsTray() override;

    // Tooltip lines under the name, e.g. the configured hotkeys.
    void setToolTip(const QString &text);

Q_SIGNALS:
    void launcherRequested();  // left click or "Open launcher"
    void lensRequested();
    void settingsRequested();

private:
    std::unique_ptr<QMenu> m_menu;
    QSystemTrayIcon *m_tray = nullptr;
};

// Adds or removes the "Selsearch" value (this selsearchd.exe) in
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run. Per user, no admin.
void setStartWithWindows(bool enabled);

}  // namespace selsearch
