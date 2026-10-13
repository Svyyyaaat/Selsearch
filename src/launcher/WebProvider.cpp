// SPDX-License-Identifier: MIT
#include "WebProvider.h"

#include "core/Config.h"
#include "search/SearchEngines.h"

#include <QDesktopServices>
#include <QRegularExpression>
#include <QSet>

namespace selsearch {

namespace {

// Generic TLDs people actually type, plus every two-letter ccTLD.
const QSet<QString> &knownTlds()
{
    static const QSet<QString> tlds = [] {
        QSet<QString> s;
        for (const char *t : {"com", "org", "net", "edu", "gov", "mil", "int", "info", "biz", "name", "pro",
                              "dev", "app", "io", "ai", "xyz", "online", "site", "tech", "page", "cloud", "blog",
                              "shop", "store", "news", "wiki", "art", "design", "social", "space", "website",
                              "live", "life", "world", "today", "email", "games", "media", "network", "systems",
                              "software", "codes", "chat", "link", "click", "top", "club", "fun", "one", "group",
                              "company", "agency", "studio", "zone", "works", "tools", "academy", "museum",
                              "travel", "aero", "coop", "jobs", "mobi", "asia", "cat", "tel", "eu", "onion",
                              "local", "lan", "home", "arpa", "test", "example", "internal"})
            s.insert(QString::fromLatin1(t));
        // Two-letter country codes (ISO 3166-1 alpha-2 + uk/eu/ac).
        const char *cc =
            "ac ad ae af ag ai al am ao aq ar as at au aw ax az ba bb bd be bf bg bh bi bj bm bn bo br bs bt bw by "
            "bz ca cc cd cf cg ch ci ck cl cm cn co cr cu cv cw cx cy cz de dj dk dm do dz ec ee eg er es et eu fi "
            "fj fk fm fo fr ga gd ge gf gg gh gi gl gm gn gp gq gr gs gt gu gw gy hk hm hn hr ht hu id ie il im in "
            "io iq ir is it je jm jo jp ke kg kh ki km kn kp kr kw ky kz la lb lc li lk lr ls lt lu lv ly ma mc md "
            "me mg mh mk ml mm mn mo mp mq mr ms mt mu mv mw mx my mz na nc ne nf ng ni nl no np nr nu nz om pa pe "
            "pf pg ph pk pl pm pn pr ps pt pw py qa re ro rs ru rw sa sb sc sd se sg sh si sk sl sm sn so sr ss st "
            "su sv sx sy sz tc td tf tg th tj tk tl tm tn to tr tt tv tw tz ua ug uk us uy uz va vc ve vg vi vn vu "
            "wf ws ye yt za zm zw";
        for (const QString &t : QString::fromLatin1(cc).split(QLatin1Char(' ')))
            s.insert(t);
        return s;
    }();
    return tlds;
}

// TLDs that are also common file extensions: "readme.md", "setup.py",
// "run.sh", "lib.rs", "main.pl". These need "www." or a path/port to count.
const QSet<QString> &ambiguousTlds()
{
    static const QSet<QString> s = {
        QStringLiteral("md"), QStringLiteral("py"), QStringLiteral("sh"), QStringLiteral("rs"),
        QStringLiteral("pl"), QStringLiteral("cc"), QStringLiteral("hs"), QStringLiteral("ml"),
        QStringLiteral("so"), QStringLiteral("mk"), QStringLiteral("am"), QStringLiteral("ps"),
        QStringLiteral("rb"), QStringLiteral("cs"), QStringLiteral("in"), QStringLiteral("db"),
        QStringLiteral("gz"), QStringLiteral("xz"), QStringLiteral("zip"), QStringLiteral("jar"),
        QStringLiteral("sc"), QStringLiteral("cpp"),
        QStringLiteral("hh"), QStringLiteral("tf"), QStringLiteral("ts"), QStringLiteral("js"),
        QStringLiteral("cr"), QStringLiteral("nu"), QStringLiteral("ms"), QStringLiteral("info"),
        QStringLiteral("local"), QStringLiteral("home"), QStringLiteral("lan"),
    };
    return s;
}

}  // namespace

WebProvider::WebProvider(const Config *config)
    : m_config(config)
{
}

QUrl WebProvider::detectUrl(const QString &input)
{
    const QString text = input.trimmed();
    if (text.isEmpty() || text.size() > 2048)
        return {};
    for (QChar c : text) {
        if (c.isSpace())
            return {};
    }

    // Explicit scheme. "localhost:8080" also parses as scheme:path, so only
    // accept "scheme://" or a few well-known schemes without slashes.
    static const QRegularExpression schemeRe(QStringLiteral(R"(^([A-Za-z][A-Za-z0-9+.\-]*):(.*)$)"));
    const auto sm = schemeRe.match(text);
    if (sm.hasMatch()) {
        const QString scheme = sm.captured(1).toLower();
        static const QSet<QString> bare = {QStringLiteral("mailto"), QStringLiteral("magnet"),
                                           QStringLiteral("about"), QStringLiteral("tel")};
        if (sm.captured(2).startsWith(QLatin1String("//")) || bare.contains(scheme)) {
            const QUrl u(text, QUrl::StrictMode);
            if (u.isValid() && !sm.captured(2).isEmpty() && sm.captured(2) != QLatin1String("//"))
                return u;
            return {};
        }
    }

    static const QRegularExpression hostRe(
        QStringLiteral(R"(^(?<host>[^/:?#@]+)(?::(?<port>\d{1,5}))?(?<rest>[/?#].*)?$)"));
    const auto hm = hostRe.match(text);
    if (!hm.hasMatch())
        return {};
    const QString host = hm.captured(u"host").toLower();
    const QString port = hm.captured(u"port");
    const QString rest = hm.captured(u"rest");
    const bool hasExtra = !port.isEmpty() || !rest.isEmpty();
    if (!port.isEmpty() && port.toInt() > 65535)
        return {};

    auto make = [&](const QString &scheme) {
        QUrl u(scheme + QStringLiteral("://") + text, QUrl::TolerantMode);
        return u.isValid() && !u.host().isEmpty() ? u : QUrl();
    };

    if (host == QLatin1String("localhost"))
        return make(QStringLiteral("http"));

    static const QRegularExpression ipv4(QStringLiteral(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)"));
    const auto im = ipv4.match(host);
    if (im.hasMatch()) {
        for (int i = 1; i <= 4; ++i) {
            if (im.captured(i).toInt() > 255)
                return {};
        }
        return make(QStringLiteral("http"));
    }

    static const QRegularExpression label(QStringLiteral(R"(^[\p{L}\p{N}](?:[\p{L}\p{N}\-]{0,61}[\p{L}\p{N}])?$)"));
    const QStringList labels = host.split(QLatin1Char('.'));
    if (labels.size() < 2)
        return {};
    for (const QString &l : labels) {
        if (!label.match(l).hasMatch())
            return {};
    }
    const QString tld = labels.last();
    const bool idnTld = tld.startsWith(QLatin1String("xn--"));
    if (!idnTld && !knownTlds().contains(tld))
        return {};
    if (ambiguousTlds().contains(tld) && !hasExtra && labels.first() != QLatin1String("www"))
        return {};
    return make(QStringLiteral("https"));
}

QList<LauncherItem> WebProvider::query(const QString &text, int limit)
{
    const QString q = text.trimmed();
    QList<LauncherItem> out;
    if (q.isEmpty() || limit <= 0)
        return out;

    const QUrl url = detectUrl(q);
    if (url.isValid()) {
        LauncherItem open;
        open.kind = LauncherItem::Url;
        open.id = url.toString();
        open.title = QStringLiteral("Open %1").arg(q);
        open.subtitle = url.toDisplayString();
        open.icon = QStringLiteral("lucide:external-link");
        open.group = QStringLiteral("Web");
        open.score = 1000;
        open.data.insert(QStringLiteral("url"), url);
        out << open;
    }

    const SearchEngine engine = SearchEngines::current(m_config);
    LauncherItem search;
    search.kind = LauncherItem::WebSearch;
    search.id = QStringLiteral("web-search");
    search.title = QStringLiteral("Search %1 for “%2”").arg(engine.name, q);
    search.subtitle = QUrl(engine.homepage).host();
    search.icon = engine.icon.isEmpty() ? QStringLiteral("lucide:search") : engine.icon;
    search.group = QStringLiteral("Web");
    search.score = 0;
    search.data.insert(QStringLiteral("url"), engine.searchUrlFor(q));
    search.data.insert(QStringLiteral("engine"), engine.id);
    search.data.insert(QStringLiteral("query"), q);
    out << search;
    return out;
}

QUrl WebProvider::urlFor(const LauncherItem &item)
{
    return item.data.value(QStringLiteral("url")).toUrl();
}

bool WebProvider::activate(const LauncherItem &item)
{
    const QUrl url = urlFor(item);
    return url.isValid() && QDesktopServices::openUrl(url);
}

}  // namespace selsearch
