// SPDX-License-Identifier: MIT
#include "Accent.h"

#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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

// Noctalia 4 writes the active colours to <config dir>/colors.json; its
// config dir is $NOCTALIA_CONFIG_DIR, else $XDG_CONFIG_HOME/noctalia.
QString noctaliaPath()
{
    const QString dir = qEnvironmentVariable("NOCTALIA_CONFIG_DIR");
    if (!dir.isEmpty())
        return QDir(dir).filePath(QStringLiteral("colors.json"));
    return envDir("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config")) + QStringLiteral("/noctalia/colors.json");
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

std::optional<QColor> accentFromSource(const Config &config)
{
    const QString s = accentSource(config);
    if (s == QLatin1String("portal"))
        return portalAccent();
    if (s == QLatin1String("pywal") || s == QLatin1String("noctalia") || s == QLatin1String("file"))
        return readAccentFile(s, accentSourcePath(config));
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
    auto check = [this] {
        rewatch();  // a replaced file has a new inode: watch it again
        const QColor c = accentColor(m_config);
        if (c != m_last) {
            m_last = c;
            Q_EMIT changed();
        }
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, check);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, check);
    connect(&config, &Config::changed, this, [this, check](const QString &key) {
        if (key.startsWith(QLatin1String("general.accent")))
            check();
    });
    connect(&config, &Config::reloaded, this, check);
    rewatch();
    m_last = accentColor(m_config);
}

void AccentWatcher::rewatch()
{
    const QString path = accentSourcePath(m_config);
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_path = path;
    if (path.isEmpty())
        return;
    const QString dir = QFileInfo(path).absolutePath();
    if (QFileInfo(dir).isDir())
        m_watcher.addPath(dir);
    if (QFileInfo::exists(path))
        m_watcher.addPath(path);
}

}  // namespace selsearch
