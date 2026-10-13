// SPDX-License-Identifier: MIT
#include "AiHistory.h"

#include "core/Config.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace selsearch {

namespace {

constexpr auto kOwnerRw = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

QString roleName(AiMessage::Role role)
{
    switch (role) {
    case AiMessage::System:
        return QStringLiteral("system");
    case AiMessage::Assistant:
        return QStringLiteral("assistant");
    case AiMessage::User:
        break;
    }
    return QStringLiteral("user");
}

QJsonObject toJson(const AiThread &t)
{
    QJsonArray messages;
    for (const AiHistoryMessage &m : t.messages) {
        QJsonObject o{{QStringLiteral("role"), m.role}, {QStringLiteral("text"), m.text}};
        if (!m.thumbnails.isEmpty())
            o.insert(QStringLiteral("images"), QJsonArray::fromStringList(m.thumbnails));
        messages.append(o);
    }
    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("id"), t.id},
        {QStringLiteral("title"), t.title},
        {QStringLiteral("created"), t.created.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("updated"), t.updated.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("messages"), messages},
    };
}

AiThread fromJson(const QJsonObject &o)
{
    AiThread t;
    t.id = o.value(QStringLiteral("id")).toString();
    t.title = o.value(QStringLiteral("title")).toString();
    t.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
    t.updated = QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODateWithMs);
    for (const QJsonValue &v : o.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject mo = v.toObject();
        AiHistoryMessage m;
        m.role = mo.value(QStringLiteral("role")).toString();
        m.text = mo.value(QStringLiteral("text")).toString();
        for (const QJsonValue &img : mo.value(QStringLiteral("images")).toArray())
            m.thumbnails << img.toString();
        t.messages << m;
    }
    return t;
}

}  // namespace

void AiThread::append(const AiMessage &message)
{
    AiHistoryMessage m;
    m.role = roleName(message.role);
    m.text = message.text;
    for (const QImage &img : message.images) {
        const QString thumb = AiHistory::makeThumbnail(img);
        if (!thumb.isEmpty())
            m.thumbnails << thumb;
    }
    messages << m;
}

QList<AiMessage> AiThread::toConversation() const
{
    QList<AiMessage> out;
    for (const AiHistoryMessage &m : messages) {
        AiMessage am;
        am.role = m.role == QLatin1String("system")      ? AiMessage::System
            : m.role == QLatin1String("assistant") ? AiMessage::Assistant
                                                    : AiMessage::User;
        am.text = m.text;
        for (const QString &b64 : m.thumbnails) {
            QImage img;
            if (img.loadFromData(QByteArray::fromBase64(b64.toLatin1()), "JPEG"))
                am.images << img;
        }
        out << am;
    }
    return out;
}

AiHistory::AiHistory(const Config *config, const QString &directory)
    : m_config(config)
    , m_dir(directory.isEmpty() ? defaultDirectory() : directory)
{
}

QString AiHistory::defaultDirectory()
{
    // GenericDataLocation honours $XDG_DATA_HOME (default ~/.local/share).
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/selsearch/history");
}

bool AiHistory::isEnabled() const
{
    return !m_config || m_config->boolean(QStringLiteral("ai.history"));
}

bool AiHistory::isValidId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(id).hasMatch();
}

QString AiHistory::filePath(const QString &id) const { return m_dir + QLatin1Char('/') + id + QStringLiteral(".json"); }

QString AiHistory::makeThumbnail(const QImage &image)
{
    if (image.isNull())
        return {};
    QImage img = image;
    if (qMax(img.width(), img.height()) > kThumbnailSide)
        img = img.scaled(kThumbnailSide, kThumbnailSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (img.hasAlphaChannel()) {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "JPEG", 80))
        return {};
    return QString::fromLatin1(bytes.toBase64());
}

QString AiHistory::makeTitle(const AiThread &thread)
{
    for (const AiHistoryMessage &m : thread.messages) {
        if (m.role != QLatin1String("user"))
            continue;
        const QString text = m.text.simplified();
        if (text.isEmpty())
            continue;
        if (text.size() <= kTitleLength)
            return text;
        return text.left(kTitleLength - 1).trimmed() + QStringLiteral("…");
    }
    for (const AiHistoryMessage &m : thread.messages)
        if (m.role == QLatin1String("user") && !m.thumbnails.isEmpty())
            return QStringLiteral("Image");
    return QStringLiteral("Conversation");
}

std::optional<AiThread> AiHistory::load(const QString &id) const
{
    if (!isValidId(id))
        return std::nullopt;
    QFile f(filePath(id));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return std::nullopt;
    AiThread t = fromJson(doc.object());
    t.id = id;  // the file name is authoritative
    return t;
}

QList<AiThread> AiHistory::list() const
{
    QList<AiThread> out;
    const QDir dir(m_dir);
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &file : files) {
        if (auto t = load(file.chopped(5)))
            out << *t;
    }
    std::stable_sort(out.begin(), out.end(), [](const AiThread &a, const AiThread &b) {
        if (a.updated != b.updated)
            return a.updated > b.updated;
        return a.id > b.id;
    });
    return out;
}

bool AiHistory::save(AiThread &thread) const
{
    if (!isEnabled())
        return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (thread.id.isEmpty())
        thread.id = now.toString(QStringLiteral("yyyyMMdd-HHmmss-"))
            + QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!isValidId(thread.id))
        return false;
    if (thread.title.isEmpty())
        thread.title = makeTitle(thread);
    if (!thread.created.isValid())
        thread.created = now;
    thread.updated = now;

    if (!QDir().mkpath(m_dir))
        return false;
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const QString path = filePath(thread.id);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.setPermissions(kOwnerRw);  // the temporary file, before any content is written
    f.write(QJsonDocument(toJson(thread)).toJson(QJsonDocument::Indented));
    if (!f.commit())
        return false;
    QFile::setPermissions(path, kOwnerRw);
    return true;
}

bool AiHistory::remove(const QString &id) const
{
    if (!isValidId(id))
        return false;
    return QFile::remove(filePath(id));
}

int AiHistory::deleteAll() const
{
    int n = 0;
    const QDir dir(m_dir);
    for (const QString &file : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        if (isValidId(file.chopped(5)) && QFile::remove(dir.filePath(file)))
            ++n;
    }
    return n;
}

QList<AiThread> AiHistory::search(const QString &query) const
{
    const QString q = query.trimmed();
    QList<AiThread> all = list();
    if (q.isEmpty())
        return all;
    QList<AiThread> out;
    for (const AiThread &t : all) {
        bool hit = t.title.contains(q, Qt::CaseInsensitive);
        for (int i = 0; !hit && i < t.messages.size(); ++i)
            hit = t.messages[i].text.contains(q, Qt::CaseInsensitive);
        if (hit)
            out << t;
    }
    return out;
}

}  // namespace selsearch
