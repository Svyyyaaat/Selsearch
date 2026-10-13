// SPDX-License-Identifier: MIT
#include "SuggestionFetcher.h"

#include "PrivateNetwork.h"
#include "core/Config.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace selsearch {

namespace {
constexpr qsizetype kMaxBody = 64 * 1024;
}  // namespace

QByteArray SuggestionFetcher::userAgent()
{
    return genericUserAgent();
}

SuggestionFetcher::SuggestionFetcher(const Config *config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_nam(createPrivateNetworkAccessManager(this))
{
    m_nam->setTransferTimeout(4000);  // stale suggestions are useless
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, &SuggestionFetcher::start);
}

SuggestionFetcher::~SuggestionFetcher()
{
    cancel();
}

bool SuggestionFetcher::isEnabled() const
{
    return m_config && m_config->boolean(QStringLiteral("search.suggestions"));
}

SearchEngine SuggestionFetcher::engine() const
{
    return m_override.isValid() ? m_override : SearchEngines::current(m_config);
}

void SuggestionFetcher::fetch(const QString &query)
{
    const QString q = query.trimmed();
    if (!isEnabled() || q.isEmpty()) {
        cancel();
        return;
    }
    m_pending = q;
    m_debounce.start();
}

void SuggestionFetcher::cancel()
{
    m_debounce.stop();
    m_pending.clear();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void SuggestionFetcher::start()
{
    const QString q = m_pending;
    if (q.isEmpty() || !isEnabled())
        return;
    // Only the newest query matters.
    if (m_reply) {
        QNetworkReply *old = m_reply;
        m_reply = nullptr;
        old->disconnect(this);
        old->abort();
        old->deleteLater();
    }
    const SearchEngine e = engine();
    const QUrl url = e.suggestUrlFor(q);
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")))
        return;

    QNetworkRequest req(url);
    preparePrivateRequest(req);
    req.setRawHeader("Accept", "application/json, application/x-suggestions+json;q=0.9, */*;q=0.1");

    QNetworkReply *reply = m_nam->get(req);
    m_reply = reply;
    const SuggestFormat format = e.suggestFormat;
    connect(reply, &QNetworkReply::finished, this, [this, reply, q, format] {
        reply->deleteLater();
        if (m_reply == reply)
            m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError)
                Q_EMIT failed(q, reply->errorString());
            return;
        }
        const QByteArray body = reply->read(kMaxBody);
        Q_EMIT suggestionsReady(q, SearchEngines::parseSuggestions(body, format));
    });
}

}  // namespace selsearch
