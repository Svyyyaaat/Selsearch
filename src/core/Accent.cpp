// SPDX-License-Identifier: MIT
#include "Accent.h"

#include "Config.h"

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>

#include <optional>

namespace selsearch {

QColor defaultAccent()
{
    return QColor(QStringLiteral("#8e9bff"));
}

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

QColor accentColor(const Config &config)
{
    const QColor explicitColor(config.string(QStringLiteral("general.accent")));
    if (explicitColor.isValid())
        return explicitColor;
    if (config.boolean(QStringLiteral("general.follow_portal_accent"))) {
        if (auto c = portalAccent())
            return *c;
    }
    return defaultAccent();
}

}  // namespace selsearch
