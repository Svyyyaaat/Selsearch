// SPDX-License-Identifier: MIT
//
// Tesseract 5 engine: the small, widely packaged fallback. Weaker than PP-OCR
// on UI screenshots but covers 100+ languages via traineddata packages.

#include "OcrEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <memory>

namespace selsearch {
namespace {

QString tesseractCode(const QString &iso)
{
    static const QHash<QString, QString> map = {
        {"en", "eng"}, {"ru", "rus"}, {"uk", "ukr"}, {"be", "bel"}, {"bg", "bul"}, {"sr", "srp"},
        {"de", "deu"}, {"fr", "fra"}, {"es", "spa"}, {"it", "ita"}, {"pt", "por"}, {"nl", "nld"},
        {"pl", "pol"}, {"cs", "ces"}, {"sk", "slk"}, {"sl", "slv"}, {"hr", "hrv"}, {"ro", "ron"},
        {"hu", "hun"}, {"fi", "fin"}, {"sv", "swe"}, {"da", "dan"}, {"no", "nor"}, {"nb", "nor"},
        {"et", "est"}, {"lv", "lav"}, {"lt", "lit"}, {"el", "ell"}, {"tr", "tur"}, {"he", "heb"},
        {"ar", "ara"}, {"fa", "fas"}, {"ur", "urd"}, {"hi", "hin"}, {"mr", "mar"}, {"ne", "nep"},
        {"bn", "ben"}, {"ta", "tam"}, {"te", "tel"}, {"th", "tha"}, {"vi", "vie"}, {"id", "ind"},
        {"ms", "msa"}, {"ja", "jpn"}, {"ko", "kor"}, {"zh", "chi_sim"}, {"ka", "kat"}, {"hy", "hye"},
        {"kk", "kaz"}, {"ky", "kir"}, {"uz", "uzb"}, {"az", "aze"}, {"mn", "mon"}, {"ca", "cat"},
        {"eu", "eus"}, {"gl", "glg"}, {"ga", "gle"}, {"cy", "cym"}, {"is", "isl"}, {"mk", "mkd"},
        {"sq", "sqi"}, {"af", "afr"}, {"sw", "swa"}, {"tl", "tgl"}, {"la", "lat"},
    };
    return map.value(iso, iso);
}

QString findTessdata()
{
    const QString env = qEnvironmentVariable("TESSDATA_PREFIX");
    QStringList candidates;
    if (!env.isEmpty())
        candidates << env << env + QStringLiteral("/tessdata");
    candidates << QStringLiteral("/usr/share/tessdata") << QStringLiteral("/usr/share/tesseract-ocr/5/tessdata")
               << QStringLiteral("/usr/share/tesseract-ocr/4.00/tessdata") << QStringLiteral("/usr/local/share/tessdata");
    for (const QString &c : candidates)
        if (QFileInfo::exists(c + QStringLiteral("/eng.traineddata")))
            return c;
    return {};
}

// Tesseract's layout analysis expects dark text on a light page and skips
// light-on-dark UI panels. Normalise: greyscale, then invert every pixel whose
// local background (a heavily downscaled copy) is dark.
QImage normaliseForTesseract(const QImage &input, qreal factor)
{
    QImage gray = input.convertToFormat(QImage::Format_Grayscale8);
    if (factor != 1.0)
        gray = gray.scaled(gray.size() * factor, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QSize bgSize(std::max(1, gray.width() / 24), std::max(1, gray.height() / 24));
    const QImage bg = gray.scaled(bgSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .scaled(gray.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < gray.height(); ++y) {
        uchar *row = gray.scanLine(y);
        const uchar *b = bg.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            if (b[x] < 110)
                row[x] = 255 - row[x];
    }
    return gray;
}

class TesseractEngine final : public OcrEngine {
public:
    TesseractEngine()
        : m_tessdata(findTessdata())
    {
    }
    QString id() const override { return QStringLiteral("tesseract"); }
    bool isAvailable() const override { return !m_tessdata.isEmpty(); }

    void unload() override
    {
        m_api.reset();
        m_loadedLangs.clear();
    }

    OcrResult recognize(const QImage &input, const QStringList &languages) override
    {
        OcrResult result;
        QStringList codes;
        for (const QString &l : languages) {
            const QString code = tesseractCode(l);
            if (!codes.contains(code) && QFileInfo::exists(m_tessdata + QLatin1Char('/') + code + QStringLiteral(".traineddata")))
                codes << code;
        }
        if (codes.isEmpty())
            codes << QStringLiteral("eng");
        const QString langSpec = codes.join(QLatin1Char('+'));
        if (!m_api || m_loadedLangs != langSpec) {
            m_api = std::make_unique<tesseract::TessBaseAPI>();
            if (m_api->Init(m_tessdata.toUtf8().constData(), langSpec.toUtf8().constData(), tesseract::OEM_LSTM_ONLY) != 0) {
                m_api.reset();
                result.error = QStringLiteral("tesseract init failed for %1").arg(langSpec);
                return result;
            }
            m_api->SetPageSegMode(tesseract::PSM_AUTO);
            m_api->SetVariable("preserve_interword_spaces", "1");
            m_loadedLangs = langSpec;
        }

        // Tesseract wants ~20-40 px glyphs; screen text at 1x is smaller.
        const qreal factor = (input.width() * input.height() < 6'000'000) ? 2.0 : 1.0;
        const QImage img = normaliseForTesseract(input, factor);
        m_api->SetImage(img.constBits(), img.width(), img.height(), 1, int(img.bytesPerLine()));
        m_api->SetSourceResolution(144);
        if (m_api->Recognize(nullptr) != 0) {
            result.error = QStringLiteral("tesseract recognition failed");
            return result;
        }
        std::unique_ptr<tesseract::ResultIterator> it(m_api->GetIterator());
        if (it) {
            OcrLine current;
            do {
                if (it->Empty(tesseract::RIL_WORD))
                    continue;
                if (it->IsAtBeginningOf(tesseract::RIL_TEXTLINE) && !current.words.isEmpty()) {
                    result.lines << current;
                    current = OcrLine{};
                }
                std::unique_ptr<char[]> txt(it->GetUTF8Text(tesseract::RIL_WORD));
                const QString text = QString::fromUtf8(txt.get()).trimmed();
                const float conf = it->Confidence(tesseract::RIL_WORD);
                if (text.isEmpty() || conf < 30.0f)
                    continue;
                int x1, y1, x2, y2;
                it->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                current.words << OcrWord{text, QRectF(x1 / factor, y1 / factor, (x2 - x1) / factor, (y2 - y1) / factor),
                                         conf / 100.0f};
            } while (it->Next(tesseract::RIL_WORD));
            if (!current.words.isEmpty())
                result.lines << current;
        }
        m_api->Clear();
        sortReadingOrder(result.lines);
        return result;
    }

private:
    QString m_tessdata;
    QString m_loadedLangs;
    std::unique_ptr<tesseract::TessBaseAPI> m_api;
};

}  // namespace

std::unique_ptr<OcrEngine> createTesseractOcrEngine()
{
    return std::make_unique<TesseractEngine>();
}

}  // namespace selsearch
