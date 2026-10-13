// SPDX-License-Identifier: MIT
#include "Compositor.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSet>
#include <QtEndian>

#include <algorithm>

namespace selsearch {

// ---- Hyprland --------------------------------------------------------------

QList<WindowInfo> parseHyprlandWindows(const QByteArray &clientsJson, const QByteArray &monitorsJson)
{
    QSet<qint64> visibleWorkspaces;
    for (const auto &m : QJsonDocument::fromJson(monitorsJson).array()) {
        const auto mon = m.toObject();
        if (mon.value(QLatin1String("disabled")).toBool())
            continue;
        visibleWorkspaces.insert(mon.value(QLatin1String("activeWorkspace")).toObject().value(QLatin1String("id")).toInteger());
        const auto special = mon.value(QLatin1String("specialWorkspace")).toObject();
        if (special.value(QLatin1String("id")).toInteger() != 0)
            visibleWorkspaces.insert(special.value(QLatin1String("id")).toInteger());
    }

    struct Entry {
        WindowInfo info;
        qint64 focusHistory;
        bool special;
    };
    QList<Entry> entries;
    for (const auto &c : QJsonDocument::fromJson(clientsJson).array()) {
        const auto o = c.toObject();
        if (!o.value(QLatin1String("mapped")).toBool(true) || o.value(QLatin1String("hidden")).toBool())
            continue;
        const qint64 ws = o.value(QLatin1String("workspace")).toObject().value(QLatin1String("id")).toInteger();
        if (!visibleWorkspaces.contains(ws))
            continue;
        const auto at = o.value(QLatin1String("at")).toArray();
        const auto size = o.value(QLatin1String("size")).toArray();
        WindowInfo w;
        w.title = o.value(QLatin1String("title")).toString();
        w.appId = o.value(QLatin1String("class")).toString();
        w.geometry = QRect(at.at(0).toInt(), at.at(1).toInt(), size.at(0).toInt(), size.at(1).toInt());
        w.floating = o.value(QLatin1String("floating")).toBool();
        const qint64 fh = o.value(QLatin1String("focusHistoryID")).toInteger(1 << 20);
        w.focused = fh == 0;
        if (w.geometry.isEmpty())
            continue;
        entries.append({w, fh, ws < 0});
    }
    // Topmost first: special workspace, then floating, then by focus recency.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.special != b.special)
            return a.special;
        if (a.info.floating != b.info.floating)
            return a.info.floating;
        return a.focusHistory < b.focusHistory;
    });
    QList<WindowInfo> out;
    for (const auto &e : entries)
        out << e.info;
    return out;
}

QString parseFocusedOutput(const QByteArray &outputsJson)
{
    for (const auto &m : QJsonDocument::fromJson(outputsJson).array()) {
        const auto o = m.toObject();
        if (o.value(QLatin1String("focused")).toBool())
            return o.value(QLatin1String("name")).toString();
    }
    return {};
}

class Hyprland final : public Compositor {
public:
    explicit Hyprland(QString socketPath)
        : m_socket(std::move(socketPath))
    {
    }
    QString id() const override { return QStringLiteral("hyprland"); }

    QList<WindowInfo> visibleWindows() override
    {
        return parseHyprlandWindows(request("j/clients"), request("j/monitors"));
    }

    QString focusedOutput() override { return parseFocusedOutput(request("j/monitors")); }

private:
    QByteArray request(const QByteArray &cmd)
    {
        QLocalSocket s;
        s.connectToServer(m_socket);
        if (!s.waitForConnected(500))
            return {};
        s.write(cmd);
        s.waitForBytesWritten(500);
        QByteArray out;
        // Hyprland closes the connection after replying.
        while (s.waitForReadyRead(500))
            out += s.readAll();
        out += s.readAll();
        return out;
    }
    QString m_socket;
};

// ---- sway (i3 IPC) ---------------------------------------------------------

static void walkSway(const QJsonObject &node, bool floating, QList<WindowInfo> &tiled, QList<WindowInfo> &floats)
{
    const QString type = node.value(QLatin1String("type")).toString();
    const bool isView = (type == QLatin1String("con") || type == QLatin1String("floating_con"))
        && node.contains(QLatin1String("pid")) && node.value(QLatin1String("nodes")).toArray().isEmpty();
    if (isView && node.value(QLatin1String("visible")).toBool()) {
        const auto r = node.value(QLatin1String("rect")).toObject();
        WindowInfo w;
        w.title = node.value(QLatin1String("name")).toString();
        w.appId = node.value(QLatin1String("app_id")).toString();
        if (w.appId.isEmpty())
            w.appId = node.value(QLatin1String("window_properties")).toObject().value(QLatin1String("class")).toString();
        w.geometry = QRect(r.value(QLatin1String("x")).toInt(), r.value(QLatin1String("y")).toInt(),
                           r.value(QLatin1String("width")).toInt(), r.value(QLatin1String("height")).toInt());
        w.focused = node.value(QLatin1String("focused")).toBool();
        w.floating = floating;
        (floating ? floats : tiled) << w;
    }
    for (const auto &n : node.value(QLatin1String("nodes")).toArray())
        walkSway(n.toObject(), floating, tiled, floats);
    for (const auto &n : node.value(QLatin1String("floating_nodes")).toArray())
        walkSway(n.toObject(), true, tiled, floats);
}

QList<WindowInfo> parseSwayTree(const QByteArray &treeJson)
{
    QList<WindowInfo> tiled, floats;
    walkSway(QJsonDocument::fromJson(treeJson).object(), false, tiled, floats);
    std::reverse(floats.begin(), floats.end());  // later floating nodes are drawn on top
    return floats + tiled;
}

class Sway final : public Compositor {
public:
    explicit Sway(QString socketPath)
        : m_socket(std::move(socketPath))
    {
    }
    QString id() const override { return QStringLiteral("sway"); }

    QList<WindowInfo> visibleWindows() override { return parseSwayTree(request(4)); }       // GET_TREE
    QString focusedOutput() override { return parseFocusedOutput(request(3)); }             // GET_OUTPUTS

private:
    QByteArray request(quint32 type)
    {
        QLocalSocket s;
        s.connectToServer(m_socket);
        if (!s.waitForConnected(500))
            return {};
        QByteArray msg("i3-ipc");
        quint32 len = 0;
        msg.append(reinterpret_cast<const char *>(&len), 4);
        msg.append(reinterpret_cast<const char *>(&type), 4);
        s.write(msg);
        s.waitForBytesWritten(500);
        QByteArray buf;
        while (buf.size() < 14 && s.waitForReadyRead(500))
            buf += s.readAll();
        if (buf.size() < 14)
            return {};
        const quint32 payloadLen = qFromLittleEndian<quint32>(buf.constData() + 6);
        while (quint32(buf.size()) < 14 + payloadLen && s.waitForReadyRead(500))
            buf += s.readAll();
        return buf.mid(14, payloadLen);
    }

    QString m_socket;
};

std::unique_ptr<Compositor> createCompositor()
{
    const QByteArray his = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his.isEmpty()) {
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        QString path = runtime + QStringLiteral("/hypr/") + QString::fromUtf8(his) + QStringLiteral("/.socket.sock");
        if (!QFile::exists(path))
            path = QStringLiteral("/tmp/hypr/") + QString::fromUtf8(his) + QStringLiteral("/.socket.sock");
        return std::make_unique<Hyprland>(path);
    }
    const QString sway = qEnvironmentVariable("SWAYSOCK");
    if (!sway.isEmpty())
        return std::make_unique<Sway>(sway);
    return nullptr;
}

}  // namespace selsearch
