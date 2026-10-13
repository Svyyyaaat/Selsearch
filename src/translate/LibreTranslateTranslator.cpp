// SPDX-License-Identifier: MIT
#include "LanguageDetect.h"
#include "TranslationLanguages.h"
#include "TranslatorBackends.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QTimer>

namespace selsearch {

namespace {

constexpr int kFailedProbeTtlMs = 30000;
constexpr int kTranslateTimeoutMs = 60000;

class NoCookieJar : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
};

QString detectedFrom(const QJsonValue &v)
{
    if (v.isObject())
        return v.toObject().value(QStringLiteral("language")).toString();
    if (v.isArray()) {
        // One entry per segment: report the most confident one.
        QString best;
        double conf = -1;
        for (const QJsonValue &el : v.toArray()) {
            const QJsonObject o = el.toObject();
            const double c = o.value(QStringLiteral("confidence")).toDouble();
            const QString lang = o.value(QStringLiteral("language")).toString();
            if (!lang.isEmpty() && c > conf) {
                conf = c;
                best = lang;
            }
        }
        return best;
    }
    return {};
}

}  // namespace

LibreTranslateTranslator::LibreTranslateTranslator(const QString &url, const QString &apiKey, QObject *parent)
    : Translator(parent)
    , m_apiKey(apiKey.trimmed())
{
    QString u = url.trimmed();
    if (!u.isEmpty() && !u.contains(QLatin1String("://")))
        u.prepend(QStringLiteral("http://"));
    while (u.endsWith(QLatin1Char('/')))
        u.chop(1);
    m_url = QUrl(u);
    m_nam.setCookieJar(new NoCookieJar(&m_nam));
    m_nam.setRedirectPolicy(QNetworkRequest::SameOriginRedirectPolicy);
}

bool LibreTranslateTranslator::isAvailable() const
{
    if (!m_url.isValid() || m_url.host().isEmpty())
        return false;
    return !(m_probe == Probe::Failed && m_probeAge.isValid() && m_probeAge.elapsed() < kFailedProbeTtlMs);
}

QNetworkRequest LibreTranslateTranslator::makeRequest(const QString &path) const
{
    QUrl url = m_url;
    url.setPath(url.path() + path);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Selsearch/" SELSEARCH_VERSION));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    return req;
}

QList<LibreTranslateTranslator::Language> LibreTranslateTranslator::parseLanguages(const QByteArray &body)
{
    QList<Language> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(body).array()) {
        const QJsonObject o = v.toObject();
        Language l;
        l.code = o.value(QStringLiteral("code")).toString();
        l.name = o.value(QStringLiteral("name")).toString();
        for (const QJsonValue &t : o.value(QStringLiteral("targets")).toArray())
            l.targets << t.toString();
        if (!l.code.isEmpty())
            out << l;
    }
    return out;
}

TranslationResult LibreTranslateTranslator::parseTranslateReply(int status, const QByteArray &body, int count)
{
    TranslationResult r;
    r.backend = QStringLiteral("libretranslate");
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    const QJsonObject obj = doc.object();
    const QString serverError = obj.value(QStringLiteral("error")).toString();
    if (status >= 400 || !serverError.isEmpty()) {
        QString msg = serverError;
        if (msg.isEmpty())
            msg = QString::fromUtf8(body).trimmed().left(300);
        r.error = status > 0 ? QStringLiteral("LibreTranslate: HTTP %1: %2").arg(status).arg(msg)
                             : QStringLiteral("LibreTranslate: %1").arg(msg);
        if (status == 403 || (status == 400 && msg.contains(QLatin1String("api_key"), Qt::CaseInsensitive)))
            r.error += QStringLiteral("\nCheck translate.libretranslate_key.");
        return r;
    }
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("LibreTranslate: unexpected reply");
        return r;
    }
    const QJsonValue text = obj.value(QStringLiteral("translatedText"));
    if (text.isArray()) {
        for (const QJsonValue &v : text.toArray())
            r.segments << v.toString();
    } else if (text.isString()) {
        r.segments << text.toString();
    }
    if (r.segments.size() != count) {
        r.error = QStringLiteral("LibreTranslate returned %1 translations for %2 segments")
                      .arg(r.segments.size())
                      .arg(count);
        r.segments.clear();
        return r;
    }
    r.detectedSource = normalizeLanguageCode(detectedFrom(obj.value(QStringLiteral("detectedLanguage"))));
    return r;
}

QString LibreTranslateTranslator::serverCode(const QString &code) const
{
    if (m_languages.isEmpty() || code.isEmpty())
        return code;
    for (const Language &l : m_languages)
        if (l.code == code)
            return code;
    // Newer servers use zh-Hans / zh-Hant, pt-BR, ...
    for (const Language &l : m_languages)
        if (l.code.startsWith(code + QLatin1Char('-')))
            return l.code;
    return code;
}

void LibreTranslateTranslator::probe(int timeoutMs, std::function<void(bool)> done)
{
    if (m_probe == Probe::Ok || (m_probe == Probe::Failed && m_probeAge.elapsed() < kFailedProbeTtlMs)) {
        const bool ok = m_probe == Probe::Ok;
        QTimer::singleShot(0, this, [done = std::move(done), ok] { done(ok); });
        return;
    }
    m_probeWaiting << std::move(done);
    if (m_probe == Probe::Running)
        return;
    m_probe = Probe::Running;

    auto finish = [this](bool ok) {
        m_probe = ok ? Probe::Ok : Probe::Failed;
        m_probeAge.start();
        const auto waiting = std::exchange(m_probeWaiting, {});
        for (const auto &cb : waiting)
            cb(ok);
    };
    if (!m_url.isValid() || m_url.host().isEmpty()) {
        QTimer::singleShot(0, this, [finish] { finish(false); });
        return;
    }
    QNetworkRequest req = makeRequest(QStringLiteral("/languages"));
    req.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_nam.get(req);
    // The transfer timeout does not cover every connect stall; abort explicitly too.
    QTimer::singleShot(timeoutMs, reply, [reply] { reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, finish] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QList<Language> langs;
        if (reply->error() == QNetworkReply::NoError && status == 200)
            langs = parseLanguages(reply->readAll());
        if (!langs.isEmpty())
            m_languages = langs;
        finish(!langs.isEmpty());
    });
}

void LibreTranslateTranslator::fetchLanguages(std::function<void(const QList<Language> &, const QString &)> done)
{
    probe(5000, [this, done = std::move(done)](bool ok) {
        done(m_languages, ok ? QString() : tr("LibreTranslate at %1 is not reachable.").arg(m_url.toString()));
    });
}

void LibreTranslateTranslator::translate(const TranslationRequest &request, Callback callback)
{
    const QString source = normalizeLanguageCode(request.source);
    QString target = normalizeLanguageCode(request.target);
    if (target.isEmpty())
        target = systemLanguageCode();

    // Empty segments are not sent.
    QList<int> index;
    QJsonArray q;
    for (int i = 0; i < request.segments.size(); ++i) {
        if (request.segments[i].trimmed().isEmpty())
            continue;
        index << i;
        q.append(request.segments[i]);
    }
    if (q.isEmpty() || (!source.isEmpty() && source == target)) {
        TranslationResult r;
        r.backend = id();
        r.segments = request.segments;
        r.detectedSource = source;
        QTimer::singleShot(0, this, [callback, r] { callback(r); });
        return;
    }

    auto send = [this, source, target, index, q, segments = request.segments, callback]() {
        QJsonObject body{
            {QStringLiteral("q"), q},
            {QStringLiteral("source"), source.isEmpty() ? QStringLiteral("auto") : serverCode(source)},
            {QStringLiteral("target"), serverCode(target)},
            {QStringLiteral("format"), QStringLiteral("text")},
        };
        if (!m_apiKey.isEmpty())
            body.insert(QStringLiteral("api_key"), m_apiKey);
        QNetworkRequest req = makeRequest(QStringLiteral("/translate"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Accept", "application/json");
        req.setTransferTimeout(kTranslateTimeoutMs);
        QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, source, index, segments, callback] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = reply->readAll();
            TranslationResult r;
            if (status == 0 && reply->error() != QNetworkReply::NoError) {
                r.backend = id();
                r.error = tr("LibreTranslate at %1: %2").arg(m_url.host(), reply->errorString());
                m_probe = Probe::Failed;
                m_probeAge.start();
                callback(r);
                return;
            }
            r = parseTranslateReply(status, data, int(index.size()));
            if (!r.ok()) {
                callback(r);
                return;
            }
            const QStringList translated = r.segments;
            r.segments = segments;
            for (int i = 0; i < index.size(); ++i)
                r.segments[index[i]] = translated[i];
            if (!source.isEmpty())
                r.detectedSource = source;
            else if (r.detectedSource.isEmpty())
                r.detectedSource = detectLanguage(segments).code;
            callback(r);
        });
    };

    // Learn the server's language codes first (cached), so "zh" can become "zh-Hans".
    if (m_languages.isEmpty() && m_probe != Probe::Ok)
        probe(2000, [send](bool) { send(); });
    else
        send();
}

}  // namespace selsearch
