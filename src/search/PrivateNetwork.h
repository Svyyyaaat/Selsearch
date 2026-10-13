// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>

class QNetworkAccessManager;
class QNetworkRequest;
class QObject;

namespace selsearch {

// Network access for requests Selsearch itself makes to search engines
// (suggestions, native image uploads): a cookie jar that never stores or
// sends cookies, no disk cache, only redirects that keep https, and a
// transfer timeout.
QNetworkAccessManager *createPrivateNetworkAccessManager(QObject *parent);

// A common desktop browser User-Agent; nothing Selsearch-specific.
QByteArray genericUserAgent();

// Apply the per-request side of the above (UA, no cookies, no cache).
void preparePrivateRequest(QNetworkRequest &request);

}  // namespace selsearch
