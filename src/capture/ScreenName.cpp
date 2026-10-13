// SPDX-License-Identifier: MIT
#include "Capture.h"

#include <QGuiApplication>
#include <QScreen>

namespace selsearch {

QString screenName(QScreen *screen)
{
    if (!screen)
        return {};
    const QString name = screen->name();
    int earlier = 0;
    for (QScreen *s : QGuiApplication::screens()) {
        if (s == screen)
            break;
        if (s->name() == name)
            ++earlier;
    }
    return earlier ? QStringLiteral("%1 (%2)").arg(name).arg(earlier + 1) : name;
}

}  // namespace selsearch
