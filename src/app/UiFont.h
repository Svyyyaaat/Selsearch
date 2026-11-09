// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QString>

namespace selsearch {

class Config;

// The interface font (general.font, "" = the system's), exposed to QML as the
// context property `uiFont`. Every Text binds font.family to uiFont.family:
// changing QGuiApplication's font at run time does not reach existing Qt
// Quick text items, so the binding is what makes a change apply live.
class UiFont : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString family READ family NOTIFY changed)
public:
    explicit UiFont(Config &config, QObject *parent = nullptr);
    QString family() const { return m_family; }

Q_SIGNALS:
    void changed();

private:
    void apply();

    Config &m_config;
    QString m_systemFamily;
    QString m_family;
};

}  // namespace selsearch
