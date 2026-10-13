// SPDX-License-Identifier: MIT
//
// Global shortcuts through org.freedesktop.portal.GlobalShortcuts (KDE Plasma,
// GNOME 48+, and other desktops whose portal implements it). The app suggests
// keys; the desktop shows its own confirmation and owns the final binding.

#include "GlobalShortcuts.h"
#include "KeyNames.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QLoggingCategory>
#include <QRandomGenerator>

Q_LOGGING_CATEGORY(lcPortalKeys, "selsearch.shortcuts")

namespace {

// a(sa{sv}) element of BindShortcuts / its response.
struct PortalShortcut {
    QString id;
    QVariantMap properties;
};

QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s)
{
    arg.beginStructure();
    arg << s.id << s.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s)
{
    arg.beginStructure();
    arg >> s.id >> s.properties;
    arg.endStructure();
    return arg;
}

}  // namespace

Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(QList<PortalShortcut>)

namespace selsearch {
namespace {

const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kIface = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");

QString newToken()
{
    return QStringLiteral("selsearch%1").arg(QRandomGenerator::global()->generate());
}

QString requestPath(const QString &token)
{
    QString sender = QDBusConnection::sessionBus().baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

class PortalShortcuts final : public GlobalShortcuts {
    Q_OBJECT
public:
    explicit PortalShortcuts(QObject *parent)
        : GlobalShortcuts(parent)
    {
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<QList<PortalShortcut>>();
        QDBusConnection::sessionBus().connect(kService, kPath, kIface, QStringLiteral("Activated"), this,
                                              SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    }
    ~PortalShortcuts() override { closeSession(); }

    static bool available()
    {
        QDBusInterface props(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"));
        if (!props.isValid())
            return false;
        QDBusReply<QDBusVariant> v = props.call(QStringLiteral("Get"), kIface, QStringLiteral("version"));
        return v.isValid();
    }

    Kind kind() const override { return Kind::Portal; }
    QString backendName() const override { return QStringLiteral("portal"); }

    void setShortcuts(const QList<Shortcut> &shortcuts) override
    {
        m_wanted = shortcuts;
        // Portals bind once per session: start a fresh session for new keys.
        closeSession();
        createSession();
    }

private Q_SLOTS:
    void onActivated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &)
    {
        if (session.path() == m_session)
            Q_EMIT activated(id);
    }

    void onSessionCreated(uint response, const QVariantMap &results)
    {
        if (response != 0) {
            qCWarning(lcPortalKeys) << "CreateSession refused" << response;
            for (const auto &s : std::as_const(m_wanted))
                Q_EMIT failed(s.id, tr("The desktop refused global shortcuts"));
            return;
        }
        m_session = results.value(QStringLiteral("session_handle")).toString();
        bind();
    }

    void onBound(uint response, const QVariantMap &results)
    {
        if (response != 0) {
            for (const auto &s : std::as_const(m_wanted))
                Q_EMIT failed(s.id, tr("Not confirmed"));
            return;
        }
        const auto list = qdbus_cast<QList<PortalShortcut>>(results.value(QStringLiteral("shortcuts")).value<QDBusArgument>());
        for (const auto &s : list)
            Q_EMIT bound(s.id, s.properties.value(QStringLiteral("trigger_description")).toString());
    }

private:
    void createSession()
    {
        const QString token = newToken();
        auto bus = QDBusConnection::sessionBus();
        bus.connect(kService, requestPath(token), QStringLiteral("org.freedesktop.portal.Request"),
                    QStringLiteral("Response"), this, SLOT(onSessionCreated(uint, QVariantMap)));
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("CreateSession"));
        msg << QVariantMap{{QStringLiteral("handle_token"), token},
                           {QStringLiteral("session_handle_token"), newToken()}};
        bus.asyncCall(msg);
    }

    void bind()
    {
        QList<PortalShortcut> list;
        for (const auto &s : std::as_const(m_wanted)) {
            QVariantMap props{{QStringLiteral("description"), s.description}};
            const QString trigger = toXdgTrigger(s.keys);
            if (!trigger.isEmpty())
                props.insert(QStringLiteral("preferred_trigger"), trigger);
            list << PortalShortcut{s.id, props};
        }
        const QString token = newToken();
        auto bus = QDBusConnection::sessionBus();
        bus.connect(kService, requestPath(token), QStringLiteral("org.freedesktop.portal.Request"),
                    QStringLiteral("Response"), this, SLOT(onBound(uint, QVariantMap)));
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("BindShortcuts"));
        msg << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariant::fromValue(list) << QString()
            << QVariantMap{{QStringLiteral("handle_token"), token}};
        bus.asyncCall(msg);
    }

    void closeSession()
    {
        if (m_session.isEmpty())
            return;
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, m_session, QStringLiteral("org.freedesktop.portal.Session"),
                                                          QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(msg);
        m_session.clear();
    }

    QList<Shortcut> m_wanted;
    QString m_session;
};

}  // namespace

std::unique_ptr<GlobalShortcuts> createPortalShortcuts(QObject *parent)
{
    if (!PortalShortcuts::available())
        return nullptr;
    return std::make_unique<PortalShortcuts>(parent);
}

}  // namespace selsearch

#include "PortalShortcuts.moc"
