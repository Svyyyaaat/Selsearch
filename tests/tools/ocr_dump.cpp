// SPDX-License-Identifier: MIT
// Dev tool: run an OCR engine on an image file and print the result as JSON.
//   selsearch-ocr-dump IMAGE [--engine paddle|tesseract] [--lang en,ru]
//                            [--repeat N] [--debug OUT.png] [--download]
// --download first fetches the PP-OCR models needed for --lang (ModelStore).
// "timingsMs" is wall time per run, "cpuMs" the process CPU time (all threads).
// Models are read from OcrService::modelDir() ($SELSEARCH_MODEL_DIR overrides).
// Set SELSEARCH_PPOCR_DEBUG=1 for a per-stage timing breakdown on stderr.
#include "ocr/ModelStore.h"
#include "ocr/OcrEngine.h"
#include "ocr/OcrService.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <cstdio>

#include <sys/resource.h>

using namespace selsearch;

static long maxRssKb()
{
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

static double cpuMs()
{
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

static QJsonArray rectJson(const QRectF &r)
{
    return QJsonArray{qRound(r.x() * 10) / 10.0, qRound(r.y() * 10) / 10.0, qRound(r.width() * 10) / 10.0,
                      qRound(r.height() * 10) / 10.0};
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);  // QPainter text/fonts for the debug image
    QCommandLineParser p;
    p.addHelpOption();
    p.addPositionalArgument(QStringLiteral("image"), QStringLiteral("Image file to recognise"));
    p.addOption({QStringLiteral("engine"), QStringLiteral("paddle (default) or tesseract"), QStringLiteral("id"),
                 QStringLiteral("paddle")});
    p.addOption({QStringLiteral("lang"), QStringLiteral("Comma-separated ISO 639-1 codes"), QStringLiteral("codes"),
                 QStringLiteral("en")});
    p.addOption({QStringLiteral("repeat"), QStringLiteral("Run N times and report each timing"), QStringLiteral("n"),
                 QStringLiteral("1")});
    p.addOption({QStringLiteral("debug"), QStringLiteral("Write a PNG with line/word boxes drawn"),
                 QStringLiteral("png")});
    p.addOption({QStringLiteral("download"), QStringLiteral("Download missing PP-OCR models for --lang first")});
    p.process(app);
    if (p.positionalArguments().isEmpty())
        p.showHelp(1);
    const QStringList langs = p.value(QStringLiteral("lang")).split(QLatin1Char(','), Qt::SkipEmptyParts);

    if (p.isSet(QStringLiteral("download"))) {
        ModelStore store;
        std::fprintf(stderr, "models in %s, missing: %s\n", qPrintable(store.directory()),
                     qPrintable(store.missingFor(langs).join(QLatin1Char(' '))));
        int status = 0;
        QObject::connect(&store, &ModelStore::progress, [](qint64 got, qint64 total) {
            std::fprintf(stderr, "\r%lld / %lld KiB", got / 1024, total / 1024);
        });
        QObject::connect(&store, &ModelStore::finished, &app, [&] { QCoreApplication::exit(0); });
        QObject::connect(&store, &ModelStore::failed, &app, [&](const QString &e) {
            std::fprintf(stderr, "\ndownload failed: %s\n", qPrintable(e));
            QCoreApplication::exit(1);
        });
        QElapsedTimer t;
        t.start();
        store.ensure(langs);
        status = app.exec();
        std::fprintf(stderr, "\ndownload %s in %lld ms\n", status ? "failed" : "done", t.elapsed());
        if (status)
            return status;
    }

    QImage image(p.positionalArguments().first());
    if (image.isNull()) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(p.positionalArguments().first()));
        return 1;
    }
    const QString engineId = p.value(QStringLiteral("engine"));
    std::unique_ptr<OcrEngine> engine = engineId == QLatin1String("tesseract")
                                            ? createTesseractOcrEngine()
                                            : createPaddleOcrEngine(OcrService::modelDir());
    if (!engine || !engine->isAvailable()) {
        std::fprintf(stderr, "engine %s not available (models in %s)\n", qPrintable(engineId),
                     qPrintable(OcrService::modelDir()));
        return 1;
    }
    const int repeat = std::max(1, p.value(QStringLiteral("repeat")).toInt());

    OcrResult r;
    QJsonArray timings, cpuTimings;
    for (int i = 0; i < repeat; ++i) {
        QElapsedTimer t;
        t.start();
        const double c0 = cpuMs();
        r = engine->recognize(image, langs);
        timings << double(t.elapsed());
        cpuTimings << qRound(cpuMs() - c0);
    }

    QJsonArray lines;
    for (const OcrLine &l : r.lines) {
        QJsonArray words;
        for (const OcrWord &w : l.words)
            words << QJsonObject{{QStringLiteral("text"), w.text},
                                 {QStringLiteral("box"), rectJson(w.box)},
                                 {QStringLiteral("conf"), qRound(w.confidence * 1000) / 1000.0}};
        lines << QJsonObject{{QStringLiteral("text"), l.text()},
                             {QStringLiteral("box"), rectJson(l.box)},
                             {QStringLiteral("words"), words}};
    }
    const QJsonObject out{{QStringLiteral("engine"), engine->id()},
                          {QStringLiteral("image"), QJsonArray{image.width(), image.height()}},
                          {QStringLiteral("error"), r.error},
                          {QStringLiteral("timingsMs"), timings},
                          {QStringLiteral("cpuMs"), cpuTimings},
                          {QStringLiteral("maxRssMB"), qRound(maxRssKb() / 1024.0)},
                          {QStringLiteral("lines"), lines}};
    const QByteArray json = QJsonDocument(out).toJson(QJsonDocument::Indented);
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);

    if (p.isSet(QStringLiteral("debug"))) {
        QImage dbg = image.convertToFormat(QImage::Format_RGB32);
        QPainter painter(&dbg);
        for (const OcrLine &l : r.lines) {
            painter.setPen(QPen(QColor(0, 120, 255), 1));
            painter.drawRect(l.box);
            for (const OcrWord &w : l.words) {
                painter.setPen(QPen(QColor(255, 40, 40, 200), 1));
                painter.drawRect(w.box);
            }
        }
        painter.end();
        if (!dbg.save(p.value(QStringLiteral("debug")))) {
            std::fprintf(stderr, "cannot write %s\n", qPrintable(p.value(QStringLiteral("debug"))));
            return 1;
        }
    }
    return r.ok() ? 0 : 1;
}
