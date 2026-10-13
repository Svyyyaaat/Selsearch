// SPDX-License-Identifier: MIT
#include "core/Config.h"
#include "search/ImageSearch.h"
#include "search/SearchEngines.h"
#include "search/SuggestionFetcher.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace selsearch;

// A tiny HTTP server that records request heads and answers every request
// with a fixed OpenSearch suggestion body (and tries to set a cookie).
class FakeSuggestServer : public QObject {
    Q_OBJECT
public:
    FakeSuggestServer()
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] {
                    buffer[s] += s->readAll();
                    if (!buffer[s].contains("\r\n\r\n"))
                        return;
                    requests << buffer[s];
                    const QByteArray q = QRegularExpression(QStringLiteral("q=([^ &]*)"))
                                             .match(QString::fromLatin1(buffer[s])).captured(1).toLatin1();
                    const QByteArray body = "[\"" + QByteArray::fromPercentEncoding(q) + "\",[\"" +
                        QByteArray::fromPercentEncoding(q) + " one\",\"" + QByteArray::fromPercentEncoding(q) + " two\"]]";
                    auto respond = [s, body] {
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-suggestions+json\r\n"
                                 "Set-Cookie: track=me; Path=/\r\nConnection: close\r\nContent-Length: "
                                 + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        s->disconnectFromHost();
                    };
                    if (delayMs > 0)
                        QTimer::singleShot(delayMs, s, respond);
                    else
                        respond();
                });
            }
        });
        server.listen(QHostAddress::LocalHost);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }

    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffer;
    QList<QByteArray> requests;
    int delayMs = 0;
};

class TestSearch : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void registry()
    {
        const QStringList expected = {
            QStringLiteral("google"), QStringLiteral("duckduckgo"), QStringLiteral("brave"), QStringLiteral("kagi"),
            QStringLiteral("bing"), QStringLiteral("startpage"), QStringLiteral("yandex"), QStringLiteral("ecosia"),
            QStringLiteral("qwant"), QStringLiteral("mojeek"),
        };
        QStringList ids;
        for (const SearchEngine &e : SearchEngines::builtin()) {
            ids << e.id;
            QVERIFY2(e.isValid(), qPrintable(e.id));
            QVERIFY(e.searchUrl.startsWith(QLatin1String("https://")));
            QVERIFY(!e.name.isEmpty());
            QVERIFY(!e.icon.isEmpty());
            QVERIFY(QUrl(e.homepage).isValid());
            QVERIFY(e.searchUrlFor(QStringLiteral("x")).isValid());
            if (e.suggestFormat != SuggestFormat::None)
                QVERIFY2(e.suggestUrlFor(QStringLiteral("x")).scheme() == QLatin1String("https"), qPrintable(e.id));
        }
        QCOMPARE(ids, expected);

        QTemporaryDir tmp;
        Config config;
        QVERIFY(config.load(tmp.filePath(QStringLiteral("config.toml"))));
        QCOMPARE(config.string(QStringLiteral("search.searxng_url")), QStringLiteral("https://searx.be"));
        QStringList all;
        for (const SearchEngine &e : SearchEngines::all(&config))
            all << e.id;
        QVERIFY(all.contains(QStringLiteral("searxng")));
        QVERIFY(!all.contains(QStringLiteral("custom")));  // no template set
        config.setValue(QStringLiteral("search.custom_url"), QStringLiteral("https://www.example.org/find?q=%s&lang=en"));
        all.clear();
        for (const SearchEngine &e : SearchEngines::all(&config))
            all << e.id;
        QVERIFY(all.contains(QStringLiteral("custom")));
    }

    void urlBuilding()
    {
        auto url = [](const char *id, const QString &q) {
            return SearchEngines::byId(QString::fromLatin1(id), nullptr).searchUrlFor(q).toString(QUrl::FullyEncoded);
        };
        QCOMPARE(url("google", QStringLiteral("a b&c+d")), QStringLiteral("https://www.google.com/search?q=a%20b%26c%2Bd"));
        QCOMPARE(url("duckduckgo", QStringLiteral("café #1")), QStringLiteral("https://duckduckgo.com/?q=caf%C3%A9%20%231"));
        QCOMPARE(url("brave", QStringLiteral("x")), QStringLiteral("https://search.brave.com/search?q=x"));
        QCOMPARE(url("kagi", QStringLiteral("x")), QStringLiteral("https://kagi.com/search?q=x"));
        QCOMPARE(url("bing", QStringLiteral("x")), QStringLiteral("https://www.bing.com/search?q=x"));
        QCOMPARE(url("startpage", QStringLiteral("x")), QStringLiteral("https://www.startpage.com/sp/search?query=x"));
        QCOMPARE(url("yandex", QStringLiteral("привет")), QStringLiteral("https://yandex.com/search/?text=%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82"));
        QCOMPARE(url("ecosia", QStringLiteral("x")), QStringLiteral("https://www.ecosia.org/search?q=x"));
        QCOMPARE(url("qwant", QStringLiteral("x")), QStringLiteral("https://www.qwant.com/?q=x"));
        QCOMPARE(url("mojeek", QStringLiteral("100%")), QStringLiteral("https://www.mojeek.com/search?q=100%25"));
        // Unknown ids fall back to DuckDuckGo.
        QCOMPARE(url("nope", QStringLiteral("x")), QStringLiteral("https://duckduckgo.com/?q=x"));

        const SearchEngine ddg = SearchEngines::byId(QStringLiteral("duckduckgo"), nullptr);
        QCOMPARE(ddg.suggestUrlFor(QStringLiteral("a b")).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://duckduckgo.com/ac/?q=a%20b&type=list"));
        QVERIFY(!SearchEngines::byId(QStringLiteral("mojeek"), nullptr).suggestUrlFor(QStringLiteral("x")).isValid());

        QTemporaryDir tmp;
        Config config;
        QVERIFY(config.load(tmp.filePath(QStringLiteral("config.toml"))));
        QCOMPARE(SearchEngines::byId(QStringLiteral("searxng"), &config).searchUrlFor(QStringLiteral("x")),
                 QUrl(QStringLiteral("https://searx.be/search?q=x")));
        config.setValue(QStringLiteral("search.searxng_url"), QStringLiteral("https://search.example.net/searx/"));
        const SearchEngine sx = SearchEngines::byId(QStringLiteral("searxng"), &config);
        QCOMPARE(sx.searchUrlFor(QStringLiteral("x")), QUrl(QStringLiteral("https://search.example.net/searx/search?q=x")));
        QCOMPARE(sx.suggestUrlFor(QStringLiteral("x")), QUrl(QStringLiteral("https://search.example.net/searx/autocompleter?q=x")));

        // Custom: empty template falls back, a set one is used.
        QCOMPARE(SearchEngines::byId(QStringLiteral("custom"), &config).id, QStringLiteral("duckduckgo"));
        config.setValue(QStringLiteral("search.custom_url"), QStringLiteral("https://www.example.org/find?q=%s&lang=en"));
        config.setValue(QStringLiteral("search.engine"), QStringLiteral("custom"));
        const SearchEngine custom = SearchEngines::current(&config);
        QCOMPARE(custom.id, QStringLiteral("custom"));
        QCOMPARE(custom.name, QStringLiteral("example.org"));
        QCOMPARE(custom.searchUrlFor(QStringLiteral("a&b")).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://www.example.org/find?q=a%26b&lang=en"));
    }

    void suggestionParsing()
    {
        // Google (client=firefox): extra members after the list.
        const QByteArray google = R"(["hello",["hello","hello kitty","hello fresh"],[],{"google:suggesttype":["QUERY","QUERY","QUERY"]}])";
        QCOMPARE(SearchEngines::parseSuggestions(google, SuggestFormat::OpenSearch),
                 (QStringList{QStringLiteral("hello"), QStringLiteral("hello kitty"), QStringLiteral("hello fresh")}));
        // Yandex suggest-ff, with non-ASCII.
        const QByteArray yandex = R"(["привет",["привет","привет мир"]])";
        QCOMPARE(SearchEngines::parseSuggestions(yandex, SuggestFormat::OpenSearch).value(1), QStringLiteral("привет мир"));
        // DuckDuckGo phrase objects, accepted under either format.
        const QByteArray ddg = R"([{"phrase":"helloworld"},{"phrase":"hello"},{"phrase":"hello"},{"phrase":"  "}])";
        QCOMPARE(SearchEngines::parseSuggestions(ddg, SuggestFormat::DuckDuckGo),
                 (QStringList{QStringLiteral("helloworld"), QStringLiteral("hello")}));
        QCOMPARE(SearchEngines::parseSuggestions(ddg, SuggestFormat::OpenSearch).size(), 2);
        // Qwant.
        const QByteArray qwant = R"({"status":"success","data":{"items":[{"value":"helloworld","suggestType":0},{"value":"hello kitty","suggestType":0}],"special":[]}})";
        QCOMPARE(SearchEngines::parseSuggestions(qwant, SuggestFormat::Qwant),
                 (QStringList{QStringLiteral("helloworld"), QStringLiteral("hello kitty")}));
        // Limits and garbage.
        const QByteArray many = R"(["q",["1","2","3","4","5","6","7","8","9","10"]])";
        QCOMPARE(SearchEngines::parseSuggestions(many, SuggestFormat::OpenSearch, 3).size(), 3);
        QCOMPARE(SearchEngines::parseSuggestions(many, SuggestFormat::OpenSearch).size(), 8);
        QVERIFY(SearchEngines::parseSuggestions("<html>429</html>", SuggestFormat::OpenSearch).isEmpty());
        QVERIFY(SearchEngines::parseSuggestions("{}", SuggestFormat::OpenSearch).isEmpty());
        QVERIFY(SearchEngines::parseSuggestions(google, SuggestFormat::None).isEmpty());
    }

    void suggestionFetcherDisabledByDefault()
    {
        QTemporaryDir tmp;
        Config config;
        QVERIFY(config.load(tmp.filePath(QStringLiteral("config.toml"))));
        FakeSuggestServer server;
        SuggestionFetcher fetcher(&config);
        SearchEngine e;
        e.id = QStringLiteral("fake");
        e.name = QStringLiteral("Fake");
        e.searchUrl = server.base() + QStringLiteral("/search?q=%s");
        e.suggestUrl = server.base() + QStringLiteral("/s?q=%s");
        e.suggestFormat = SuggestFormat::OpenSearch;
        fetcher.setEngineOverride(e);
        QSignalSpy spy(&fetcher, &SuggestionFetcher::suggestionsReady);
        QVERIFY(!fetcher.isEnabled());
        fetcher.fetch(QStringLiteral("hello"));
        QTest::qWait(400);
        QCOMPARE(spy.count(), 0);
        QVERIFY(server.requests.isEmpty());
    }

    void suggestionFetcherDebounceAndCancel()
    {
        QTemporaryDir tmp;
        Config config;
        QVERIFY(config.load(tmp.filePath(QStringLiteral("config.toml"))));
        config.setValue(QStringLiteral("search.suggestions"), true);
        FakeSuggestServer server;
        SuggestionFetcher fetcher(&config);
        SearchEngine e;
        e.id = QStringLiteral("fake");
        e.name = QStringLiteral("Fake");
        e.searchUrl = server.base() + QStringLiteral("/search?q=%s");
        e.suggestUrl = server.base() + QStringLiteral("/s?q=%s");
        e.suggestFormat = SuggestFormat::OpenSearch;
        fetcher.setEngineOverride(e);
        QSignalSpy spy(&fetcher, &SuggestionFetcher::suggestionsReady);

        // Typing quickly: only the last query goes out.
        fetcher.fetch(QStringLiteral("h"));
        fetcher.fetch(QStringLiteral("he"));
        fetcher.fetch(QStringLiteral("hel lo"));
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("hel lo"));
        QCOMPARE(spy.at(0).at(1).toStringList(), (QStringList{QStringLiteral("hel lo one"), QStringLiteral("hel lo two")}));
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(server.requests[0].startsWith("GET /s?q=hel%20lo "));
        QVERIFY(server.requests[0].toLower().contains("user-agent: mozilla/5.0 (x11; linux x86_64"));

        // Second request: the cookie set above is not sent back.
        fetcher.fetch(QStringLiteral("again"));
        QTRY_COMPARE(spy.count(), 2);
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(!server.requests[1].toLower().contains("cookie"));

        // A slow in-flight request is aborted by a newer query.
        server.delayMs = 400;
        fetcher.fetch(QStringLiteral("slow"));
        QTRY_COMPARE(server.requests.size(), 3);  // "slow" is in flight
        fetcher.fetch(QStringLiteral("fast"));
        QTRY_COMPARE(spy.count(), 3);
        QTest::qWait(600);
        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.at(2).at(0).toString(), QStringLiteral("fast"));

        // cancel() drops a pending fetch.
        server.delayMs = 0;
        fetcher.fetch(QStringLiteral("never"));
        fetcher.cancel();
        QTest::qWait(300);
        QCOMPARE(spy.count(), 3);
    }

    void imageEncoding()
    {
        QImage big(3000, 1500, QImage::Format_ARGB32);
        big.fill(Qt::transparent);
        QPainter p(&big);
        p.fillRect(0, 0, 1500, 1500, Qt::red);
        p.end();
        const QByteArray jpeg = ImageSearch::encodeJpeg(big);
        QVERIFY(jpeg.startsWith("\xFF\xD8"));
        QImage decoded;
        QVERIFY(decoded.loadFromData(jpeg, "JPEG"));
        QCOMPARE(decoded.size(), QSize(1000, 500));
        // Transparent areas become white, not black.
        QVERIFY(qGray(decoded.pixel(900, 250)) > 240);
        QVERIFY(qRed(decoded.pixel(100, 250)) > 200);

        QImage small(200, 100, QImage::Format_RGB32);
        small.fill(Qt::blue);
        QImage d2;
        QVERIFY(d2.loadFromData(ImageSearch::encodeJpeg(small), "JPEG"));
        QCOMPARE(d2.size(), QSize(200, 100));  // never upscaled
        QVERIFY(ImageSearch::encodeJpeg(QImage()).isEmpty());
    }

    void imageSearchHtml_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("field");
        QTest::newRow("google_lens") << QStringLiteral("google_lens") << QStringLiteral("https://lens.google.com/v3/upload?ep=gsbubb&amp;re=df&amp;hl=") << QStringLiteral("encoded_image");
        QTest::newRow("bing") << QStringLiteral("bing") << QStringLiteral("https://www.bing.com/images/search?view=detailv2&amp;iss=sbiupload") << QStringLiteral("imageBin");
        QTest::newRow("tineye") << QStringLiteral("tineye") << QStringLiteral("https://tineye.com/api/v1/result_json/") << QStringLiteral("image");
    }

    void imageSearchHtml()
    {
        QFETCH(QString, engine);
        QFETCH(QString, action);
        QFETCH(QString, field);
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::darkGreen);
        const QByteArray jpeg = ImageSearch::encodeJpeg(img);
        const auto e = ImageSearch::engine(engine);
        QVERIFY(e);
        const QString html = ImageSearch::buildHtml(jpeg, *e);
        const QString b64 = QString::fromLatin1(jpeg.toBase64());

        QVERIFY(html.startsWith(QLatin1String("<!doctype html>")));
        QVERIFY(html.contains(QLatin1String("<form id=\"f\" method=\"post\" enctype=\"multipart/form-data\" action=\"") + action));
        QVERIFY2(html.contains(QStringLiteral("name=\"%1\"").arg(field)), qPrintable(field));
        QVERIFY(html.contains(b64));
        QVERIFY(html.contains(QStringLiteral("Uploading image to %1").arg(e->name)));
        QVERIFY(html.contains(QLatin1String("<button type=\"submit\"")));
        QVERIFY(html.contains(QLatin1String("Content-Security-Policy")));
        QVERIFY(html.contains(QLatin1String("<meta name=\"referrer\" content=\"no-referrer\">")));
        if (engine == QLatin1String("bing")) {
            QVERIFY(html.contains(QStringLiteral("name=\"imageBin\" value=\"%1\"").arg(b64)));
            QVERIFY(html.contains(QLatin1String("name=\"cbir\" value=\"sbi\"")));
        } else {
            QVERIFY(html.contains(QStringLiteral("<input type=\"file\" name=\"%1\"").arg(field)));
            QVERIFY(html.contains(QLatin1String("new DataTransfer()")));
            QVERIFY(html.contains(QLatin1String("input.files = dt.files")));
        }
        if (engine == QLatin1String("tineye")) {
            const QString sha1 = QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha1).toHex());
            QVERIFY(html.contains(QStringLiteral("https://tineye.com/search/") + sha1));
            QVERIFY(html.contains(QLatin1String("target=\"sink\"")));
        }
        if (engine == QLatin1String("google_lens"))
            QVERIFY(html.contains(QRegularExpression(QStringLiteral("&amp;st=\\d{13}"))));
        // The script must not be able to break out of its element.
        const qsizetype scriptStart = html.indexOf(QLatin1String("<script>"));
        QVERIFY(scriptStart > 0);
        QCOMPARE(html.indexOf(QLatin1String("</script>"), scriptStart), html.lastIndexOf(QLatin1String("</script>")));
    }

    void yandexReply()
    {
        // Trimmed real reply from the JSON upload endpoint.
        const QByteArray reply = R"({"cnt":"pageview_candidate","blocks":[{"name":{"block":"b-page_type_search-by-image__link","mods":{}},"params":{"originalImageUrl":"https://avatars.mds.yandex.net/get-images-cbir/1750003/3b0vSxdMM1c35SYVaZVFvw9718/orig","cbirId":"1750003/3b0vSxdMM1c35SYVaZVFvw9718","bundles":[]},"html":""}],"metadata":{}})";
        const QUrl url = ImageSearch::parseYandexReply(reply);
        QCOMPARE(url.toString(QUrl::FullyEncoded),
                 QStringLiteral("https://yandex.com/images/search?rpt=imageview&cbir_id=1750003%2F3b0vSxdMM1c35SYVaZVFvw9718"));
        QVERIFY(!ImageSearch::parseYandexReply(R"({"blocks":[{"params":{}}]})").isValid());
        QVERIFY(!ImageSearch::parseYandexReply("<html>captcha</html>").isValid());
        const auto e = ImageSearch::engine(QStringLiteral("yandex"));
        QVERIFY(e && !e->pageBased());
        QCOMPARE(e->field, QStringLiteral("upfile"));
        QVERIFY(e->action.startsWith(QLatin1String("https://yandex.com/images/search?rpt=imageview&format=json&request=")));
    }

    void nativeUpload()
    {
        // A local stand-in for the Yandex endpoint: reads the whole multipart
        // request, answers with the JSON reply.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray request;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = server.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [&, s] {
                request += s->readAll();
                const qsizetype headerEnd = request.indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                const auto m = QRegularExpression(QStringLiteral("(?i)content-length: *(\\d+)")).match(QString::fromLatin1(request.left(headerEnd)));
                if (!m.hasMatch() || request.size() - headerEnd - 4 < m.captured(1).toLongLong())
                    return;
                const QByteArray body = R"({"blocks":[{"params":{"cbirId":"42/abc"}}]})";
                s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                         + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                s->disconnectFromHost();
            });
        });
        auto e = *ImageSearch::engine(QStringLiteral("yandex"));
        e.action = QStringLiteral("http://127.0.0.1:%1/images/search?rpt=imageview&format=json").arg(server.serverPort());
        QImage img(2000, 1000, QImage::Format_RGB32);
        img.fill(Qt::cyan);
        QUrl result;
        QString error;
        bool called = false;
        ImageSearch::start(img, e, this, [&](const QUrl &u, const QString &err) {
            result = u;
            error = err;
            called = true;
        });
        QTRY_VERIFY(called);
        QVERIFY2(result.isValid(), qPrintable(error));
        QCOMPARE(result.toString(QUrl::FullyEncoded), QStringLiteral("https://yandex.com/images/search?rpt=imageview&cbir_id=42%2Fabc"));
        QVERIFY(request.startsWith("POST /images/search?rpt=imageview&format=json "));
        QVERIFY(request.contains("name=\"upfile\"; filename=\"image.jpg\""));
        QVERIFY(request.toLower().contains("content-type: image/jpeg"));
        QVERIFY(!request.toLower().contains("\r\ncookie:"));
        // The uploaded image was downscaled.
        const qsizetype jpegStart = request.indexOf("\xFF\xD8");
        QVERIFY(jpegStart > 0);
        QImage uploaded;
        QVERIFY(uploaded.loadFromData(request.mid(jpegStart, request.lastIndexOf("\xFF\xD9") + 2 - jpegStart), "JPEG"));
        QCOMPARE(uploaded.size(), QSize(1000, 500));
    }

    void imageSearchPrepare()
    {
        QTemporaryDir tmp;
        const QByteArray saved = qgetenv("XDG_RUNTIME_DIR");
        qputenv("XDG_RUNTIME_DIR", tmp.path().toUtf8());

        QImage img(1600, 1200, QImage::Format_RGB32);
        img.fill(Qt::yellow);
        QString error;
        const QUrl url = ImageSearch::prepare(img, QStringLiteral("google_lens"), &error);
        QVERIFY2(url.isValid(), qPrintable(error));
        QVERIFY(url.isLocalFile());
        const QFileInfo fi(url.toLocalFile());
        QCOMPARE(fi.absolutePath(), tmp.path() + QStringLiteral("/selsearch"));
        QVERIFY(fi.fileName().startsWith(QLatin1String("imgsearch-")));
        QVERIFY(fi.fileName().endsWith(QLatin1String(".html")));
#ifndef Q_OS_WIN  // NTFS has ACLs; the per-user temp dir is private anyway
        QCOMPARE(fi.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFile::Permissions());
#endif
        QVERIFY(fi.permissions() & QFile::ReadOwner);
        QFile f(fi.absoluteFilePath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(f.readAll());
        f.close();  // Windows cannot delete open files (cleanup below)
        QVERIFY(html.contains(QLatin1String("encoded_image")));
        // Downscaled: the embedded JPEG is 1000x750.
        const QString b64 = QRegularExpression(QStringLiteral("var B64 = \"([A-Za-z0-9+/=]+)\"")).match(html).captured(1);
        QImage embedded;
        QVERIFY(embedded.loadFromData(QByteArray::fromBase64(b64.toLatin1()), "JPEG"));
        QCOMPARE(embedded.size(), QSize(1000, 750));

        // Old pages are cleaned up, fresh ones kept.
        const QUrl second = ImageSearch::prepare(img, QStringLiteral("bing"));
        QVERIFY(second.isValid() && second != url);
        QFile old(url.toLocalFile());
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        old.close();
        QCOMPARE(ImageSearch::cleanup(fi.absolutePath()), 1);
        QVERIFY(!QFile::exists(url.toLocalFile()));
        QVERIFY(QFile::exists(second.toLocalFile()));

        QVERIFY(!ImageSearch::prepare(img, QStringLiteral("nope"), &error).isValid());
        QVERIFY(error.contains(QLatin1String("nope")));
        // Yandex is uploaded natively, not through a page.
        QVERIFY(!ImageSearch::prepare(img, QStringLiteral("yandex"), &error).isValid());

        // start() resolves page engines asynchronously with the page URL.
        QUrl started;
        QString startError;
        bool called = false;
        ImageSearch::start(img, QStringLiteral("tineye"), this, [&](const QUrl &u, const QString &e) {
            started = u;
            startError = e;
            called = true;
        });
        QVERIFY(!called);  // never synchronous
        QTRY_VERIFY(called);
        QVERIFY2(started.isLocalFile(), qPrintable(startError));
        called = false;
        ImageSearch::start(img, QStringLiteral("nope"), this, [&](const QUrl &u, const QString &e) {
            started = u;
            startError = e;
            called = true;
        });
        QTRY_VERIFY(called);
        QVERIFY(!started.isValid());
        QVERIFY(!startError.isEmpty());

        if (saved.isNull())
            qunsetenv("XDG_RUNTIME_DIR");
        else
            qputenv("XDG_RUNTIME_DIR", saved);
    }
};

QTEST_GUILESS_MAIN(TestSearch)
#include "tst_search.moc"
