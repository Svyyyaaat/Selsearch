// SPDX-License-Identifier: MIT
#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

namespace selsearch {

// Launch counts and last-use times for frecency ranking, kept in
// $XDG_STATE_HOME/selsearch/usage.json:
//   {"version":1,"items":{"firefox.desktop":{"count":12,"last":1791850000}}}
// Thread-safe. Only ids (desktop file ids) are stored, never queries.
class UsageStore {
public:
    static QString defaultPath();

    explicit UsageStore(const QString &path = defaultPath());

    void record(const QString &id, qint64 nowSecs = -1);  // saves immediately
    // count weighted by recency (Firefox-style buckets); 0 when never used.
    double frecency(const QString &id, qint64 nowSecs = -1) const;
    // Ids sorted by frecency, best first.
    QStringList top(int limit, qint64 nowSecs = -1) const;
    QString path() const { return m_path; }

private:
    struct Entry {
        int count = 0;
        qint64 last = 0;
    };
    void load();
    void save() const;
    static double weight(const Entry &e, qint64 now);

    QString m_path;
    mutable QMutex m_mutex;
    QHash<QString, Entry> m_items;
};

}  // namespace selsearch
