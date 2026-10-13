// SPDX-License-Identifier: MIT
#include "FrameStore.h"

#include <QFile>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrlQuery>


namespace selsearch {

FrameStore::FrameStore()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

void FrameStore::set(const QString &screen, const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    m_frames.insert(screen, image);
}

void FrameStore::clear()
{
    QMutexLocker lock(&m_mutex);
    m_frames.clear();
}

QImage FrameStore::frame(const QString &screen) const
{
    QMutexLocker lock(&m_mutex);
    return m_frames.value(screen);
}

QImage FrameStore::requestImage(const QString &id, QSize *size, const QSize &)
{
    // id = "<screen>/<generation>"
    const QString screen = id.section(QLatin1Char('/'), 0, 0);
    QImage img;
    {
        QMutexLocker lock(&m_mutex);
        img = m_frames.value(screen);
    }
    if (img.isNull()) {
        img = QImage(1, 1, QImage::Format_RGB32);
        img.fill(Qt::black);
    }
    if (size)
        *size = img.size();
    return img;
}

IconProvider::IconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage IconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString name = id.section(QLatin1Char('?'), 0, 0);
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    const QString color = query.queryItemValue(QStringLiteral("color"), QUrl::FullyDecoded);
    const QString weight = query.queryItemValue(QStringLiteral("stroke"));
    QFile f(QStringLiteral(":/data/icons/") + name + QStringLiteral(".svg"));
    QByteArray svg;
    if (f.open(QIODevice::ReadOnly))
        svg = f.readAll();
    svg.replace("currentColor", (color.isEmpty() ? QStringLiteral("#ffffff") : color).toUtf8());
    if (!weight.isEmpty())
        svg.replace("stroke-width=\"2\"", "stroke-width=\"" + weight.toUtf8() + "\"");
    const QSize s = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(48, 48);
    QImage img(s, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QSvgRenderer renderer(svg);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p);
    p.end();
    if (size)
        *size = img.size();
    return img;
}

}  // namespace selsearch
