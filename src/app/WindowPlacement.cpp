// SPDX-License-Identifier: MIT
#include "WindowPlacement.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <LayerShellQt/Window>
#endif

namespace selsearch {

#ifdef Q_OS_WIN

namespace {

// Borderless, above other windows, no taskbar button.
constexpr Qt::WindowFlags kOverlayFlags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool;

QScreen *screenUnderCursor()
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    return screen ? screen : QGuiApplication::primaryScreen();
}

}  // namespace

void setupLensWindow(QQuickWindow *window, QScreen *screen)
{
    window->setFlags(kOverlayFlags);
    window->setScreen(screen);
    window->setGeometry(screen->geometry());
}

void prepareLensWindow(QQuickWindow *window, QScreen *screen, bool)
{
    // Screens can change resolution or scale between sessions.
    if (window->screen() != screen)
        window->setScreen(screen);
    if (window->geometry() != screen->geometry())
        window->setGeometry(screen->geometry());
}

void setupLauncherWindow(QQuickWindow *window)
{
    window->setFlags(kOverlayFlags);
}

void placeLauncherWindow(QQuickWindow *window)
{
    QScreen *screen = screenUnderCursor();
    if (!screen)
        return;
    if (window->screen() != screen)
        window->setScreen(screen);
    window->setGeometry(screen->geometry());
}

void setupResultsWindow(QQuickWindow *window)
{
    window->setFlags(kOverlayFlags);
}

void placeResultsWindow(QQuickWindow *window, const QPoint &onScreen)
{
    QScreen *screen = QGuiApplication::screenAt(onScreen);
    if (!screen)
        screen = screenUnderCursor();
    if (!screen)
        return;
    if (window->screen() != screen)
        window->setScreen(screen);
    // Same spot as the Wayland layer surface: centred, 72 px below the top.
    const QRect area = screen->availableGeometry();
    window->setPosition(area.x() + (area.width() - window->width()) / 2, area.y() + 72);
}

void activateWindow(QWindow *window)
{
    window->requestActivate();
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const HWND foreground = GetForegroundWindow();
    if (foreground == hwnd)
        return;
    // Windows only lets the process that received the last input event take
    // the foreground. A hotkey we registered counts, as does a click on our
    // tray icon; for requests over IPC the CLI calls AllowSetForegroundWindow.
    // Sharing the foreground thread's input state covers the remaining cases.
    const DWORD self = GetCurrentThreadId();
    const DWORD other = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = other && other != self && AttachThreadInput(other, self, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    if (attached)
        AttachThreadInput(other, self, FALSE);
}

#else  // Wayland: layer-shell

void setupLensWindow(QQuickWindow *window, QScreen *screen)
{
    window->setScreen(screen);
    window->setGeometry(screen->geometry());
    if (auto *layer = LayerShellQt::Window::get(window)) {
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layer->setScope(QStringLiteral("selsearch-lens"));
    }
}

void prepareLensWindow(QQuickWindow *window, QScreen *, bool focused)
{
    if (auto *layer = LayerShellQt::Window::get(window))
        layer->setKeyboardInteractivity(focused ? LayerShellQt::Window::KeyboardInteractivityExclusive
                                                : LayerShellQt::Window::KeyboardInteractivityOnDemand);
}

void setupLauncherWindow(QQuickWindow *window)
{
    if (auto *layer = LayerShellQt::Window::get(window)) {
        // Full-screen transparent layer: clicks outside the card close it and
        // the compositor puts it on the focused output.
        layer->setLayer(LayerShellQt::Window::LayerTop);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        layer->setScreenConfiguration(LayerShellQt::Window::ScreenFromCompositor);
        layer->setScope(QStringLiteral("selsearch-launcher"));
    }
}

void placeLauncherWindow(QQuickWindow *)
{
}

void setupResultsWindow(QQuickWindow *window)
{
    if (auto *layer = LayerShellQt::Window::get(window)) {
        layer->setLayer(LayerShellQt::Window::LayerTop);
        layer->setAnchors(LayerShellQt::Window::AnchorTop);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layer->setScope(QStringLiteral("selsearch-results"));
        layer->setScreenConfiguration(LayerShellQt::Window::ScreenFromCompositor);  // the focused output
        layer->setMargins(QMargins(0, 72, 0, 0));
    }
}

void placeResultsWindow(QQuickWindow *, const QPoint &)
{
}

void activateWindow(QWindow *window)
{
    window->requestActivate();
}

#endif

}  // namespace selsearch
