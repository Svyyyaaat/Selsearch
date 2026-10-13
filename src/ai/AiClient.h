// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;

namespace selsearch {

struct AiMessage {
    enum Role { System, User, Assistant };
    Role role = User;
    QString text;
    QList<QImage> images;  // sent as base64 PNG image_url parts (user messages only)
};

struct AiSettings {
    QString endpoint;  // base URL of an OpenAI-compatible API, e.g. http://localhost:11434/v1
    QString model;
    QString apiKey;    // optional; sent as "Authorization: Bearer ..."
    double temperature = 0.3;
    int maxImageSide = 1600;  // images are downscaled to this before upload
};

// Streams a chat completion from any OpenAI-compatible endpoint
// (ollama, llama.cpp llama-server, vLLM, LM Studio, OpenRouter, OpenAI, ...).
class AiClient : public QObject {
    Q_OBJECT
public:
    explicit AiClient(QObject *parent = nullptr);
    void setSettings(const AiSettings &settings) { m_settings = settings; }
    const AiSettings &settings() const { return m_settings; }
    bool isLocalEndpoint() const;  // true for loopback / unix-socket hosts

    // Starts a streaming request; any running request is aborted first.
    void ask(const QList<AiMessage> &conversation);
    void abort();
    bool isBusy() const { return !m_reply.isNull(); }

Q_SIGNALS:
    void delta(const QString &text);    // incremental text
    void finished(const QString &full); // complete answer
    void failed(const QString &error);

private:
    void onReadyRead();
    void onFinished();

    AiSettings m_settings;
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer;
    QString m_full;
};

}  // namespace selsearch
