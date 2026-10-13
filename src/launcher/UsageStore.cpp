// SPDX-License-Identifier: MIT
#include "UsageStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace selsearch {

QString UsageStore::defaultPath()
{
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
#ifdef Q_OS_WIN
    // %LOCALAPPDATA%, next to the config and models.
    if (state.isEmpty() || !QDir::isAbsolutePath(state))
        state = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    if (state.isEmpty() || !QDir::isAbsolutePath(state))
        state = QDir::homePath() + QStringLiteral("/.local/state");
    return state + QStringLiteral("/selsearch/usage.json");
}

UsageStore::UsageStore(const QString &path)
    : m_path(path)
{
    load();
}

static qint64 nowOr(qint64 now)
{
    return now >= 0 ? now : QDateTime::currentSecsSinceEpoch();
}

void UsageStore::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject items = QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("items")).toObject();
    for (auto it = items.begin(); it != items.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Entry e;
        e.count = o.value(QLatin1String("count")).toInt();
        e.last = qint64(o.value(QLatin1String("last")).toDouble());
        if (e.count > 0)
            m_items.insert(it.key(), e);
    }
}

void UsageStore::save() const
{
    QJsonObject items;
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
        items.insert(it.key(), QJsonObject{{QStringLiteral("count"), it->count}, {QStringLiteral("last"), double(it->last)}});
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("items"), items}})
                .toJson(QJsonDocument::Compact));
    f.commit();
}

void UsageStore::record(const QString &id, qint64 nowSecs)
{
    QMutexLocker lock(&m_mutex);
    Entry &e = m_items[id];
    e.count = std::min(e.count + 1, 100000);
    e.last = nowOr(nowSecs);
    // Keep the file small: forget the least valuable entries.
    if (m_items.size() > 500) {
        const qint64 now = nowOr(nowSecs);
        QList<std::pair<double, QString>> ranked;
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
            ranked.append({weight(*it, now), it.key()});
        std::sort(ranked.begin(), ranked.end());
        for (qsizetype i = 0; i < ranked.size() - 400; ++i)
            m_items.remove(ranked[i].second);
    }
    save();
}

double UsageStore::weight(const Entry &e, qint64 now)
{
    const double days = double(now - e.last) / 86400.0;
    double w = 0.1;
    if (days < 4)
        w = 1.0;
    else if (days < 14)
        w = 0.7;
    else if (days < 31)
        w = 0.5;
    else if (days < 90)
        w = 0.3;
    return e.count * w;
}

double UsageStore::frecency(const QString &id, qint64 nowSecs) const
{
    QMutexLocker lock(&m_mutex);
    auto it = m_items.constFind(id);
    return it == m_items.constEnd() ? 0 : weight(*it, nowOr(nowSecs));
}

QStringList UsageStore::top(int limit, qint64 nowSecs) const
{
    QMutexLocker lock(&m_mutex);
    const qint64 now = nowOr(nowSecs);
    QList<std::pair<double, QString>> ranked;
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
        ranked.append({weight(*it, now), it.key()});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    QStringList out;
    for (const auto &r : ranked) {
        if (out.size() >= limit)
            break;
        out << r.second;
    }
    return out;
}

}  // namespace selsearch
