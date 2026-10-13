// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace selsearch {

class Config;

// How an engine's suggestion endpoint answers.
enum class SuggestFormat {
    None,
    OpenSearch,  // ["query", ["s1", "s2", ...], ...]   (application/x-suggestions+json)
    DuckDuckGo,  // [{"phrase": "s1"}, {"phrase": "s2"}]
    Qwant,       // {"status":"success","data":{"items":[{"value":"s1"}, ...]}}
};

// A text search engine. Templates use %s for the URL-encoded query.
struct SearchEngine {
    QString id;           // "duckduckgo"
    QString name;         // "DuckDuckGo"
    QString searchUrl;    // "https://duckduckgo.com/?q=%s"
    QString suggestUrl;   // empty when the engine has no (usable) endpoint
    SuggestFormat suggestFormat = SuggestFormat::None;
    QString homepage;
    QString icon;         // icon id: "engine-<id>" for bundled logos, "lucide:globe" fallback

    bool isValid() const { return !id.isEmpty() && searchUrl.contains(QLatin1String("%s")); }
    QUrl searchUrlFor(const QString &query) const;
    QUrl suggestUrlFor(const QString &query) const;  // invalid when there is none
};

// Registry of the built-in engines plus the config-driven "searxng" (base
// from search.searxng_url) and "custom" (template from search.custom_url).
//
// Endpoint status (checked with curl from a datacenter network, 2026-10):
//   search pages answered 200 or a bot-check/captcha redirect that kept the
//   query (which confirms the template); suggestion endpoints returned the
//   documented JSON for: google, duckduckgo, kagi (kagisuggest.com), bing,
//   startpage, yandex, ecosia, qwant, searx.be. brave answered 429 (rate
//   limit) so its suggest URL is taken from its opensearch.xml; mojeek has no
//   suggestion endpoint.
class SearchEngines {
public:
    static const QList<SearchEngine> &builtin();
    // Built-ins with searxng/custom resolved against `config` (may be null).
    static QList<SearchEngine> all(const Config *config);
    // Engine by id; falls back to DuckDuckGo for unknown ids or an empty
    // custom template.
    static SearchEngine byId(const QString &id, const Config *config);
    // The engine selected in search.engine.
    static SearchEngine current(const Config *config);

    static SearchEngine searxng(const QString &baseUrl);
    static SearchEngine custom(const QString &urlTemplate);

    // Fill a %s template with the percent-encoded query.
    static QUrl fillTemplate(const QString &urlTemplate, const QString &query);
    // Parse a suggestion response. Returns at most `max` unique, non-empty
    // suggestions; tolerant of extra array members and unknown fields.
    static QStringList parseSuggestions(const QByteArray &body, SuggestFormat format, int max = 8);
};

}  // namespace selsearch
