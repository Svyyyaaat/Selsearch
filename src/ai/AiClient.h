// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

class QJsonObject;
class QNetworkReply;
class QTimer;

namespace selsearch {

class Config;

struct AiMessage {
    enum Role { System, User, Assistant };
    Role role = User;
    QString text;
    QList<QImage> images;  // sent as base64 JPEG/PNG image_url parts (user messages only)
};

struct AiSettings {
    QString endpoint;  // base URL of an OpenAI-compatible API, e.g. http://localhost:11434/v1
    QString model;
    QString apiKey;    // optional; sent as "Authorization: Bearer ..."
    double temperature = 0.3;
    int maxImageSide = 1600;  // images are downscaled to this before upload
};

// ai.endpoint / ai.model / ai.api_key (or the env var named by ai.api_key_env).
AiSettings aiSettingsFromConfig(const Config &config);

// Incremental parser for a text/event-stream body. Bytes may be fed split
// anywhere (mid-line, mid-UTF-8 sequence, between \r and \n). Returns the
// "data" payload of every event completed by this chunk; comment lines
// (":...") and other fields are ignored. As a fallback for servers that send
// newline-delimited JSON instead of SSE, a bare line starting with '{' is
// returned as an event of its own.
class SseParser {
public:
    QList<QByteArray> feed(const QByteArray &chunk);
    QList<QByteArray> finish();  // end of stream: flush a pending event
    void reset();

private:
    void processLine(const QByteArray &line, QList<QByteArray> &out);
    QByteArray m_pending;  // bytes of the current, incomplete line
    QByteArray m_data;     // data of the current event
    bool m_hasData = false;
    bool m_skipLf = false;  // last chunk ended with '\r'
};

// Streams a chat completion from any OpenAI-compatible endpoint
// (ollama, llama.cpp llama-server, vLLM, LM Studio, OpenRouter, OpenAI, ...).
//
// Privacy: no cookie jar, redirects only within the same origin, nothing is
// sent anywhere but the configured endpoint.
class AiClient : public QObject {
    Q_OBJECT
public:
    explicit AiClient(QObject *parent = nullptr);
    ~AiClient() override;

    void setSettings(const AiSettings &settings) { m_settings = settings; }
    const AiSettings &settings() const { return m_settings; }

    // true for localhost / *.localhost / loopback addresses / ::1 / *.local.
    // Anything else means data leaves the machine: show endpointHost().
    bool isLocalEndpoint() const;
    QString endpointHost() const;
    static bool isLocalUrl(const QUrl &url);

    // Normalised base URL: trailing '/' and a pasted "/chat/completions"
    // removed; "/v1" appended when the URL has no path at all.
    static QUrl baseUrl(const QString &endpoint);

    // Timeouts (ms). Connect: until the request has been written to the
    // socket. Idle: maximum time without receiving any bytes.
    void setTimeouts(int connectMs, int idleMs);

    // Starts a streaming request; any running request is aborted first.
    void ask(const QList<AiMessage> &conversation);
    void abort();
    bool isBusy() const { return !m_reply.isNull(); }

    // GET {endpoint}/models; emits modelsListed or modelsFailed.
    void listModels();

    // --- Helpers, public for tests -------------------------------------
    // Request body for ask().
    static QJsonObject buildRequest(const AiSettings &settings, const QList<AiMessage> &conversation);
    // "data:image/jpeg;base64,..." (or image/png for flat, text-like images),
    // downscaled so the longest side is <= maxSide.
    static QString imageDataUrl(const QImage &image, int maxSide);
    static bool looksFlat(const QImage &image);  // few distinct colours: screenshots of text, UI
    // Human readable message for an HTTP error status + body, with hints
    // (e.g. "ollama pull <model>" when ollama reports a missing model).
    static QString describeHttpError(int status, const QByteArray &body, const AiSettings &settings);
    static QStringList parseModelList(const QByteArray &body);

Q_SIGNALS:
    void delta(const QString &text);      // incremental text
    void reasoning(const QString &text);  // incremental "thinking" text, if the server separates it
    void finished(const QString &full);   // complete answer
    void failed(const QString &error);
    void modelsListed(const QStringList &models);
    void modelsFailed(const QString &error);

private:
    QNetworkRequest makeRequest(const QString &path) const;
    void onReadyRead();
    void onFinished();
    void handleEvent(const QByteArray &data);
    void fail(const QString &error);
    QString networkErrorText(QNetworkReply *reply) const;

    AiSettings m_settings;
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_reply;
    QPointer<QNetworkReply> m_modelsReply;
    QTimer *m_connectTimer = nullptr;
    SseParser m_parser;
    QByteArray m_buffer;  // raw body: kept for error messages / non-streaming replies
    QString m_full;
    bool m_done = false;
    bool m_gotEvent = false;
    bool m_failed = false;
    bool m_connectTimedOut = false;
    int m_connectTimeoutMs = 10000;
    int m_idleTimeoutMs = 120000;
};

}  // namespace selsearch
