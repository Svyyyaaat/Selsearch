// SPDX-License-Identifier: MIT
#pragma once

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace selsearch {

// System-wide hotkeys owned by the app, where the platform allows it:
//  - Windows: RegisterHotKey (fully in-app)
//  - KDE / GNOME 48+ / other portals: org.freedesktop.portal.GlobalShortcuts
//    (the desktop confirms and may let the user change the keys)
//  - wlroots compositors (Hyprland, sway, niri): not possible from an app;
//    keys live in the compositor config (see Settings → Shortcuts snippet).
class GlobalShortcuts : public QObject {
    Q_OBJECT
public:
    struct Shortcut {
        QString id;           // "launcher", "lens", "lens_text"
        QString description;  // shown by the desktop's shortcut UI
        QKeySequence keys;    // e.g. "Alt+Space", "Meta+S"
    };
    enum class Kind {
        None,    // the compositor/WM owns keys; generate config instead
        InApp,   // we register the exact keys ourselves (Windows)
        Portal,  // the desktop owns the binding; we only suggest keys
    };

    using QObject::QObject;
    virtual Kind kind() const = 0;
    virtual QString backendName() const = 0;
    // (Re)register the given shortcuts. Asynchronous for the portal.
    virtual void setShortcuts(const QList<Shortcut> &shortcuts) = 0;
    // Native key recording for the Settings key recorder: while on, every
    // key press is swallowed (no system menu on Alt, no Start menu on Win,
    // no other app's hotkeys) and reported through keysCaptured(). Only
    // where kind() is InApp.
    virtual bool canCaptureKeys() const { return false; }
    virtual void setCapturingKeys(bool) {}

Q_SIGNALS:
    void activated(const QString &id);
    // Per-shortcut problem, e.g. "already used by another app".
    void failed(const QString &id, const QString &reason);
    // Portal: the keys the desktop actually bound ("Meta+S"), per id.
    void bound(const QString &id, const QString &trigger);
    // A key press while capturing, as a portable sequence ("Alt+Shift+S", "Esc").
    void keysCaptured(const QString &sequence);
};

// The best backend for this platform/session; never null (a no-op backend
// with Kind::None when nothing is possible).
std::unique_ptr<GlobalShortcuts> createGlobalShortcuts(QObject *parent = nullptr);

// Implemented per platform (return nullptr when unavailable).
std::unique_ptr<GlobalShortcuts> createWindowsShortcuts(QObject *parent);
std::unique_ptr<GlobalShortcuts> createPortalShortcuts(QObject *parent);

}  // namespace selsearch
