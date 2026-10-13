// SPDX-License-Identifier: MIT
#pragma once

#include "DesktopEntry.h"
#include "Provider.h"
#include "UsageStore.h"

#include <QFuture>
#include <QMutex>
#include <QObject>
#include <QTimer>

#include <memory>
#include <optional>

class QFileSystemWatcher;

namespace selsearch {

// Installed applications from freedesktop .desktop files (on Windows: Start
// Menu shortcuts, see WindowsApps.h).
// Rows: one per visible app (kind App) plus its desktop actions as secondary
// rows (kind Action, "Firefox — New Private Window") that rank lower.
// Ranking: fuzzy match over name, generic name, keywords and exec basename,
// plus a frecency bonus from UsageStore. An empty query lists the most-used
// apps. Rescans (debounced) when an application dir changes.
class AppsProvider : public QObject, public Provider {
    Q_OBJECT
public:
    struct Options {
        QStringList applicationDirs;  // highest priority first; default: defaultApplicationDirs()
        QString locale;               // default: desktopentry::currentLocale()
        QStringList desktops;         // default: $XDG_CURRENT_DESKTOP
        QString usagePath;            // default: UsageStore::defaultPath()
        bool watch = true;            // QFileSystemWatcher on the dirs
    };

    // $XDG_DATA_HOME/applications, the flatpak export dirs, then every
    // $XDG_DATA_DIRS/*/applications, in precedence order, de-duplicated.
    static QStringList defaultApplicationDirs();

    explicit AppsProvider(QObject *parent = nullptr);
    explicit AppsProvider(Options options, QObject *parent = nullptr);
    ~AppsProvider() override;

    QString id() const override { return QStringLiteral("apps"); }
    void refresh() override;  // thread-safe; rescans every dir
    void refreshAsync();      // refresh() on the thread pool unless one is pending
    QList<LauncherItem> query(const QString &text, int limit) override;
    bool activate(const LauncherItem &item) override;
    bool activateAction(const LauncherItem &item, const QString &actionId) override;

    // Visible entries (after Hidden/NoDisplay/OnlyShowIn/TryExec), sorted by name.
    QList<DesktopEntry> entries() const;
    std::optional<DesktopEntry> entry(const QString &desktopId) const;
    // The full argv for launching `entry` (or one of its actions), field
    // codes expanded and wrapped in the terminal when Terminal=true.
    static QStringList launchArgv(const DesktopEntry &entry, const QString &actionId = {},
                                  const QStringList &files = {});
    UsageStore &usage() { return *m_usage; }
    QString lastError() const;

Q_SIGNALS:
    void changed();  // entries were rescanned

private:
    struct Indexed;
    struct State;
    bool launch(const DesktopEntry &entry, const QString &actionId);
    void updateWatches();

    Options m_options;
    std::unique_ptr<UsageStore> m_usage;
    mutable QMutex m_mutex;  // guards m_state, m_error
    std::shared_ptr<const State> m_state;
    QString m_error;
    QMutex m_refreshMutex;  // serialises refresh()
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer m_debounce;
    QFuture<void> m_pending;  // debounced rescan
};

}  // namespace selsearch
