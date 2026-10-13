// SPDX-License-Identifier: MIT
#pragma once

#include <QList>
#include <QRect>
#include <QString>

#include <memory>

namespace selsearch {

struct WindowInfo {
    QString title;
    QString appId;   // Wayland app_id / Hyprland class / Windows exe name
    QRect geometry;  // logical, global layout coordinates
    bool focused = false;
    bool floating = false;
};

// Read-only view of the running compositor's windows (IPC). Used for
// snap-to-window selection and as context for "ask about this window".
class Compositor {
public:
    virtual ~Compositor() = default;
    virtual QString id() const = 0;
    // Windows visible on screen right now, topmost first.
    virtual QList<WindowInfo> visibleWindows() = 0;
    // Name of the output that has focus (where the user is looking), or empty.
    virtual QString focusedOutput() = 0;
};

// Hyprland ($HYPRLAND_INSTANCE_SIGNATURE), sway ($SWAYSOCK), or nullptr.
// On Windows: the desktop's top-level windows (EnumWindows).
std::unique_ptr<Compositor> createCompositor();
std::unique_ptr<Compositor> createWindowsCompositor();  // GUI thread only

// Exposed for tests: parse Hyprland's `j/clients` + `j/monitors` replies.
QList<WindowInfo> parseHyprlandWindows(const QByteArray &clientsJson, const QByteArray &monitorsJson);
// Exposed for tests: parse sway's GET_TREE reply.
QList<WindowInfo> parseSwayTree(const QByteArray &treeJson);
// Exposed for tests: the focused output from Hyprland `j/monitors` / sway GET_OUTPUTS.
QString parseFocusedOutput(const QByteArray &outputsJson);

}  // namespace selsearch
