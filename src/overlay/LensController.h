// SPDX-License-Identifier: MIT
#pragma once

#include "capture/Capture.h"
#include "ocr/OcrEngine.h"

#include <QColor>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QRect>
#include <QVariantList>

#include <atomic>
#include <memory>

class QQuickWindow;
class QScreen;

namespace selsearch {

class Config;
class Compositor;
class FrameStore;
class OcrService;
class Translator;

// State of one screen during a lens session. One overlay window per screen
// binds to one of these.
class LensScreen : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString frameUrl READ frameUrl NOTIFY frameChanged)
    Q_PROPERTY(QSize frameSize READ frameSize NOTIFY frameChanged)
    Q_PROPERTY(QVariantList words READ words NOTIFY wordsChanged)
    Q_PROPERTY(QVariantList windows READ windows NOTIFY windowsChanged)
    Q_PROPERTY(bool ocrBusy READ ocrBusy NOTIFY ocrBusyChanged)
    Q_PROPERTY(bool focused READ focused NOTIFY focusedChanged)
    Q_PROPERTY(QVariantList translations READ translations NOTIFY translationsChanged)
    Q_PROPERTY(bool translating READ translating NOTIFY translatingChanged)
public:
    explicit LensScreen(QScreen *screen, QObject *parent = nullptr);

    QScreen *screen() const { return m_screen; }
    QString name() const { return m_name; }
    QString frameUrl() const;
    QSize frameSize() const { return m_frameSize; }
    QVariantList words() const { return m_words; }
    QVariantList windows() const { return m_windows; }
    bool ocrBusy() const { return m_ocrBusy; }
    bool focused() const { return m_focused; }
    QVariantList translations() const { return m_translations; }
    bool translating() const { return m_translating; }

    // Logical (window) pixels per physical frame pixel.
    qreal toLogical() const;

    QQuickWindow *window = nullptr;

Q_SIGNALS:
    void frameChanged();
    void wordsChanged();
    void windowsChanged();
    void ocrBusyChanged();
    void focusedChanged();
    void translationsChanged();
    void translatingChanged();
    void reset();  // a new session starts: QML clears its selection state

private:
    friend class LensController;
    QScreen *m_screen;
    QString m_name;
    int m_generation = 0;
    QSize m_frameSize;
    QVariantList m_words;  // [{x,y,w,h,text,line,index}] in logical window coords
    QVariantList m_windows;  // [{x,y,w,h,title,appId}] in logical window coords, topmost first
    QVariantList m_translations;  // [{x,y,w,h,text,bg,fg}]
    bool m_ocrBusy = false;
    bool m_ocrStarted = false;   // whole-screen OCR requested this session
    int m_ocrRequest = 0;        // newest request; older results are dropped
    QRectF m_ocrRegion;          // region of the last region-only OCR
    bool m_focused = false;
    bool m_translating = false;
};

class LensController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY activeChanged)
    Q_PROPERTY(QString theme READ theme NOTIFY settingsChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY settingsChanged)
    Q_PROPERTY(bool darkUi READ darkUi NOTIFY settingsChanged)
    Q_PROPERTY(bool recognizeText READ recognizeText NOTIFY settingsChanged)
    Q_PROPERTY(bool snapToWindows READ snapToWindows NOTIFY settingsChanged)
    Q_PROPERTY(bool showHints READ showHints NOTIFY settingsChanged)
    Q_PROPERTY(QString onSelect READ onSelect NOTIFY settingsChanged)
    Q_PROPERTY(bool aiEnabled READ aiEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString searchEngineName READ searchEngineName NOTIFY settingsChanged)
    Q_PROPERTY(QString imageEngineName READ imageEngineName NOTIFY settingsChanged)
    Q_PROPERTY(QString translateTarget READ translateTarget NOTIFY settingsChanged)
    Q_PROPERTY(QString activeScreen READ activeScreen WRITE setActiveScreen NOTIFY activeScreenChanged)
    Q_PROPERTY(bool ocrSelectionOnly READ ocrSelectionOnly NOTIFY settingsChanged)
    Q_PROPERTY(qreal dotBrightness READ dotBrightness NOTIFY settingsChanged)
public:
    LensController(Config &config, OcrService &ocr, QObject *parent = nullptr);
    ~LensController() override;

    void setEngine(QQmlEngine *engine, FrameStore *frames);
    void setTranslator(Translator *translator) { m_translator = translator; }

    bool active() const { return m_active; }
    QString mode() const { return m_mode; }
    QString theme() const;
    QColor accent() const;
    bool darkUi() const { return true; }
    bool recognizeText() const;
    bool ocrSelectionOnly() const;
    qreal dotBrightness() const;
    bool snapToWindows() const;
    bool showHints() const;
    QString onSelect() const;
    bool aiEnabled() const;
    QString searchEngineName() const;
    QString imageEngineName() const;
    QString translateTarget() const;
    QString activeScreen() const { return m_activeScreen; }
    void setActiveScreen(const QString &name);

    // Starts a session: capture, then show one overlay per screen.
    void start(const QString &mode = QStringLiteral("region"));

    // ---- called from QML ---------------------------------------------------
    Q_INVOKABLE void close();
    Q_INVOKABLE void copyImage(const QString &screen, const QRectF &rect);
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE void saveImage(const QString &screen, const QRectF &rect);
    Q_INVOKABLE void searchText(const QString &text);
    Q_INVOKABLE void searchImage(const QString &screen, const QRectF &rect);
    Q_INVOKABLE void translateRegion(const QString &screen, const QRectF &rect);
    Q_INVOKABLE void clearTranslations(const QString &screen);
    // Opens the configured web translator (Google Translate, DeepL) with `text`.
    Q_INVOKABLE void translateOnWeb(const QString &text);
    Q_INVOKABLE void ask(const QString &screen, const QRectF &rect, const QString &selectedText);
    Q_INVOKABLE void setTranslateTarget(const QString &code);
    Q_INVOKABLE QVariantList translateLanguages() const;
    Q_INVOKABLE QStringList recentTranslateLanguages() const;
    // Selection-only OCR: read the text inside `rect` (logical) of `screen`.
    Q_INVOKABLE void recognizeRegion(const QString &screen, const QRectF &rect);
    // A key pressed on an overlay without the selection goes to the overlay
    // that has it (keyboard focus can sit on another screen's overlay).
    Q_INVOKABLE void forwardKey(int key, int modifiers) { Q_EMIT keyForwarded(key, modifiers); }

    QList<LensScreen *> screens() const { return m_screens; }

Q_SIGNALS:
    void activeChanged();
    void settingsChanged();
    void activeScreenChanged();
    void notify(const QString &message);   // snackbar text on the active overlay
    void keyForwarded(int key, int modifiers);
    void openUrl(const QUrl &url);         // handled by the app (panel or browser)
    void askRequested(const QImage &image, const QString &text);  // handled by the app
    void failed(const QString &message);

private:
    void rebuildScreens();
    void createWindow(LensScreen *s);
    void onCaptured(const CaptureResult &result);
    void runOcr(LensScreen *s, const QRectF &logicalRegion = {});
    QImage crop(const QString &screen, const QRectF &logicalRect) const;
    LensScreen *find(const QString &name) const;
    void showWindows();
    void hideWindows();

    Config &m_config;
    OcrService &m_ocr;
    Translator *m_translator = nullptr;
    QPointer<QQmlEngine> m_engine;
    FrameStore *m_frames = nullptr;
    std::unique_ptr<CaptureBackend> m_capture;
    std::unique_ptr<Compositor> m_compositor;
    QList<LensScreen *> m_screens;
    QFutureWatcher<CaptureResult> m_captureWatcher;
    bool m_active = false;
    bool m_starting = false;
    QString m_mode;
    QString m_activeScreen;
    int m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_ocrCancel;
    QString m_lastTranslateText;
};

}  // namespace selsearch
