// SPDX-License-Identifier: MIT
#include "AppsProvider.h"

#include "FuzzyMatcher.h"
#include "Launch.h"
#ifdef Q_OS_WIN
#include "WindowsApps.h"
#endif

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace selsearch {

struct AppsProvider::Indexed {
    DesktopEntry entry;
    // Folded copies for matching.
    QString name;
    QString genericName;
    QStringList keywords;
    QString execBase;
    QStringList actionNames;  // "firefox new private window"
};

struct AppsProvider::State {
    QList<Indexed> apps;
    QHash<QString, qsizetype> byId;
    QStringList watchDirs;
};

static void appendUnique(QStringList &list, const QString &dir)
{
    const QString clean = QDir::cleanPath(dir);
    if (!clean.isEmpty() && !list.contains(clean))
        list << clean;
}

QStringList AppsProvider::defaultApplicationDirs()
{
#ifdef Q_OS_WIN
    return winshell::startMenuDirs();
#endif
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty() || !QDir::isAbsolutePath(dataHome))
        dataHome = QDir::homePath() + QStringLiteral("/.local/share");
    QStringList dataDirs = qEnvironmentVariable("XDG_DATA_DIRS").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    dataDirs.removeIf([](const QString &d) { return !QDir::isAbsolutePath(d); });
    if (dataDirs.isEmpty())
        dataDirs = {QStringLiteral("/usr/local/share"), QStringLiteral("/usr/share")};

    QStringList out;
    appendUnique(out, dataHome + QStringLiteral("/applications"));
    // Flatpak normally adds its export dirs to XDG_DATA_DIRS via profile.d,
    // but sessions started without a login shell often miss them.
    QStringList flatpak{dataHome + QStringLiteral("/flatpak/exports/share"),
                        QStringLiteral("/var/lib/flatpak/exports/share")};
    for (const QString &d : flatpak) {
        if (!dataDirs.contains(d) && !dataDirs.contains(d + QLatin1Char('/')))
            appendUnique(out, d + QStringLiteral("/applications"));
    }
    for (const QString &d : dataDirs)
        appendUnique(out, d + QStringLiteral("/applications"));
    return out;
}

AppsProvider::AppsProvider(QObject *parent)
    : AppsProvider(Options{}, parent)
{
}

AppsProvider::AppsProvider(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_state(std::make_shared<State>())
{
    if (m_options.applicationDirs.isEmpty())
        m_options.applicationDirs = defaultApplicationDirs();
    if (m_options.locale.isEmpty())
        m_options.locale = desktopentry::currentLocale();
    if (m_options.desktops.isEmpty())
        m_options.desktops = desktopentry::currentDesktops();
    m_usage = std::make_unique<UsageStore>(m_options.usagePath.isEmpty() ? UsageStore::defaultPath() : m_options.usagePath);

    // Package managers touch many files at once; rescan once they settle.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(800);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        if (m_pending.isRunning())
            m_debounce.start();  // try again once the running scan is done
        else
            refreshAsync();
    });
    if (m_options.watch) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    }
}

AppsProvider::~AppsProvider()
{
    m_debounce.stop();
    // Wait for a scan that may still run on the pool.
    m_pending.waitForFinished();
}

void AppsProvider::refreshAsync()
{
    if (!m_pending.isRunning())
        m_pending = QtConcurrent::run([this] { refresh(); });
}

void AppsProvider::refresh()
{
    QMutexLocker refreshLock(&m_refreshMutex);
    auto state = std::make_shared<State>();
    QSet<QString> seen;
    for (const QString &root : std::as_const(m_options.applicationDirs)) {
        if (!QFileInfo(root).isDir())
            continue;
        state->watchDirs << root;
        // Sorted for deterministic results when a dir contains duplicate ids.
        QStringList files;
#ifdef Q_OS_WIN
        // Start Menu shortcuts (.url: e.g. games launched through a store).
        const QStringList patterns{QStringLiteral("*.desktop"), QStringLiteral("*.lnk"), QStringLiteral("*.url")};
#else
        const QStringList patterns{QStringLiteral("*.desktop")};
#endif
        QDirIterator it(root, patterns, QDir::Files | QDir::Readable,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext())
            files << it.next();
        files.sort();
        QDirIterator dirs(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext())
            state->watchDirs << dirs.next();

        for (const QString &path : std::as_const(files)) {
#ifdef Q_OS_WIN
            const bool shortcut = !path.endsWith(QLatin1String(".desktop"));
            const QString id = shortcut ? QDir(root).relativeFilePath(path) : desktopentry::desktopFileId(root, path);
#else
            const QString id = desktopentry::desktopFileId(root, path);
#endif
            if (seen.contains(id))
                continue;  // a higher-priority dir already defines (or hides) this id
#ifdef Q_OS_WIN
            auto e = shortcut ? winshell::shortcutEntry(root, path) : desktopentry::parseFile(path, id, m_options.locale);
#else
            auto e = desktopentry::parseFile(path, id, m_options.locale);
#endif
            if (!e)
                continue;
            seen.insert(id);
            if (!e->shouldShow(m_options.desktops))
                continue;
            Indexed ix;
            ix.name = foldForMatch(e->name);
            ix.genericName = foldForMatch(e->genericName);
            for (const QString &k : std::as_const(e->keywords))
                ix.keywords << foldForMatch(k);
            bool ok = false;
            const QStringList argv = desktopentry::splitExec(e->exec, &ok);
            if (ok && !argv.isEmpty()) {
                // Skip wrappers so "flatpak run org.foo.Bar" matches "bar".
                QString prog = QFileInfo(argv.first()).fileName();
                if (prog == QLatin1String("flatpak")) {
                    // flatpak run [--opts] org.vendor.App ...
                    for (const QString &a : argv.mid(1)) {
                        if (!a.startsWith(QLatin1Char('-')) && a != QLatin1String("run")) {
                            prog = a.section(QLatin1Char('.'), -1);
                            break;
                        }
                    }
                } else if (prog == QLatin1String("env")) {
                    for (const QString &a : argv.mid(1)) {
                        if (!a.contains(QLatin1Char('=')) && !a.startsWith(QLatin1Char('-'))) {
                            prog = QFileInfo(a).fileName();
                            break;
                        }
                    }
                }
                ix.execBase = foldForMatch(prog);
            }
            for (const DesktopAction &a : std::as_const(e->actions))
                ix.actionNames << foldForMatch(e->name + QLatin1Char(' ') + a.name);
            ix.entry = std::move(*e);
            state->apps << std::move(ix);
        }
    }
    std::sort(state->apps.begin(), state->apps.end(), [](const Indexed &a, const Indexed &b) {
        return a.entry.name.compare(b.entry.name, Qt::CaseInsensitive) < 0;
    });
    for (qsizetype i = 0; i < state->apps.size(); ++i)
        state->byId.insert(state->apps[i].entry.id, i);
    {
        QMutexLocker lock(&m_mutex);
        m_state = std::move(state);
    }
    if (m_options.watch)
        QMetaObject::invokeMethod(this, &AppsProvider::updateWatches, Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, &AppsProvider::changed, Qt::QueuedConnection);
}

void AppsProvider::updateWatches()
{
    if (!m_watcher)
        return;
    std::shared_ptr<const State> state;
    {
        QMutexLocker lock(&m_mutex);
        state = m_state;
    }
    QStringList wanted = state->watchDirs;
    // Watch the parents of missing application dirs so creating
    // ~/.local/share/applications is noticed too.
    for (const QString &d : std::as_const(m_options.applicationDirs)) {
        if (!QFileInfo(d).isDir()) {
            const QString parent = QFileInfo(d).absolutePath();
            if (QFileInfo(parent).isDir())
                wanted << parent;
        }
    }
    const QStringList current = m_watcher->directories();
    QStringList remove;
    for (const QString &d : current) {
        if (!wanted.contains(d))
            remove << d;
    }
    if (!remove.isEmpty())
        m_watcher->removePaths(remove);
    QStringList add;
    for (const QString &d : std::as_const(wanted)) {
        if (!current.contains(d) && !add.contains(d))
            add << d;
    }
    if (!add.isEmpty())
        m_watcher->addPaths(add);
}

QList<DesktopEntry> AppsProvider::entries() const
{
    std::shared_ptr<const State> state;
    {
        QMutexLocker lock(&m_mutex);
        state = m_state;
    }
    QList<DesktopEntry> out;
    out.reserve(state->apps.size());
    for (const Indexed &ix : state->apps)
        out << ix.entry;
    return out;
}

std::optional<DesktopEntry> AppsProvider::entry(const QString &desktopId) const
{
    std::shared_ptr<const State> state;
    {
        QMutexLocker lock(&m_mutex);
        state = m_state;
    }
    auto it = state->byId.constFind(desktopId);
    if (it == state->byId.constEnd())
        return std::nullopt;
    return state->apps.at(*it).entry;
}

static LauncherItem appItem(const DesktopEntry &e, double score)
{
    LauncherItem item;
    item.kind = LauncherItem::App;
    item.id = e.id;
    item.title = e.name;
    item.subtitle = !e.genericName.isEmpty() && e.genericName != e.name ? e.genericName : e.comment;
    item.icon = e.icon.isEmpty() ? QStringLiteral("lucide:app-window") : e.icon;
    item.group = QStringLiteral("Apps");
    item.score = score;
    item.data.insert(QStringLiteral("desktopId"), e.id);
    for (const DesktopAction &a : e.actions)
        item.actions << LauncherAction{a.id, a.name, a.icon};
    return item;
}

QList<LauncherItem> AppsProvider::query(const QString &text, int limit)
{
    std::shared_ptr<const State> state;
    {
        QMutexLocker lock(&m_mutex);
        state = m_state;
    }
    QList<LauncherItem> out;
    if (limit <= 0)
        return out;

    const QString q = foldForMatch(QStringView(text).trimmed());
    if (q.isEmpty()) {
        // Most-used apps first.
        for (const QString &id : m_usage->top(limit * 2)) {
            auto it = state->byId.constFind(id);
            if (it == state->byId.constEnd())
                continue;
            out << appItem(state->apps.at(*it).entry, m_usage->frecency(id));
            if (out.size() >= limit)
                break;
        }
        return out;
    }

    for (const Indexed &ix : state->apps) {
        double s = fuzzyScore(q, ix.name);
        s = std::max(s, fuzzyScore(q, ix.genericName) * 0.75);
        for (const QString &k : ix.keywords)
            s = std::max(s, fuzzyScore(q, k) * 0.7);
        s = std::max(s, fuzzyScore(q, ix.execBase) * 0.6);
        const double usage = m_usage->frecency(ix.entry.id);
        const double bonus = std::min(120.0, 25.0 * std::log2(1.0 + usage));
        if (s > 0)
            out << appItem(ix.entry, s + bonus);

        // Desktop actions as secondary rows: need at least two characters and
        // always rank below the app itself.
        if (q.size() < 2)
            continue;
        for (qsizetype i = 0; i < ix.actionNames.size(); ++i) {
            const double as = fuzzyScore(q, ix.actionNames[i]);
            if (as <= 0)
                continue;
            const DesktopAction &a = ix.entry.actions[i];
            LauncherItem item;
            item.kind = LauncherItem::Action;
            item.id = ix.entry.id + QLatin1Char(':') + a.id;
            item.title = ix.entry.name + QStringLiteral(" — ") + a.name;
            item.subtitle = ix.entry.genericName.isEmpty() ? ix.entry.comment : ix.entry.genericName;
            item.icon = !a.icon.isEmpty() ? a.icon : (ix.entry.icon.isEmpty() ? QStringLiteral("lucide:app-window") : ix.entry.icon);
            item.group = QStringLiteral("Apps");
            item.score = std::min(as * 0.8 - 40 + bonus * 0.5, s > 0 ? s + bonus - 1 : 1e9);
            item.data.insert(QStringLiteral("desktopId"), ix.entry.id);
            item.data.insert(QStringLiteral("action"), a.id);
            out << item;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LauncherItem &a, const LauncherItem &b) { return a.score > b.score; });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

QStringList AppsProvider::launchArgv(const DesktopEntry &entry, const QString &actionId, const QStringList &files)
{
    QString exec = entry.exec;
    if (!actionId.isEmpty()) {
        exec.clear();
        for (const DesktopAction &a : entry.actions) {
            if (a.id == actionId)
                exec = a.exec;
        }
        if (exec.isEmpty())
            return {};
    }
    bool ok = false;
    QStringList argv = desktopentry::splitExec(exec, &ok);
    if (!ok || argv.isEmpty())
        return {};
    argv = desktopentry::expandFieldCodes(argv, entry, files);
    if (argv.isEmpty())
        return {};
    if (entry.terminal) {
        const QStringList term = terminalCommand();
        if (!term.isEmpty())
            argv = term + argv;
    }
    return argv;
}

bool AppsProvider::launch(const DesktopEntry &entry, const QString &actionId)
{
    QString error;
#ifdef Q_OS_WIN
    // Start Menu shortcuts: let the shell resolve and start them.
    if (entry.exec.isEmpty() && actionId.isEmpty() && !entry.filePath.endsWith(QLatin1String(".desktop"))) {
        const bool ok = winshell::open(entry.filePath, &error);
        if (ok)
            m_usage->record(entry.id);
        QMutexLocker lock(&m_mutex);
        m_error = ok ? QString() : error;
        return ok;
    }
#endif
    const QStringList argv = launchArgv(entry, actionId);
    if (argv.isEmpty()) {
        error = QStringLiteral("%1: invalid Exec line").arg(entry.id);
    } else if (launchDetached(argv, entry.workingDir, entry.id, &error)) {
        m_usage->record(entry.id);
        QMutexLocker lock(&m_mutex);
        m_error.clear();
        return true;
    }
    QMutexLocker lock(&m_mutex);
    m_error = error;
    return false;
}

bool AppsProvider::activate(const LauncherItem &item)
{
    const QString id = item.data.value(QStringLiteral("desktopId"), item.id).toString();
    auto e = entry(id);
    if (!e) {
        QMutexLocker lock(&m_mutex);
        m_error = QStringLiteral("%1 is no longer installed").arg(id);
        return false;
    }
    return launch(*e, item.data.value(QStringLiteral("action")).toString());
}

bool AppsProvider::activateAction(const LauncherItem &item, const QString &actionId)
{
    auto e = entry(item.data.value(QStringLiteral("desktopId"), item.id).toString());
    return e && launch(*e, actionId);
}

QString AppsProvider::lastError() const
{
    QMutexLocker lock(&m_mutex);
    return m_error;
}

}  // namespace selsearch
