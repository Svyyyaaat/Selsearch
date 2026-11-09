// SPDX-License-Identifier: MIT
#include "UiFont.h"

#include "core/Config.h"

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>

namespace selsearch {

UiFont::UiFont(Config &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_systemFamily(QGuiApplication::font().family())
{
    connect(&m_config, &Config::changed, this, [this](const QString &key) {
        if (key == QLatin1String("general.font"))
            apply();
    });
    connect(&m_config, &Config::reloaded, this, &UiFont::apply);
    apply();
}

void UiFont::apply()
{
    QString family = m_config.string(QStringLiteral("general.font")).trimmed();
    if (family.isEmpty() || !QFontDatabase::hasFamily(family))
        family = m_systemFamily;
    // Controls created from now on (buttons, fields) pick it up as well.
    QFont f = QGuiApplication::font();
    f.setFamily(family);
    QGuiApplication::setFont(f);
    if (family == m_family)
        return;
    m_family = family;
    Q_EMIT changed();
}

}  // namespace selsearch
