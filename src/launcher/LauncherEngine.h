// SPDX-License-Identifier: MIT
#pragma once

#include "Provider.h"

#include <QObject>
#include <QUrl>

#include <memory>

namespace selsearch {

class AppsProvider;
class CalculatorProvider;
class Config;
class FilesProvider;
class WebProvider;

// What the UI should do after activating a row.
struct ActivateResult {
    enum Status { Done, OpenUrl, Error };
    Status status = Done;
    QUrl url;       // for OpenUrl: open in the results panel or the browser (daemon decides)
    QString error;  // for Error: short, user-facing
};

// Owns the launcher providers and turns a query into grouped rows.
//
// query() answers synchronously from the cheap providers (calculator, apps,
// web) and emits resultsReady() right away; the files provider runs on a
// worker thread and a second resultsReady() with the same generation follows
// when it is done. Results of superseded generations are dropped.
//
// Grouping: Calculator, Apps, Files, Web. launcher.max_results caps the rows
// before the Web group (filled round-robin so one group can't starve the
// others); the Web group (search row, plus "Open <url>") is always present
// for a non-empty query.
class LauncherEngine : public QObject {
    Q_OBJECT
public:
    explicit LauncherEngine(Config *config, QObject *parent = nullptr);
    ~LauncherEngine() override;

    // Kick off the initial app scan and file indexing (in the background).
    void start();

    // Returns the generation of this query.
    quint64 query(const QString &text);
    ActivateResult activate(const LauncherItem &item, const QString &actionId = {});

    quint64 generation() const { return m_generation; }
    AppsProvider *apps() const { return m_apps.get(); }
    FilesProvider *files() const { return m_files.get(); }

    // The merge step, exposed for tests.
    static QList<LauncherItem> merge(QList<LauncherItem> rows, int maxResults);

Q_SIGNALS:
    void resultsReady(const QList<LauncherItem> &items, quint64 generation);

private:
    Provider *providerFor(const LauncherItem &item) const;
    void applyConfig();

    Config *m_config;
    std::unique_ptr<CalculatorProvider> m_calc;
    std::unique_ptr<AppsProvider> m_apps;
    std::unique_ptr<FilesProvider> m_files;
    std::unique_ptr<WebProvider> m_web;
    quint64 m_generation = 0;
    QString m_text;
    QList<LauncherItem> m_syncRows;  // rows of the current generation so far
};

}  // namespace selsearch
