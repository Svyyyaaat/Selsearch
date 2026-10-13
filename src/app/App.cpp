// SPDX-License-Identifier: MIT
#include "App.h"

#include "Launcher.h"
#include "Settings.h"
#include "WebPanel.h"
#include "search/ImageSearch.h"
#include "search/SearchEngines.h"
#include "core/Config.h"
#include "core/Ipc.h"
#include "ocr/OcrService.h"
#include "overlay/FrameStore.h"
#include "overlay/LensController.h"
#include "translate/Translator.h"

#include <QCursor>
#include <QDesktopServices>
#include <QScreen>
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
    // Windows reference the controllers; tear down the UI first.
    m_settings.reset();
    m_launcher.reset();
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
    connect(m_lens.get(), &LensController::openUrl, this, [this](const QUrl &url) { openUrl(url); });
    connect(m_lens.get(), &LensController::failed, this, [](const QString &msg) { qCWarning(lcApp) << msg; });

    m_web = std::make_unique<WebPanel>();
    connect(m_web.get(), &WebPanel::openExternal, this, [](const QUrl &url) { QDesktopServices::openUrl(url); });

    m_launcher = std::make_unique<Launcher>(*m_config);
    m_launcher->setEngine(m_engine.get());
    connect(m_launcher.get(), &Launcher::openUrl, this, &App::openUrl);
    connect(m_launcher.get(), &Launcher::lensRequested, this, [this] { m_lens->start(); });

    m_settings = std::make_unique<Settings>(*m_config, *m_ocr);
    m_settings->setEngine(m_engine.get());
    connect(m_launcher.get(), &Launcher::settingsRequested, m_settings.get(), &Settings::show);
    if (!qEnvironmentVariableIsSet("SELSEARCH_NO_WELCOME"))
        QTimer::singleShot(500, m_settings.get(), &Settings::showWelcomeIfFirstRun);

    connect(m_lens.get(), &LensController::askRequested, this, [this](const QImage &image, const QString &text) {
        if (text == QLatin1String("selsearch:image-search")) {
            ImageSearch::start(image, m_config->string(QStringLiteral("search.image_engine")), this,
                               [this](const QUrl &url, const QString &error) {
                                   if (url.isValid())
                                       openUrl(url, tr("Image search"));
                                   else
                                       qCWarning(lcApp) << "image search failed:" << error;
                               });
            return;
        }
        m_launcher->askAbout(image, text);
    });

    m_ipc->setHandler([this](const QJsonObject &req, const IpcServer::Reply &reply) { handle(req, reply); });
    return true;
}

void App::openUrl(const QUrl &url, const QString &title)
{
    QUrl target = url;
    QString pageTitle = title;
    if (url.scheme() == QLatin1String("selsearch") && url.path() == QLatin1String("search")) {
        const QString q = QUrlQuery(url).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
        target = SearchEngines::current(m_config.get()).searchUrlFor(q);
        pageTitle = q;
    }
    const bool web = target.scheme() == QLatin1String("https") || target.scheme() == QLatin1String("http")
        || (target.isLocalFile() && target.path().endsWith(QLatin1String(".html")) && target.path().contains(QLatin1String("/selsearch/")));
    if (web && m_config->string(QStringLiteral("search.open_in")) == QLatin1String("panel") && WebPanel::isAvailable()) {
        QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        const QPoint where = screen ? screen->geometry().center() : QPoint();
        m_web->open(target, pageTitle, where, m_config->boolean(QStringLiteral("search.persist_cookies")));
        return;
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
        if (m_launcher->shown())
            m_launcher->hide();
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
    } else if (cmd == QLatin1String("launcher")) {
        const QString action = req.value(QStringLiteral("action")).toString();
        if (m_lens->active())
            m_lens->close();
        if (action == QLatin1String("show"))
            m_launcher->show();
        else if (action == QLatin1String("hide"))
            m_launcher->hide();
        else
            m_launcher->toggle();
        reply(ok);
    } else if (cmd == QLatin1String("ask")) {
        m_launcher->askAbout(QImage(), req.value(QStringLiteral("text")).toString());
        reply(ok);
    } else if (cmd == QLatin1String("settings")) {
        m_settings->show();
        reply(ok);
    } else {
        reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unknown command: ") + cmd}});
    }
}

}  // namespace selsearch
