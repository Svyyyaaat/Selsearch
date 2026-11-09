// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include <optional>

namespace selsearch {

class Config;

// The accent colour comes from general.accent_source:
//   "selsearch" general.accent (an explicit "#rrggbb") or the built-in colour
//   "portal"    the xdg-desktop-portal accent (GNOME, KDE; Windows: the
//               system accent)
//   "pywal"     ~/.cache/wal/colors.json (pywal, wallust's pywal template)
//   "noctalia"  mPrimary from Noctalia's colors.json
//   "file"      general.accent_file: a colors.json in one of the formats
//               above, or any text file whose first "#rrggbb" is the accent
//               (matugen, Noctalia 5 or wallust templates)
// An empty source keeps the old behaviour: general.accent, else the portal
// when general.follow_portal_accent is on, else the built-in colour.
// Whatever the source, an unreadable one falls back to general.accent and
// then the built-in colour.
QColor accentColor(const Config &config);

QColor defaultAccent();

// The effective source ("selsearch", "portal", "pywal", "noctalia", "file").
QString accentSource(const Config &config);
// The file a file-based source reads ("" for selsearch/portal).
QString accentSourcePath(const Config &config);
// The colour a file-based source provides right now, if it can be read.
std::optional<QColor> accentFromSource(const Config &config);

// Watches the file behind a file-based accent source and emits changed()
// when it is rewritten (new wallpaper, new theme), so the UI can re-read
// accentColor(). Tools often replace the file instead of writing it in
// place, so the directory is watched too.
class AccentWatcher : public QObject {
    Q_OBJECT
public:
    explicit AccentWatcher(const Config &config, QObject *parent = nullptr);

Q_SIGNALS:
    void changed();

private:
    void check();
    void rewatch();

    const Config &m_config;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QTimer m_confirm;
    QTimer m_poll;
    QString m_path;
    QString m_stamp;
    QColor m_last;
};

}  // namespace selsearch
