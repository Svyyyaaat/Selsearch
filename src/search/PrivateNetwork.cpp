// SPDX-License-Identifier: MIT
#include "PrivateNetwork.h"

#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkRequest>

namespace selsearch {

namespace {

// A jar that never stores or returns cookies, so an engine can't tie our
// requests together.
class NoCookieJar : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
};

}  // namespace

QNetworkAccessManager *createPrivateNetworkAccessManager(QObject *parent)
{
    auto *nam = new QNetworkAccessManager(parent);
    nam->setCookieJar(new NoCookieJar(nam));
    nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    nam->setTransferTimeout(15000);
    return nam;
}

QByteArray genericUserAgent()
{
    return QByteArrayLiteral("Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
}

void preparePrivateRequest(QNetworkRequest &request)
{
    request.setHeader(QNetworkRequest::UserAgentHeader, genericUserAgent());
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

}  // namespace selsearch
