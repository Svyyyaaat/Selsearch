// SPDX-License-Identifier: MIT
#include "GlobalShortcuts.h"

namespace selsearch {
namespace {

class NoShortcuts final : public GlobalShortcuts {
public:
    using GlobalShortcuts::GlobalShortcuts;
    Kind kind() const override { return Kind::None; }
    QString backendName() const override { return QStringLiteral("compositor"); }
    void setShortcuts(const QList<Shortcut> &) override {}
};

// wlroots-style compositors bind keys in their own config; an app cannot.
bool compositorOwnsKeys()
{
    return qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE") || qEnvironmentVariableIsSet("SWAYSOCK")
        || qEnvironmentVariableIsSet("NIRI_SOCKET");
}

}  // namespace

#ifndef Q_OS_WIN
std::unique_ptr<GlobalShortcuts> createWindowsShortcuts(QObject *) { return nullptr; }
#endif

std::unique_ptr<GlobalShortcuts> createGlobalShortcuts(QObject *parent)
{
#ifdef Q_OS_WIN
    if (auto s = createWindowsShortcuts(parent))
        return s;
#else
    if (!compositorOwnsKeys())
        if (auto s = createPortalShortcuts(parent))
            return s;
#endif
    return std::make_unique<NoShortcuts>(parent);
}

}  // namespace selsearch
