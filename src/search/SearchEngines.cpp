// SPDX-License-Identifier: MIT
#include "SearchEngines.h"

#include "core/Config.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace selsearch {

QUrl SearchEngine::searchUrlFor(const QString &query) const
{
    return SearchEngines::fillTemplate(searchUrl, query);
}

QUrl SearchEngine::suggestUrlFor(const QString &query) const
{
    if (suggestUrl.isEmpty() || suggestFormat == SuggestFormat::None)
        return {};
    return SearchEngines::fillTemplate(suggestUrl, query);
}

const QList<SearchEngine> &SearchEngines::builtin()
{
    using F = SuggestFormat;
    static const QList<SearchEngine> engines = {
        {QStringLiteral("google"), QStringLiteral("Google"),
         QStringLiteral("https://www.google.com/search?q=%s"),
         QStringLiteral("https://suggestqueries.google.com/complete/search?client=firefox&q=%s"), F::OpenSearch,
         QStringLiteral("https://www.google.com/"), QStringLiteral("engine-google")},
        {QStringLiteral("duckduckgo"), QStringLiteral("DuckDuckGo"),
         QStringLiteral("https://duckduckgo.com/?q=%s"),
         QStringLiteral("https://duckduckgo.com/ac/?q=%s&type=list"), F::OpenSearch,
         QStringLiteral("https://duckduckgo.com/"), QStringLiteral("engine-duckduckgo")},
        {QStringLiteral("brave"), QStringLiteral("Brave Search"),
         QStringLiteral("https://search.brave.com/search?q=%s"),
         QStringLiteral("https://search.brave.com/api/suggest?q=%s"), F::OpenSearch,
         QStringLiteral("https://search.brave.com/"), QStringLiteral("engine-brave")},
        {QStringLiteral("kagi"), QStringLiteral("Kagi"),
         QStringLiteral("https://kagi.com/search?q=%s"),
         QStringLiteral("https://kagisuggest.com/api/autosuggest?q=%s"), F::OpenSearch,
         QStringLiteral("https://kagi.com/"), QStringLiteral("engine-kagi")},
        {QStringLiteral("bing"), QStringLiteral("Bing"),
         QStringLiteral("https://www.bing.com/search?q=%s"),
         QStringLiteral("https://api.bing.com/osjson.aspx?query=%s"), F::OpenSearch,
         QStringLiteral("https://www.bing.com/"), QStringLiteral("engine-bing")},
        {QStringLiteral("startpage"), QStringLiteral("Startpage"),
         QStringLiteral("https://www.startpage.com/sp/search?query=%s"),
         QStringLiteral("https://www.startpage.com/osuggestions?q=%s"), F::OpenSearch,
         QStringLiteral("https://www.startpage.com/"), QStringLiteral("engine-startpage")},
        {QStringLiteral("yandex"), QStringLiteral("Yandex"),
         QStringLiteral("https://yandex.com/search/?text=%s"),
         QStringLiteral("https://suggest.yandex.com/suggest-ff.cgi?part=%s"), F::OpenSearch,
         QStringLiteral("https://yandex.com/"), QStringLiteral("engine-yandex")},
        {QStringLiteral("ecosia"), QStringLiteral("Ecosia"),
         QStringLiteral("https://www.ecosia.org/search?q=%s"),
         QStringLiteral("https://ac.ecosia.org/autocomplete?q=%s&type=list"), F::OpenSearch,
         QStringLiteral("https://www.ecosia.org/"), QStringLiteral("engine-ecosia")},
        {QStringLiteral("qwant"), QStringLiteral("Qwant"),
         QStringLiteral("https://www.qwant.com/?q=%s"),
         QStringLiteral("https://api.qwant.com/v3/suggest?q=%s"), F::Qwant,
         QStringLiteral("https://www.qwant.com/"), QStringLiteral("engine-qwant")},
        {QStringLiteral("mojeek"), QStringLiteral("Mojeek"),
         QStringLiteral("https://www.mojeek.com/search?q=%s"),
         QString(), F::None,
         QStringLiteral("https://www.mojeek.com/"), QStringLiteral("engine-mojeek")},
    };
    return engines;
}

SearchEngine SearchEngines::searxng(const QString &baseUrl)
{
    QString base = baseUrl.trimmed();
    if (base.isEmpty())
        base = Config::defaults().value(QStringLiteral("search.searxng_url")).toString();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    SearchEngine e;
    e.id = QStringLiteral("searxng");
    e.name = QStringLiteral("SearXNG");
    e.searchUrl = base + QStringLiteral("/search?q=%s");
    // OpenSearch JSON is SearXNG's default autocompleter output; instances
    // without an autocomplete backend answer with an empty list.
    e.suggestUrl = base + QStringLiteral("/autocompleter?q=%s");
    e.suggestFormat = SuggestFormat::OpenSearch;
    e.homepage = base + QLatin1Char('/');
    e.icon = QStringLiteral("engine-searxng");
    return e;
}

SearchEngine SearchEngines::custom(const QString &urlTemplate)
{
    SearchEngine e;
    e.id = QStringLiteral("custom");
    e.name = QUrl(urlTemplate).host();
    if (e.name.startsWith(QLatin1String("www.")))
        e.name = e.name.mid(4);
    if (e.name.isEmpty())
        e.name = QStringLiteral("Custom");
    e.searchUrl = urlTemplate.trimmed();
    const QUrl u(urlTemplate);
    e.homepage = u.scheme() + QStringLiteral("://") + u.authority() + QLatin1Char('/');
    e.icon = QStringLiteral("lucide:globe");
    return e;
}

QList<SearchEngine> SearchEngines::all(const Config *config)
{
    QList<SearchEngine> out = builtin();
    out << searxng(config ? config->string(QStringLiteral("search.searxng_url")) : QString());
    const SearchEngine c = custom(config ? config->string(QStringLiteral("search.custom_url")) : QString());
    if (c.isValid())
        out << c;
    return out;
}

SearchEngine SearchEngines::byId(const QString &id, const Config *config)
{
    if (id == QLatin1String("searxng"))
        return searxng(config ? config->string(QStringLiteral("search.searxng_url")) : QString());
    if (id == QLatin1String("custom")) {
        const SearchEngine c = custom(config ? config->string(QStringLiteral("search.custom_url")) : QString());
        if (c.isValid())
            return c;
    }
    for (const SearchEngine &e : builtin()) {
        if (e.id == id)
            return e;
    }
    return builtin().at(1);  // duckduckgo
}

SearchEngine SearchEngines::current(const Config *config)
{
    return byId(config ? config->string(QStringLiteral("search.engine")) : QStringLiteral("duckduckgo"), config);
}

QUrl SearchEngines::fillTemplate(const QString &urlTemplate, const QString &query)
{
    if (!urlTemplate.contains(QLatin1String("%s")))
        return {};
    QString filled = urlTemplate;
    // Encode everything except unreserved characters so '&', '+', '#' and
    // '%' in the query can't break out of the parameter.
    filled.replace(QLatin1String("%s"), QString::fromLatin1(QUrl::toPercentEncoding(query)));
    return QUrl::fromEncoded(filled.toUtf8(), QUrl::TolerantMode);
}

QStringList SearchEngines::parseSuggestions(const QByteArray &body, SuggestFormat format, int max)
{
    QStringList out;
    auto add = [&](const QString &s) {
        const QString t = s.trimmed();
        if (!t.isEmpty() && !out.contains(t) && out.size() < max)
            out << t;
    };
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    switch (format) {
    case SuggestFormat::None:
        break;
    case SuggestFormat::OpenSearch:
    case SuggestFormat::DuckDuckGo:
        // Accept either shape for both: DuckDuckGo serves both depending on
        // the type= parameter.
        if (doc.isArray()) {
            const QJsonArray a = doc.array();
            if (a.size() >= 2 && a.at(0).isString() && a.at(1).isArray()) {
                for (const QJsonValue &v : a.at(1).toArray())
                    add(v.toString());
            } else {
                for (const QJsonValue &v : a) {
                    if (v.isObject())
                        add(v.toObject().value(QLatin1String("phrase")).toString());
                }
            }
        }
        break;
    case SuggestFormat::Qwant: {
        const QJsonArray items = doc.object().value(QLatin1String("data")).toObject().value(QLatin1String("items")).toArray();
        for (const QJsonValue &v : items)
            add(v.toObject().value(QLatin1String("value")).toString());
        break;
    }
    }
    return out;
}

}  // namespace selsearch
