// SPDX-License-Identifier: MIT
#pragma once

#include "Provider.h"

#include <QFuture>
#include <QMutex>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>

namespace selsearch {

// File and folder names under launcher.file_roots (default "~").
//
// Privacy: everything here is local. File names and paths never leave the
// machine; the provider talks to no network service and nothing is logged.
//
// Backends:
//  - plocate, when installed and its database is readable:
//    `plocate -i -b -0 -l N -- <words>`, filtered to the roots.
//  - otherwise an in-memory name index built in the background: walks the
//    roots breadth-first, skips hidden entries, node_modules, .git, .cache,
//    build dirs and dirs tagged with CACHEDIR.TAG, stops at maxEntries /
//    maxDepth, and rebuilds every reindexMinutes.
// query() is thread-safe and never waits for indexing: while the first index
// is being built it searches the partial snapshot published so far.
class FilesProvider : public QObject, public Provider {
    Q_OBJECT
public:
    struct Options {
        QStringList roots{QStringLiteral("~")};
        bool allowPlocate = true;
        QString plocateDatabase = QStringLiteral("/var/lib/plocate/plocate.db");
        int maxEntries = 400000;
        int maxDepth = 16;
        int reindexMinutes = 30;
    };

    explicit FilesProvider(QObject *parent = nullptr);
    explicit FilesProvider(Options options, QObject *parent = nullptr);
    ~FilesProvider() override;

    QString id() const override { return QStringLiteral("files"); }
    // Builds the in-memory index synchronously on the calling thread (no-op
    // with plocate). Safe to call from a worker thread.
    void refresh() override;
    QList<LauncherItem> query(const QString &text, int limit) override;
    bool activate(const LauncherItem &item) override;
    bool activateAction(const LauncherItem &item, const QString &actionId) override;

    // Start a background (re)index unless one is running. Returns immediately.
    void startIndexing();
    void setRoots(const QStringList &roots);  // triggers a re-index
    QStringList roots() const;
    bool usingPlocate() const;
    bool isIndexing() const { return m_indexing.load(); }
    qsizetype indexedCount() const;
    // Block until a running background index finishes (tests, shutdown).
    void waitForIndex();

    // "~" and "~/x" expanded, cleaned, absolute.
    static QString expandRoot(const QString &root);
    // Directory names never descended into.
    static bool isSkippedDir(const QString &name);

Q_SIGNALS:
    void indexed(qsizetype count);

private:
    struct Index;
    struct Candidate;
    std::shared_ptr<const Index> snapshot() const;
    QList<LauncherItem> queryIndex(const QString &folded, int limit) const;
    QList<LauncherItem> queryPlocate(const QString &text, const QString &folded, int limit, bool *ok) const;
    QList<LauncherItem> finish(QList<Candidate> &candidates, int limit) const;
    bool plocateUsable() const;

    Options m_options;
    mutable QMutex m_mutex;  // guards m_index, m_options.roots, m_plocateFailed
    std::shared_ptr<const Index> m_index;
    mutable bool m_plocateFailed = false;
    std::atomic_bool m_indexing{false};
    std::atomic_int m_generation{0};  // bumped by setRoots() to abandon stale walks
    QFuture<void> m_future;
    QTimer m_reindex;
};

}  // namespace selsearch
