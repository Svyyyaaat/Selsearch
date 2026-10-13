// SPDX-License-Identifier: MIT
#include "FrameStore.h"

#include <QFile>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrlQuery>

#include <algorithm>
#include <vector>

namespace selsearch {

FrameStore::FrameStore()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

void FrameStore::set(const QString &screen, const QImage &image, const QImage &blurred)
{
    QMutexLocker lock(&m_mutex);
    m_frames.insert(screen, image);
    m_blurred.insert(screen, blurred);
}

void FrameStore::clear()
{
    QMutexLocker lock(&m_mutex);
    m_frames.clear();
    m_blurred.clear();
}

QImage FrameStore::frame(const QString &screen) const
{
    QMutexLocker lock(&m_mutex);
    return m_frames.value(screen);
}

QImage FrameStore::requestImage(const QString &id, QSize *size, const QSize &)
{
    // id = "<screen>/<generation>[/blur]"
    const QStringList parts = id.split(QLatin1Char('/'));
    QImage img;
    {
        QMutexLocker lock(&m_mutex);
        if (!parts.isEmpty()) {
            const bool blur = parts.size() > 2 && parts[2] == QLatin1String("blur");
            img = blur ? m_blurred.value(parts[0]) : m_frames.value(parts[0]);
        }
    }
    if (img.isNull()) {
        img = QImage(1, 1, QImage::Format_RGB32);
        img.fill(Qt::black);
    }
    if (size)
        *size = img.size();
    return img;
}

static void boxBlurH(const QRgb *src, QRgb *dst, int w, int h, int r)
{
    const int div = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        const QRgb *row = src + y * w;
        QRgb *out = dst + y * w;
        int sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; ++i) {
            const QRgb p = row[std::clamp(i, 0, w - 1)];
            sr += qRed(p);
            sg += qGreen(p);
            sb += qBlue(p);
        }
        for (int x = 0; x < w; ++x) {
            out[x] = qRgb(sr / div, sg / div, sb / div);
            const QRgb add = row[std::min(x + r + 1, w - 1)];
            const QRgb sub = row[std::max(x - r, 0)];
            sr += qRed(add) - qRed(sub);
            sg += qGreen(add) - qGreen(sub);
            sb += qBlue(add) - qBlue(sub);
        }
    }
}

QImage FrameStore::makeBlurred(const QImage &image, int downscale, int radius)
{
    if (image.isNull())
        return {};
    const QSize small(std::max(1, image.width() / downscale), std::max(1, image.height() / downscale));
    QImage img = image.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    std::vector<QRgb> a(size_t(w) * h), b(size_t(w) * h), t(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        std::copy_n(reinterpret_cast<const QRgb *>(img.constScanLine(y)), w, a.begin() + y * w);
    auto transpose = [](const std::vector<QRgb> &in, std::vector<QRgb> &out, int iw, int ih) {
        for (int y = 0; y < ih; ++y)
            for (int x = 0; x < iw; ++x)
                out[size_t(x) * ih + y] = in[size_t(y) * iw + x];
    };
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurH(a.data(), b.data(), w, h, radius);
        transpose(b, t, w, h);
        boxBlurH(t.data(), b.data(), h, w, radius);
        transpose(b, a, h, w);
    }
    QImage out(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        std::copy_n(a.begin() + y * w, w, reinterpret_cast<QRgb *>(out.scanLine(y)));
    return out;
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
