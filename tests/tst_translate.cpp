// SPDX-License-Identifier: MIT
#include "core/Config.h"
#include "translate/LanguageDetect.h"
#include "translate/TranslationLanguages.h"
#include "translate/Translator.h"
#include "translate/TranslatorBackends.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <optional>

using namespace selsearch;

namespace {

// Minimal HTTP/1.1 server (one request per connection, chunked replies).
struct FakeRequest {
    QByteArray method;
    QByteArray path;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

struct FakeResponse {
    int status = 200;
    QByteArray contentType = "application/json";
    QList<QByteArray> chunks;
};

class FakeServer : public QObject {
public:
    std::function<FakeResponse(const FakeRequest &)> handler;
    QList<FakeRequest> requests;

    FakeServer()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection())
                serve(s);
        });
        m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString url(const QString &path = QString()) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }

private:
    void serve(QTcpSocket *s)
    {
        auto buf = std::make_shared<QByteArray>();
        QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buf] {
            buf->append(s->readAll());
            const qsizetype end = buf->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            FakeRequest req;
            const QList<QByteArray> lines = buf->left(end).split('\n');
            const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
            req.method = first.value(0);
            req.path = first.value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const qsizetype c = lines[i].indexOf(':');
                if (c > 0)
                    req.headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
            }
            const int len = req.headers.value("content-length").toInt();
            if (buf->size() < end + 4 + len)
                return;
            req.body = buf->mid(end + 4, len);
            buf->clear();
            requests << req;
            const FakeResponse r = handler ? handler(req) : FakeResponse{};
            QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Type: " + r.contentType
                + "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
            for (const QByteArray &c : r.chunks)
                if (!c.isEmpty())
                    out += QByteArray::number(c.size(), 16) + "\r\n" + c + "\r\n";
            out += "0\r\n\r\n";
            s->write(out);
            s->flush();
            s->disconnectFromHost();
        });
    }

    QTcpServer m_server;
};

quint16 deadPort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return s.serverPort();
}

std::optional<TranslationResult> run(Translator &t, const TranslationRequest &req, int timeoutMs = 10000)
{
    std::optional<TranslationResult> result;
    t.translate(req, [&result](const TranslationResult &r) { result = r; });
    QElapsedTimer timer;
    timer.start();
    while (!result && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    return result;
}

QByteArray sse(const QString &content, int pieceSize = 5)
{
    QByteArray out;
    for (qsizetype i = 0; i < content.size(); i += pieceSize) {
        const QJsonObject o{{QStringLiteral("choices"),
                             QJsonArray{QJsonObject{{QStringLiteral("delta"),
                                                     QJsonObject{{QStringLiteral("content"), content.mid(i, pieceSize)}}}}}}};
        out += "data: " + QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n\n";
    }
    out += "data: [DONE]\n\n";
    return out;
}

// Puts a copy of tests/data/fake-translateLocally first on PATH.
class FakeTranslateLocally {
public:
    FakeTranslateLocally()
    {
        m_oldPath = qgetenv("PATH");
        const QString dst = m_dir.filePath(QStringLiteral("translateLocally"));
        QFile::copy(QStringLiteral(SELSEARCH_TEST_DATA "/fake-translateLocally/translateLocally"), dst);
        QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("PATH", m_dir.path().toLocal8Bit() + ":/usr/bin:/bin");
        m_log = m_dir.filePath(QStringLiteral("log"));
        qputenv("FAKE_TL_LOG", m_log.toLocal8Bit());
    }
    ~FakeTranslateLocally()
    {
        qputenv("PATH", m_oldPath);
        qunsetenv("FAKE_TL_LOG");
        qunsetenv("FAKE_TL_FAIL");
    }
    QStringList log() const
    {
        QFile f(m_log);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_oldPath;
    QString m_log;
};

// PATH without translateLocally.
class NoTranslateLocally {
public:
    NoTranslateLocally()
        : m_oldPath(qgetenv("PATH"))
    {
        qputenv("PATH", "/usr/bin:/bin");
    }
    ~NoTranslateLocally() { qputenv("PATH", m_oldPath); }

private:
    QByteArray m_oldPath;
};

}  // namespace

class TestTranslate : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // --- language detection ------------------------------------------------

    void detect_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("code");
        auto row = [](const char *name, const char *text, const char *code) {
            QTest::newRow(name) << QString::fromUtf8(text) << QString::fromLatin1(code);
        };
        row("en", "The quick brown fox jumps over the lazy dog and runs into the forest.", "en");
        row("en-ui", "Click here to download the latest version of the app", "en");
        row("de", "Der schnelle braune Fuchs springt über den faulen Hund und läuft in den Wald.", "de");
        row("fr", "Le renard brun rapide saute par-dessus le chien paresseux et court dans la forêt.", "fr");
        row("es", "El rápido zorro marrón salta sobre el perro perezoso y corre hacia el bosque.", "es");
        row("it", "La volpe veloce salta sopra il cane pigro e corre nel bosco con gli amici.", "it");
        row("pt", "A raposa marrom rápida pula sobre o cão preguiçoso e corre para a floresta.", "pt");
        row("nl", "De snelle bruine vos springt over de luie hond en rent het bos in.", "nl");
        row("pl", "Szybki brązowy lis przeskakuje nad leniwym psem i biegnie do lasu.", "pl");
        row("cs", "Rychlá hnědá liška skáče přes líného psa a běží do lesa.", "cs");
        row("sv", "Den snabba bruna räven hoppar över den lata hunden och springer till skogen.", "sv");
        row("tr", "Hızlı kahverengi tilki tembel köpeğin üzerinden atlar ve ormana koşar.", "tr");
        row("vi", "Con cáo nâu nhanh nhẹn nhảy qua con chó lười và chạy vào rừng.", "vi");
        row("ru", "Съешь же ещё этих мягких французских булок, да выпей чаю.", "ru");
        row("ru-2", "Привет, как дела? Это тестовое сообщение.", "ru");
        row("uk", "Привіт, як справи? Це тестове повідомлення українською мовою.", "uk");
        row("uk-2", "Їжак їсть яблуко, а ґудзик лежить на столі.", "uk");
        row("be", "Прывітанне, як справы? Гэта тэставае паведамленне на беларускай мове.", "be");
        row("bg", "Здравей, как си? Това е тестово съобщение на български език.", "bg");
        row("sr", "Ђаци ће учити ћирилицу у школи, џак је пун.", "sr");
        row("kk", "Қазақстан Республикасы — Орталық Азиядағы мемлекет.", "kk");
        row("ja", "素早い茶色の狐がのろまな犬を飛び越える。", "ja");
        row("zh", "敏捷的棕色狐狸跳过了懒狗。", "zh");
        row("ko", "빠른 갈색 여우가 게으른 개를 뛰어넘는다.", "ko");
        row("ar", "الثعلب البني السريع يقفز فوق الكلب الكسول.", "ar");
        row("fa", "روباه قهوه‌ای سریع از روی سگ تنبل می‌پرد.", "fa");
        row("he", "השועל החום המהיר קופץ מעל הכלב העצלן.", "he");
        row("el", "Η γρήγορη καφέ αλεπού πηδάει πάνω από τον τεμπέλη σκύλο.", "el");
        row("th", "สุนัขจิ้งจอกสีน้ำตาลกระโดดข้ามสุนัขขี้เกียจ", "th");
        row("hi", "तेज़ भूरी लोमड़ी आलसी कुत्ते के ऊपर कूदती है।", "hi");
    }

    void detect()
    {
        QFETCH(QString, text);
        QFETCH(QString, code);
        const DetectedLanguage d = detectLanguage(text);
        QCOMPARE(d.code, code);
        QVERIFY2(d.confidence > 0.3, qPrintable(QString::number(d.confidence)));
        QVERIFY(d.confidence <= 1.0);
    }

    void detectEdgeCases()
    {
        QVERIFY(!detectLanguage(QStringLiteral("12345 !!! ---")).isValid());
        QVERIFY(!detectLanguage(QString()).isValid());
        // No evidence: weak English guess.
        const DetectedLanguage weak = detectLanguage(QStringLiteral("Xyzzy Plugh"));
        QCOMPARE(weak.code, QStringLiteral("en"));
        QVERIFY(weak.confidence < 0.3);
        // Mixed: mostly Russian with a Latin brand name.
        QCOMPARE(detectLanguage(QStringLiteral("Скачать Firefox для Linux бесплатно и без регистрации")).code,
                 QStringLiteral("ru"));
        QCOMPARE(detectLanguage(QStringList{QStringLiteral("Guten Tag"), QStringLiteral("Wie geht es dir?")}).code,
                 QStringLiteral("de"));
    }

    // --- languages ---------------------------------------------------------

    void languageList()
    {
        const auto &langs = translationLanguages();
        QVERIFY(langs.size() >= 60);
        QSet<QString> codes;
        for (const auto &l : langs) {
            QVERIFY(!l.englishName.isEmpty());
            QVERIFY(!l.nativeName.isEmpty());
            QVERIFY2(!codes.contains(l.code), qPrintable(l.code));
            codes.insert(l.code);
        }
        QCOMPARE(translationLanguage(QStringLiteral("de"))->nativeName, QStringLiteral("Deutsch"));
        QCOMPARE(translationLanguage(QStringLiteral("uk_UA"))->englishName, QStringLiteral("Ukrainian"));
        QCOMPARE(normalizeLanguageCode(QStringLiteral("de-AT")), QStringLiteral("de"));
        QCOMPARE(normalizeLanguageCode(QStringLiteral("deu")), QStringLiteral("de"));
        QCOMPARE(normalizeLanguageCode(QStringLiteral("eng")), QStringLiteral("en"));
        QCOMPARE(normalizeLanguageCode(QStringLiteral("iw")), QStringLiteral("he"));
        QCOMPARE(normalizeLanguageCode(QStringLiteral("zh-Hans")), QStringLiteral("zh"));
        QCOMPARE(normalizeLanguageCode(QStringLiteral("auto")), QString());
        QVERIFY(!systemLanguageCode().isEmpty());
    }

    void recentLanguages()
    {
        QTemporaryDir dir;
        Config config;
        QVERIFY(config.load(dir.filePath(QStringLiteral("config.toml"))));
        QVERIFY(config.defaults().contains(QStringLiteral("translate.recent")));
        QVERIFY(recentTranslationLanguages(config).isEmpty());
        addRecentTranslationLanguage(config, QStringLiteral("de"));
        addRecentTranslationLanguage(config, QStringLiteral("fr"));
        addRecentTranslationLanguage(config, QStringLiteral("ja"));
        addRecentTranslationLanguage(config, QStringLiteral("de-DE"));
        QCOMPARE(recentTranslationLanguages(config),
                 (QStringList{QStringLiteral("de"), QStringLiteral("ja"), QStringLiteral("fr")}));
        addRecentTranslationLanguage(config, QStringLiteral("uk"));
        QCOMPARE(recentTranslationLanguages(config),
                 (QStringList{QStringLiteral("uk"), QStringLiteral("de"), QStringLiteral("ja")}));
        Config reloaded;
        QVERIFY(reloaded.load(config.path()));
        QCOMPARE(recentTranslationLanguages(reloaded).size(), 3);
    }

    // --- LLM reply parsing --------------------------------------------------

    void llmParse()
    {
        const QStringList ab = {QStringLiteral("Hallo"), QStringLiteral("Welt")};
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("[\"Hallo\", \"Welt\"]"), 2), ab);
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("```json\n[\"Hallo\",\n \"Welt\"]\n```"), 2), ab);
        QCOMPARE(LlmTranslator::parseReply(
                     QStringLiteral("Sure! Here are the translations [in order]:\n```\n[\"Hallo\", \"Welt\"]\n```\nLet me know!"), 2),
                 ab);
        QCOMPARE(LlmTranslator::parseReply(
                     QStringLiteral("<think>The user wants [German]. OK.</think>\n[\"Hallo\", \"Welt\"]"), 2),
                 ab);
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("{\"translations\": [\"Hallo\", \"Welt\"]}"), 2), ab);
        QCOMPARE(LlmTranslator::parseReply(
                     QStringLiteral("[{\"id\":1,\"translation\":\"Hallo\"},{\"id\":2,\"translation\":\"Welt\"}]"), 2),
                 ab);
        // Brackets inside strings, escapes.
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("[\"a [b] \\\"c\\\"\", \"x\\ny\"]"), 2),
                 (QStringList{QStringLiteral("a [b] \"c\""), QStringLiteral("x\ny")}));
        // Line fallback: numbered list, quoted lines.
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("1. Hallo\n2. Welt\n"), 2), ab);
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("\"Hallo\",\n\"Welt\"\n"), 2), ab);
        // Single segment: plain text is fine.
        QCOMPARE(LlmTranslator::parseReply(QStringLiteral("  Hallo Welt  "), 1), QStringList{QStringLiteral("Hallo Welt")});
        // Wrong count.
        QVERIFY(!LlmTranslator::parseReply(QStringLiteral("[\"Hallo\"]"), 2).has_value());
        QVERIFY(!LlmTranslator::parseReply(QStringLiteral("I cannot help with that."), 3).has_value());
    }

    void llmBatches()
    {
        QStringList segs;
        for (int i = 0; i < 95; ++i)
            segs << QStringLiteral("segment %1").arg(i);
        segs[3] = QStringLiteral("   ");
        auto batches = LlmTranslator::makeBatches(segs);
        QCOMPARE(batches.size(), 3);
        QCOMPARE(batches[0].size(), 40);
        QVERIFY(!batches[0].contains(3));
        int total = 0;
        for (const auto &b : batches)
            total += int(b.size());
        QCOMPARE(total, 94);

        const QStringList big = {QString(2000, QLatin1Char('a')), QString(2000, QLatin1Char('b')), QStringLiteral("c")};
        batches = LlmTranslator::makeBatches(big);
        QCOMPARE(batches.size(), 2);
        QCOMPARE(batches[0], QList<int>{0});
        QCOMPARE(batches[1], (QList<int>{1, 2}));
    }

    void llmTranslateEndToEnd()
    {
        FakeServer server;
        int calls = 0;
        bool loseCount = true;
        server.handler = [&](const FakeRequest &req) {
            ++calls;
            const QJsonArray msgs = req.json().value(QStringLiteral("messages")).toArray();
            const QJsonArray items =
                QJsonDocument::fromJson(msgs[1].toObject().value(QStringLiteral("content")).toString().toUtf8()).array();
            QJsonArray out;
            for (const QJsonValue &v : items)
                out.append(QStringLiteral("DE:") + v.toString());
            if (loseCount && items.size() > 1) {
                loseCount = false;
                out.removeLast();  // model loses count once -> halves are retried
            }
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = {sse(QStringLiteral("Here you go:\n```json\n") + QString::fromUtf8(QJsonDocument(out).toJson())
                            + QStringLiteral("```"))};
            return r;
        };

        LlmTranslator llm({server.url(QStringLiteral("/v1")), QStringLiteral("m"), {}, 0.7, 1600});
        QVERIFY(llm.isAvailable());
        QStringList segs;
        for (int i = 0; i < 45; ++i)
            segs << QStringLiteral("Line %1").arg(i);
        segs[10] = QString();
        const auto r = run(llm, {segs, QStringLiteral("en"), QStringLiteral("de")});
        QVERIFY(r.has_value());
        QVERIFY2(r->ok(), qPrintable(r->error));
        QCOMPARE(r->backend, QStringLiteral("llm"));
        QCOMPARE(r->segments.size(), 45);
        QCOMPARE(r->segments[0], QStringLiteral("DE:Line 0"));
        QCOMPARE(r->segments[10], QString());
        QCOMPARE(r->segments[44], QStringLiteral("DE:Line 44"));
        // 40 (lost count) -> 20 + 19 (index 10 is empty), then 5.
        QCOMPARE(calls, 4);

        const QJsonObject first = server.requests.first().json();
        const QJsonArray msgs = first.value(QStringLiteral("messages")).toArray();
        QCOMPARE(msgs[0].toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        const QString sys = msgs[0].toObject().value(QStringLiteral("content")).toString();
        QVERIFY(sys.contains(QStringLiteral("German")));
        QVERIFY(sys.contains(QStringLiteral("English")));
        QVERIFY(sys.contains(QStringLiteral("JSON array")));
        QVERIFY(first.value(QStringLiteral("temperature")).toDouble() < 0.3);
    }

    void llmTranslateError()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.status = 500;
            r.chunks = {"{\"error\":\"llama runner process has terminated\"}"};
            return r;
        };
        LlmTranslator llm({server.url(QStringLiteral("/v1")), QStringLiteral("m"), {}, 0.3, 1600});
        const auto r = run(llm, {{QStringLiteral("Hello")}, QString(), QStringLiteral("de")});
        QVERIFY(r.has_value());
        QVERIFY(!r->ok());
        QVERIFY(r->error.contains(QStringLiteral("llama runner")));
        QCOMPARE(r->detectedSource, QStringLiteral("en"));
    }

    // --- LibreTranslate -----------------------------------------------------

    void libreTranslate()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &req) {
            FakeResponse r;
            if (req.path == "/languages") {
                r.chunks = {"[{\"code\":\"en\",\"name\":\"English\",\"targets\":[\"de\",\"zh-Hans\"]},"
                            "{\"code\":\"de\",\"name\":\"German\",\"targets\":[\"en\"]},"
                            "{\"code\":\"zh-Hans\",\"name\":\"Chinese\",\"targets\":[\"en\"]}]"};
            } else if (req.path == "/translate") {
                QJsonArray out, detected;
                for (const QJsonValue &v : req.json().value(QStringLiteral("q")).toArray()) {
                    out.append(QStringLiteral("T(") + v.toString() + QLatin1Char(')'));
                    detected.append(QJsonObject{{QStringLiteral("confidence"), 90}, {QStringLiteral("language"), QStringLiteral("en")}});
                }
                r.chunks = {QJsonDocument(QJsonObject{{QStringLiteral("translatedText"), out},
                                                      {QStringLiteral("detectedLanguage"), detected}})
                                .toJson()};
            } else {
                r.status = 404;
            }
            return r;
        };

        LibreTranslateTranslator libre(server.url() + QLatin1Char('/'), QStringLiteral("secret"));
        QVERIFY(libre.isAvailable());
        const auto r = run(libre, {{QStringLiteral("Hello"), QString(), QStringLiteral("World")}, QStringLiteral("auto"),
                                   QStringLiteral("zh")});
        QVERIFY(r.has_value());
        QVERIFY2(r->ok(), qPrintable(r->error));
        QCOMPARE(r->segments, (QStringList{QStringLiteral("T(Hello)"), QString(), QStringLiteral("T(World)")}));
        QCOMPARE(r->detectedSource, QStringLiteral("en"));
        QCOMPARE(r->backend, QStringLiteral("libretranslate"));

        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.requests[0].path, QByteArray("/languages"));
        const FakeRequest &tr = server.requests[1];
        QCOMPARE(tr.method, QByteArray("POST"));
        QCOMPARE(tr.path, QByteArray("/translate"));
        const QJsonObject body = tr.json();
        QCOMPARE(body.value(QStringLiteral("q")).toArray().size(), 2);
        QCOMPARE(body.value(QStringLiteral("source")).toString(), QStringLiteral("auto"));
        QCOMPARE(body.value(QStringLiteral("target")).toString(), QStringLiteral("zh-Hans"));
        QCOMPARE(body.value(QStringLiteral("format")).toString(), QStringLiteral("text"));
        QCOMPARE(body.value(QStringLiteral("api_key")).toString(), QStringLiteral("secret"));

        QList<LibreTranslateTranslator::Language> langs;
        bool called = false;
        libre.fetchLanguages([&](const QList<LibreTranslateTranslator::Language> &l, const QString &) {
            langs = l;
            called = true;
        });
        QTRY_VERIFY(called);
        QCOMPARE(langs.size(), 3);
        QCOMPARE(langs[0].targets, (QStringList{QStringLiteral("de"), QStringLiteral("zh-Hans")}));
    }

    void libreReplies()
    {
        auto r = LibreTranslateTranslator::parseTranslateReply(
            200, "{\"translatedText\":\"Hallo\",\"detectedLanguage\":{\"confidence\":83,\"language\":\"en\"}}", 1);
        QVERIFY(r.ok());
        QCOMPARE(r.segments, QStringList{QStringLiteral("Hallo")});
        QCOMPARE(r.detectedSource, QStringLiteral("en"));

        r = LibreTranslateTranslator::parseTranslateReply(400, "{\"error\":\"Invalid request: missing q parameter\"}", 1);
        QVERIFY(!r.ok());
        QVERIFY(r.error.contains(QStringLiteral("missing q parameter")));

        r = LibreTranslateTranslator::parseTranslateReply(403, "{\"error\":\"Visit https://portal.libretranslate.com to get an API key\"}", 1);
        QVERIFY(r.error.contains(QStringLiteral("libretranslate_key")));

        r = LibreTranslateTranslator::parseTranslateReply(200, "{\"translatedText\":[\"a\"]}", 2);
        QVERIFY(!r.ok());
    }

    void libreUnreachable()
    {
        LibreTranslateTranslator libre(QStringLiteral("http://127.0.0.1:%1").arg(deadPort()), {});
        bool done = false, ok = true;
        QElapsedTimer t;
        t.start();
        libre.probe(500, [&](bool r) {
            ok = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(!ok);
        QVERIFY(t.elapsed() < 2000);
        QVERIFY(!libre.isAvailable());  // cached failure
    }

    // --- Bergamot / translateLocally ----------------------------------------

    void bergamotModelList()
    {
        const QByteArray out = "Czech-English type: base version: 1; To invoke do -m cs-en-base\n"
                               "Norwegian (Bokm\xc3\xa5l)-English type: tiny version: 1; To invoke do -m nb-en-tiny\n"
                               "Serbo-Croatian-English type: tiny version: 2; To invoke do -m hbs-eng-tiny\n"
                               "garbage line\n";
        const auto models = BergamotTranslator::parseModelList(out);
        QCOMPARE(models.size(), 3);
        QCOMPARE(models[0].shortName, QStringLiteral("cs-en-base"));
        QCOMPARE(models[0].sources, QStringList{QStringLiteral("cs")});
        QCOMPARE(models[0].target, QStringLiteral("en"));
        QCOMPARE(models[0].type, QStringLiteral("base"));
        QCOMPARE(models[1].sources, QStringList{QStringLiteral("nb")});
        QCOMPARE(models[2].sources, (QStringList{QStringLiteral("sr"), QStringLiteral("hr"), QStringLiteral("bs")}));
        QCOMPARE(models[2].target, QStringLiteral("en"));
        QCOMPARE(models[2].version, 2);
    }

    void bergamotTranslateWithPivot()
    {
        FakeTranslateLocally fake;
        BergamotTranslator berg;
        QVERIFY(!berg.executable().isEmpty());
        QVERIFY(berg.isAvailable());

        bool loaded = false;
        berg.loadModels([&](bool ok) { loaded = ok; });
        QTRY_VERIFY(berg.modelsLoaded());
        QVERIFY(loaded);
        QCOMPARE(berg.models().size(), 5);
        QCOMPARE(berg.route(QStringLiteral("de"), QStringLiteral("en")), QStringList{QStringLiteral("de-en-tiny")});
        QCOMPARE(berg.route(QStringLiteral("ru"), QStringLiteral("de")),
                 (QStringList{QStringLiteral("ru-en-tiny"), QStringLiteral("en-de-base")}));  // base preferred
        QCOMPARE(berg.route(QStringLiteral("hr"), QStringLiteral("de")),
                 (QStringList{QStringLiteral("hbs-eng-tiny"), QStringLiteral("en-de-base")}));
        QVERIFY(berg.route(QStringLiteral("en"), QStringLiteral("ru")).isEmpty());
        QVERIFY(berg.route(QStringLiteral("ja"), QStringLiteral("de")).isEmpty());

        const QStringList segs = {QStringLiteral("Привет, мир"), QString(), QStringLiteral("Первая строка\nвторая строка"),
                                  QStringLiteral("  "), QStringLiteral("Табуляция\tвнутри")};
        const auto r = run(berg, {segs, QStringLiteral("ru"), QStringLiteral("de")});
        QVERIFY(r.has_value());
        QVERIFY2(r->ok(), qPrintable(r->error));
        QCOMPARE(r->backend, QStringLiteral("bergamot"));
        QCOMPARE(r->segments,
                 (QStringList{QStringLiteral("de[en[Привет, мир]]"), QString(),
                              QStringLiteral("de[en[Первая строка]]\nde[en[вторая строка]]"), QStringLiteral("  "),
                              QStringLiteral("de[en[Табуляция внутри]]")}));
        const QStringList log = fake.log();
        QCOMPARE(log, (QStringList{QStringLiteral("-l"), QStringLiteral("-m ru-en-tiny"), QStringLiteral("-m en-de-base")}));

        // Source detection.
        const auto auto1 = run(berg, {{QStringLiteral("Это тестовое сообщение")}, QString(), QStringLiteral("en")});
        QVERIFY(auto1 && auto1->ok());
        QCOMPARE(auto1->detectedSource, QStringLiteral("ru"));
        QCOMPARE(auto1->segments, QStringList{QStringLiteral("en[Это тестовое сообщение]")});

        // No route.
        const auto none = run(berg, {{QStringLiteral("こんにちは")}, QStringLiteral("ja"), QStringLiteral("de")});
        QVERIFY(none && !none->ok());
        QVERIFY(none->error.contains(QStringLiteral("No translateLocally model")));

        // CLI failure: stderr is surfaced.
        qputenv("FAKE_TL_FAIL", "1");
        const auto fail = run(berg, {{QStringLiteral("Hallo")}, QStringLiteral("de"), QStringLiteral("en")});
        qunsetenv("FAKE_TL_FAIL");
        QVERIFY(fail && !fail->ok());
        QVERIFY2(fail->error.contains(QStringLiteral("out of memory")), qPrintable(fail->error));

        // Same language: no process at all.
        const int before = int(fake.log().size());
        const auto same = run(berg, {{QStringLiteral("Hallo")}, QStringLiteral("de"), QStringLiteral("de")});
        QVERIFY(same && same->ok());
        QCOMPARE(same->segments, QStringList{QStringLiteral("Hallo")});
        QCOMPARE(int(fake.log().size()), before);
    }

    void bergamotNotInstalled()
    {
        NoTranslateLocally none;
        BergamotTranslator berg;
        QVERIFY(!berg.isAvailable());
        const auto r = run(berg, {{QStringLiteral("Hallo")}, QStringLiteral("de"), QStringLiteral("en")});
        QVERIFY(r && !r->ok());
    }

    // Optional: SELSEARCH_LIVE_AI_ENDPOINT=http://127.0.0.1:8080/v1 ./tst_translate liveLlm
    void liveLlm()
    {
        const QString endpoint = qEnvironmentVariable("SELSEARCH_LIVE_AI_ENDPOINT");
        if (endpoint.isEmpty())
            QSKIP("SELSEARCH_LIVE_AI_ENDPOINT not set");
        LlmTranslator llm({endpoint, qEnvironmentVariable("SELSEARCH_LIVE_AI_MODEL"), {}, 0.2, 1600});
        const QStringList segs = {QStringLiteral("Good morning"), QStringLiteral("Where is the train station?"),
                                  QStringLiteral("Thank you very much")};
        const auto r = run(llm, {segs, QStringLiteral("en"), QStringLiteral("de")}, 180000);
        QVERIFY(r.has_value());
        QVERIFY2(r->ok(), qPrintable(r->error));
        qInfo() << r->segments;
        QCOMPARE(r->segments.size(), 3);
    }

    // --- auto ---------------------------------------------------------------

    void autoSelection()
    {
        QTemporaryDir dir;
        Config config;
        QVERIFY(config.load(dir.filePath(QStringLiteral("config.toml"))));

        FakeServer libreServer;
        libreServer.handler = [](const FakeRequest &req) {
            FakeResponse r;
            if (req.path == "/languages")
                r.chunks = {"[{\"code\":\"en\",\"name\":\"English\",\"targets\":[\"de\"]}]"};
            else
                r.chunks = {"{\"translatedText\":[\"LT\"]}"};
            return r;
        };
        FakeServer aiServer;
        aiServer.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = {sse(QStringLiteral("[\"LLM\"]"))};
            return r;
        };

        // 1. No translateLocally, LibreTranslate up -> libretranslate.
        {
            NoTranslateLocally noBergamot;
            config.setValue(QStringLiteral("translate.libretranslate_url"), libreServer.url());
            config.setValue(QStringLiteral("ai.enabled"), false);
            auto t = createTranslator(config);
            QCOMPARE(t->id(), QStringLiteral("auto"));
            QVERIFY(t->isAvailable());
            const auto r = run(*t, {{QStringLiteral("Hello world")}, QString(), QStringLiteral("de")});
            QVERIFY(r && r->ok());
            QCOMPARE(r->backend, QStringLiteral("libretranslate"));
            QCOMPARE(r->segments, QStringList{QStringLiteral("LT")});
        }
        // 2. LibreTranslate down, AI enabled -> llm.
        {
            NoTranslateLocally noBergamot;
            config.setValue(QStringLiteral("translate.libretranslate_url"),
                            QStringLiteral("http://127.0.0.1:%1").arg(deadPort()));
            config.setValue(QStringLiteral("ai.enabled"), true);
            config.setValue(QStringLiteral("ai.endpoint"), aiServer.url(QStringLiteral("/v1")));
            auto t = createTranslator(config);
            const auto r = run(*t, {{QStringLiteral("Hello world")}, QString(), QStringLiteral("de")});
            QVERIFY(r.has_value());
            QVERIFY2(r->ok(), qPrintable(r->error));
            QCOMPARE(r->backend, QStringLiteral("llm"));
            QCOMPARE(r->segments, QStringList{QStringLiteral("LLM")});
        }
        // 3. translateLocally with a route -> bergamot, even with the others up.
        {
            FakeTranslateLocally fake;
            config.setValue(QStringLiteral("translate.libretranslate_url"), libreServer.url());
            auto t = createTranslator(config);
            const auto r = run(*t, {{QStringLiteral("Привет, как дела?")}, QString(), QStringLiteral("de")});
            QVERIFY(r && r->ok());
            QCOMPARE(r->backend, QStringLiteral("bergamot"));
            QCOMPARE(r->detectedSource, QStringLiteral("ru"));
            QCOMPARE(r->segments, QStringList{QStringLiteral("de[en[Привет, как дела?]]")});

            // No model for ja -> falls through to LibreTranslate.
            const auto r2 = run(*t, {{QStringLiteral("こんにちは")}, QString(), QStringLiteral("en")});
            QVERIFY(r2 && r2->ok());
            QCOMPARE(r2->backend, QStringLiteral("libretranslate"));
        }
        // 4. Nothing available.
        {
            NoTranslateLocally noBergamot;
            config.setValue(QStringLiteral("translate.libretranslate_url"),
                            QStringLiteral("http://127.0.0.1:%1").arg(deadPort()));
            config.setValue(QStringLiteral("ai.enabled"), false);
            auto t = createTranslator(config);
            const auto r = run(*t, {{QStringLiteral("Hello")}, QString(), QStringLiteral("de")});
            QVERIFY(r && !r->ok());
            QVERIFY(r->error.contains(QStringLiteral("No translation backend")));
        }
        // Explicit backends.
        config.setValue(QStringLiteral("translate.backend"), QStringLiteral("libretranslate"));
        QCOMPARE(createTranslator(config)->id(), QStringLiteral("libretranslate"));
        config.setValue(QStringLiteral("translate.backend"), QStringLiteral("bergamot"));
        QCOMPARE(createTranslator(config)->id(), QStringLiteral("bergamot"));
        config.setValue(QStringLiteral("translate.backend"), QStringLiteral("llm"));
        QCOMPARE(createTranslator(config)->id(), QStringLiteral("llm"));
    }
};

QTEST_MAIN(TestTranslate)
#include "tst_translate.moc"
