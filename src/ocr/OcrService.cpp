// SPDX-License-Identifier: MIT
#include "OcrService.h"

#include "core/Config.h"

#include <QElapsedTimer>
#include <QStandardPaths>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace selsearch {

#ifndef SELSEARCH_HAVE_ONNX
std::unique_ptr<OcrEngine> createPaddleOcrEngine(const QString &) { return nullptr; }
#endif
#ifndef SELSEARCH_HAVE_TESSERACT
std::unique_ptr<OcrEngine> createTesseractOcrEngine() { return nullptr; }
#endif

QString OcrService::modelDir()
{
    const QString env = qEnvironmentVariable("SELSEARCH_MODEL_DIR");
    if (!env.isEmpty())
        return env;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/selsearch/models/ppocr");
}

OcrService::OcrService(const Config &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_paddle(createPaddleOcrEngine(modelDir()))
    , m_tesseract(createTesseractOcrEngine())
{
    m_pool.setMaxThreadCount(1);
    m_unloadTimer.setSingleShot(true);
    connect(&m_unloadTimer, &QTimer::timeout, this, [this] {
        // Unload on the worker so it never races a running recognition.
        (void)QtConcurrent::run(&m_pool, [this] {
            std::lock_guard lock(m_mutex);
            if (m_paddle)
                m_paddle->unload();
            if (m_tesseract)
                m_tesseract->unload();
        });
    });
}

OcrService::~OcrService()
{
    m_shutdown = true;  // running engines notice and stop early
    m_pool.clear();     // queued requests never start
    m_pool.waitForDone();
}

QStringList OcrService::availableEngines() const
{
    QStringList out;
    if (m_paddle && m_paddle->isAvailable())
        out << m_paddle->id();
    if (m_tesseract && m_tesseract->isAvailable())
        out << m_tesseract->id();
    return out;
}

OcrEngine *OcrService::pickEngine(const QStringList &)
{
    const QString pref = m_config.string(QStringLiteral("ocr.engine"));
    OcrEngine *paddle = m_paddle && m_paddle->isAvailable() ? m_paddle.get() : nullptr;
    OcrEngine *tess = m_tesseract && m_tesseract->isAvailable() ? m_tesseract.get() : nullptr;
    if (pref == QLatin1String("tesseract"))
        return tess ? tess : paddle;
    return paddle ? paddle : tess;
}

QFuture<OcrResult> OcrService::recognize(const QImage &image, const QStringList &languages, CancelToken cancel)
{
    const QStringList langs = languages.isEmpty() ? effectiveOcrLanguages(m_config) : languages;
    OcrEngine *engine = pickEngine(langs);
    m_unloadTimer.stop();
    scheduleUnload();
    if (!engine) {
        OcrResult r;
        r.error = QStringLiteral("no OCR engine available (install onnxruntime models or tesseract)");
        return QtFuture::makeReadyValueFuture(r);
    }
    return QtConcurrent::run(&m_pool, [this, engine, image, langs, cancel] {
        if (cancel && cancel->load()) {
            OcrResult r;
            r.error = QStringLiteral("cancelled");
            return r;
        }
        // Run (and spawn the engine's helper threads) at low CPU priority so
        // the overlay keeps animating smoothly while text is recognised.
        static thread_local bool niced = [] {
#ifdef Q_OS_WIN
            // Threads do not inherit this on Windows; PaddleOcr lowers its own.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
            setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 10);
#endif
            return true;
        }();
        (void)niced;
        std::lock_guard lock(m_mutex);
        engine->setCancelCheck([this, cancel] { return m_shutdown.load() || (cancel && cancel->load()); });
        QElapsedTimer t;
        t.start();
        OcrResult r = engine->recognize(image, langs);
        engine->setCancelCheck({});
        segmentCjkWords(r.lines);
        r.engine = engine->id();
        r.elapsedMs = t.elapsed();
        return r;
    });
}

void OcrService::scheduleUnload()
{
    const int secs = m_config.integer(QStringLiteral("ocr.unload_after_s"));
    if (secs > 0)
        m_unloadTimer.start(secs * 1000);
}

}  // namespace selsearch
