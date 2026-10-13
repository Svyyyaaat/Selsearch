// SPDX-License-Identifier: MIT
#include "WindowsIntegration.h"

#include "overlay/FrameStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QMenu>
#include <QPixmap>
#include <QSettings>
#include <QSystemTrayIcon>

namespace selsearch {

static QIcon trayIcon()
{
    // The lens glyph in the default accent: readable on light and dark taskbars.
    IconProvider icons;
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48, 64})
        icon.addPixmap(QPixmap::fromImage(icons.requestImage(QStringLiteral("scan-text?color=%238e9bff&stroke=2.5"), nullptr, QSize(size, size))));
    return icon;
}

WindowsTray::WindowsTray(QObject *parent)
    : QObject(parent)
    , m_menu(std::make_unique<QMenu>())
{
    m_menu->addAction(tr("Open launcher"), this, &WindowsTray::launcherRequested);
    m_menu->addAction(tr("Search screen"), this, &WindowsTray::lensRequested);
    m_menu->addAction(tr("Settings"), this, &WindowsTray::settingsRequested);
    m_menu->addSeparator();
    m_menu->addAction(tr("Quit Selsearch"), qApp, &QCoreApplication::quit);

    m_tray = new QSystemTrayIcon(trayIcon(), this);
    m_tray->setToolTip(QStringLiteral("Selsearch"));
    m_tray->setContextMenu(m_menu.get());
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger)
            Q_EMIT launcherRequested();
    });
    m_tray->show();
}

WindowsTray::~WindowsTray() = default;

void WindowsTray::setToolTip(const QString &text)
{
    m_tray->setToolTip(text.isEmpty() ? QStringLiteral("Selsearch") : QStringLiteral("Selsearch\n") + text);
}

void setStartWithWindows(bool enabled)
{
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"), QSettings::NativeFormat);
    const QString name = QStringLiteral("Selsearch");
    if (enabled) {
        // Always rewrite: keeps the entry right after the app moved or updated.
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(name, QLatin1Char('"') + exe + QLatin1Char('"'));
    } else if (run.contains(name)) {
        run.remove(name);
    }
}

}  // namespace selsearch
