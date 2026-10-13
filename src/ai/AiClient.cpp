// SPDX-License-Identifier: MIT
#include "AiClient.h"

#include "core/Config.h"

#include <QBuffer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QPainter>
#include <QSet>
#include <QTimer>

namespace selsearch {

namespace {

constexpr qsizetype kMaxRawBody = 4 * 1024 * 1024;  // kept for non-streaming replies / error text

// Never stores or sends cookies.
class NoCookieJar : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
};

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

QString jsonErrorMessage(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (const auto *key : {"message", "error", "detail", "msg"}) {
            const QString s = jsonErrorMessage(o.value(QLatin1String(key)));
            if (!s.isEmpty())
                return s;
        }
    }
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue &el : v.toArray()) {
            const QString s = jsonErrorMessage(el);
            if (!s.isEmpty())
                parts << s;
        }
        return parts.join(QStringLiteral("; "));
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Settings

AiSettings aiSettingsFromConfig(const Config &config)
{
    AiSettings s;
    s.endpoint = config.string(QStringLiteral("ai.endpoint")).trimmed();
    s.model = config.string(QStringLiteral("ai.model")).trimmed();
    s.apiKey = config.string(QStringLiteral("ai.api_key")).trimmed();
    if (s.apiKey.isEmpty()) {
        const QString env = config.string(QStringLiteral("ai.api_key_env")).trimmed();
        if (!env.isEmpty())
            s.apiKey = qEnvironmentVariable(env.toLocal8Bit().constData()).trimmed();
    }
    return s;
}

// ---------------------------------------------------------------------------
// SSE

void SseParser::reset()
{
    m_pending.clear();
    m_data.clear();
    m_hasData = false;
    m_skipLf = false;
}

QList<QByteArray> SseParser::feed(const QByteArray &chunk)
{
    QList<QByteArray> out;
    qsizetype start = 0;
    const qsizetype n = chunk.size();
    if (m_skipLf && n > 0) {
        m_skipLf = false;
        if (chunk.at(0) == '\n')
            start = 1;
    }
    for (qsizetype i = start; i < n; ++i) {
        const char c = chunk.at(i);
        if (c != '\n' && c != '\r')
            continue;
        m_pending.append(chunk.constData() + start, i - start);
        processLine(m_pending, out);
        m_pending.clear();
        if (c == '\r') {
            if (i + 1 < n) {
                if (chunk.at(i + 1) == '\n')
                    ++i;
            } else {
                m_skipLf = true;
            }
        }
        start = i + 1;
    }
    if (start < n)
        m_pending.append(chunk.constData() + start, n - start);
    return out;
}

QList<QByteArray> SseParser::finish()
{
    QList<QByteArray> out;
    if (!m_pending.isEmpty()) {
        processLine(m_pending, out);
        m_pending.clear();
    }
    processLine(QByteArray(), out);  // dispatch a pending event
    m_skipLf = false;
    return out;
}

void SseParser::processLine(const QByteArray &line, QList<QByteArray> &out)
{
    if (line.isEmpty()) {
        if (m_hasData)
            out << m_data;
        m_data.clear();
        m_hasData = false;
        return;
    }
    if (line.startsWith(':'))
        return;  // comment / keep-alive
    if (line.startsWith('{') && !m_hasData) {
        out << line;  // NDJSON fallback
        return;
    }
    const qsizetype colon = line.indexOf(':');
    const QByteArray field = colon < 0 ? line : line.left(colon);
    if (field != "data")
        return;  // event:, id:, retry: and unknown fields are not needed
    QByteArray value = colon < 0 ? QByteArray() : line.mid(colon + 1);
    if (value.startsWith(' '))
        value.remove(0, 1);
    if (m_hasData)
        m_data.append('\n');
    m_data.append(value);
    m_hasData = true;
}

// ---------------------------------------------------------------------------
// AiClient

AiClient::AiClient(QObject *parent)
    : QObject(parent)
    , m_connectTimer(new QTimer(this))
{
    m_nam.setCookieJar(new NoCookieJar(&m_nam));
    m_nam.setRedirectPolicy(QNetworkRequest::SameOriginRedirectPolicy);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this] {
        if (!m_reply)
            return;
        fail(tr("Could not connect to %1 within %2 s.")
                 .arg(endpointHost())
                 .arg(m_connectTimeoutMs / 1000.0));
    });
}

AiClient::~AiClient()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_modelsReply) {
        m_modelsReply->disconnect(this);
        m_modelsReply->abort();
    }
}

void AiClient::setTimeouts(int connectMs, int idleMs)
{
    m_connectTimeoutMs = connectMs;
    m_idleTimeoutMs = idleMs;
}

QUrl AiClient::baseUrl(const QString &endpoint)
{
    QString e = endpoint.trimmed();
    if (e.isEmpty())
        return {};
    if (!e.contains(QLatin1String("://")))
        e.prepend(QStringLiteral("http://"));
    QUrl url(e);
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.endsWith(QLatin1String("/chat/completions")))
        path.chop(int(qstrlen("/chat/completions")));
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.isEmpty())
        path = QStringLiteral("/v1");
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

bool AiClient::isLocalUrl(const QUrl &url)
{
    QString host = url.host().toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty())
        return false;
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost"))
        || host.endsWith(QLatin1String(".local")))
        return true;
    QHostAddress addr;
    if (addr.setAddress(host))
        return addr.isLoopback();
    return false;
}

bool AiClient::isLocalEndpoint() const { return isLocalUrl(baseUrl(m_settings.endpoint)); }

QString AiClient::endpointHost() const { return baseUrl(m_settings.endpoint).host(); }

QNetworkRequest AiClient::makeRequest(const QString &path) const
{
    QUrl url = baseUrl(m_settings.endpoint);
    url.setPath(url.path() + path);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Selsearch/" SELSEARCH_VERSION));
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    req.setTransferTimeout(m_idleTimeoutMs);
    if (!m_settings.apiKey.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_settings.apiKey.toUtf8());
    return req;
}

bool AiClient::looksFlat(const QImage &image)
{
    if (image.isNull())
        return false;
    const int w = image.width();
    const int h = image.height();
    const int stepX = qMax(1, w / 64);
    const int stepY = qMax(1, h / 64);
    QSet<QRgb> colors;
    int samples = 0;
    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            colors.insert(image.pixel(x, y) & 0xffffff);
            ++samples;
        }
    }
    return samples > 0 && colors.size() * 8 <= samples;
}

QString AiClient::imageDataUrl(const QImage &source, int maxSide)
{
    if (source.isNull())
        return {};
    QImage img = source;
    if (maxSide > 0 && qMax(img.width(), img.height()) > maxSide)
        img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (img.hasAlphaChannel()) {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    } else if (img.format() != QImage::Format_RGB32) {
        img = img.convertToFormat(QImage::Format_RGB32);
    }
    const bool png = looksFlat(img);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, png ? "PNG" : "JPEG", png ? -1 : 90);
    return (png ? QStringLiteral("data:image/png;base64,") : QStringLiteral("data:image/jpeg;base64,"))
        + QString::fromLatin1(bytes.toBase64());
}

QJsonObject AiClient::buildRequest(const AiSettings &settings, const QList<AiMessage> &conversation)
{
    QJsonArray messages;
    for (const AiMessage &m : conversation) {
        QJsonObject msg{{QStringLiteral("role"), roleName(m.role)}};
        if (m.images.isEmpty() || m.role != AiMessage::User) {
            msg.insert(QStringLiteral("content"), m.text);
        } else {
            QJsonArray parts;
            if (!m.text.isEmpty())
                parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                         {QStringLiteral("text"), m.text}});
            for (const QImage &image : m.images) {
                const QString url = imageDataUrl(image, settings.maxImageSide);
                if (url.isEmpty())
                    continue;
                parts.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("image_url")},
                    {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), url}}}});
            }
            msg.insert(QStringLiteral("content"), parts);
        }
        messages.append(msg);
    }
    QJsonObject body{
        {QStringLiteral("messages"), messages},
        {QStringLiteral("stream"), true},
        {QStringLiteral("temperature"), settings.temperature},
    };
    if (!settings.model.isEmpty())
        body.insert(QStringLiteral("model"), settings.model);
    return body;
}

QString AiClient::describeHttpError(int status, const QByteArray &body, const AiSettings &settings)
{
    QString message;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body.trimmed(), &perr);
    if (perr.error == QJsonParseError::NoError)
        message = doc.isObject() ? jsonErrorMessage(doc.object()) : jsonErrorMessage(doc.array());
    if (message.isEmpty()) {
        message = QString::fromUtf8(body).trimmed();
        if (message.startsWith(QLatin1Char('<')))  // an HTML error page
            message.clear();
        if (message.size() > 500)
            message = message.left(500) + QStringLiteral("…");
    }
    QString text = status > 0 ? QStringLiteral("HTTP %1").arg(status) : QStringLiteral("Error");
    if (!message.isEmpty())
        text += QStringLiteral(": ") + message;

    const QString lower = message.toLower();
    const QUrl url = baseUrl(settings.endpoint);
    const bool ollama = url.port() == 11434 || lower.contains(QLatin1String("pull"))
        || lower.contains(QLatin1String("ollama"));
    if (lower.contains(QLatin1String("not found")) && (lower.contains(QLatin1String("model")) || status == 404)
        && !settings.model.isEmpty()) {
        if (ollama)
            text += QStringLiteral("\nDownload the model with: ollama pull %1").arg(settings.model);
        else
            text += QStringLiteral("\nCheck that the model name \"%1\" is served by %2.").arg(settings.model, url.host());
    } else if (status == 401 || status == 403) {
        text += QStringLiteral("\nCheck the API key (ai.api_key or ai.api_key_env).");
    } else if (status == 429) {
        text += QStringLiteral("\nThe server is rate limiting requests; try again later.");
    } else if (status == 404) {
        text += QStringLiteral("\nCheck ai.endpoint: it should be the API base URL, usually ending in /v1.");
    } else if (lower.contains(QLatin1String("image"))
               && (lower.contains(QLatin1String("support")) || lower.contains(QLatin1String("vision"))
                   || lower.contains(QLatin1String("multimodal")))) {
        text += QStringLiteral("\nThe model may not accept images; choose a vision model.");
    }
    return text;
}

QStringList AiClient::parseModelList(const QByteArray &body)
{
    QStringList out;
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    auto add = [&out](const QJsonArray &arr) {
        for (const QJsonValue &v : arr) {
            QString id;
            if (v.isString()) {
                id = v.toString();
            } else {
                const QJsonObject o = v.toObject();
                for (const auto *key : {"id", "name", "model"}) {
                    id = o.value(QLatin1String(key)).toString();
                    if (!id.isEmpty())
                        break;
                }
            }
            if (!id.isEmpty() && !out.contains(id))
                out << id;
        }
    };
    add(obj.value(QStringLiteral("data")).toArray());
    if (out.isEmpty())
        add(obj.value(QStringLiteral("models")).toArray());  // ollama /api/tags shape
    out.sort(Qt::CaseInsensitive);
    return out;
}

QString AiClient::networkErrorText(QNetworkReply *reply) const
{
    const QString host = endpointHost();
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError: {
        QString s = tr("Could not connect to %1: connection refused. Is the AI server running?").arg(host);
        if (baseUrl(m_settings.endpoint).port() == 11434)
            s += QStringLiteral("\nStart it with: ollama serve");
        return s;
    }
    case QNetworkReply::HostNotFoundError:
        return tr("Host not found: %1").arg(host);
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return tr("No response from %1 for %2 s.").arg(host).arg(m_idleTimeoutMs / 1000.0);
    default:
        return reply->errorString();
    }
}

void AiClient::ask(const QList<AiMessage> &conversation)
{
    abort();
    m_parser.reset();
    m_buffer.clear();
    m_full.clear();
    m_done = false;
    m_gotEvent = false;
    m_failed = false;

    if (m_settings.endpoint.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, [this] { Q_EMIT failed(tr("No AI endpoint configured (ai.endpoint).")); });
        return;
    }

    QNetworkRequest req = makeRequest(QStringLiteral("/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "text/event-stream, application/json");
    const QByteArray body = QJsonDocument(buildRequest(m_settings, conversation)).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_nam.post(req, body);
    m_reply = reply;
    m_connectTimer->start(m_connectTimeoutMs);
    connect(reply, &QNetworkReply::requestSent, m_connectTimer, &QTimer::stop);
    connect(reply, &QNetworkReply::metaDataChanged, m_connectTimer, &QTimer::stop);
    connect(reply, &QNetworkReply::readyRead, this, &AiClient::onReadyRead);
    connect(reply, &QNetworkReply::finished, this, &AiClient::onFinished);
}

void AiClient::abort()
{
    m_connectTimer->stop();
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void AiClient::fail(const QString &error)
{
    if (m_failed)
        return;
    m_failed = true;
    abort();
    Q_EMIT failed(error);
}

void AiClient::onReadyRead()
{
    QNetworkReply *reply = m_reply.data();
    if (!reply || sender() != reply)
        return;
    m_connectTimer->stop();
    const QByteArray chunk = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_buffer.size() < kMaxRawBody)
        m_buffer.append(chunk);
    if (status >= 400)
        return;  // error body: reported in onFinished
    for (const QByteArray &event : m_parser.feed(chunk)) {
        handleEvent(event);
        if (m_failed || !m_reply)
            return;
    }
}

void AiClient::handleEvent(const QByteArray &data)
{
    const QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty())
        return;
    if (trimmed == "[DONE]") {
        m_done = true;
        return;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        // Some servers put several JSON objects into one event.
        if (trimmed.contains('\n')) {
            for (const QByteArray &line : trimmed.split('\n')) {
                handleEvent(line);
                if (m_failed)
                    return;
            }
        }
        return;
    }
    m_gotEvent = true;
    const QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("error"))) {
        fail(describeHttpError(0, trimmed, m_settings));
        return;
    }

    QString text;
    QString thinking;
    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const QJsonObject choice = choices.first().toObject();
        QJsonObject d = choice.value(QStringLiteral("delta")).toObject();
        if (d.isEmpty())
            d = choice.value(QStringLiteral("message")).toObject();  // non-streaming reply
        text = d.value(QStringLiteral("content")).toString();
        if (text.isEmpty())
            text = choice.value(QStringLiteral("text")).toString();  // legacy completions shape
        thinking = d.value(QStringLiteral("reasoning_content")).toString();
        if (thinking.isEmpty())
            thinking = d.value(QStringLiteral("reasoning")).toString();
    } else if (obj.contains(QStringLiteral("message"))) {
        // ollama native /api/chat shape
        const QJsonObject m = obj.value(QStringLiteral("message")).toObject();
        text = m.value(QStringLiteral("content")).toString();
        thinking = m.value(QStringLiteral("thinking")).toString();
        if (obj.value(QStringLiteral("done")).toBool())
            m_done = true;
    }
    if (!thinking.isEmpty())
        Q_EMIT reasoning(thinking);
    if (!text.isEmpty()) {
        m_full += text;
        Q_EMIT delta(text);
    }
}

void AiClient::onFinished()
{
    QNetworkReply *reply = m_reply.data();
    if (!reply || sender() != reply)
        return;
    m_connectTimer->stop();

    const QByteArray rest = reply->isOpen() ? reply->readAll() : QByteArray();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_buffer.size() < kMaxRawBody)
        m_buffer.append(rest);

    if (status >= 400) {
        fail(describeHttpError(status, m_buffer, m_settings));
        return;
    }
    QList<QByteArray> events = m_parser.feed(rest);
    events += m_parser.finish();
    for (const QByteArray &event : events) {
        handleEvent(event);
        if (m_failed)
            return;
    }
    if (reply->error() != QNetworkReply::NoError && !m_done) {
        fail(networkErrorText(reply));
        return;
    }
    if (!m_gotEvent && !m_buffer.trimmed().isEmpty()) {
        handleEvent(m_buffer);  // a pretty-printed, non-streaming JSON reply
        if (m_failed)
            return;
    }
    if (!m_gotEvent && !m_done) {
        fail(tr("The AI server at %1 returned an empty or unrecognised response.").arg(endpointHost()));
        return;
    }

    m_reply = nullptr;
    reply->disconnect(this);
    reply->deleteLater();
    Q_EMIT finished(m_full);
}

void AiClient::listModels()
{
    if (m_modelsReply) {
        m_modelsReply->disconnect(this);
        m_modelsReply->abort();
        m_modelsReply->deleteLater();
    }
    if (m_settings.endpoint.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, [this] { Q_EMIT modelsFailed(tr("No AI endpoint configured (ai.endpoint).")); });
        return;
    }
    QNetworkRequest req = makeRequest(QStringLiteral("/models"));
    req.setTransferTimeout(qMin(m_idleTimeoutMs, 15000));
    QNetworkReply *reply = m_nam.get(req);
    m_modelsReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_modelsReply != reply)
            return;
        m_modelsReply = nullptr;
        const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400) {
            Q_EMIT modelsFailed(describeHttpError(status, body, m_settings));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT modelsFailed(networkErrorText(reply));
            return;
        }
        Q_EMIT modelsListed(parseModelList(body));
    });
}

}  // namespace selsearch
