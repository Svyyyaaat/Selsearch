// SPDX-License-Identifier: MIT
//
// PP-OCRv5 engine and model store tests. Recognition tests need the models:
// download them once (selsearchd does it on demand; or fetch the files listed
// in ModelStore.cpp) into $SELSEARCH_MODEL_DIR. Without them those tests skip.
// Set SELSEARCH_TEST_DUMP_DIR to save the rendered test images.
#include "ocr/ModelStore.h"
#include "ocr/OcrEngine.h"
#include "ocr/OcrService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace selsearch;

namespace {

struct TruthWord {
    QString text;
    QRectF box;
};
struct TruthLine {
    QString text;
    QList<TruthWord> words;
    QRectF box;
};
struct Sample {
    QImage image;
    QList<TruthLine> lines;
};

const QStringList kEnglish = {
    QStringLiteral("The quick brown fox jumps over the lazy dog"),
    QStringLiteral("Settings Preferences Window Help 2024"),
    QStringLiteral("Open the file in a new tab with Ctrl+T"),
    QStringLiteral("Downloads are saved to your home folder"),
};
const QStringList kRussian = {
    QStringLiteral("Съешь же ещё этих мягких французских булок"),
    QStringLiteral("Настройки системы и параметры экрана"),
    QStringLiteral("Открыть файл в новой вкладке"),
    QStringLiteral("Загрузки сохраняются в домашнюю папку"),
};

QFont uiFont(int px, const QString &family = QStringLiteral("DejaVu Sans"))
{
    QFont f(family);
    f.setPixelSize(px);
    f.setHintingPreference(QFont::PreferDefaultHinting);
    return f;
}

// Render `texts` as UI-like lines and record where each word was drawn.
// Colour schemes: dark-on-light, light-on-dark, and dim grey on dark grey
// (low contrast, like comments or inactive labels in dark editor themes).
enum Theme { Light, Dark, Dim };
const char *themeName(Theme t)
{
    return t == Light ? "light" : t == Dark ? "dark" : "dim";
}

Sample render(const QStringList &texts, const QFont &font, const QColor &bg, const QColor &fg)
{
    const int px = font.pixelSize();
    const QFontMetricsF fm(font);
    const int margin = 2 * px;
    const qreal lineStep = std::ceil(fm.height() * 1.9);
    qreal maxW = 0;
    for (const QString &t : texts)
        maxW = std::max(maxW, fm.horizontalAdvance(t));
    Sample s;
    s.image = QImage(int(maxW) + 2 * margin, int(lineStep * texts.size()) + 2 * margin, QImage::Format_RGB32);
    s.image.fill(bg);
    QPainter p(&s.image);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(fg);
    for (int i = 0; i < texts.size(); ++i) {
        const qreal baseline = margin + i * lineStep + fm.ascent();
        p.drawText(QPointF(margin, baseline), texts[i]);
        TruthLine line;
        line.text = texts[i];
        qsizetype pos = 0;
        for (const QString &w : texts[i].split(QLatin1Char(' '))) {
            const qreal x = margin + fm.horizontalAdvance(texts[i].left(pos));
            line.words << TruthWord{w, QRectF(x, baseline - fm.ascent(), fm.horizontalAdvance(w), fm.ascent() + fm.descent())};
            pos += w.size() + 1;
        }
        line.box = QRectF(margin, baseline - fm.ascent(), fm.horizontalAdvance(texts[i]), fm.ascent() + fm.descent());
        s.lines << line;
    }
    p.end();
    return s;
}

Sample render(const QStringList &texts, int px, Theme theme)
{
    return render(texts, uiFont(px),
                  theme == Light ? QColor(0xfa, 0xfa, 0xfa) : theme == Dark ? QColor(0x1e, 0x1f, 0x22) : QColor(0x25, 0x25, 0x26),
                  theme == Light ? QColor(0x20, 0x20, 0x20) : theme == Dark ? QColor(0xdc, 0xdd, 0xde) : QColor(0x85, 0x85, 0x85));
}

int editDistance(const QString &a, const QString &b)
{
    QList<int> prev(b.size() + 1), cur(b.size() + 1);
    for (qsizetype j = 0; j <= b.size(); ++j)
        prev[j] = int(j);
    for (qsizetype i = 1; i <= a.size(); ++i) {
        cur[0] = int(i);
        for (qsizetype j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

qreal iou(const QRectF &a, const QRectF &b)
{
    const QRectF i = a.intersected(b);
    if (i.isEmpty())
        return 0;
    const qreal inter = i.width() * i.height();
    return inter / (a.width() * a.height() + b.width() * b.height() - inter);
}

// Collect OCR lines whose vertical centre falls inside the truth line's band.
QList<OcrWord> wordsOnLine(const OcrResult &r, const TruthLine &t)
{
    QList<OcrWord> words;
    for (const OcrLine &l : r.lines)
        if (std::abs(l.box.center().y() - t.box.center().y()) < t.box.height() * 0.6)
            words << l.words;
    std::sort(words.begin(), words.end(), [](const OcrWord &a, const OcrWord &b) { return a.box.left() < b.box.left(); });
    return words;
}

QString joinWords(const QList<OcrWord> &words)
{
    QStringList parts;
    for (const OcrWord &w : words)
        parts << w.text;
    return parts.join(QLatin1Char(' '));
}

struct Score {
    int errors = 0;
    int chars = 0;
    double cer() const { return chars ? double(errors) / chars : 0; }
};

Score scoreResult(const OcrResult &r, const Sample &s)
{
    Score sc;
    for (const TruthLine &t : s.lines) {
        sc.errors += editDistance(joinWords(wordsOnLine(r, t)), t.text);
        sc.chars += int(t.text.size());
    }
    return sc;
}

void maybeDump(const Sample &s, const QString &name)
{
    const QString dir = qEnvironmentVariable("SELSEARCH_TEST_DUMP_DIR");
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        s.image.save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
    }
}

// A typical 1920x1080 screen: a browser-like window with tabs, an address
// bar, a navigation sidebar and an article (~2500 characters).
QImage renderTypicalScreen()
{
    QImage img(1920, 1080, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(QRect(0, 0, 1920, 84), QColor(0xde, 0xe1, 0xe6));
    p.setPen(QColor(0x20, 0x21, 0x24));
    p.setFont(uiFont(13));
    for (int i = 0; i < 5; ++i)
        p.drawText(20 + i * 240, 26, QStringLiteral("Tab %1 - Example page title").arg(i + 1));
    p.setFont(uiFont(14));
    p.drawText(120, 62, QStringLiteral("https://example.org/articles/screen-text-recognition"));
    p.setFont(uiFont(15));
    const QStringList nav = {QStringLiteral("Home"), QStringLiteral("Getting started"), QStringLiteral("Installation"),
                             QStringLiteral("Configuration"), QStringLiteral("Keyboard shortcuts"),
                             QStringLiteral("Troubleshooting"), QStringLiteral("Release notes"), QStringLiteral("About")};
    for (int i = 0; i < nav.size(); ++i)
        p.drawText(40, 150 + i * 36, nav[i]);
    p.setFont(uiFont(30));
    p.drawText(420, 160, QStringLiteral("Recognising text on the screen"));
    p.setFont(uiFont(16));
    p.setPen(QColor(0x33, 0x33, 0x33));
    const QString para = QStringLiteral(
        "Screen text is rendered crisply at small sizes, which makes it easier to read than photos of documents, "
        "but the fonts are tiny and the backgrounds vary from white pages to dark editor themes and toolbars.");
    for (int i = 0; i < 20; ++i)
        p.drawText(420, 220 + i * 30, para.mid((i * 37) % 120, 95));
    p.setFont(uiFont(13));
    p.setPen(QColor(0x66, 0x66, 0x66));
    p.drawText(420, 1040, QStringLiteral("Last updated 12 October 2026 - Licensed under CC BY-SA 4.0"));
    p.end();
    return img;
}

// A very busy 1920x1080 "desktop" (~7500 characters): dark sidebar, light
// content with paragraphs in two columns, a menu bar, a table.
QImage renderDenseScreen()
{
    QImage img(1920, 1080, QImage::Format_RGB32);
    img.fill(QColor(0xf5, 0xf5, 0xf5));
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(QRect(0, 0, 1920, 40), QColor(0x2b, 0x2d, 0x30));
    p.fillRect(QRect(0, 40, 300, 1040), QColor(0x1e, 0x1f, 0x22));
    p.setFont(uiFont(14));
    p.setPen(QColor(0xdd, 0xdd, 0xdd));
    const QStringList menu = {QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View"),
                              QStringLiteral("Selection"), QStringLiteral("Go"), QStringLiteral("Run"),
                              QStringLiteral("Terminal"), QStringLiteral("Help")};
    int x = 16;
    for (const QString &m : menu) {
        p.drawText(x, 26, m);
        x += QFontMetrics(p.font()).horizontalAdvance(m) + 24;
    }
    for (int i = 0; i < 40; ++i)
        p.drawText(20, 70 + i * 25, QStringLiteral("project/src/module_%1/file_%2.cpp").arg(i % 7).arg(i));
    p.setPen(QColor(0x22, 0x22, 0x22));
    const QString para = QStringLiteral(
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore");
    for (int col = 0; col < 2; ++col)
        for (int i = 0; i < 30; ++i) {
            p.setFont(uiFont(i % 10 == 0 ? 20 : 13));
            p.drawText(330 + col * 800, 80 + i * 22, para.left(70 + (i * 7) % 30));
        }
    p.setFont(uiFont(12));
    for (int r = 0; r < 12; ++r)
        for (int c = 0; c < 6; ++c)
            p.drawText(330 + c * 260, 760 + r * 24, QStringLiteral("Cell %1:%2 value %3").arg(r).arg(c).arg(r * c * 17));
    p.end();
    return img;
}

}  // namespace

class TestPaddleOcr : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<OcrEngine> m_paddle;
    std::unique_ptr<OcrEngine> m_tesseract;
    Score m_paddleScore[2], m_tessScore[2];  // [0]=en, [1]=ru

private Q_SLOTS:
    void initTestCase()
    {
        m_paddle = createPaddleOcrEngine(OcrService::modelDir());
        m_tesseract = createTesseractOcrEngine();
    }

    // ---- model table / language choice (no models needed) ----

    void languageChoice_data()
    {
        QTest::addColumn<QStringList>("langs");
        QTest::addColumn<QString>("model");
        const QString def = QStringLiteral("PP-OCRv5_mobile_rec");
        QTest::newRow("empty") << QStringList{} << def;
        QTest::newRow("en") << QStringList{QStringLiteral("en")} << def;
        QTest::newRow("zh") << QStringList{QStringLiteral("zh")} << def;
        QTest::newRow("ja+en") << QStringList{QStringLiteral("ja"), QStringLiteral("en")} << def;
        QTest::newRow("ru+en") << QStringList{QStringLiteral("ru"), QStringLiteral("en")} << QStringLiteral("eslav_PP-OCRv5_mobile_rec");
        QTest::newRow("en+uk") << QStringList{QStringLiteral("en"), QStringLiteral("uk")} << QStringLiteral("eslav_PP-OCRv5_mobile_rec");
        QTest::newRow("bg") << QStringList{QStringLiteral("bg")} << QStringLiteral("cyrillic_PP-OCRv5_mobile_rec");
        QTest::newRow("ru+bg") << QStringList{QStringLiteral("ru"), QStringLiteral("bg")} << QStringLiteral("cyrillic_PP-OCRv5_mobile_rec");
        QTest::newRow("de+en") << QStringList{QStringLiteral("de"), QStringLiteral("en")} << QStringLiteral("latin_PP-OCRv5_mobile_rec");
        QTest::newRow("pt_BR") << QStringList{QStringLiteral("pt_BR")} << QStringLiteral("latin_PP-OCRv5_mobile_rec");
        QTest::newRow("ko") << QStringList{QStringLiteral("ko")} << QStringLiteral("korean_PP-OCRv5_mobile_rec");
        QTest::newRow("ar") << QStringList{QStringLiteral("ar")} << QStringLiteral("arabic_PP-OCRv5_mobile_rec");
        QTest::newRow("hi") << QStringList{QStringLiteral("hi")} << QStringLiteral("devanagari_PP-OCRv5_mobile_rec");
        QTest::newRow("th") << QStringList{QStringLiteral("th")} << QStringLiteral("th_PP-OCRv5_mobile_rec");
        QTest::newRow("el") << QStringList{QStringLiteral("el")} << QStringLiteral("el_PP-OCRv5_mobile_rec");
        QTest::newRow("ta") << QStringList{QStringLiteral("ta")} << QStringLiteral("ta_PP-OCRv5_mobile_rec");
        QTest::newRow("te") << QStringList{QStringLiteral("te")} << QStringLiteral("te_PP-OCRv5_mobile_rec");
        QTest::newRow("ru+ja") << QStringList{QStringLiteral("ru"), QStringLiteral("ja")} << QStringLiteral("eslav_PP-OCRv5_mobile_rec");
        QTest::newRow("ja+ru") << QStringList{QStringLiteral("ja"), QStringLiteral("ru")} << def;
        QTest::newRow("unknown") << QStringList{QStringLiteral("xx")} << def;
    }

    void languageChoice()
    {
        QFETCH(QStringList, langs);
        QFETCH(QString, model);
        QCOMPARE(ModelStore::bestRecognitionModel(langs), model);
    }

    void languageChoiceAmongPresent()
    {
        const QStringList present{QStringLiteral("PP-OCRv5_mobile_rec"), QStringLiteral("cyrillic_PP-OCRv5_mobile_rec")};
        QCOMPARE(ModelStore::bestRecognitionModel({QStringLiteral("ru")}, present), present[1]);
        QCOMPARE(ModelStore::bestRecognitionModel({QStringLiteral("de")}, present), present[0]);
    }

    void specTableSane()
    {
        QSet<QString> ids;
        for (const ModelSpec &s : ModelStore::defaultSpecs()) {
            QVERIFY(!ids.contains(s.id));
            ids << s.id;
            QVERIFY(s.onnxFile().endsWith(QStringLiteral(".onnx")));
            QCOMPARE(s.license, QStringLiteral("Apache-2.0"));
            for (const ModelFile &f : s.files) {
                QCOMPARE(f.sha256.size(), 64);
                QVERIFY(f.size > 0);
                QVERIFY(f.url.startsWith(QStringLiteral("https://")));
            }
            if (s.kind == ModelSpec::Recognition)
                QCOMPARE(s.files.size(), 2);  // graph + dictionary yml
        }
        QVERIFY(ModelStore::findSpec(ModelStore::detectionModelId()));
    }

    void missingAndEnsureFromLocalFiles()
    {
        QTemporaryDir src, dst;
        auto makeFile = [&](const QString &name, const QByteArray &data) -> ModelFile {
            QFile f(src.filePath(name));
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
                qFatal("cannot write %s", qPrintable(f.fileName()));
            ModelFile mf{name, QUrl::fromLocalFile(f.fileName()).toString(),
                         QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(), data.size()};
            return mf;
        };
        ModelSpec det{QStringLiteral("det"), ModelSpec::Detection, {}, {}, QStringLiteral("Apache-2.0")};
        ModelSpec rec{QStringLiteral("rec"), ModelSpec::Recognition, {}, {QStringLiteral("en")}, QStringLiteral("Apache-2.0")};
        ModelSpec ru{QStringLiteral("ru"), ModelSpec::Recognition, {}, {QStringLiteral("ru")}, QStringLiteral("Apache-2.0")};
        det.files << makeFile(QStringLiteral("det.onnx"), QByteArray(5000, 'd'));
        rec.files << makeFile(QStringLiteral("rec.onnx"), QByteArray(7000, 'r'))
                  << makeFile(QStringLiteral("rec.yml"), QByteArray("character_dict:\n- a\n"));
        ru.files << makeFile(QStringLiteral("ru.onnx"), QByteArray(3000, 'u'));
        ru.files[0].sha256 = QByteArray(64, '0');  // deliberately wrong

        ModelStore store(dst.path(), {det, rec, ru});
        QCOMPARE(store.missingFor({QStringLiteral("en")}), (QStringList{QStringLiteral("det"), QStringLiteral("rec")}));
        QVERIFY(!store.haveModelsFor({QStringLiteral("en")}));

        {
            QSignalSpy done(&store, &ModelStore::finished);
            QSignalSpy failed(&store, &ModelStore::failed);
            QSignalSpy progress(&store, &ModelStore::progress);
            store.ensure({QStringLiteral("en")});
            QVERIFY(done.wait(5000));
            QCOMPARE(failed.count(), 0);
            QVERIFY(progress.count() > 0);
            QCOMPARE(progress.last().at(0).toLongLong(), 5000 + 7000 + 20);
            QVERIFY(store.haveModelsFor({QStringLiteral("en")}));
            QVERIFY(!QFile::exists(dst.filePath(QStringLiteral("rec.onnx.part"))));
        }
        {
            QSignalSpy done(&store, &ModelStore::finished);
            store.ensure({QStringLiteral("en")});  // nothing to do
            QVERIFY(done.wait(1000));
        }
        {
            QSignalSpy done(&store, &ModelStore::finished);
            QSignalSpy failed(&store, &ModelStore::failed);
            store.ensure({QStringLiteral("ru")});
            QVERIFY(failed.wait(5000));
            QVERIFY(failed.first().first().toString().contains(QStringLiteral("checksum")));
            QCOMPARE(done.count(), 0);
            QVERIFY(!QFile::exists(dst.filePath(QStringLiteral("ru.onnx"))));
            QVERIFY(!QFile::exists(dst.filePath(QStringLiteral("ru.onnx.part"))));
            QCOMPARE(store.missingFor({QStringLiteral("ru")}), QStringList{QStringLiteral("ru")});
        }
    }

    void engineUnavailableWithoutModels()
    {
        QTemporaryDir empty;
        auto engine = createPaddleOcrEngine(empty.path());
        if (!engine)
            QSKIP("built without ONNX Runtime");
        QVERIFY(!engine->isAvailable());
        const OcrResult r = engine->recognize(QImage(64, 64, QImage::Format_RGB32), {QStringLiteral("en")});
        QVERIFY(!r.ok());
        QVERIFY(QDir(empty.path()).isEmpty());  // never downloads
    }

    // ---- recognition (needs models) ----

    void recognize_data()
    {
        QTest::addColumn<bool>("russian");
        QTest::addColumn<int>("theme");
        QTest::addColumn<int>("px");
        for (bool ru : {false, true})
            for (Theme theme : {Light, Dark, Dim})
                for (int px : {12, 13, 14, 16, 24})
                    QTest::addRow("%s-%s-%dpx", ru ? "ru" : "en", themeName(theme), px) << ru << int(theme) << px;
    }

    void recognize()
    {
        QFETCH(bool, russian);
        QFETCH(int, theme);
        QFETCH(int, px);
        if (!m_paddle)
            QSKIP("built without ONNX Runtime");
        const QStringList langs = russian ? QStringList{QStringLiteral("ru"), QStringLiteral("en")} : QStringList{QStringLiteral("en")};
        if (!m_paddle->isAvailable() || !ModelStore(OcrService::modelDir(), ModelStore::defaultSpecs()).haveModelsFor(langs))
            QSKIP("PP-OCRv5 models not present; set SELSEARCH_MODEL_DIR");

        const Sample s = render(russian ? kRussian : kEnglish, px, Theme(theme));
        maybeDump(s, QStringLiteral("%1-%2-%3px").arg(QLatin1String(russian ? "ru" : "en"), QLatin1String(themeName(Theme(theme)))).arg(px));
        const OcrResult r = m_paddle->recognize(s.image, langs);
        QVERIFY2(r.ok(), qPrintable(r.error));

        const Score sc = scoreResult(r, s);
        m_paddleScore[russian] .errors += sc.errors;
        m_paddleScore[russian].chars += sc.chars;
        if (m_tesseract && m_tesseract->isAvailable()) {
            const Score ts = scoreResult(m_tesseract->recognize(s.image, langs), s);
            m_tessScore[russian].errors += ts.errors;
            m_tessScore[russian].chars += ts.chars;
        }

        for (const TruthLine &t : s.lines) {
            const QList<OcrWord> words = wordsOnLine(r, t);
            const QString got = joinWords(words);
            const int dist = editDistance(got, t.text);
            QVERIFY2(dist <= std::max<qsizetype>(2, t.text.size() / 12),
                     qPrintable(QStringLiteral("expected \"%1\" got \"%2\" (distance %3)").arg(t.text, got).arg(dist)));
            // Word boxes: the first three words must line up with where they
            // were drawn.
            for (int i = 0; i < 3 && i < t.words.size(); ++i) {
                const TruthWord &tw = t.words[i];
                auto best = std::max_element(words.begin(), words.end(), [&](const OcrWord &a, const OcrWord &b) {
                    return iou(a.box, tw.box) < iou(b.box, tw.box);
                });
                QVERIFY(best != words.end());
                const qreal v = iou(best->box, tw.box);
                QVERIFY2(v > 0.5, qPrintable(QStringLiteral("word \"%1\" truth (%2,%3 %4x%5) got \"%6\" (%7,%8 %9x%10) IoU %11")
                                                 .arg(tw.text)
                                                 .arg(tw.box.x())
                                                 .arg(tw.box.y())
                                                 .arg(tw.box.width())
                                                 .arg(tw.box.height())
                                                 .arg(best->text)
                                                 .arg(best->box.x())
                                                 .arg(best->box.y())
                                                 .arg(best->box.width())
                                                 .arg(best->box.height())
                                                 .arg(v)));
            }
        }
    }

    void compareWithTesseract()
    {
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        for (int lang : {0, 1}) {
            const char *name = lang ? "ru" : "en";
            qInfo("CER %s: paddle %.2f%% (%d/%d)  tesseract %s", name, 100 * m_paddleScore[lang].cer(),
                  m_paddleScore[lang].errors, m_paddleScore[lang].chars,
                  m_tessScore[lang].chars ? qPrintable(QStringLiteral("%1% (%2/%3)")
                                                           .arg(100 * m_tessScore[lang].cer(), 0, 'f', 2)
                                                           .arg(m_tessScore[lang].errors)
                                                           .arg(m_tessScore[lang].chars))
                                          : "n/a");
        }
    }

    // Harder material, reported rather than asserted line by line: tiny
    // sizes, serif and monospace faces, coloured buttons and banners.
    void compareHard()
    {
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        struct Colors {
            QColor bg, fg;
        };
        const QList<Colors> colors = {{QColor(0xff, 0xff, 0xff), QColor(0x55, 0x55, 0x55)},
                                      {QColor(0x0a, 0x64, 0xd8), QColor(0xff, 0xff, 0xff)},
                                      {QColor(0xff, 0xe0, 0x82), QColor(0x3a, 0x2a, 0x00)},
                                      {QColor(0x1e, 0x1e, 0x1e), QColor(0x6a, 0x99, 0x55)}};
        const QStringList families = {QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Serif"),
                                      QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Sans")};
        for (int ru : {0, 1}) {
            const QStringList langs = ru ? QStringList{QStringLiteral("ru"), QStringLiteral("en")} : QStringList{QStringLiteral("en")};
            if (!ModelStore(OcrService::modelDir(), ModelStore::defaultSpecs()).haveModelsFor(langs))
                continue;
            Score paddle, tess;
            for (int px : {10, 11, 12})
                for (int f = 0; f < families.size(); ++f) {
                    const Colors &c = colors[(px + f) % colors.size()];
                    const Sample s = render(ru ? kRussian : kEnglish, uiFont(px, families[f]), c.bg, c.fg);
                    maybeDump(s, QStringLiteral("hard-%1-%2px-%3").arg(QLatin1String(ru ? "ru" : "en")).arg(px).arg(f));
                    const Score a = scoreResult(m_paddle->recognize(s.image, langs), s);
                    paddle.errors += a.errors;
                    paddle.chars += a.chars;
                    if (m_tesseract && m_tesseract->isAvailable()) {
                        const Score b = scoreResult(m_tesseract->recognize(s.image, langs), s);
                        tess.errors += b.errors;
                        tess.chars += b.chars;
                    }
                }
            qInfo("hard set %s: paddle CER %.2f%% (%d/%d), tesseract CER %.2f%% (%d/%d)", ru ? "ru" : "en", 100 * paddle.cer(),
                  paddle.errors, paddle.chars, 100 * tess.cer(), tess.errors, tess.chars);
            QVERIFY(paddle.cer() < 0.05);
        }
    }

    void fullScreenTiming_data()
    {
        QTest::addColumn<bool>("dense");
        QTest::newRow("typical") << false;
        QTest::newRow("dense") << true;
    }

    // Reports timings; the assertions only check that the text came out.
    void fullScreenTiming()
    {
        QFETCH(bool, dense);
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        const QImage screen = dense ? renderDenseScreen() : renderTypicalScreen();
        if (const QString dir = qEnvironmentVariable("SELSEARCH_TEST_DUMP_DIR"); !dir.isEmpty())
            screen.save(dir + (dense ? QStringLiteral("/screen-dense.png") : QStringLiteral("/screen-typical.png")));
        m_paddle->unload();
        QElapsedTimer t;
        t.start();
        OcrResult r = m_paddle->recognize(screen, {QStringLiteral("en")});
        const qint64 cold = t.restart();
        r = m_paddle->recognize(screen, {QStringLiteral("en")});
        const qint64 warm = t.elapsed();
        QVERIFY2(r.ok(), qPrintable(r.error));
        qInfo("1920x1080 %s: cold %lld ms (incl. model load), warm %lld ms, %d lines, %d chars", dense ? "dense" : "typical",
              cold, warm, int(r.lines.size()), int(r.text().size()));
        if (dense) {
            QVERIFY(r.lines.size() > 150);
            QVERIFY(r.text().contains(QStringLiteral("Terminal")));
            QVERIFY(r.text().contains(QStringLiteral("consectetur")));
        } else {
            QVERIFY(r.text().contains(QStringLiteral("Recognising text on the screen")));
            QVERIFY(r.text().contains(QStringLiteral("Keyboard shortcuts")));
            QVERIFY(r.text().contains(QStringLiteral("https://example.org/articles/screen-text-recognition")));
        }
    }

    // Chinese/Japanese through the general model: runs without spaces become
    // one word each, Latin words separated by spaces stay separate.
    void cjk()
    {
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        if (!QFontDatabase::families().contains(QStringLiteral("WenQuanYi Zen Hei")))
            QSKIP("no CJK font");
        const QStringList texts = {QStringLiteral("打开系统设置"), QStringLiteral("文件夹 Linux 下载"),
                                   QStringLiteral("ファイルを開く"), QStringLiteral("使用Linux系统")};
        const Sample s = render(texts, uiFont(18, QStringLiteral("WenQuanYi Zen Hei")), Qt::white, Qt::black);
        maybeDump(s, QStringLiteral("cjk"));
        const OcrResult r = m_paddle->recognize(s.image, {QStringLiteral("zh"), QStringLiteral("ja")});
        QVERIFY2(r.ok(), qPrintable(r.error));
        for (const TruthLine &t : s.lines) {
            const QList<OcrWord> words = wordsOnLine(r, t);
            QCOMPARE(joinWords(words), t.text);
            QCOMPARE(words.size(), t.words.size());
            for (int i = 0; i < words.size(); ++i)
                QVERIFY2(iou(words[i].box, t.words[i].box) > 0.5, qPrintable(words[i].text));
        }
    }

    // A selection of a single word: much smaller than the detector's usual
    // input, exercises the upscaling path.
    void tinyCrop()
    {
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        Sample s = render({QStringLiteral("Preferences")}, 13, Dark);
        const QRectF word = s.lines.first().words.first().box.adjusted(-4, -3, 4, 3);
        const QImage crop = s.image.copy(word.toAlignedRect());
        const OcrResult r = m_paddle->recognize(crop, {QStringLiteral("en")});
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.text(), QStringLiteral("Preferences"));
    }

    // Slightly rotated text (photos of screens, rotated windows).
    void skewedText()
    {
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        const Sample s = render(kEnglish, 18, Light);
        QImage rotated(s.image.width() + 80, s.image.height() + 120, QImage::Format_RGB32);
        rotated.fill(QColor(0xfa, 0xfa, 0xfa));
        QPainter p(&rotated);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(40, 60);
        p.rotate(-4);
        p.drawImage(0, 40, s.image);
        p.end();
        if (const QString dir = qEnvironmentVariable("SELSEARCH_TEST_DUMP_DIR"); !dir.isEmpty())
            rotated.save(dir + QStringLiteral("/skewed.png"));
        const OcrResult r = m_paddle->recognize(rotated, {QStringLiteral("en")});
        QVERIFY2(r.ok(), qPrintable(r.error));
        int errors = 0, chars = 0;
        const QStringList got = r.text().split(QLatin1Char('\n'));
        for (const QString &t : kEnglish) {
            int best = int(t.size());
            for (const QString &g : got)
                best = std::min(best, editDistance(g, t));
            errors += best;
            chars += int(t.size());
        }
        qInfo("skewed 4 degrees: %d/%d character errors", errors, chars);
        QVERIFY2(errors <= chars / 20, qPrintable(r.text()));
    }

    void unloadAndReload()
    {
        if (!m_paddle || !m_paddle->isAvailable())
            QSKIP("PP-OCRv5 models not present");
        const Sample s = render({QStringLiteral("Reload works")}, 16, Light);
        m_paddle->unload();
        const OcrResult r = m_paddle->recognize(s.image, {QStringLiteral("en")});
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.text(), QStringLiteral("Reload works"));
    }
};

QTEST_MAIN(TestPaddleOcr)
#include "tst_paddleocr.moc"
