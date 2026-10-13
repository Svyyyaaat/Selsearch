// SPDX-License-Identifier: MIT
#pragma once

// Where and how Selsearch's surfaces appear, per platform:
//  - Wayland: layer-shell surfaces (LayerShellQt); the compositor places them.
//  - Windows: frameless, always-on-top tool windows placed by us, with a
//    foreground workaround so they get keyboard focus.
// Everything platform-specific about the lens, launcher and results windows
// lives here so the controllers stay free of #ifdefs.

#include <QPoint>

class QQuickWindow;
class QScreen;
class QWindow;

namespace selsearch {

// Lens overlay: one per screen, above everything, covering `screen`.
void setupLensWindow(QQuickWindow *window, QScreen *screen);
// Just before a lens session shows: the focused screen's overlay takes the
// keyboard exclusively (Wayland); the geometry follows `screen` (Windows).
void prepareLensWindow(QQuickWindow *window, QScreen *screen, bool focused);

// Launcher: a transparent full-screen surface with the card in the middle.
void setupLauncherWindow(QQuickWindow *window);
// Before showing: on Windows, cover the monitor under the mouse pointer
// (on Wayland the compositor puts it on the focused output).
void placeLauncherWindow(QQuickWindow *window);

// Results panel (selsearch-web): centred below the top edge.
void setupResultsWindow(QQuickWindow *window);
// Before showing: on Windows, move to the screen containing `onScreen`.
void placeResultsWindow(QQuickWindow *window, const QPoint &onScreen);

// Give a just-shown window keyboard focus. On Windows this works around the
// focus-stealing prevention, which otherwise only flashes the taskbar.
void activateWindow(QWindow *window);

}  // namespace selsearch
