// SPDX-License-Identifier: MIT
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace selsearch {

// A launcher result row.
struct LauncherItem {
    enum Kind { App, File, Folder, Calculator, WebSearch, Url, History, Action };
    Kind kind = App;
    QString id;        // stable id, e.g. desktop file id or absolute path
    QString title;
    QString subtitle;
    QString icon;      // freedesktop icon name, absolute path, or "lucide:<name>"
    QString group;     // "Apps", "Files", "Web", ... (rows are grouped by this)
    double score = 0;  // higher is better, comparable within one provider
    QVariantMap data;  // provider-specific payload passed back to activate()
};

// Providers answer a query synchronously from in-memory state when they can.
// Slow providers (file search) run query() on a worker thread and must be
// thread-safe for concurrent query() calls with refresh() guarded internally.
class Provider {
public:
    virtual ~Provider() = default;
    virtual QString id() const = 0;
    // Build or refresh indexes. Called once at startup on a worker thread and
    // again when the provider signals staleness.
    virtual void refresh() {}
    virtual QList<LauncherItem> query(const QString &text, int limit) = 0;
    // Perform the default action. Returns false when it could not be done.
    virtual bool activate(const LauncherItem &item) = 0;
};

}  // namespace selsearch
