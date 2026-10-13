// SPDX-License-Identifier: MIT
#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

namespace selsearch {

// Holds the frozen screen images (and their blurred variants) for the current
// lens session and serves them to QML as image://frame/<screen>/<gen>[/blur].
class FrameStore : public QQuickImageProvider {
public:
    FrameStore();

    void set(const QString &screen, const QImage &image, const QImage &blurred);
    void clear();
    QImage frame(const QString &screen) const;

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // Cheap, good-looking blur for the frosted background: downscale, three
    // box-blur passes (≈ gaussian), returned small; the GPU upscales it smoothly.
    static QImage makeBlurred(const QImage &image, int downscale = 6, int radius = 4);

private:
    mutable QMutex m_mutex;
    QHash<QString, QImage> m_frames;
    QHash<QString, QImage> m_blurred;
};

// image://icon/<name>?color=%23rrggbb : recoloured Lucide SVG icons.
class IconProvider : public QQuickImageProvider {
public:
    IconProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

}  // namespace selsearch
