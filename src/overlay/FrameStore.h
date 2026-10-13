// SPDX-License-Identifier: MIT
#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

namespace selsearch {

// Holds the frozen screen images for the current lens session and serves them
// to QML as image://frame/<screen>/<gen>.
class FrameStore : public QQuickImageProvider {
public:
    FrameStore();

    void set(const QString &screen, const QImage &image);
    void clear();
    QImage frame(const QString &screen) const;

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    mutable QMutex m_mutex;
    QHash<QString, QImage> m_frames;
};

// image://icon/<name>?color=%23rrggbb : recoloured Lucide SVG icons.
class IconProvider : public QQuickImageProvider {
public:
    IconProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

}  // namespace selsearch
