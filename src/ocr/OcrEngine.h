// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace selsearch {

// One recognised word. Boxes are in the pixel space of the image that was
// passed to OcrEngine::recognize() (i.e. physical screen pixels).
struct OcrWord {
    QString text;
    QRectF box;
    float confidence = 1.0f;
    bool spaceBefore = true;  // false between pieces of one CJK run (no spaces in the source)
};

// A line of text in reading order. Words are left-to-right (or right-to-left
// for RTL scripts, in logical order).
struct OcrLine {
    QRectF box;
    QList<OcrWord> words;
    QString text() const;
};

struct OcrResult {
    QList<OcrLine> lines;  // sorted in reading order: top-to-bottom, then left-to-right
    QString engine;        // "paddle", "tesseract", ...
    qint64 elapsedMs = 0;
    QString error;         // non-empty if recognition failed
    bool ok() const { return error.isEmpty(); }
    QString text() const;  // lines joined with '\n'
};

// Abstract OCR engine. Implementations must be safe to call from a worker
// thread; a single instance is never used from two threads at once.
class OcrEngine {
public:
    virtual ~OcrEngine() = default;
    virtual QString id() const = 0;
    // `languages` are ISO 639-1 codes ("en", "ru", "ja", ...). Engines pick the
    // models/traineddata that cover them and ignore codes they cannot handle.
    virtual OcrResult recognize(const QImage &image, const QStringList &languages) = 0;
    // Release models/memory. The next recognize() call reloads lazily.
    virtual void unload() {}
    virtual bool isAvailable() const = 0;

    // Long-running engines poll this and return early (with an error) once it
    // returns true. Set by OcrService before each recognize() call.
    void setCancelCheck(std::function<bool()> check) { m_cancelCheck = std::move(check); }

protected:
    bool cancelled() const { return m_cancelCheck && m_cancelCheck(); }
    std::function<bool()> m_cancelCheck;
};

// Factory functions. Return nullptr when the engine was not compiled in.
std::unique_ptr<OcrEngine> createPaddleOcrEngine(const QString &modelDir);
std::unique_ptr<OcrEngine> createTesseractOcrEngine();

// Sort lines into reading order and recompute line boxes. Shared helper.
void sortReadingOrder(QList<OcrLine> &lines);

// Split Chinese/Japanese runs (which OCR returns as one "word", since the
// scripts use no spaces) into dictionary words (ICU), so they can be selected
// one by one. Boxes are divided proportionally to character widths.
void segmentCjkWords(QList<OcrLine> &lines);
bool containsCjk(QStringView text);

}  // namespace selsearch
