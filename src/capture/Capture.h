// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

#include <memory>

namespace selsearch {

// A frozen image of one output, upright, in physical pixels.
struct OutputGrab {
    QString name;   // connector / output name, matches QScreen::name()
    QImage image;   // Format_RGB32 or Format_ARGB32_Premultiplied
};

struct CaptureResult {
    QList<OutputGrab> outputs;
    QString backend;
    QString error;
    qint64 elapsedMs = 0;
    bool ok() const { return error.isEmpty() && !outputs.isEmpty(); }
};

// Logical layout of the screens as the GUI sees them (needed by backends that
// return one combined image, e.g. the portal).
struct ScreenLayout {
    QString name;
    QRect geometry;  // logical, global coordinates
    qreal scale = 1.0;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual QString id() const = 0;
    virtual bool isAvailable() = 0;
    // Grab every output. Blocking; call from a worker thread.
    virtual CaptureResult grabAll(const QList<ScreenLayout> &layout) = 0;
};

// Picks the best backend for the running session:
// ext-image-copy-capture-v1 / wlr-screencopy (Hyprland, sway, river, niri, ...),
// then xdg-desktop-portal Screenshot (GNOME, KDE).
// `forced` may name one of: "wayland", "portal".
std::unique_ptr<CaptureBackend> createCaptureBackend(const QString &forced = {});

std::unique_ptr<CaptureBackend> createWaylandCaptureBackend();
std::unique_ptr<CaptureBackend> createPortalCaptureBackend();

}  // namespace selsearch
