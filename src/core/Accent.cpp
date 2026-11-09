// SPDX-License-Identifier: MIT
#include "Accent.h"

#include "Config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QJsonObject>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <QGuiApplication>
#include <QPalette>
#else
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#endif

#include <optional>

Q_LOGGING_CATEGORY(lcAccent, "selsearch.accent")

namespace selsearch {

QColor defaultAccent()
{
    return QColor(QStringLiteral("#8e9bff"));
}

#ifdef Q_OS_WIN
// The accent from Windows Settings > Personalisation > Colours.
static std::optional<QColor> portalAccent()
{
    if (!qGuiApp)
        return std::nullopt;
    const QColor c = QGuiApplication::palette().color(QPalette::Accent);
    return c.isValid() ? std::optional<QColor>(c) : std::nullopt;
}
#else
static std::optional<QColor> portalAccent()
{
    static std::optional<std::optional<QColor>> cached;
    if (cached)
        return *cached;
    std::optional<QColor> result;
    QDBusInterface iface(QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
                         QStringLiteral("org.freedesktop.portal.Settings"));
    iface.setTimeout(300);
    if (iface.isValid()) {
        QDBusReply<QDBusVariant> reply = iface.call(QStringLiteral("ReadOne"), QStringLiteral("org.freedesktop.appearance"),
                                                    QStringLiteral("accent-color"));
        if (reply.isValid()) {
            // (ddd) in 0..1; values outside the range mean "unset".
            const QDBusArgument arg = reply.value().variant().value<QDBusArgument>();
            double r = -1, g = -1, b = -1;
            arg.beginStructure();
            arg >> r >> g >> b;
            arg.endStructure();
            if (r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1)
                result = QColor::fromRgbF(float(r), float(g), float(b));
        }
    }
    cached = result;
    return result;
}
#endif

namespace {

QString envDir(const char *var, const QString &fallback)
{
    const QString v = qEnvironmentVariable(var);
    return v.isEmpty() ? fallback : v;
}

QString pywalPath()
{
    return envDir("XDG_CACHE_HOME", QDir::homePath() + QStringLiteral("/.cache")) + QStringLiteral("/wal/colors.json");
}

// Noctalia files that carry the primary (accent) colour, and how to read it.
// Noctalia 4 writes the palette to <config dir>/colors.json
// ($NOCTALIA_CONFIG_DIR, else $XDG_CONFIG_HOME/noctalia). Noctalia 5 keeps
// the palette in memory and only writes it through the theme templates the
// user enables (assets/templates/builtin.toml); each of these carries
// colors.primary, so whichever the user has enabled works.
struct NoctaliaFile {
    QString path;
    QRegularExpression pattern;  // capture 1: rrggbb, or r,g,b (kde)
};

const QList<NoctaliaFile> &noctaliaFiles()
{
    static const QList<NoctaliaFile> files = [] {
        const QString cfg = envDir("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config"));
        const QString data = envDir("XDG_DATA_HOME", QDir::homePath() + QStringLiteral("/.local/share"));
        const QString v4dir = qEnvironmentVariable("NOCTALIA_CONFIG_DIR");
        const QString v4 = v4dir.isEmpty() ? cfg + QStringLiteral("/noctalia/colors.json") : QDir(v4dir).filePath(QStringLiteral("colors.json"));
        auto re = [](const char *p) { return QRegularExpression(QString::fromLatin1(p)); };
        return QList<NoctaliaFile>{
            {v4, {}},  // JSON, mPrimary
            {cfg + QStringLiteral("/hypr/noctalia.lua"), re(R"re(\bprimary\s*=\s*"rgb\(([0-9a-fA-F]{6})\)")re")},
            {cfg + QStringLiteral("/hypr/noctalia.conf"), re(R"re(\$primary\s*=\s*rgb\(([0-9a-fA-F]{6})\))re")},
            {cfg + QStringLiteral("/niri/noctalia.kdl"), re(R"re(active-color\s+"#([0-9a-fA-F]{6})")re")},
            {cfg + QStringLiteral("/gtk-4.0/noctalia.css"), re(R"re(@define-color accent_color #([0-9a-fA-F]{6}))re")},
            {cfg + QStringLiteral("/gtk-3.0/noctalia.css"), re(R"re(@define-color accent_color #([0-9a-fA-F]{6}))re")},
            {cfg + QStringLiteral("/kitty/themes/noctalia.conf"), re(R"re(active_border_color\s+#([0-9a-fA-F]{6}))re")},
            {cfg + QStringLiteral("/sway/noctalia"), re(R"re(set \$primary\s+#([0-9a-fA-F]{6}))re")},
            {cfg + QStringLiteral("/mango/noctalia.conf"), re(R"re(focus_color\s*=\s*0x([0-9a-fA-F]{6}))re")},
            {data + QStringLiteral("/color-schemes/noctalia.colors"), re(R"re(DecorationFocus=(\d+,\d+,\d+))re")},
        };
    }();
    return files;
}

// The Noctalia file to read: the most recently written one that exists (all
// are rewritten together on a palette change; a stale one from an old setup
// loses). Noctalia 4's colors.json when none exists, for the status text.
QString noctaliaPath()
{
    QString best;
    QDateTime bestTime;
    for (const NoctaliaFile &f : noctaliaFiles()) {
        const QFileInfo fi(f.path);
        if (fi.exists() && (best.isEmpty() || fi.lastModified() > bestTime)) {
            best = f.path;
            bestTime = fi.lastModified();
        }
    }
    return best.isEmpty() ? noctaliaFiles().first().path : best;
}

std::optional<QColor> fromNoctaliaTemplate(const QString &path, const QByteArray &data)
{
    for (const NoctaliaFile &f : noctaliaFiles()) {
        if (f.path != path || !f.pattern.isValid() || f.pattern.pattern().isEmpty())
            continue;
        const QRegularExpressionMatch m = f.pattern.match(QString::fromUtf8(data));
        if (!m.hasMatch())
            return std::nullopt;
        const QString v = m.captured(1);
        if (v.contains(QLatin1Char(','))) {
            const QStringList rgb = v.split(QLatin1Char(','));
            return QColor(rgb.value(0).toInt(), rgb.value(1).toInt(), rgb.value(2).toInt());
        }
        return QColor(QLatin1Char('#') + v);
    }
    return std::nullopt;
}

QString expandHome(QString path)
{
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    return path;
}

std::optional<QColor> colorValue(const QJsonValue &v)
{
    const QColor c(v.toString());
    return c.isValid() ? std::optional<QColor>(c) : std::nullopt;
}

// pywal: the most vivid of color1..color6 (color0/7/8/15 are the
// background/foreground greys), lifted a little when too dark to show on
// the darkened screen.
std::optional<QColor> fromPywal(const QJsonObject &root)
{
    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    std::optional<QColor> best;
    double bestScore = -1;
    for (int i = 1; i <= 6; ++i) {
        const auto c = colorValue(colors.value(QStringLiteral("color%1").arg(i)));
        if (!c)
            continue;
        const double score = c->hslSaturationF() * (0.35 + std::min(double(c->lightnessF()), 0.65));
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }
    if (best && best->lightnessF() < 0.45)
        best = QColor::fromHslF(best->hslHueF(), best->hslSaturationF(), 0.55);
    return best;
}

// Noctalia: flat {"mPrimary": ...} (what Noctalia 4 writes); palette files
// with "dark"/"light" objects are accepted too.
std::optional<QColor> fromNoctalia(const QJsonObject &root)
{
    if (auto c = colorValue(root.value(QStringLiteral("mPrimary"))))
        return c;
    for (const char *mode : {"dark", "light"})
        if (auto c = colorValue(root.value(QLatin1String(mode)).toObject().value(QStringLiteral("mPrimary"))))
            return c;
    return std::nullopt;
}

std::optional<QColor> readAccentFile(const QString &kind, const QString &path)
{
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray data = f.read(256 * 1024);
    if (kind == QLatin1String("noctalia") && !path.endsWith(QLatin1String(".json")))
        return fromNoctaliaTemplate(path, data);
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (kind == QLatin1String("pywal"))
            return fromPywal(root);
        if (kind == QLatin1String("noctalia"))
            return fromNoctalia(root);
        // "file": whichever known shape it has.
        if (auto c = fromNoctalia(root))
            return c;
        for (const char *key : {"accent", "primary"})
            if (auto c = colorValue(root.value(QLatin1String(key))))
                return c;
        if (root.contains(QStringLiteral("colors")))
            return fromPywal(root);
    }
    if (kind != QLatin1String("file"))
        return std::nullopt;
    // Plain text (a template output): the first #rrggbb in it.
    static const QRegularExpression hex(QStringLiteral("#[0-9a-fA-F]{6}\\b"));
    const QRegularExpressionMatch m = hex.match(QString::fromUtf8(data));
    if (m.hasMatch())
        return QColor(m.captured());
    return std::nullopt;
}

}  // namespace

QString accentSource(const Config &config)
{
    const QString s = config.string(QStringLiteral("general.accent_source"));
    if (!s.isEmpty())
        return s;
    // Before accent_source existed: an explicit colour wins over the portal.
    if (!QColor(config.string(QStringLiteral("general.accent"))).isValid()
        && config.boolean(QStringLiteral("general.follow_portal_accent")))
        return QStringLiteral("portal");
    return QStringLiteral("selsearch");
}

QString accentSourcePath(const Config &config)
{
    const QString s = accentSource(config);
    if (s == QLatin1String("pywal"))
        return pywalPath();
    if (s == QLatin1String("noctalia"))
        return noctaliaPath();
    if (s == QLatin1String("file"))
        return expandHome(config.string(QStringLiteral("general.accent_file")));
    return {};
}

QStringList accentWatchPaths(const Config &config)
{
    if (accentSource(config) == QLatin1String("noctalia")) {
        QStringList out;
        for (const NoctaliaFile &f : noctaliaFiles())
            out << f.path;
        return out;
    }
    const QString p = accentSourcePath(config);
    return p.isEmpty() ? QStringList() : QStringList{p};
}

std::optional<QColor> accentFromSource(const Config &config)
{
    const QString s = accentSource(config);
    if (s == QLatin1String("portal"))
        return portalAccent();
    if (s == QLatin1String("pywal") || s == QLatin1String("noctalia") || s == QLatin1String("file")) {
        // Theme tools rewrite these files in place (truncate, then write) or
        // via unlink + create; a read in between finds nothing usable. Keep
        // the last colour read from the same file instead of flashing the
        // fallback colour.
        static QHash<QString, QColor> lastGood;
        const QString path = accentSourcePath(config);
        if (auto c = readAccentFile(s, path)) {
            lastGood.insert(s + QLatin1Char('|') + path, *c);
            return c;
        }
        const QColor last = lastGood.value(s + QLatin1Char('|') + path);
        if (last.isValid())
            return last;
        return std::nullopt;
    }
    return std::nullopt;
}

QColor accentColor(const Config &config)
{
    if (auto c = accentFromSource(config))
        return *c;
    const QColor explicitColor(config.string(QStringLiteral("general.accent")));
    if (explicitColor.isValid())
        return explicitColor;
    return defaultAccent();
}

AccentWatcher::AccentWatcher(const Config &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    // File events come in bursts while a tool writes (truncate, write,
    // close; or unlink, create, rename): read once things settle, and once
    // more a little later in case the last write raced the first read.
    m_settle.setSingleShot(true);
    m_settle.setInterval(150);
    m_confirm.setSingleShot(true);
    m_confirm.setInterval(1200);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        check();
        m_confirm.start();
    });
    connect(&m_confirm, &QTimer::timeout, this, &AccentWatcher::check);
    auto soon = [this] { m_settle.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, soon);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, soon);
    connect(&config, &Config::changed, this, [this](const QString &key) {
        if (key.startsWith(QLatin1String("general.accent")) || key == QLatin1String("general.follow_portal_accent"))
            check();
    });
    connect(&config, &Config::reloaded, this, &AccentWatcher::check);
    // Safety net for what inotify does not see (a symlinked file whose target
    // is replaced elsewhere, a recreated directory): compare the file's
    // modification time every few seconds. One stat() per poll.
    m_poll.setInterval(3000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (stamps() != m_stamp)
            m_settle.start();
    });
    check();
}

void AccentWatcher::check()
{
    rewatch();  // a replaced file has a new inode: watch it again
    m_stamp = stamps();
    const QColor c = accentColor(m_config);
    if (c == m_last)
        return;
    m_last = c;
    qCInfo(lcAccent).noquote() << "accent" << c.name() << "from" << accentSource(m_config)
                               << accentSourcePath(m_config);
    Q_EMIT changed();
}

QString AccentWatcher::stamps() const
{
    QString out;
    for (const QString &p : m_paths) {
        const QFileInfo fi(p);
        if (fi.exists())
            out += QStringLiteral("%1:%2/%3;").arg(p).arg(fi.lastModified().toMSecsSinceEpoch()).arg(fi.size());
    }
    return out;
}

void AccentWatcher::rewatch()
{
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_paths = accentWatchPaths(m_config);
    if (m_paths.isEmpty()) {
        m_poll.stop();
        return;
    }
    QStringList watch;
    for (const QString &path : std::as_const(m_paths)) {
        const QFileInfo fi(path);
        watch << fi.absolutePath() << path;
        // A symlink: watch where it points as well.
        const QString target = fi.canonicalFilePath();
        if (!target.isEmpty() && target != fi.absoluteFilePath())
            watch << target << QFileInfo(target).absolutePath();
    }
    watch.removeDuplicates();
    for (const QString &p : std::as_const(watch))
        if (QFileInfo::exists(p))
            m_watcher.addPath(p);
    if (!m_poll.isActive())
        m_poll.start();
}

}  // namespace selsearch
