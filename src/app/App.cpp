// SPDX-License-Identifier: MIT
#include "App.h"

#include "core/Config.h"
#include "core/Ipc.h"
#include "ocr/OcrService.h"
#include "overlay/FrameStore.h"
#include "overlay/LensController.h"
#include "translate/Translator.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QTimer>
#include <QUrlQuery>

Q_LOGGING_CATEGORY(lcApp, "selsearch.app")

namespace selsearch {

App::App(QObject *parent)
    : QObject(parent)
    , m_config(std::make_unique<Config>())
    , m_ipc(std::make_unique<IpcServer>())
{
}

App::~App()
{
    // Windows reference the controller; tear down the UI first.
    m_lens.reset();
    m_engine.reset();
}

bool App::init(QString *error)
{
    if (!m_ipc->listen(error))
        return false;
    if (!m_config->load())
        qCWarning(lcApp) << "config error, using defaults:" << m_config->lastError();

    m_ocr = std::make_unique<OcrService>(*m_config);
    m_translator = createTranslator(*m_config);

    m_engine = std::make_unique<QQmlEngine>();
    m_frames = new FrameStore;
    m_engine->addImageProvider(QStringLiteral("frame"), m_frames);
    m_engine->addImageProvider(QStringLiteral("icon"), new IconProvider);

    m_lens = std::make_unique<LensController>(*m_config, *m_ocr);
    m_lens->setTranslator(m_translator.get());
    m_lens->setEngine(m_engine.get(), m_frames);
    connect(m_lens.get(), &LensController::openUrl, this, &App::openUrl);
    connect(m_lens.get(), &LensController::failed, this, [](const QString &msg) { qCWarning(lcApp) << msg; });

    m_ipc->setHandler([this](const QJsonObject &req, const IpcServer::Reply &reply) { handle(req, reply); });
    return true;
}

void App::openUrl(const QUrl &url)
{
    QUrl target = url;
    if (url.scheme() == QLatin1String("selsearch") && url.path() == QLatin1String("search")) {
        const QString q = QUrlQuery(url).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
        // Placeholder until the search-engine registry is wired in.
        target = QUrl(QStringLiteral("https://duckduckgo.com/?q=") + QString::fromUtf8(QUrl::toPercentEncoding(q)));
    }
    QDesktopServices::openUrl(target);
}

QJsonObject App::status() const
{
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("version"), QStringLiteral(SELSEARCH_VERSION)},
        {QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())},
        {QStringLiteral("lensActive"), m_lens->active()},
        {QStringLiteral("ocrEngines"), QJsonArray::fromStringList(m_ocr->availableEngines())},
        {QStringLiteral("config"), m_config->path()},
    };
}

void App::handle(const QJsonObject &req, const std::function<void(const QJsonObject &)> &reply)
{
    const QString cmd = req.value(QStringLiteral("cmd")).toString();
    const QJsonObject ok{{QStringLiteral("ok"), true}};
    if (cmd == QLatin1String("lens")) {
        m_lens->start(req.value(QStringLiteral("mode")).toString(QStringLiteral("region")));
        reply(ok);
    } else if (cmd == QLatin1String("status")) {
        reply(status());
    } else if (cmd == QLatin1String("reload")) {
        m_config->load(m_config->path());
        reply(ok);
    } else if (cmd == QLatin1String("quit")) {
        reply(ok);
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    } else if (cmd == QLatin1String("ocr")) {
        QImageReader reader(req.value(QStringLiteral("path")).toString());
        reader.setAutoTransform(true);
        const QImage img = reader.read();
        if (img.isNull()) {
            reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), reader.errorString()}});
            return;
        }
        QStringList langs;
        for (const auto &v : req.value(QStringLiteral("languages")).toArray())
            langs << v.toString();
        auto *watcher = new QFutureWatcher<OcrResult>(this);
        connect(watcher, &QFutureWatcher<OcrResult>::finished, this, [watcher, reply] {
            watcher->deleteLater();
            const OcrResult r = watcher->result();
            QJsonArray lines;
            for (const auto &l : r.lines) {
                QJsonArray words;
                for (const auto &w : l.words)
                    words.append(QJsonObject{{QStringLiteral("text"), w.text},
                                             {QStringLiteral("box"), QJsonArray{w.box.x(), w.box.y(), w.box.width(), w.box.height()}}});
                lines.append(QJsonObject{{QStringLiteral("text"), l.text()}, {QStringLiteral("words"), words}});
            }
            reply({{QStringLiteral("ok"), r.ok()}, {QStringLiteral("error"), r.error}, {QStringLiteral("engine"), r.engine},
                   {QStringLiteral("ms"), r.elapsedMs}, {QStringLiteral("text"), r.text()}, {QStringLiteral("lines"), lines}});
        });
        watcher->setFuture(m_ocr->recognize(img, langs));
    } else if (cmd == QLatin1String("launcher") || cmd == QLatin1String("ask") || cmd == QLatin1String("settings")) {
        reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("not implemented yet")}});
    } else {
        reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unknown command: ") + cmd}});
    }
}

}  // namespace selsearch
