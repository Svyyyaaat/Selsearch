// SPDX-License-Identifier: MIT
// The individual translation backends behind createTranslator(). Exposed for
// tests and for UI that wants to talk to one backend directly.
#pragma once

#include "Translator.h"
#include "ai/AiClient.h"

#include <QElapsedTimer>
#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>

#include <memory>
#include <optional>

class QNetworkReply;

namespace selsearch {

// ---------------------------------------------------------------------------
// Bergamot models (Firefox Translations) through the translateLocally CLI.
//
// CLI facts this relies on (translateLocally src/cli/CLIParsing.h and
// CommandLineIface.cpp):
//   translateLocally -l          one line per installed model:
//       "<Src name>-<Trg name> type: <type> version: <n>; To invoke do -m <shortName>"
//       shortName is "<src>-<trg>-<type>", codes are ISO 639-1 or -3 ("hbs-eng-tiny").
//   translateLocally -m <model>  reads stdin until EOF in batches of 320 lines,
//       writes the translation of each batch to stdout. Bergamot runs with
//       ssplit-mode "paragraph" = one paragraph per line, so line i of the
//       output is the translation of line i of the input.
// The CLI only flushes after 320 lines or EOF, so it cannot be kept alive as
// a streaming server: every hop is one process with stdin closed after the
// input. There is no pivoting in the CLI (only in the native-messaging mode),
// so ru->de runs ru-en, then en-de.
class BergamotTranslator : public Translator {
    Q_OBJECT
public:
    struct Model {
        QString shortName;  // -m argument
        QStringList sources;  // ISO 639-1 codes ("hbs" -> sr, hr, bs)
        QString target;
        QString type;  // tiny | base | ...
        int version = 0;
    };

    explicit BergamotTranslator(QObject *parent = nullptr, const QString &program = QStringLiteral("translateLocally"));

    QString id() const override { return QStringLiteral("bergamot"); }
    // The executable is on PATH and (once listed) at least one model exists.
    bool isAvailable() const override;
    void translate(const TranslationRequest &request, Callback callback) override;

    // Runs `translateLocally -l` once and caches the result (reload = refresh).
    void loadModels(std::function<void(bool ok)> done, bool reload = false);
    bool modelsLoaded() const { return m_modelsLoaded; }
    QList<Model> models() const { return m_models; }
    // Model shortNames to run in order; empty when there is no route.
    QStringList route(const QString &source, const QString &target) const;
    bool canTranslate(const QString &source, const QString &target) const { return !route(source, target).isEmpty(); }

    QString executable() const;  // resolved on PATH, empty when not installed
    void setTimeout(int ms) { m_timeoutMs = ms; }

    static QList<Model> parseModelList(const QByteArray &output);

private:
    void runChain(QStringList chain, QStringList lines, std::function<void(QStringList, QString)> done);
    void runModel(const QString &model, const QStringList &lines, std::function<void(QStringList, QString)> done);

    QString m_program;
    QList<Model> m_models;
    bool m_modelsLoaded = false;
    bool m_loading = false;
    QList<std::function<void(bool)>> m_waiting;
    int m_timeoutMs = 120000;
};

// ---------------------------------------------------------------------------
// LibreTranslate HTTP API: POST /translate, GET /languages.
class LibreTranslateTranslator : public Translator {
    Q_OBJECT
public:
    struct Language {
        QString code;
        QString name;
        QStringList targets;
    };

    LibreTranslateTranslator(const QString &url, const QString &apiKey, QObject *parent = nullptr);

    QString id() const override { return QStringLiteral("libretranslate"); }
    // Configured, and not known to be down.
    bool isAvailable() const override;
    void translate(const TranslationRequest &request, Callback callback) override;

    // GET /languages within `timeoutMs`; the result is cached (a failure for
    // 30 s, success for the lifetime of the object).
    void probe(int timeoutMs, std::function<void(bool ok)> done);
    void fetchLanguages(std::function<void(const QList<Language> &, const QString &error)> done);
    QList<Language> languages() const { return m_languages; }

    static QList<Language> parseLanguages(const QByteArray &body);
    // Parses a /translate reply for `count` segments.
    static TranslationResult parseTranslateReply(int status, const QByteArray &body, int count);

private:
    QNetworkRequest makeRequest(const QString &path) const;
    QString serverCode(const QString &code) const;  // "zh" -> "zh-Hans" when the server wants that

    QUrl m_url;
    QString m_apiKey;
    QNetworkAccessManager m_nam;
    QList<Language> m_languages;
    enum class Probe { Unknown, Running, Ok, Failed } m_probe = Probe::Unknown;
    QElapsedTimer m_probeAge;
    QList<std::function<void(bool)>> m_probeWaiting;
};

// ---------------------------------------------------------------------------
// Any OpenAI-compatible chat model, through AiClient (text only).
class LlmTranslator : public Translator {
    Q_OBJECT
public:
    explicit LlmTranslator(const AiSettings &settings, QObject *parent = nullptr);

    QString id() const override { return QStringLiteral("llm"); }
    bool isAvailable() const override { return !m_settings.endpoint.trimmed().isEmpty(); }
    void translate(const TranslationRequest &request, Callback callback) override;

    static constexpr int kMaxBatchSegments = 40;
    static constexpr int kMaxBatchChars = 3000;
    // Splits segment indices into batches of <= maxSegments / ~maxChars.
    static QList<QList<int>> makeBatches(const QStringList &segments, int maxSegments = kMaxBatchSegments,
                                         int maxChars = kMaxBatchChars);
    // `source` may be empty (unknown). Codes, not names.
    static QString systemPrompt(const QString &source, const QString &target, int count);
    // Extracts `expected` translations from a model reply: a JSON array
    // (possibly in a code fence, after <think> or other prose), an object
    // holding such an array, or as a last resort one line per segment.
    static std::optional<QStringList> parseReply(const QString &reply, int expected);

private:
    struct Job {
        QStringList segments;
        QStringList out;
        QString source;  // may be empty
        QString target;
        QString detected;
        QList<QList<int>> pending;  // batches still to translate
        Callback callback;
    };
    void pump();
    void onAnswer(const QString &text);
    void onError(const QString &error);
    void finishJob(const QString &error);

    AiSettings m_settings;
    AiClient *m_client;
    QList<std::shared_ptr<Job>> m_jobs;  // head is running
    QList<int> m_batch;                  // batch in flight
    bool m_busy = false;
};

}  // namespace selsearch
