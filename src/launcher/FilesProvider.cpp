// SPDX-License-Identifier: MIT
#include "FilesProvider.h"

#include "FuzzyMatcher.h"
#include "Launch.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <deque>
#include <vector>

#ifdef Q_OS_WIN
#include "WindowsApps.h"

#include <windows.h>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace selsearch {

struct FilesProvider::Index {
    QStringList dirs;  // parent directory paths, referenced by Node::parent
    struct Node {
        QString name;
        QString folded;  // shares name's data when folding changes nothing
        qint64 mtime = 0;
        qint32 parent = 0;
        quint8 depth = 0;
        bool dir = false;
    };
    std::vector<Node> nodes;
    bool complete = false;
};

struct FilesProvider::Candidate {
    double score = 0;
    QString path;
    QString name;
    bool dir = false;
};

FilesProvider::FilesProvider(QObject *parent)
    : FilesProvider(Options{}, parent)
{
}

FilesProvider::FilesProvider(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    m_reindex.setInterval(std::max(1, m_options.reindexMinutes) * 60 * 1000);
    connect(&m_reindex, &QTimer::timeout, this, &FilesProvider::startIndexing);
}

FilesProvider::~FilesProvider()
{
    ++m_generation;  // make a running walk bail out early
    m_future.waitForFinished();
}

QString FilesProvider::expandRoot(const QString &root)
{
    QString r = root.trimmed();
    if (r == QLatin1String("~"))
        r = QDir::homePath();
    else if (r.startsWith(QLatin1String("~/")))
        r = QDir::homePath() + r.mid(1);
    return QDir::cleanPath(QDir(r).absolutePath());
}

bool FilesProvider::isSkippedDir(const QString &name)
{
    static const QStringList skipped = {
        QStringLiteral("node_modules"), QStringLiteral("__pycache__"), QStringLiteral("build"),
        QStringLiteral("_build"), QStringLiteral("venv"), QStringLiteral("site-packages"),
        QStringLiteral("CMakeFiles"), QStringLiteral("lost+found"),
    };
    return name.startsWith(QLatin1Char('.'))  // .git, .cache, .local, ...
        || skipped.contains(name) || name.startsWith(QLatin1String("cmake-build-"));
}

QStringList FilesProvider::roots() const
{
    QMutexLocker lock(&m_mutex);
    QStringList out;
    for (const QString &r : m_options.roots)
        out << expandRoot(r);
    out.removeDuplicates();
    return out;
}

void FilesProvider::setRoots(const QStringList &roots)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_options.roots == roots)
            return;
        m_options.roots = roots;
        m_index.reset();
    }
    ++m_generation;
    m_future.waitForFinished();
    startIndexing();
}

bool FilesProvider::plocateUsable() const
{
    if (!m_options.allowPlocate)
        return false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_plocateFailed)
            return false;
    }
    static const bool installed = !QStandardPaths::findExecutable(QStringLiteral("plocate")).isEmpty();
    return installed && QFileInfo(m_options.plocateDatabase).isReadable();
}

bool FilesProvider::usingPlocate() const
{
    return plocateUsable();
}

std::shared_ptr<const FilesProvider::Index> FilesProvider::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    return m_index;
}

qsizetype FilesProvider::indexedCount() const
{
    auto idx = snapshot();
    return idx ? qsizetype(idx->nodes.size()) : 0;
}

void FilesProvider::startIndexing()
{
    if (plocateUsable()) {
        m_reindex.stop();
        return;
    }
    if (!m_reindex.isActive())
        m_reindex.start();
    if (m_indexing.exchange(true))
        return;  // already running
    m_future = QtConcurrent::run([this] {
        refresh();
    });
}

void FilesProvider::waitForIndex()
{
    m_future.waitForFinished();
}

void FilesProvider::refresh()
{
    if (plocateUsable()) {
        m_indexing = false;
        return;
    }
    m_indexing = true;
    const int generation = m_generation.load();
    const QStringList rootList = roots();
    const bool hadComplete = [this] {
        auto idx = snapshot();
        return idx && idx->complete;
    }();

    auto index = std::make_shared<Index>();
    auto publish = [&](bool complete) {
        auto copy = std::make_shared<Index>(*index);
        copy->complete = complete;
        QMutexLocker lock(&m_mutex);
        if (generation == m_generation.load())
            m_index = std::move(copy);
    };

    // Breadth-first so the entry cap drops the deepest paths first.
    struct Pending {
        QString path;
        int depth;
    };
    std::deque<Pending> queue;
    for (const QString &r : rootList)
        queue.push_back({r, 0});
    qsizetype nextPublish = 50000;

    while (!queue.empty() && index->nodes.size() < size_t(m_options.maxEntries)) {
        if (generation != m_generation.load())
            break;
        const Pending cur = std::move(queue.front());
        queue.pop_front();
#ifdef Q_OS_WIN
        // FindFirstFileEx with the basic info level skips 8.3 names and fetches
        // large batches; it is the fastest plain directory listing on NTFS.
        const std::wstring pattern = QDir::toNativeSeparators(cur.path + QStringLiteral("/*")).toStdWString();
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        const qint32 parentIdx = qint32(index->dirs.size());
        index->dirs << cur.path;
        do {
            if (fd.cFileName[0] == L'.')  // dotfiles, "." and ".."
                continue;
            // Hidden/system: AppData, desktop.ini, the profile's legacy junctions.
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                continue;
            Index::Node node;
            node.name = QString::fromWCharArray(fd.cFileName);
            const QString folded = foldForMatch(node.name);
            node.folded = folded == node.name ? node.name : folded;
            // FILETIME: 100 ns ticks since 1601.
            const quint64 ticks = (quint64(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            node.mtime = qint64(ticks / 10000000ULL) - 11644473600LL;
            node.parent = parentIdx;
            node.depth = quint8(std::min(cur.depth, 255));
            node.dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            // Junctions and symlinks are listed but not followed (loops);
            // cloud placeholder folders (OneDrive) are ordinary to walk.
            const bool link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                && (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK);
            const QString child = cur.path + QLatin1Char('/') + node.name;
            if (node.dir && !link && cur.depth + 1 < m_options.maxDepth && !isSkippedDir(node.name)
                && GetFileAttributesW(QDir::toNativeSeparators(child + QStringLiteral("/CACHEDIR.TAG")).toStdWString().c_str())
                    == INVALID_FILE_ATTRIBUTES) {
                queue.push_back({child, cur.depth + 1});
            }
            index->nodes.push_back(std::move(node));
            if (index->nodes.size() >= size_t(m_options.maxEntries))
                break;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
#else
        const QByteArray encoded = QFile::encodeName(cur.path);
        const int dfd = ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0)
            continue;
        DIR *dir = ::fdopendir(dfd);
        if (!dir) {
            ::close(dfd);
            continue;
        }
        const qint32 parentIdx = qint32(index->dirs.size());
        index->dirs << cur.path;
        while (struct dirent *de = ::readdir(dir)) {
            const char *n = de->d_name;
            if (n[0] == '.')  // hidden, "." and ".."
                continue;
            struct stat st;
            if (::fstatat(dfd, n, &st, AT_SYMLINK_NOFOLLOW) != 0)
                continue;
            Index::Node node;
            node.name = QFile::decodeName(n);
            const QString folded = foldForMatch(node.name);
            node.folded = folded == node.name ? node.name : folded;
            node.mtime = st.st_mtime;
            node.parent = parentIdx;
            node.depth = quint8(std::min(cur.depth, 255));
            node.dir = S_ISDIR(st.st_mode);
            if (node.dir && cur.depth + 1 < m_options.maxDepth && !isSkippedDir(node.name)
                && ::faccessat(dfd, (QByteArray(n) + "/CACHEDIR.TAG").constData(), F_OK, 0) != 0) {
                queue.push_back({cur.path + QLatin1Char('/') + node.name, cur.depth + 1});
            }
            index->nodes.push_back(std::move(node));
            if (index->nodes.size() >= size_t(m_options.maxEntries))
                break;
        }
        ::closedir(dir);  // also closes dfd
#endif
        // Until the first full index exists, let queries see partial results.
        if (!hadComplete && qsizetype(index->nodes.size()) >= nextPublish) {
            publish(false);
            nextPublish += 50000;
        }
    }
    const qsizetype count = qsizetype(index->nodes.size());
    if (generation == m_generation.load()) {
        index->complete = true;
        QMutexLocker lock(&m_mutex);
        m_index = std::move(index);
    }
    m_indexing = false;
    QMetaObject::invokeMethod(this, [this, count] { Q_EMIT indexed(count); }, Qt::QueuedConnection);
}

static double recencyBoost(qint64 mtime, qint64 now)
{
    const qint64 age = now - mtime;
    if (age < 86400)
        return 40;
    if (age < 7 * 86400)
        return 25;
    if (age < 30 * 86400)
        return 10;
    return 0;
}

// Folders first for exact names; otherwise a small nudge to shallow paths.
static double exactBoost(QStringView folded, QStringView query, bool dir)
{
    const qsizetype dot = folded.lastIndexOf(QLatin1Char('.'));
    const bool exact = folded == query || (dot > 0 && folded.left(dot) == query);
    if (!exact)
        return 0;
    return dir ? 60 : 30;
}

QList<LauncherItem> FilesProvider::query(const QString &text, int limit)
{
    const QString trimmed = text.trimmed();
    if (limit <= 0 || trimmed.size() < 2)
        return {};
    const QString folded = foldForMatch(trimmed);
    if (plocateUsable()) {
        bool ok = false;
        auto items = queryPlocate(trimmed, folded, limit, &ok);
        if (ok)
            return items;
        {
            QMutexLocker lock(&m_mutex);
            m_plocateFailed = true;
        }
        QMetaObject::invokeMethod(this, &FilesProvider::startIndexing, Qt::QueuedConnection);
        return {};
    }
    if (!snapshot() && !m_indexing.load())
        QMetaObject::invokeMethod(this, &FilesProvider::startIndexing, Qt::QueuedConnection);
    return queryIndex(folded, limit);
}

QList<LauncherItem> FilesProvider::queryIndex(const QString &folded, int limit) const
{
    auto idx = snapshot();
    if (!idx)
        return {};
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    std::vector<std::pair<double, size_t>> hits;
    for (size_t i = 0; i < idx->nodes.size(); ++i) {
        const Index::Node &n = idx->nodes[i];
        double s = fuzzyScore(folded, n.folded);
        if (s <= 0)
            continue;
        s += recencyBoost(n.mtime, now) - std::min(30.0, 3.0 * n.depth) + exactBoost(n.folded, folded, n.dir);
        hits.emplace_back(s, i);
    }
    const size_t keep = std::min(hits.size(), size_t(limit));
    std::partial_sort(hits.begin(), hits.begin() + keep, hits.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    QList<Candidate> candidates;
    for (size_t k = 0; k < keep; ++k) {
        const Index::Node &n = idx->nodes[hits[k].second];
        candidates.append({hits[k].first, idx->dirs.at(n.parent) + QLatin1Char('/') + n.name, n.name, n.dir});
    }
    return finish(candidates, limit);
}

QList<LauncherItem> FilesProvider::queryPlocate(const QString &text, const QString &folded, int limit, bool *ok) const
{
    *ok = false;
    const QStringList rootList = roots();
    QStringList args{QStringLiteral("-i"), QStringLiteral("-b"), QStringLiteral("-0"),
                     QStringLiteral("-d"), m_options.plocateDatabase,
                     // The limit applies before our root filter, so ask for more.
                     QStringLiteral("-l"), QString::number(std::max(2000, limit * 200)), QStringLiteral("--")};
    // Every word must appear in the basename (plocate ANDs patterns).
    args << text.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QProcess proc;
    proc.start(QStringLiteral("plocate"), args, QIODevice::ReadOnly);
    if (!proc.waitForStarted(1000))
        return {};
    if (!proc.waitForFinished(2500)) {
        proc.kill();
        proc.waitForFinished(500);
        *ok = true;  // slow, not broken
        return {};
    }
    // Exit status 1 just means "no matches".
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() > 1)
        return {};
    *ok = true;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QList<Candidate> candidates;
    const QByteArray out = proc.readAllStandardOutput();
    for (const QByteArray &raw : out.split('\0')) {
        if (raw.isEmpty())
            continue;
        const QString path = QFile::decodeName(raw);
        // Keep it under a root, without hidden or skipped components.
        QString rel;
        for (const QString &r : rootList) {
            const QString prefix = r.endsWith(QLatin1Char('/')) ? r : r + QLatin1Char('/');
            if (path.startsWith(prefix)) {
                rel = path.mid(prefix.size());
                break;
            }
        }
        if (rel.isEmpty())
            continue;
        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        bool skip = parts.size() > m_options.maxDepth || parts.last().startsWith(QLatin1Char('.'));
        for (qsizetype i = 0; i + 1 < parts.size() && !skip; ++i)
            skip = isSkippedDir(parts[i]);
        if (skip)
            continue;
        const QString name = parts.last();
        const QString nameFolded = foldForMatch(name);
        double s = fuzzyScore(folded, nameFolded);
        if (s <= 0)
            continue;
        const QFileInfo fi(path);
        if (!fi.exists())
            continue;  // the database is updated daily; skip deleted files
        const bool dir = fi.isDir();
        s += recencyBoost(fi.lastModified().toSecsSinceEpoch(), now) - std::min(30.0, 3.0 * (parts.size() - 1))
            + exactBoost(nameFolded, folded, dir);
        candidates.append({s, path, name, dir});
        if (candidates.size() >= 400)
            break;
    }
    return finish(candidates, limit);
}

QList<LauncherItem> FilesProvider::finish(QList<Candidate> &candidates, int limit) const
{
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    if (candidates.size() > limit)
        candidates.resize(limit);
    static const QMimeDatabase mimeDb;  // thread-safe
    const QString home = QDir::homePath();
    QList<LauncherItem> out;
    for (const Candidate &c : std::as_const(candidates)) {
        LauncherItem item;
        item.kind = c.dir ? LauncherItem::Folder : LauncherItem::File;
        item.id = c.path;
        item.title = c.name;
        QString parent = QFileInfo(c.path).path();
        if (parent == home || parent.startsWith(home + QLatin1Char('/')))
            parent = QLatin1Char('~') + parent.mid(home.size());
        item.subtitle = parent;
#ifdef Q_OS_WIN
        // No freedesktop icon theme on Windows: the shell's icon for the file.
        item.icon = winshell::iconId(c.path);
#else
        if (c.dir) {
            item.icon = QStringLiteral("folder");
        } else {
            const QMimeType mime = mimeDb.mimeTypeForFile(c.name, QMimeDatabase::MatchExtension);
            item.icon = mime.isDefault() ? QStringLiteral("lucide:file") : mime.iconName();
        }
#endif
        item.group = QStringLiteral("Files");
        item.score = c.score;
        item.data.insert(QStringLiteral("path"), c.path);
        item.actions << LauncherAction{QStringLiteral("open-folder"), QStringLiteral("Open containing folder"),
                                       QStringLiteral("lucide:folder")};
        out << item;
    }
    return out;
}

static bool openPath(const QString &path)
{
#ifdef Q_OS_WIN
    return winshell::open(path);
#endif
    if (!QStandardPaths::findExecutable(QStringLiteral("xdg-open")).isEmpty())
        return launchDetached({QStringLiteral("xdg-open"), path}, QFileInfo(path).absolutePath(), QStringLiteral("xdg-open"));
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool FilesProvider::activate(const LauncherItem &item)
{
    const QString path = item.data.value(QStringLiteral("path"), item.id).toString();
    if (!QFileInfo::exists(path))
        return false;
    return openPath(path);
}

bool FilesProvider::activateAction(const LauncherItem &item, const QString &actionId)
{
    if (actionId != QLatin1String("open-folder"))
        return false;
    const QString path = item.data.value(QStringLiteral("path"), item.id).toString();
    const QString parent = QFileInfo(path).absolutePath();
    if (!QFileInfo(parent).isDir())
        return false;
#ifdef Q_OS_WIN
    return winshell::reveal(path);
#else
    // Ask the file manager to reveal and select the item; fall back to just
    // opening the folder when no FileManager1 implementation answers.
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                      QStringLiteral("/org/freedesktop/FileManager1"),
                                                      QStringLiteral("org.freedesktop.FileManager1"),
                                                      QStringLiteral("ShowItems"));
    msg << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return openPath(parent);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [parent](QDBusPendingCallWatcher *w) {
        if (w->isError())
            openPath(parent);
        w->deleteLater();
    });
    return true;
#endif
}

}  // namespace selsearch
