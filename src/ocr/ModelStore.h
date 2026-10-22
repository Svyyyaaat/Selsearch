// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace selsearch {

// One file of a model (the ONNX graph, or the YAML config holding the
// recognition dictionary).
struct ModelFile {
    QString fileName;    // name inside the model directory
    QString url;         // download URL, pinned to an immutable revision
    QByteArray sha256;   // lowercase hex
    qint64 size = 0;     // bytes
};

struct ModelSpec {
    enum Kind { Detection, Recognition };
    QString id;              // e.g. "eslav_PP-OCRv5_mobile_rec"
    Kind kind = Recognition;
    QList<ModelFile> files;  // files[0] is the .onnx graph
    QStringList languages;   // ISO 639-1 codes the model reads (recognition only)
    QString license;

    QString onnxFile() const { return files.isEmpty() ? QString() : files.first().fileName; }
};

// Downloads PP-OCRv5 models on demand into OcrService::modelDir() and answers
// which models are present. Nothing is ever downloaded implicitly: only
// ensure() touches the network.
//
// The default table uses the official PaddlePaddle ONNX exports on Hugging Face
// (https://huggingface.co/PaddlePaddle, Apache-2.0), pinned to a commit and a
// SHA-256 per file.
class ModelStore : public QObject {
    Q_OBJECT
public:
    explicit ModelStore(QObject *parent = nullptr);  // OcrService::modelDir(), default table
    ModelStore(const QString &dir, QList<ModelSpec> specs, QObject *parent = nullptr);
    ~ModelStore() override;

    // ---- static model table and language logic (no I/O) ----
    static const QList<ModelSpec> &defaultSpecs();
    static QString detectionModelId();
    // The single recognition model that best covers `languages`, among
    // `candidates` (ids; empty = all recognition models in the table).
    // Returns an empty string only if `candidates` is non-empty and contains no
    // recognition model. See the .cpp for the exact ranking.
    static QString bestRecognitionModel(const QStringList &languages, const QStringList &candidates = {});
    // The recognition models to run for `languages`: the fewest that cover
    // every requested language that has a model, ordered by the first
    // language each one is needed for (ja+ru: zh/ja/en model, then eslav;
    // ru+en: eslav alone, since every model reads English). Never empty when
    // `candidates` holds a recognition model: falls back to
    // bestRecognitionModel(). At most kMaxRecognitionModels.
    static QStringList recognitionModelsFor(const QStringList &languages, const QStringList &candidates = {});
    static constexpr int kMaxRecognitionModels = 4;
    static const ModelSpec *findSpec(const QString &id);
    // A model is "present" when all of its files exist with the expected size
    // (hashes are verified at download time, before the final rename).
    static bool isPresent(const QString &dir, const ModelSpec &spec);
    static QStringList presentRecognitionModels(const QString &dir);

    // ---- instance API ----
    QString directory() const { return m_dir; }
    const QList<ModelSpec> &specs() const { return m_specs; }
    // Model ids needed for `languages`: the detector plus recognitionModelsFor().
    QStringList requiredFor(const QStringList &languages) const;
    QStringList missingFor(const QStringList &languages) const;
    bool haveModelsFor(const QStringList &languages) const { return missingFor(languages).isEmpty(); }

    // Download whatever missingFor(languages) reports. Asynchronous; emits
    // progress() while running and exactly one of finished()/failed().
    // Calling ensure() while a download is running is rejected with failed().
    void ensure(const QStringList &languages);
    void cancel();
    bool isBusy() const { return m_reply != nullptr || !m_queue.isEmpty(); }

Q_SIGNALS:
    void progress(qint64 receivedBytes, qint64 totalBytes);
    void finished();
    void failed(const QString &error);

private:
    struct Job;
    const ModelSpec *spec(const QString &id) const;
    void startNext();
    void onReadyRead();
    void onFinished();
    void fail(const QString &error);

    QString m_dir;
    QList<ModelSpec> m_specs;
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QList<ModelFile> m_queue;
    std::unique_ptr<Job> m_job;
    qint64 m_doneBytes = 0;
    qint64 m_totalBytes = 0;
};

}  // namespace selsearch
