// SPDX-License-Identifier: MIT
#pragma once

#include "OcrEngine.h"

#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <memory>
#include <mutex>

namespace selsearch {

class Config;

// Owns the OCR engines, runs recognition on a dedicated worker thread, and
// unloads models after `ocr.unload_after_s` seconds without use.
class OcrService : public QObject {
    Q_OBJECT
public:
    explicit OcrService(const Config &config, QObject *parent = nullptr);
    ~OcrService() override;

    static QString modelDir();  // $XDG_DATA_HOME/selsearch/models/ppocr

    // `languages` empty = effectiveOcrLanguages(config).
    QFuture<OcrResult> recognize(const QImage &image, const QStringList &languages = {});
    QStringList availableEngines() const;

private:
    OcrEngine *pickEngine(const QStringList &languages);
    void scheduleUnload();

    const Config &m_config;
    QThreadPool m_pool;
    QTimer m_unloadTimer;
    std::mutex m_mutex;  // guards engine use across the (single) worker and unload
    std::unique_ptr<OcrEngine> m_paddle;
    std::unique_ptr<OcrEngine> m_tesseract;
};

}  // namespace selsearch
