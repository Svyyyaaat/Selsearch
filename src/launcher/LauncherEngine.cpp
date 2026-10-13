// SPDX-License-Identifier: MIT
#include "LauncherEngine.h"

#include "AppsProvider.h"
#include "CalculatorProvider.h"
#include "FilesProvider.h"
#include "WebProvider.h"
#include "core/Config.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace selsearch {

namespace {

const QString kCalculator = QStringLiteral("Calculator");
const QString kApps = QStringLiteral("Apps");
const QString kFiles = QStringLiteral("Files");
const QString kWeb = QStringLiteral("Web");

int groupRank(const QString &group)
{
    if (group == kCalculator)
        return 0;
    if (group == kApps)
        return 1;
    if (group == kFiles)
        return 2;
    if (group == kWeb)
        return 4;
    return 3;
}

void tag(QList<LauncherItem> &rows, const QString &provider)
{
    for (LauncherItem &r : rows)
        r.provider = provider;
}

}  // namespace

LauncherEngine::LauncherEngine(Config *config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_calc(std::make_unique<CalculatorProvider>())
    , m_apps(std::make_unique<AppsProvider>())
    , m_web(std::make_unique<WebProvider>(config))
{
    FilesProvider::Options fo;
    if (m_config)
        fo.roots = m_config->list(QStringLiteral("launcher.file_roots"));
    if (fo.roots.isEmpty())
        fo.roots = {QStringLiteral("~")};
    m_files = std::make_unique<FilesProvider>(fo);

    // Re-run the visible query when the app list changes underneath it.
    connect(m_apps.get(), &AppsProvider::changed, this, [this] {
        if (m_generation > 0)
            query(m_text);
    });
    if (m_config) {
        connect(m_config, &Config::changed, this, [this](const QString &key) {
            if (key.startsWith(QLatin1String("launcher.")))
                applyConfig();
        });
        connect(m_config, &Config::reloaded, this, &LauncherEngine::applyConfig);
    }
}

LauncherEngine::~LauncherEngine()
{
    ++m_generation;
    // Pending file queries reference m_files; let them finish first.
    const auto watchers = findChildren<QFutureWatcherBase *>();
    for (QFutureWatcherBase *w : watchers)
        w->waitForFinished();
}

static bool enabled(const Config *config, const char *key)
{
    return !config || config->boolean(QString::fromLatin1(key));
}

void LauncherEngine::start()
{
    if (enabled(m_config, "launcher.apps"))
        m_apps->refreshAsync();
    if (enabled(m_config, "launcher.files"))
        m_files->startIndexing();
}

void LauncherEngine::applyConfig()
{
    if (!m_config)
        return;
    QStringList roots = m_config->list(QStringLiteral("launcher.file_roots"));
    if (roots.isEmpty())
        roots = {QStringLiteral("~")};
    if (enabled(m_config, "launcher.files")) {
        if (roots != m_files->roots())
            m_files->setRoots(roots);
        else if (m_files->indexedCount() == 0)
            m_files->startIndexing();
    }
    if (enabled(m_config, "launcher.apps") && m_apps->entries().isEmpty())
        m_apps->refreshAsync();
}

quint64 LauncherEngine::query(const QString &text)
{
    const quint64 gen = ++m_generation;
    m_text = text;
    const int maxResults = m_config ? std::max(1, m_config->integer(QStringLiteral("launcher.max_results"))) : 8;
    const QString trimmed = text.trimmed();

    QList<LauncherItem> rows;
    if (enabled(m_config, "launcher.calculator") && !trimmed.isEmpty()) {
        auto r = m_calc->query(trimmed, 1);
        tag(r, m_calc->id());
        rows << r;
    }
    if (enabled(m_config, "launcher.apps")) {
        auto r = m_apps->query(trimmed, maxResults);
        tag(r, m_apps->id());
        rows << r;
    }
    {
        auto r = m_web->query(trimmed, 2);
        tag(r, m_web->id());
        rows << r;
    }
    m_syncRows = rows;
    Q_EMIT resultsReady(merge(rows, maxResults), gen);

    if (enabled(m_config, "launcher.files") && trimmed.size() >= 2) {
        auto *watcher = new QFutureWatcher<QList<LauncherItem>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, gen, maxResults] {
            watcher->deleteLater();
            if (gen != m_generation)
                return;  // superseded by a newer query
            QList<LauncherItem> files = watcher->result();
            if (files.isEmpty())
                return;
            tag(files, m_files->id());
            Q_EMIT resultsReady(merge(m_syncRows + files, maxResults), gen);
        });
        FilesProvider *fp = m_files.get();
        watcher->setFuture(QtConcurrent::run([fp, trimmed, maxResults] { return fp->query(trimmed, maxResults); }));
    }
    return gen;
}

QList<LauncherItem> LauncherEngine::merge(QList<LauncherItem> rows, int maxResults)
{
    std::stable_sort(rows.begin(), rows.end(), [](const LauncherItem &a, const LauncherItem &b) {
        const int ga = groupRank(a.group), gb = groupRank(b.group);
        if (ga != gb)
            return ga < gb;
        // In the Web group "Open <url>" goes first and the search row last.
        if (ga == 4 && a.kind != b.kind)
            return a.kind == LauncherItem::Url;
        return a.score > b.score;
    });

    // Split into consecutive group buckets.
    struct Bucket {
        qsizetype begin, size, cap, perRound;
        qsizetype taken = 0;
    };
    QList<Bucket> buckets;
    QList<LauncherItem> web;
    for (qsizetype i = 0; i < rows.size();) {
        qsizetype j = i;
        while (j < rows.size() && rows[j].group == rows[i].group)
            ++j;
        if (rows[i].group == kWeb) {
            web << rows.mid(i, j - i);
        } else {
            const bool calc = rows[i].group == kCalculator;
            const qsizetype cap = calc ? 1 : qsizetype(std::ceil(maxResults * 0.6));
            buckets.append({i, j - i, std::min(cap, j - i), rows[i].group == kApps ? 2 : 1});
        }
        i = j;
    }
    // Round-robin allocation so one group cannot starve the others; apps get
    // two slots per round. Second phase: lift the per-group caps (except the
    // calculator's) so a short group leaves its budget to the others.
    qsizetype total = 0;
    for (int phase = 0; phase < 2; ++phase) {
        if (phase == 1) {
            for (Bucket &b : buckets) {
                if (rows[b.begin].group != kCalculator)
                    b.cap = b.size;
            }
        }
        for (bool progress = true; progress && total < maxResults;) {
            progress = false;
            for (Bucket &b : buckets) {
                for (qsizetype k = 0; k < b.perRound && b.taken < b.cap && total < maxResults; ++k) {
                    ++b.taken;
                    ++total;
                    progress = true;
                }
            }
        }
    }

    QList<LauncherItem> out;
    for (const Bucket &b : std::as_const(buckets))
        out << rows.mid(b.begin, b.taken);
    out << web;
    return out;
}

Provider *LauncherEngine::providerFor(const LauncherItem &item) const
{
    const QString p = item.provider;
    if (p == m_apps->id())
        return m_apps.get();
    if (p == m_files->id())
        return m_files.get();
    if (p == m_calc->id())
        return m_calc.get();
    if (p == m_web->id())
        return m_web.get();
    switch (item.kind) {
    case LauncherItem::App:
    case LauncherItem::Action:
        return m_apps.get();
    case LauncherItem::File:
    case LauncherItem::Folder:
        return m_files.get();
    case LauncherItem::Calculator:
        return m_calc.get();
    case LauncherItem::WebSearch:
    case LauncherItem::Url:
        return m_web.get();
    default:
        return nullptr;
    }
}

ActivateResult LauncherEngine::activate(const LauncherItem &item, const QString &actionId)
{
    ActivateResult result;
    if (item.kind == LauncherItem::Url || item.kind == LauncherItem::WebSearch) {
        const QUrl url = WebProvider::urlFor(item);
        if (!url.isValid())
            return {ActivateResult::Error, {}, QStringLiteral("Invalid address")};
        return {ActivateResult::OpenUrl, url, {}};
    }
    Provider *p = providerFor(item);
    if (!p)
        return {ActivateResult::Error, {}, QStringLiteral("Nothing to do for this item")};
    const bool ok = actionId.isEmpty() ? p->activate(item) : p->activateAction(item, actionId);
    if (ok)
        return result;
    result.status = ActivateResult::Error;
    if (p == m_apps.get() && !m_apps->lastError().isEmpty())
        result.error = m_apps->lastError();
    else if (p == m_files.get())
        result.error = QStringLiteral("Could not open %1").arg(item.title);
    else if (p == m_calc.get())
        result.error = QStringLiteral("Could not copy the result");
    else
        result.error = QStringLiteral("Could not open %1").arg(item.title);
    return result;
}

}  // namespace selsearch
