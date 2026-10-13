// SPDX-License-Identifier: MIT
#include "capture/ImageTransform.h"
#include "compositor/Compositor.h"
#include "core/Config.h"
#include "ocr/OcrEngine.h"

#include <QTemporaryDir>
#include <QTest>

using namespace selsearch;

class TestCore : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void configDefaultsAndRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        Config c;
        QVERIFY(c.load(path));
        QCOMPARE(c.string(QStringLiteral("search.engine")), QStringLiteral("duckduckgo"));
        QCOMPARE(c.integer(QStringLiteral("launcher.max_results")), 8);
        c.setValue(QStringLiteral("search.engine"), QStringLiteral("google"));
        c.setValue(QStringLiteral("ocr.languages"), QStringList{QStringLiteral("ru"), QStringLiteral("en")});
        c.setValue(QStringLiteral("lens.snap_to_windows"), false);

        Config c2;
        QVERIFY(c2.load(path));
        QCOMPARE(c2.string(QStringLiteral("search.engine")), QStringLiteral("google"));
        QCOMPARE(c2.list(QStringLiteral("ocr.languages")), (QStringList{QStringLiteral("ru"), QStringLiteral("en")}));
        QCOMPARE(c2.boolean(QStringLiteral("lens.snap_to_windows")), false);
        c2.reset(QStringLiteral("search.engine"));
        QCOMPARE(c2.string(QStringLiteral("search.engine")), QStringLiteral("duckduckgo"));
    }

    void configBadFileKeepsDefaults()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is = = not toml");
        f.close();
        Config c;
        QVERIFY(!c.load(path));
        QVERIFY(!c.lastError().isEmpty());
        QCOMPARE(c.string(QStringLiteral("search.engine")), QStringLiteral("duckduckgo"));
    }

    void hyprlandParsing()
    {
        const QByteArray monitors = R"([{"id":0,"activeWorkspace":{"id":1},"specialWorkspace":{"id":0}},
                                       {"id":1,"activeWorkspace":{"id":3},"specialWorkspace":{"id":0}}])";
        const QByteArray clients = R"([
            {"at":[0,0],"size":[100,100],"title":"tiled-old","class":"a","workspace":{"id":1},"mapped":true,"hidden":false,"floating":false,"focusHistoryID":2},
            {"at":[10,10],"size":[50,50],"title":"float","class":"b","workspace":{"id":1},"mapped":true,"hidden":false,"floating":true,"focusHistoryID":3},
            {"at":[0,0],"size":[100,100],"title":"focused","class":"c","workspace":{"id":3},"mapped":true,"hidden":false,"floating":false,"focusHistoryID":0},
            {"at":[0,0],"size":[100,100],"title":"other-ws","class":"d","workspace":{"id":2},"mapped":true,"hidden":false,"floating":false,"focusHistoryID":1}
        ])";
        const auto w = parseHyprlandWindows(clients, monitors);
        QCOMPARE(w.size(), 3);
        QCOMPARE(w[0].title, QStringLiteral("float"));
        QCOMPARE(w[1].title, QStringLiteral("focused"));
        QVERIFY(w[1].focused);
        QCOMPARE(w[2].title, QStringLiteral("tiled-old"));
        QCOMPARE(w[0].geometry, QRect(10, 10, 50, 50));
    }

    void swayParsing()
    {
        const QByteArray tree = R"({"type":"root","nodes":[{"type":"output","nodes":[{"type":"workspace","nodes":[
            {"type":"con","pid":1,"app_id":"foot","name":"term","visible":true,"focused":true,"rect":{"x":0,"y":0,"width":800,"height":600},"nodes":[]},
            {"type":"con","pid":2,"app_id":"hidden","name":"h","visible":false,"rect":{"x":0,"y":0,"width":800,"height":600},"nodes":[]}
        ],"floating_nodes":[
            {"type":"floating_con","pid":3,"app_id":"pop","name":"popup","visible":true,"rect":{"x":100,"y":100,"width":200,"height":100},"nodes":[]}
        ]}]}]})";
        const auto w = parseSwayTree(tree);
        QCOMPARE(w.size(), 2);
        QCOMPARE(w[0].appId, QStringLiteral("pop"));
        QVERIFY(w[0].floating);
        QCOMPARE(w[1].title, QStringLiteral("term"));
        QVERIFY(w[1].focused);
    }

    void outputTransform()
    {
        QImage img(4, 2, QImage::Format_RGB32);
        img.fill(Qt::black);
        img.setPixel(0, 0, qRgb(255, 0, 0));  // marker top-left
        QCOMPARE(applyOutputTransform(img, 0, false).size(), QSize(4, 2));
        QCOMPARE(applyOutputTransform(img, 1, false).size(), QSize(2, 4));
        QCOMPARE(applyOutputTransform(img, 0, true).pixel(0, 1), qRgb(255, 0, 0));
    }

    void readingOrder()
    {
        auto line = [](qreal x, qreal y, const char *t) {
            OcrLine l;
            l.words << OcrWord{QString::fromUtf8(t), QRectF(x, y, 40, 10)};
            return l;
        };
        QList<OcrLine> lines{line(200, 51, "right"), line(0, 100, "second"), line(0, 50, "left")};
        sortReadingOrder(lines);
        QCOMPARE(lines[0].text(), QStringLiteral("left"));
        QCOMPARE(lines[1].text(), QStringLiteral("right"));
        QCOMPARE(lines[2].text(), QStringLiteral("second"));
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
