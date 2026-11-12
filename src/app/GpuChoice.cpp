// SPDX-License-Identifier: MIT
#include "GpuChoice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <optional>

namespace selsearch {

namespace {

const char *kNvidiaVendor = "/usr/share/glvnd/egl_vendor.d/10_nvidia.json";
const char *kMesaVendor = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";

QString readSmall(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.read(256)).trimmed() : QString();
}

struct Card {
    QString name;    // card0
    QString vendor;  // 0x10de
    QString device;  // /sys/class/drm/card0/device (resolved)
};

QList<Card> cards()
{
    QList<Card> out;
    const QDir drm(QStringLiteral("/sys/class/drm"));
    static const QRegularExpression cardRe(QStringLiteral("^card\\d+$"));
    for (const QString &n : drm.entryList(QDir::Dirs | QDir::System)) {
        if (!cardRe.match(n).hasMatch())
            continue;
        const QString dev = QFileInfo(drm.filePath(n) + QStringLiteral("/device")).canonicalFilePath();
        out.append({n, readSmall(drm.filePath(n) + QStringLiteral("/device/vendor")), dev});
    }
    return out;
}

bool isNvidia(const Card &c) { return c.vendor == QLatin1String("0x10de"); }

// general.gpu from the config file, read by hand: the Config class needs the
// application object, which must not exist yet.
QString configuredGpu()
{
    QString dir = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.config");
    QFile f(dir + QStringLiteral("/selsearch/config.toml"));
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("auto");
    bool inGeneral = false;
    static const QRegularExpression table(QStringLiteral("^\\s*\\[([^\\]]+)\\]"));
    static const QRegularExpression key(QStringLiteral("^\\s*gpu\\s*=\\s*['\"](\\w+)['\"]"));
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine());
        if (const auto m = table.match(line); m.hasMatch()) {
            inGeneral = m.captured(1).trimmed() == QLatin1String("general");
            continue;
        }
        if (inGeneral)
            if (const auto m = key.match(line); m.hasMatch())
                return m.captured(1);
    }
    return QStringLiteral("auto");
}

// The compositor's main render device, if it was pinned in the environment
// (Hyprland: AQ_DRM_DEVICES, wlroots: WLR_DRM_DEVICES; first entry wins).
std::optional<Card> compositorPrimary(const QList<Card> &all)
{
    for (const char *var : {"AQ_DRM_DEVICES", "WLR_DRM_DEVICES"}) {
        const QString v = qEnvironmentVariable(var);
        if (v.isEmpty())
            continue;
        const QString first = QFileInfo(v.section(QLatin1Char(':'), 0, 0)).canonicalFilePath();  // by-path symlinks
        const QString name = QFileInfo(first).fileName();
        for (const Card &c : all)
            if (c.name == name)
                return c;
    }
    return std::nullopt;
}

}  // namespace

static QString applyGpuChoiceImpl();

bool hasHybridNvidia()
{
    const QList<Card> all = cards();
    bool nv = false, other = false;
    for (const Card &c : all)
        (isNvidia(c) ? nv : other) = true;
    return nv && other && QFile::exists(QString::fromLatin1(kNvidiaVendor));
}

static QString g_decision;

QString gpuDecision()
{
    return g_decision;
}

QString applyGpuChoice()
{
    g_decision = applyGpuChoiceImpl();
    return g_decision;
}

static QString applyGpuChoiceImpl()
{
    // Our own choice from before a restart (exec keeps the environment):
    // drop it and decide again, the setting may have changed.
    if (qEnvironmentVariable("SELSEARCH_GPU_SET") == QLatin1String("1")) {
        for (const char *var : {"__EGL_VENDOR_LIBRARY_FILENAMES", "__NV_PRIME_RENDER_OFFLOAD", "SELSEARCH_GPU_SET"})
            qunsetenv(var);
    }
    for (const char *var : {"__EGL_VENDOR_LIBRARY_FILENAMES", "__NV_PRIME_RENDER_OFFLOAD", "DRI_PRIME"})
        if (qEnvironmentVariableIsSet(var))
            return QStringLiteral("GPU: left to the environment (%1 is set)").arg(QLatin1String(var));
    if (!hasHybridNvidia())
        return {};
    const QString pref = configuredGpu();
    const QList<Card> all = cards();
    const Card *nvidia = nullptr;
    for (const Card &c : all)
        if (isNvidia(c))
            nvidia = &c;
    QString why;
    bool useNvidia = false;
    if (pref == QLatin1String("nvidia")) {
        useNvidia = true;
        why = QStringLiteral("general.gpu = nvidia");
    } else if (pref == QLatin1String("integrated")) {
        why = QStringLiteral("general.gpu = integrated");
    } else if (const auto primary = compositorPrimary(all); primary && isNvidia(*primary)) {
        useNvidia = true;
        why = QStringLiteral("the compositor renders on %1 (NVIDIA)").arg(primary->name);
    } else if (nvidia && readSmall(nvidia->device + QStringLiteral("/power/runtime_status")) == QLatin1String("active")) {
        useNvidia = true;
        why = QStringLiteral("%1 (NVIDIA) is awake, e.g. driving a monitor").arg(nvidia->name);
    } else {
        why = QStringLiteral("the NVIDIA GPU is asleep; leaving it asleep");
    }
    if (useNvidia) {
        qputenv("__EGL_VENDOR_LIBRARY_FILENAMES", kNvidiaVendor);
        qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");
        qputenv("SELSEARCH_GPU_SET", "1");
        return QStringLiteral("GPU: NVIDIA (%1)").arg(why);
    }
    if (QFile::exists(QString::fromLatin1(kMesaVendor))) {
        qputenv("__EGL_VENDOR_LIBRARY_FILENAMES", kMesaVendor);
        qputenv("SELSEARCH_GPU_SET", "1");
    }
    return QStringLiteral("GPU: integrated (%1)").arg(why);
}

}  // namespace selsearch
