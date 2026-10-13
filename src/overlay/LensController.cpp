// SPDX-License-Identifier: MIT
#include "LensController.h"

#include "FrameStore.h"
#include "compositor/Compositor.h"
#include "core/Config.h"
#include "ocr/OcrService.h"
#include "translate/Translator.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <LayerShellQt/Window>

#include <algorithm>

Q_LOGGING_CATEGORY(lcLens, "selsearch.lens")

namespace selsearch {

namespace {

struct SessionGrab {
    CaptureResult capture;
    QHash<QString, QImage> blurred;
    QList<WindowInfo> windows;
};

// Median colour of the pixels just outside `box`: the background behind the
// text, used to paint translations over the original.
QColor backgroundAround(const QImage &img, const QRect &box)
{
    std::vector<int> r, g, b;
    auto sample = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= img.width() || y >= img.height())
            return;
        const QRgb p = img.pixel(x, y);
        r.push_back(qRed(p));
        g.push_back(qGreen(p));
        b.push_back(qBlue(p));
    };
    const QRect o = box.adjusted(-2, -2, 2, 2);
    const int step = std::max(1, o.width() / 40);
    for (int x = o.left(); x <= o.right(); x += step) {
        sample(x, o.top());
        sample(x, o.bottom());
    }
    for (int y = o.top(); y <= o.bottom(); y += std::max(1, o.height() / 8)) {
        sample(o.left(), y);
        sample(o.right(), y);
    }
    if (r.empty())
        return Qt::white;
    auto median = [](std::vector<int> &v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    return QColor(median(r), median(g), median(b));
}

}  // namespace

// ---- LensScreen ------------------------------------------------------------

LensScreen::LensScreen(QScreen *screen, QObject *parent)
    : QObject(parent)
    , m_screen(screen)
    , m_name(screen->name())
{
}

QString LensScreen::frameUrl() const
{
    return QStringLiteral("image://frame/%1/%2").arg(m_name).arg(m_generation);
}

QString LensScreen::blurUrl() const
{
    return QStringLiteral("image://frame/%1/%2/blur").arg(m_name).arg(m_generation);
}

qreal LensScreen::toLogical() const
{
    if (m_frameSize.isEmpty())
        return 1.0;
    return qreal(m_screen->geometry().width()) / m_frameSize.width();
}

// ---- LensController --------------------------------------------------------

LensController::LensController(Config &config, OcrService &ocr, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_ocr(ocr)
    , m_compositor(createCompositor())
{
    connect(&m_config, &Config::changed, this, &LensController::settingsChanged);
    connect(&m_config, &Config::reloaded, this, &LensController::settingsChanged);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &LensController::rebuildScreens);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &LensController::rebuildScreens);
}

LensController::~LensController()
{
    qDeleteAll(m_screens);
}

void LensController::setEngine(QQmlEngine *engine, FrameStore *frames)
{
    m_engine = engine;
    m_frames = frames;
    rebuildScreens();
}

QString LensController::theme() const
{
    return m_config.string(QStringLiteral("general.theme"));
}

QColor LensController::accent() const
{
    const QColor c(m_config.string(QStringLiteral("general.accent")));
    return c.isValid() ? c : QColor(QStringLiteral("#8e9bff"));
}

bool LensController::recognizeText() const { return m_config.boolean(QStringLiteral("lens.recognize_text")); }
bool LensController::snapToWindows() const { return m_config.boolean(QStringLiteral("lens.snap_to_windows")); }
bool LensController::showHints() const { return m_config.boolean(QStringLiteral("lens.show_hints")); }
QString LensController::onSelect() const { return m_config.string(QStringLiteral("lens.on_select")); }

QString LensController::searchEngineName() const
{
    return m_config.string(QStringLiteral("search.engine"));
}

QString LensController::imageEngineName() const
{
    return m_config.string(QStringLiteral("search.image_engine"));
}

QString LensController::translateTarget() const
{
    QString t = m_config.string(QStringLiteral("translate.target"));
    if (t.isEmpty())
        t = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
    if (t.isEmpty() || t == QLatin1String("C"))
        t = QStringLiteral("en");
    return t;
}

void LensController::setActiveScreen(const QString &name)
{
    if (name == m_activeScreen)
        return;
    m_activeScreen = name;
    Q_EMIT activeScreenChanged();
}

LensScreen *LensController::find(const QString &name) const
{
    for (auto *s : m_screens)
        if (s->name() == name)
            return s;
    return nullptr;
}

void LensController::rebuildScreens()
{
    if (!m_engine)
        return;
    if (m_active)
        close();
    for (auto *s : std::as_const(m_screens)) {
        if (s->window)
            s->window->deleteLater();
        s->deleteLater();
    }
    m_screens.clear();
    for (QScreen *screen : QGuiApplication::screens()) {
        auto *s = new LensScreen(screen, this);
        m_screens << s;
        createWindow(s);
    }
}

void LensController::createWindow(LensScreen *s)
{
    QQmlComponent component(m_engine, QUrl(QStringLiteral("qrc:/qml/Overlay.qml")));
    QObject *obj = component.createWithInitialProperties(
        {{QStringLiteral("lens"), QVariant::fromValue(this)}, {QStringLiteral("lensScreen"), QVariant::fromValue(s)}});
    auto *window = qobject_cast<QQuickWindow *>(obj);
    if (!window) {
        qCWarning(lcLens) << "failed to create overlay:" << component.errorString();
        delete obj;
        return;
    }
    window->setScreen(s->screen());
    window->setGeometry(s->screen()->geometry());
    if (auto *layer = LayerShellQt::Window::get(window)) {
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layer->setScope(QStringLiteral("selsearch-lens"));
    }
    s->window = window;
}

void LensController::start(const QString &mode)
{
    if (m_active) {
        close();
        return;
    }
    if (m_starting)
        return;
    if (!m_capture) {
        m_capture = createCaptureBackend(qEnvironmentVariable("SELSEARCH_CAPTURE"));
        if (!m_capture) {
            Q_EMIT failed(QStringLiteral("No screen capture method available on this desktop."));
            return;
        }
    }
    m_starting = true;
    m_mode = mode;
    QList<ScreenLayout> layout;
    for (auto *s : std::as_const(m_screens))
        layout << ScreenLayout{s->name(), s->screen()->geometry(), s->screen()->devicePixelRatio()};

    CaptureBackend *backend = m_capture.get();
    Compositor *compositor = m_compositor.get();
    const bool snap = snapToWindows();
    auto future = QtConcurrent::run([backend, compositor, layout, snap] {
        SessionGrab g;
        g.capture = backend->grabAll(layout);
        // Blur in parallel with window-list IPC; both are cheap.
        for (const auto &o : std::as_const(g.capture.outputs))
            g.blurred.insert(o.name, FrameStore::makeBlurred(o.image));
        if (compositor && snap)
            g.windows = compositor->visibleWindows();
        return g;
    });
    auto *watcher = new QFutureWatcher<SessionGrab>(this);
    connect(watcher, &QFutureWatcher<SessionGrab>::finished, this, [this, watcher] {
        watcher->deleteLater();
        m_starting = false;
        const SessionGrab g = watcher->result();
        if (!g.capture.ok()) {
            Q_EMIT failed(QStringLiteral("Screen capture failed: %1").arg(g.capture.error));
            return;
        }
        qCDebug(lcLens) << "captured with" << g.capture.backend << "in" << g.capture.elapsedMs << "ms";
        ++m_generation;
        m_frames->clear();
        QString focusedScreen;
        for (auto *s : std::as_const(m_screens)) {
            QImage frame;
            for (const auto &o : g.capture.outputs)
                if (o.name == s->name() || (g.capture.outputs.size() == 1 && m_screens.size() == 1))
                    frame = o.image;
            QImage blurred = g.blurred.value(s->name());
            if (frame.isNull()) {
                // Unknown output (e.g. portal without names): show black, still usable.
                frame = QImage(s->screen()->geometry().size(), QImage::Format_RGB32);
                frame.fill(Qt::black);
                blurred = frame;
            }
            m_frames->set(s->name(), frame, blurred);
            s->m_generation = m_generation;
            s->m_frameSize = frame.size();
            s->m_words.clear();
            s->m_translations.clear();
            s->m_translating = false;

            const QRect geo = s->screen()->geometry();
            QVariantList wins;
            for (const auto &w : g.windows) {
                const QRect local = w.geometry.intersected(geo).translated(-geo.topLeft());
                if (local.width() < 8 || local.height() < 8)
                    continue;
                wins << QVariantMap{{QStringLiteral("x"), local.x()}, {QStringLiteral("y"), local.y()},
                                    {QStringLiteral("w"), local.width()}, {QStringLiteral("h"), local.height()},
                                    {QStringLiteral("title"), w.title}, {QStringLiteral("appId"), w.appId}};
                if (w.focused && focusedScreen.isEmpty() && geo.contains(w.geometry.center()))
                    focusedScreen = s->name();
            }
            s->m_windows = wins;
            Q_EMIT s->frameChanged();
            Q_EMIT s->wordsChanged();
            Q_EMIT s->translationsChanged();
            Q_EMIT s->translatingChanged();
            Q_EMIT s->windowsChanged();
            Q_EMIT s->reset();
        }
        if (focusedScreen.isEmpty() && !m_screens.isEmpty()) {
            QScreen *primary = QGuiApplication::primaryScreen();
            focusedScreen = primary ? primary->name() : m_screens.first()->name();
        }
        for (auto *s : std::as_const(m_screens)) {
            const bool f = s->name() == focusedScreen;
            if (s->m_focused != f) {
                s->m_focused = f;
                Q_EMIT s->focusedChanged();
            }
        }
        setActiveScreen(focusedScreen);
        m_active = true;
        Q_EMIT activeChanged();
        showWindows();
        if (recognizeText()) {
            // Focused screen first; the worker runs them in order.
            if (auto *f = find(focusedScreen))
                runOcr(f);
            for (auto *s : std::as_const(m_screens))
                if (s->name() != focusedScreen)
                    runOcr(s);
        }
    });
    watcher->setFuture(future);
}

void LensController::showWindows()
{
    for (auto *s : std::as_const(m_screens)) {
        if (!s->window)
            continue;
        if (auto *layer = LayerShellQt::Window::get(s->window))
            layer->setKeyboardInteractivity(s->focused() ? LayerShellQt::Window::KeyboardInteractivityExclusive
                                                         : LayerShellQt::Window::KeyboardInteractivityOnDemand);
        s->window->show();
        if (s->focused())
            s->window->requestActivate();
    }
}

void LensController::hideWindows()
{
    for (auto *s : std::as_const(m_screens))
        if (s->window)
            s->window->hide();
}

void LensController::close()
{
    if (!m_active)
        return;
    m_active = false;
    hideWindows();
    Q_EMIT activeChanged();
    // Free the frames once the windows are gone; keep memory low while idle.
    QTimer::singleShot(0, this, [this] {
        if (m_active)
            return;
        m_frames->clear();
        ++m_generation;
        for (auto *s : std::as_const(m_screens)) {
            s->m_generation = m_generation;  // a fresh URL so the next session reloads
            s->m_words.clear();
            s->m_translations.clear();
            Q_EMIT s->frameChanged();
            Q_EMIT s->wordsChanged();
            Q_EMIT s->translationsChanged();
        }
        if (m_engine)
            m_engine->trimComponentCache();
    });
}

void LensController::runOcr(LensScreen *s)
{
    const QImage frame = m_frames->frame(s->name());
    if (frame.isNull())
        return;
    s->m_ocrBusy = true;
    Q_EMIT s->ocrBusyChanged();
    const int generation = s->m_generation;
    auto *watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this, [this, s, watcher, generation] {
        watcher->deleteLater();
        if (!m_screens.contains(s) || s->m_generation != generation)
            return;  // the session ended or restarted meanwhile
        const OcrResult r = watcher->result();
        s->m_ocrBusy = false;
        Q_EMIT s->ocrBusyChanged();
        if (!r.ok()) {
            qCWarning(lcLens) << "OCR failed:" << r.error;
            return;
        }
        qCDebug(lcLens) << "OCR" << s->name() << r.engine << r.elapsedMs << "ms," << r.lines.size() << "lines";
        const qreal f = s->toLogical();
        QVariantList words;
        int index = 0;
        for (int li = 0; li < r.lines.size(); ++li) {
            for (const auto &w : r.lines[li].words) {
                words << QVariantMap{{QStringLiteral("x"), w.box.x() * f}, {QStringLiteral("y"), w.box.y() * f},
                                     {QStringLiteral("w"), w.box.width() * f}, {QStringLiteral("h"), w.box.height() * f},
                                     {QStringLiteral("text"), w.text}, {QStringLiteral("line"), li},
                                     {QStringLiteral("index"), index++}};
            }
        }
        s->m_words = words;
        Q_EMIT s->wordsChanged();
    });
    watcher->setFuture(m_ocr.recognize(frame));
}

QImage LensController::crop(const QString &screen, const QRectF &rect) const
{
    LensScreen *s = find(screen);
    if (!s)
        return {};
    const QImage frame = m_frames->frame(screen);
    const qreal f = 1.0 / s->toLogical();
    const QRect phys = QRectF(rect.x() * f, rect.y() * f, rect.width() * f, rect.height() * f).toAlignedRect();
    return frame.copy(phys.intersected(frame.rect()));
}

void LensController::copyImage(const QString &screen, const QRectF &rect)
{
    const QImage img = crop(screen, rect);
    if (img.isNull())
        return;
    QGuiApplication::clipboard()->setImage(img);
    Q_EMIT notify(tr("Image copied"));
}

void LensController::copyText(const QString &text)
{
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    Q_EMIT notify(tr("Text copied"));
}

void LensController::saveImage(const QString &screen, const QRectF &rect)
{
    const QImage img = crop(screen, rect);
    if (img.isNull())
        return;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    dir += QStringLiteral("/Selsearch");
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/selsearch-%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    if (img.save(path))
        Q_EMIT notify(tr("Saved to %1").arg(QDir(QDir::homePath()).relativeFilePath(path).prepend(QStringLiteral("~/"))));
    else
        Q_EMIT notify(tr("Could not save image"));
}

void LensController::searchText(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    // Resolved by the app with the configured engine.
    Q_EMIT openUrl(QUrl(QStringLiteral("selsearch:search?q=") + QString::fromUtf8(QUrl::toPercentEncoding(text))));
    close();
}

void LensController::searchImage(const QString &screen, const QRectF &rect)
{
    const QImage img = crop(screen, rect);
    if (img.isNull())
        return;
    Q_EMIT askRequested(img, QStringLiteral("selsearch:image-search"));
    close();
}

void LensController::ask(const QString &screen, const QRectF &rect, const QString &selectedText)
{
    const QImage img = rect.isEmpty() ? QImage() : crop(screen, rect);
    Q_EMIT askRequested(img, selectedText);
    close();
}

void LensController::translateRegion(const QString &screen, const QRectF &rect)
{
    LensScreen *s = find(screen);
    if (!s)
        return;
    if (!m_translator || !m_translator->isAvailable()) {
        Q_EMIT notify(tr("No translator available. Install translateLocally or set one up in Settings."));
        return;
    }
    // Lines of words inside the region, each translated as one segment.
    QMap<int, QPair<QRectF, QStringList>> lines;
    for (const QVariant &v : std::as_const(s->m_words)) {
        const QVariantMap w = v.toMap();
        const QRectF box(w[QStringLiteral("x")].toReal(), w[QStringLiteral("y")].toReal(), w[QStringLiteral("w")].toReal(),
                         w[QStringLiteral("h")].toReal());
        if (!rect.isEmpty() && !rect.contains(box.center()))
            continue;
        auto &line = lines[w[QStringLiteral("line")].toInt()];
        line.first = line.first.isNull() ? box : line.first.united(box);
        line.second << w[QStringLiteral("text")].toString();
    }
    if (lines.isEmpty()) {
        Q_EMIT notify(s->ocrBusy() ? tr("Still reading the text…") : tr("No text found here"));
        return;
    }
    TranslationRequest req;
    QList<QRectF> boxes;
    for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
        req.segments << it.value().second.join(QLatin1Char(' '));
        boxes << it.value().first;
    }
    req.source = QStringLiteral("auto");
    req.target = translateTarget();
    s->m_translating = true;
    Q_EMIT s->translatingChanged();
    const QImage frame = m_frames->frame(screen);
    const qreal toPhys = 1.0 / s->toLogical();
    const int generation = s->m_generation;
    QPointer<LensScreen> guard(s);
    m_translator->translate(req, [this, guard, boxes, frame, toPhys, generation](const TranslationResult &r) {
        if (!guard || guard->m_generation != generation)
            return;
        guard->m_translating = false;
        Q_EMIT guard->translatingChanged();
        if (!r.ok() || r.segments.size() != boxes.size()) {
            Q_EMIT notify(tr("Translation failed: %1").arg(r.error));
            return;
        }
        QVariantList out;
        for (int i = 0; i < boxes.size(); ++i) {
            const QRectF b = boxes[i];
            const QRect phys = QRectF(b.x() * toPhys, b.y() * toPhys, b.width() * toPhys, b.height() * toPhys).toAlignedRect();
            const QColor bg = backgroundAround(frame, phys);
            const QColor fg = bg.lightnessF() > 0.55 ? QColor(20, 20, 24) : QColor(245, 245, 248);
            out << QVariantMap{{QStringLiteral("x"), b.x()}, {QStringLiteral("y"), b.y()},
                               {QStringLiteral("w"), b.width()}, {QStringLiteral("h"), b.height()},
                               {QStringLiteral("text"), r.segments[i]}, {QStringLiteral("bg"), bg},
                               {QStringLiteral("fg"), fg}};
        }
        guard->m_translations = out;
        Q_EMIT guard->translationsChanged();
    });
}

void LensController::clearTranslations(const QString &screen)
{
    if (LensScreen *s = find(screen)) {
        s->m_translations.clear();
        Q_EMIT s->translationsChanged();
    }
}

void LensController::setTranslateTarget(const QString &code)
{
    m_config.setValue(QStringLiteral("translate.target"), code);
    QStringList recent = m_config.list(QStringLiteral("translate.recent"));
    recent.removeAll(code);
    recent.prepend(code);
    while (recent.size() > 3)
        recent.removeLast();
    m_config.setValue(QStringLiteral("translate.recent"), recent);
}

QStringList LensController::recentTranslateLanguages() const
{
    return m_config.list(QStringLiteral("translate.recent"));
}

QVariantList LensController::translateLanguages() const
{
    // Replaced by the translation module's language table once merged.
    static const QList<QPair<const char *, const char *>> langs = {
        {"en", "English"}, {"ru", "Русский"}, {"uk", "Українська"}, {"de", "Deutsch"}, {"fr", "Français"},
        {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"}, {"pl", "Polski"}, {"nl", "Nederlands"},
        {"tr", "Türkçe"}, {"ja", "日本語"}, {"zh", "中文"}, {"ko", "한국어"}, {"ar", "العربية"},
    };
    QVariantList out;
    for (const auto &l : langs)
        out << QVariantMap{{QStringLiteral("code"), QString::fromLatin1(l.first)}, {QStringLiteral("name"), QString::fromUtf8(l.second)}};
    return out;
}

}  // namespace selsearch
