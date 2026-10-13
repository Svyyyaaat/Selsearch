// SPDX-License-Identifier: MIT
//
// xdg-desktop-portal Screenshot backend for desktops without wlroots/ext
// capture protocols (GNOME, KDE). The portal returns one image of the whole
// desktop; it is split per screen using the GUI's logical layout.

#include "Capture.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

namespace selsearch {
namespace {

class ResponseWaiter : public QObject {
    Q_OBJECT
public:
    QEventLoop loop;
    uint code = 2;
    QVariantMap results;
public Q_SLOTS:
    void onResponse(uint response, const QVariantMap &res)
    {
        code = response;
        results = res;
        loop.quit();
    }
};

class PortalCaptureBackend final : public CaptureBackend {
public:
    QString id() const override { return QStringLiteral("portal"); }

    bool isAvailable() override
    {
        QDBusInterface iface(QStringLiteral("org.freedesktop.portal.Desktop"),
                             QStringLiteral("/org/freedesktop/portal/desktop"),
                             QStringLiteral("org.freedesktop.portal.Screenshot"));
        return iface.isValid();
    }

    CaptureResult grabAll(const QList<ScreenLayout> &layout) override
    {
        QElapsedTimer timer;
        timer.start();
        CaptureResult result;
        result.backend = id();
        auto bus = QDBusConnection::sessionBus();
        const QString token = QStringLiteral("selsearch%1").arg(QRandomGenerator::global()->generate());
        QString sender = bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
        const QString requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

        ResponseWaiter waiter;
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), requestPath,
                    QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                    &waiter, SLOT(onResponse(uint, QVariantMap)));

        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Screenshot"), QStringLiteral("Screenshot"));
        msg << QString() << QVariantMap{{QStringLiteral("handle_token"), token},
                                        {QStringLiteral("interactive"), false},
                                        {QStringLiteral("modal"), false}};
        QDBusReply<QDBusObjectPath> reply = bus.call(msg);
        if (!reply.isValid()) {
            result.error = reply.error().message();
            return result;
        }
        QTimer::singleShot(30000, &waiter.loop, &QEventLoop::quit);
        waiter.loop.exec();
        if (waiter.code != 0) {
            result.error = waiter.code == 1 ? QStringLiteral("screenshot permission denied")
                                            : QStringLiteral("portal screenshot failed");
            return result;
        }
        const QString path = QUrl(waiter.results.value(QStringLiteral("uri")).toString()).toLocalFile();
        QImage full(path);
        QFile::remove(path);  // the portal drops the file into ~/Pictures; don't leave it there
        if (full.isNull()) {
            result.error = QStringLiteral("could not read portal screenshot");
            return result;
        }

        // Find the layout's bounding box and how many image pixels one logical
        // pixel is (the portal renders at the highest scale).
        QRect bounds;
        for (const auto &s : layout)
            bounds = bounds.united(s.geometry);
        if (bounds.isEmpty()) {
            result.outputs.append(OutputGrab{QString(), full.convertToFormat(QImage::Format_RGB32)});
        } else {
            const qreal fx = qreal(full.width()) / bounds.width();
            const qreal fy = qreal(full.height()) / bounds.height();
            for (const auto &s : layout) {
                const QRect src(qRound((s.geometry.x() - bounds.x()) * fx), qRound((s.geometry.y() - bounds.y()) * fy),
                                qRound(s.geometry.width() * fx), qRound(s.geometry.height() * fy));
                QImage part = full.copy(src);
                const QSize physical(qRound(s.geometry.width() * s.scale), qRound(s.geometry.height() * s.scale));
                if (part.size() != physical)
                    part = part.scaled(physical, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                result.outputs.append(OutputGrab{s.name, part.convertToFormat(QImage::Format_RGB32)});
            }
        }
        result.elapsedMs = timer.elapsed();
        return result;
    }
};

}  // namespace

std::unique_ptr<CaptureBackend> createPortalCaptureBackend()
{
    return std::make_unique<PortalCaptureBackend>();
}

std::unique_ptr<CaptureBackend> createCaptureBackend(const QString &forced)
{
    if (forced == QLatin1String("portal"))
        return createPortalCaptureBackend();
    if (forced == QLatin1String("wayland"))
        return createWaylandCaptureBackend();
    auto wl = createWaylandCaptureBackend();
    if (wl->isAvailable())
        return wl;
    auto portal = createPortalCaptureBackend();
    if (portal->isAvailable())
        return portal;
    return nullptr;
}

}  // namespace selsearch

#include "PortalCapture.moc"
