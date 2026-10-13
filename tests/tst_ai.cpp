// SPDX-License-Identifier: MIT
#include "ai/AiClient.h"
#include "ai/AiHistory.h"
#include "core/Config.h"

#include <QBuffer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

using namespace selsearch;

namespace {

// Minimal HTTP/1.1 server: one request per connection, scripted response
// written in chunks (chunked transfer encoding) with optional delays.
struct FakeRequest {
    QByteArray method;
    QByteArray path;
    QHash<QByteArray, QByteArray> headers;  // lower-case names
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

struct FakeResponse {
    int status = 200;
    QByteArray contentType = "application/json";
    QList<QByteArray> chunks;
    int chunkDelayMs = 0;
    bool stallAfterChunks = false;  // keep the connection open without finishing
    QList<QByteArray> extraHeaders;
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
        QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
    }
    QString url(const QString &path = QStringLiteral("/v1")) const
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
            respond(s, handler ? handler(req) : FakeResponse{});
        });
    }

    void respond(QTcpSocket *s, const FakeResponse &r)
    {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Type: " + r.contentType
            + "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n";
        for (const QByteArray &h : r.extraHeaders)
            head += h + "\r\n";
        s->write(head + "\r\n");
        s->flush();
        auto chunks = std::make_shared<QList<QByteArray>>(r.chunks);
        auto writeNext = std::make_shared<std::function<void()>>();
        QPointer<QTcpSocket> sp(s);
        *writeNext = [sp, chunks, writeNext, r]() {
            if (!sp)
                return;
            if (chunks->isEmpty()) {
                if (r.stallAfterChunks)
                    return;
                sp->write("0\r\n\r\n");
                sp->flush();
                sp->disconnectFromHost();
                return;
            }
            const QByteArray c = chunks->takeFirst();
            if (!c.isEmpty())
                sp->write(QByteArray::number(c.size(), 16) + "\r\n" + c + "\r\n");
            sp->flush();
            QTimer::singleShot(r.chunkDelayMs, sp, [writeNext] { (*writeNext)(); });
        };
        (*writeNext)();
    }

    QTcpServer m_server;
};

QByteArray sseDelta(const QString &text)
{
    const QJsonObject o{{QStringLiteral("choices"),
                         QJsonArray{QJsonObject{{QStringLiteral("index"), 0},
                                                {QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), text}}}}}}};
    return "data: " + QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n\n";
}

// Splits bytes into pieces of `n` bytes (cuts through UTF-8 sequences and lines).
QList<QByteArray> slices(const QByteArray &data, int n)
{
    QList<QByteArray> out;
    for (qsizetype i = 0; i < data.size(); i += n)
        out << data.mid(i, n);
    return out;
}

QImage noisyImage(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    auto *rng = QRandomGenerator::global();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, qRgb(rng->bounded(256), rng->bounded(256), rng->bounded(256)));
    return img;
}

}  // namespace

class TestAi : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_xdg.isValid());
        qputenv("XDG_DATA_HOME", m_xdg.path().toLocal8Bit());
    }

    // --- SSE parsing --------------------------------------------------------

    void sseSplitAnywhere()
    {
        const QByteArray stream = QStringLiteral(
                                      ": keep-alive comment\r\n"
                                      "data: {\"t\":\"Привет, мир 😀\"}\r\n"
                                      "\r\n"
                                      "event: message\n"
                                      "id: 7\n"
                                      "data: line1\n"
                                      "data:line2\n"
                                      "\n"
                                      "retry: 100\r"
                                      "data: cr-only\r\r"
                                      "data: [DONE]\n\n")
                                      .toUtf8();
        const QList<QByteArray> expected = {QStringLiteral("{\"t\":\"Привет, мир 😀\"}").toUtf8(), "line1\nline2",
                                            "cr-only", "[DONE]"};
        {
            SseParser p;
            QCOMPARE(p.feed(stream) + p.finish(), expected);
        }
        // Every 2-way and 3-way split.
        for (qsizetype i = 0; i <= stream.size(); ++i) {
            SseParser p;
            QList<QByteArray> got = p.feed(stream.left(i));
            got += p.feed(stream.mid(i));
            got += p.finish();
            QCOMPARE(got, expected);
        }
        for (qsizetype i = 0; i <= stream.size(); i += 3) {
            for (qsizetype j = i; j <= stream.size(); j += 2) {
                SseParser p;
                QList<QByteArray> got = p.feed(stream.left(i));
                got += p.feed(stream.mid(i, j - i));
                got += p.feed(stream.mid(j));
                got += p.finish();
                QCOMPARE(got, expected);
            }
        }
        // Byte by byte.
        SseParser p;
        QList<QByteArray> got;
        for (char c : stream)
            got += p.feed(QByteArray(1, c));
        got += p.finish();
        QCOMPARE(got, expected);
    }

    void sseUnterminatedAndNdjson()
    {
        SseParser p;
        QCOMPARE(p.feed("data: {\"a\":1}"), QList<QByteArray>{});
        QCOMPARE(p.finish(), QList<QByteArray>{"{\"a\":1}"});
        SseParser n;
        QCOMPARE(n.feed("{\"a\":1}\n{\"b\":2}\n"), (QList<QByteArray>{"{\"a\":1}", "{\"b\":2}"}));
    }

    // --- streaming over HTTP -------------------------------------------------

    void streamsDeltas()
    {
        FakeServer server;
        const QStringList parts = {QStringLiteral("Привет"), QStringLiteral(", мир "), QStringLiteral("😀 — done")};
        QByteArray body = ": ping\n\n";
        for (const QString &t : parts)
            body += sseDelta(t);
        body += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        server.handler = [&](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = slices(body, 7);  // cuts lines and multi-byte characters
            r.chunkDelayMs = 2;
            return r;
        };

        AiClient client;
        client.setSettings({server.url(), QStringLiteral("qwen3-vl:4b"), QStringLiteral("sk-test"), 0.3, 1600});
        QSignalSpy deltas(&client, &AiClient::delta);
        QSignalSpy finished(&client, &AiClient::finished);
        QSignalSpy failed(&client, &AiClient::failed);
        client.ask({{AiMessage::System, QStringLiteral("Be brief."), {}}, {AiMessage::User, QStringLiteral("Hi"), {}}});
        QVERIFY(client.isBusy());
        QVERIFY(finished.wait(5000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(finished.first().first().toString(), parts.join(QString()));
        QString joined;
        for (const auto &args : deltas)
            joined += args.first().toString();
        QCOMPARE(joined, parts.join(QString()));
        QVERIFY(!client.isBusy());

        QCOMPARE(server.requests.size(), 1);
        const FakeRequest &req = server.requests.first();
        QCOMPARE(req.method, QByteArray("POST"));
        QCOMPARE(req.path, QByteArray("/v1/chat/completions"));
        QCOMPARE(req.headers.value("authorization"), QByteArray("Bearer sk-test"));
        QVERIFY(!req.headers.contains("cookie"));
        const QJsonObject json = req.json();
        QCOMPARE(json.value(QStringLiteral("model")).toString(), QStringLiteral("qwen3-vl:4b"));
        QCOMPARE(json.value(QStringLiteral("stream")).toBool(), true);
        const QJsonArray msgs = json.value(QStringLiteral("messages")).toArray();
        QCOMPARE(msgs.size(), 2);
        QCOMPARE(msgs[0].toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        QCOMPARE(msgs[1].toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Hi"));
    }

    void nonStreamingReplyIsAccepted()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.chunks = {"{\n  \"choices\": [\n    {\"message\": {\"role\": \"assistant\", \"content\": \"whole\"}}\n  ]\n}\n"};
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("m"), {}, 0.3, 1600});
        QSignalSpy finished(&client, &AiClient::finished);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.first().first().toString(), QStringLiteral("whole"));
    }

    void reasoningIsSeparated()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = {"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"hmm\"}}]}\n\n", sseDelta(QStringLiteral("ok")),
                        "data: [DONE]\n\n"};
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("m"), {}, 0.3, 1600});
        QSignalSpy reasoning(&client, &AiClient::reasoning);
        QSignalSpy finished(&client, &AiClient::finished);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.first().first().toString(), QStringLiteral("ok"));
        QCOMPARE(reasoning.count(), 1);
    }

    void httpErrorBodies()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.status = 404;
            r.chunks = {"{\"error\":{\"message\":\"model \\\"qwen3-vl:4b\\\" not found, try pulling it first\",\"type\":\"api_error\"}}"};
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("qwen3-vl:4b"), {}, 0.3, 1600});
        QSignalSpy failed(&client, &AiClient::failed);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(failed.wait(5000));
        const QString err = failed.first().first().toString();
        QVERIFY2(err.contains(QStringLiteral("HTTP 404")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("not found, try pulling it first")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("ollama pull qwen3-vl:4b")), qPrintable(err));

        // Plain-text body, auth hint.
        const QString e401 = AiClient::describeHttpError(401, "Unauthorized: bad key", client.settings());
        QVERIFY(e401.contains(QStringLiteral("Unauthorized: bad key")));
        QVERIFY(e401.contains(QStringLiteral("API key")));
        // FastAPI / vLLM style.
        const QString e400 = AiClient::describeHttpError(400, "{\"object\":\"error\",\"message\":\"bad temperature\"}", {});
        QVERIFY(e400.contains(QStringLiteral("bad temperature")));
        // OpenAI style missing model on a remote host: no ollama hint.
        AiSettings remote{QStringLiteral("https://api.openai.com/v1"), QStringLiteral("gpt-x"), {}, 0.3, 1600};
        const QString e404 = AiClient::describeHttpError(
            404, "{\"error\":{\"message\":\"The model `gpt-x` does not exist\",\"code\":\"model_not_found\"}}", remote);
        QVERIFY(e404.contains(QStringLiteral("does not exist")));
        QVERIFY(!e404.contains(QStringLiteral("ollama")));
    }

    void errorInsideStream()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = {sseDelta(QStringLiteral("par")), "data: {\"error\":{\"message\":\"Provider overloaded\"}}\n\n"};
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("m"), {}, 0.3, 1600});
        QSignalSpy failed(&client, &AiClient::failed);
        QSignalSpy finished(&client, &AiClient::finished);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("Provider overloaded")));
        QTest::qWait(50);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(failed.count(), 1);
    }

    void connectionRefused()
    {
        quint16 port;
        {
            QTcpServer s;
            QVERIFY(s.listen(QHostAddress::LocalHost, 0));
            port = s.serverPort();
        }
        AiClient client;
        client.setSettings({QStringLiteral("http://127.0.0.1:%1/v1").arg(port), QStringLiteral("m"), {}, 0.3, 1600});
        QSignalSpy failed(&client, &AiClient::failed);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(failed.wait(5000));
        QVERIFY2(failed.first().first().toString().contains(QStringLiteral("refused")),
                 qPrintable(failed.first().first().toString()));
    }

    void idleTimeout()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = {sseDelta(QStringLiteral("a"))};
            r.stallAfterChunks = true;
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("m"), {}, 0.3, 1600});
        client.setTimeouts(2000, 300);
        QSignalSpy failed(&client, &AiClient::failed);
        QSignalSpy deltas(&client, &AiClient::delta);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(failed.wait(5000));
        QCOMPARE(deltas.count(), 1);
        QVERIFY2(failed.first().first().toString().contains(QStringLiteral("No response")),
                 qPrintable(failed.first().first().toString()));
    }

    void abortIsSilent()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.chunks = {sseDelta(QStringLiteral("a")), sseDelta(QStringLiteral("b")), "data: [DONE]\n\n"};
            r.chunkDelayMs = 200;
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("m"), {}, 0.3, 1600});
        QSignalSpy failed(&client, &AiClient::failed);
        QSignalSpy finished(&client, &AiClient::finished);
        QSignalSpy deltas(&client, &AiClient::delta);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(deltas.wait(5000));
        client.abort();
        QVERIFY(!client.isBusy());
        QTest::qWait(600);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(finished.count(), 0);
    }

    void noCookies()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &) {
            FakeResponse r;
            r.contentType = "text/event-stream";
            r.extraHeaders = {"Set-Cookie: track=1; Path=/"};
            r.chunks = {sseDelta(QStringLiteral("a")), "data: [DONE]\n\n"};
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), QStringLiteral("m"), {}, 0.3, 1600});
        QSignalSpy finished(&client, &AiClient::finished);
        client.ask({{AiMessage::User, QStringLiteral("x"), {}}});
        QVERIFY(finished.wait(5000));
        client.ask({{AiMessage::User, QStringLiteral("y"), {}}});
        QVERIFY(finished.wait(5000));
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(!server.requests[1].headers.contains("cookie"));
    }

    // --- request building ---------------------------------------------------

    void imagePartEncoding()
    {
        AiSettings s{QStringLiteral("http://localhost:11434/v1"), QStringLiteral("m"), {}, 0.3, 800};
        AiMessage photo{AiMessage::User, QStringLiteral("What is this?"), {noisyImage(1600, 1000)}};
        QImage flat(1200, 300, QImage::Format_ARGB32);
        flat.fill(Qt::white);
        {
            QPainter p(&flat);
            p.fillRect(100, 100, 400, 50, Qt::black);
        }
        AiMessage screenshot{AiMessage::User, QString(), {flat}};
        const QJsonObject body = AiClient::buildRequest(s, {photo, screenshot});
        const QJsonArray msgs = body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(msgs.size(), 2);

        const QJsonArray parts = msgs[0].toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(parts.size(), 2);
        QCOMPARE(parts[0].toObject().value(QStringLiteral("type")).toString(), QStringLiteral("text"));
        QCOMPARE(parts[0].toObject().value(QStringLiteral("text")).toString(), QStringLiteral("What is this?"));
        const QJsonObject img = parts[1].toObject();
        QCOMPARE(img.value(QStringLiteral("type")).toString(), QStringLiteral("image_url"));
        const QString url = img.value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString();
        QVERIFY(url.startsWith(QStringLiteral("data:image/jpeg;base64,")));
        QImage decoded;
        QVERIFY(decoded.loadFromData(QByteArray::fromBase64(url.section(QLatin1Char(','), 1).toLatin1()), "JPEG"));
        QCOMPARE(decoded.size(), QSize(800, 500));

        const QJsonArray parts2 = msgs[1].toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(parts2.size(), 1);  // no empty text part
        const QString url2 =
            parts2[0].toObject().value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString();
        QVERIFY(url2.startsWith(QStringLiteral("data:image/png;base64,")));
        QImage decoded2;
        QVERIFY(decoded2.loadFromData(QByteArray::fromBase64(url2.section(QLatin1Char(','), 1).toLatin1()), "PNG"));
        QCOMPARE(decoded2.size(), QSize(800, 200));
    }

    void endpointsAndPrivacy()
    {
        QCOMPARE(AiClient::baseUrl(QStringLiteral("http://localhost:11434")).toString(),
                 QStringLiteral("http://localhost:11434/v1"));
        QCOMPARE(AiClient::baseUrl(QStringLiteral("http://localhost:8080/v1/")).toString(),
                 QStringLiteral("http://localhost:8080/v1"));
        QCOMPARE(AiClient::baseUrl(QStringLiteral("https://openrouter.ai/api/v1/chat/completions")).toString(),
                 QStringLiteral("https://openrouter.ai/api/v1"));
        QCOMPARE(AiClient::baseUrl(QStringLiteral("localhost:1234")).toString(), QStringLiteral("http://localhost:1234/v1"));

        const auto local = [](const char *u) { return AiClient::isLocalUrl(QUrl(QString::fromLatin1(u))); };
        QVERIFY(local("http://localhost:11434/v1"));
        QVERIFY(local("http://127.0.0.1:8080/v1"));
        QVERIFY(local("http://127.1.2.3/v1"));
        QVERIFY(local("http://[::1]:8000/v1"));
        QVERIFY(local("http://gpu-box.local:11434/v1"));
        QVERIFY(local("http://ollama.localhost/v1"));
        QVERIFY(!local("https://openrouter.ai/api/v1"));
        QVERIFY(!local("https://api.openai.com/v1"));
        QVERIFY(!local("http://192.168.1.10:11434/v1"));
        QVERIFY(!local("http://localhost.evil.com/v1"));

        AiClient c;
        c.setSettings({QStringLiteral("https://openrouter.ai/api/v1"), {}, {}, 0.3, 1600});
        QVERIFY(!c.isLocalEndpoint());
        QCOMPARE(c.endpointHost(), QStringLiteral("openrouter.ai"));
    }

    void settingsFromConfig()
    {
        QTemporaryDir dir;
        Config config;
        QVERIFY(config.load(dir.filePath(QStringLiteral("config.toml"))));
        AiSettings s = aiSettingsFromConfig(config);
        QCOMPARE(s.endpoint, QStringLiteral("http://localhost:11434/v1"));
        QCOMPARE(s.model, QStringLiteral("qwen3-vl:4b"));
        QVERIFY(s.apiKey.isEmpty());

        qputenv("SELSEARCH_TEST_KEY", "env-key");
        config.setValue(QStringLiteral("ai.api_key_env"), QStringLiteral("SELSEARCH_TEST_KEY"));
        QCOMPARE(aiSettingsFromConfig(config).apiKey, QStringLiteral("env-key"));
        config.setValue(QStringLiteral("ai.api_key"), QStringLiteral("cfg-key"));
        QCOMPARE(aiSettingsFromConfig(config).apiKey, QStringLiteral("cfg-key"));
        qunsetenv("SELSEARCH_TEST_KEY");
    }

    void listModels()
    {
        FakeServer server;
        server.handler = [](const FakeRequest &req) {
            FakeResponse r;
            if (req.path == "/v1/models")
                r.chunks = {"{\"object\":\"list\",\"data\":[{\"id\":\"qwen3-vl:4b\",\"object\":\"model\"},{\"id\":\"gemma3:4b\"}]}"};
            else
                r.status = 404;
            return r;
        };
        AiClient client;
        client.setSettings({server.url(), {}, {}, 0.3, 1600});
        QSignalSpy listed(&client, &AiClient::modelsListed);
        client.listModels();
        QVERIFY(listed.wait(5000));
        QCOMPARE(listed.first().first().toStringList(),
                 (QStringList{QStringLiteral("gemma3:4b"), QStringLiteral("qwen3-vl:4b")}));
        QCOMPARE(server.requests.first().method, QByteArray("GET"));

        // ollama /api/tags shape
        QCOMPARE(AiClient::parseModelList("{\"models\":[{\"name\":\"llava:7b\",\"model\":\"llava:7b\"}]}"),
                 QStringList{QStringLiteral("llava:7b")});
    }

    // --- history ------------------------------------------------------------

    void historyRoundTrip()
    {
        AiHistory history;
        QCOMPARE(history.directory(), m_xdg.path() + QStringLiteral("/selsearch/history"));
        QVERIFY(history.isEnabled());
        history.deleteAll();

        AiThread a;
        a.append({AiMessage::User,
                  QStringLiteral("What is the tallest building in the world and how tall is it exactly, in metres and feet?"),
                  {noisyImage(1024, 512)}});
        a.append({AiMessage::Assistant, QStringLiteral("The Burj Khalifa, 828 m."), {}});
        QVERIFY(history.save(a));
        QVERIFY(AiHistory::isValidId(a.id));
        QVERIFY(a.title.startsWith(QStringLiteral("What is the tallest building")));
        QVERIFY(a.title.size() <= AiHistory::kTitleLength);

        const QString file = history.directory() + QLatin1Char('/') + a.id + QStringLiteral(".json");
        QVERIFY(QFileInfo::exists(file));
        const auto perms = QFileInfo(file).permissions();
        QVERIFY(perms.testFlags(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(int(perms & (QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                              | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)),
                 0);

        const auto loaded = history.load(a.id);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->title, a.title);
        QCOMPARE(loaded->messages.size(), 2);
        QCOMPARE(loaded->messages[0].role, QStringLiteral("user"));
        QCOMPARE(loaded->messages[1].text, QStringLiteral("The Burj Khalifa, 828 m."));
        QCOMPARE(loaded->messages[0].thumbnails.size(), 1);
        QCOMPARE(loaded->created, a.created);
        const QList<AiMessage> conv = loaded->toConversation();
        QCOMPARE(conv[1].role, AiMessage::Assistant);
        QCOMPARE(conv[0].images.size(), 1);
        QCOMPARE(conv[0].images[0].size(), QSize(256, 128));  // thumbnail only

        QTest::qWait(5);
        AiThread b;
        b.append({AiMessage::User, QStringLiteral("Translate «кошка»"), {}});
        QVERIFY(history.save(b));

        const QList<AiThread> all = history.list();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].id, b.id);  // newest first
        QCOMPARE(all[1].id, a.id);

        QCOMPARE(history.search(QStringLiteral("burj")).size(), 1);
        QCOMPARE(history.search(QStringLiteral("КОШКА")).size(), 1);
        QCOMPARE(history.search(QStringLiteral("zzz")).size(), 0);

        QVERIFY(!history.load(QStringLiteral("../../etc/passwd")).has_value());
        QVERIFY(history.remove(b.id));
        QCOMPARE(history.list().size(), 1);
        QCOMPARE(history.deleteAll(), 1);
        QVERIFY(history.list().isEmpty());
    }

    void historyDisabled()
    {
        QTemporaryDir cfgDir;
        Config config;
        QVERIFY(config.load(cfgDir.filePath(QStringLiteral("config.toml"))));
        config.setValue(QStringLiteral("ai.history"), false);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("h"));
        AiHistory history(&config, path);
        QVERIFY(!history.isEnabled());
        AiThread t;
        t.append({AiMessage::User, QStringLiteral("secret"), {}});
        QVERIFY(!history.save(t));
        QVERIFY(!QFileInfo::exists(path));
    }

    // Optional check against a real server, e.g.
    //   SELSEARCH_LIVE_AI_ENDPOINT=http://127.0.0.1:8080/v1 SELSEARCH_LIVE_AI_MODEL=qwen2.5 ./tst_ai liveServer
    void liveServer()
    {
        const QString endpoint = qEnvironmentVariable("SELSEARCH_LIVE_AI_ENDPOINT");
        if (endpoint.isEmpty())
            QSKIP("SELSEARCH_LIVE_AI_ENDPOINT not set");
        AiClient client;
        client.setSettings({endpoint, qEnvironmentVariable("SELSEARCH_LIVE_AI_MODEL"), {}, 0.2, 1600});
        QSignalSpy listed(&client, &AiClient::modelsListed);
        client.listModels();
        QVERIFY(listed.wait(30000));
        qInfo() << "models:" << listed.first().first().toStringList();

        QSignalSpy deltas(&client, &AiClient::delta);
        QSignalSpy finished(&client, &AiClient::finished);
        QSignalSpy failed(&client, &AiClient::failed);
        client.ask({{AiMessage::User, QStringLiteral("Reply with one short sentence: what is the capital of France?"), {}}});
        QVERIFY(finished.wait(120000));
        QCOMPARE(failed.count(), 0);
        qInfo() << "deltas:" << deltas.count() << "answer:" << finished.first().first().toString();
        QVERIFY(deltas.count() > 1);
        QVERIFY(finished.first().first().toString().contains(QStringLiteral("Paris"), Qt::CaseInsensitive));
    }

private:
    QTemporaryDir m_xdg;
};

QTEST_MAIN(TestAi)
#include "tst_ai.moc"
