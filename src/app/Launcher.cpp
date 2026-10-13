// SPDX-License-Identifier: MIT
#include "Launcher.h"

#include "core/Accent.h"
#include "core/Config.h"
#include "launcher/LauncherEngine.h"
#include "search/SearchEngines.h"
#include "search/SuggestionFetcher.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>

#include <LayerShellQt/Window>

Q_LOGGING_CATEGORY(lcLauncher, "selsearch.launcher")

namespace selsearch {

static const char *kSystemPrompt =
    "You are the assistant built into Selsearch, a desktop search tool. Answer concisely and "
    "accurately, using Markdown where it helps. When the user attaches an image, it is a region "
    "of their screen; text recognised in it may be included. If you are unsure, say so.";

// ---- LauncherImages ----------------------------------------------------------

LauncherImages::LauncherImages()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

void LauncherImages::setAttachment(const QImage &image)
{
    m_attachment = image;
}

QImage LauncherImages::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage img;
    const QSize want = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(64, 64);
    if (id.startsWith(QLatin1String("attach/"))) {
        img = m_attachment;
    } else if (id.startsWith(QLatin1String("icon/"))) {
        const QString name = id.mid(5);
        QIcon icon = name.startsWith(QLatin1Char('/')) ? QIcon(name) : QIcon::fromTheme(name);
        if (icon.isNull())
            icon = QIcon::fromTheme(QStringLiteral("application-x-executable"));
        img = icon.pixmap(want).toImage();
    }
    if (img.isNull()) {
        img = QImage(want, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
    }
    if (size)
        *size = img.size();
    return img;
}

// ---- Launcher ----------------------------------------------------------------

Launcher::Launcher(Config &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_engine(std::make_unique<LauncherEngine>(&config))
    , m_suggest(std::make_unique<SuggestionFetcher>(&config))
    , m_history(&config)
{
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName(QStringLiteral("hicolor"));
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));

    connect(m_engine.get(), &LauncherEngine::resultsReady, this, &Launcher::onResults);
    connect(m_suggest.get(), &SuggestionFetcher::suggestionsReady, this, [this](const QString &q, const QStringList &s) {
        if (q != m_query)
            return;
        m_suggestions = s;
        Q_EMIT suggestionsChanged();
    });
    connect(&m_config, &Config::changed, this, &Launcher::settingsChanged);
    connect(&m_config, &Config::reloaded, this, &Launcher::settingsChanged);

    connect(&m_ai, &AiClient::delta, this, [this](const QString &d) {
        m_streamText += d;
        if (!m_streamFlush.isActive())
            m_streamFlush.start();
    });
    connect(&m_ai, &AiClient::finished, this, [this](const QString &full) {
        m_streamFlush.stop();
        m_streamText = full;
        setLastAssistantText(full, false);
        m_thread.append(AiMessage{AiMessage::Assistant, full, {}});
        saveThread();
        Q_EMIT aiBusyChanged();
    });
    connect(&m_ai, &AiClient::failed, this, [this](const QString &error) {
        m_streamFlush.stop();
        if (!m_messages.isEmpty())
            m_messages.removeLast();  // drop the empty/partial assistant bubble
        appendMessage(QStringLiteral("error"), error);
        Q_EMIT aiBusyChanged();
    });
    // Stream updates are batched per frame-ish so long answers stay cheap.
    m_streamFlush.setInterval(40);
    m_streamFlush.setSingleShot(true);
    connect(&m_streamFlush, &QTimer::timeout, this, [this] { setLastAssistantText(m_streamText, true); });

    m_engine->start();
}

Launcher::~Launcher() = default;

void Launcher::setEngine(QQmlEngine *engine)
{
    m_qml = engine;
    m_images = new LauncherImages;
    engine->addImageProvider(QStringLiteral("launcher"), m_images);
    QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/qml/Launcher.qml")));
    QObject *obj = component.createWithInitialProperties({{QStringLiteral("launcher"), QVariant::fromValue(this)}});
    m_window = qobject_cast<QQuickWindow *>(obj);
    if (!m_window) {
        qCWarning(lcLauncher) << "failed to create launcher:" << component.errorString();
        delete obj;
        return;
    }
    if (auto *layer = LayerShellQt::Window::get(m_window)) {
        // Full-screen transparent layer: clicks outside the card close it and
        // the compositor puts it on the focused output.
        layer->setLayer(LayerShellQt::Window::LayerTop);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        layer->setScreenConfiguration(LayerShellQt::Window::ScreenFromCompositor);
        layer->setScope(QStringLiteral("selsearch-launcher"));
    }
}

QString Launcher::aiModel() const
{
    return m_config.string(QStringLiteral("ai.model"));
}

QString Launcher::aiRemoteHost() const
{
    const QUrl url = AiClient::baseUrl(m_config.string(QStringLiteral("ai.endpoint")));
    return AiClient::isLocalUrl(url) ? QString() : url.host();
}

bool Launcher::aiEnabled() const
{
    return m_config.boolean(QStringLiteral("ai.enabled"));
}

QString Launcher::engineName() const
{
    return SearchEngines::current(&m_config).name;
}

QColor Launcher::accent() const
{
    return accentColor(m_config);
}

void Launcher::setMode(const QString &mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    Q_EMIT modeChanged();
    Q_EMIT focusInput();
}

void Launcher::show()
{
    if (!m_window)
        return;
    if (!aiEnabled() && m_mode == QLatin1String("ai"))
        setMode(QStringLiteral("search"));
    if (!m_shown) {
        m_shown = true;
        Q_EMIT shownChanged();
    }
    m_window->show();
    m_window->requestActivate();
    Q_EMIT focusInput();
    if (m_mode == QLatin1String("search"))
        setQuery(m_query);  // refresh (e.g. most-used apps for an empty query)
}

void Launcher::hide()
{
    if (!m_shown)
        return;
    m_shown = false;
    Q_EMIT shownChanged();
    m_suggest->cancel();
    // Let the close animation play before unmapping.
    QTimer::singleShot(120, this, [this] {
        if (!m_shown && m_window)
            m_window->hide();
    });
}

void Launcher::toggle()
{
    m_shown ? hide() : show();
}

void Launcher::setQuery(const QString &text)
{
    if (text != m_query) {
        m_query = text;
        Q_EMIT queryChanged();
    }
    if (m_mode != QLatin1String("search"))
        return;
    m_engine->query(text);
    if (!m_suggestions.isEmpty()) {
        m_suggestions.clear();
        Q_EMIT suggestionsChanged();
    }
    if (!text.trimmed().isEmpty() && m_suggest->isEnabled())
        m_suggest->fetch(text);
}

void Launcher::onResults(const QList<LauncherItem> &items, quint64 generation)
{
    if (generation != m_engine->generation())
        return;
    m_items = items;
    QVariantList rows;
    for (int i = 0; i < items.size(); ++i) {
        const auto &it = items[i];
        QString icon = it.icon;
        if (icon.startsWith(QLatin1String("lucide:")))
            icon = QStringLiteral("image://icon/") + icon.mid(7) + QStringLiteral("?color=%23e6e8ef");
        else if (!icon.isEmpty())
            icon = QStringLiteral("image://launcher/icon/") + icon;
        QVariantList actions;
        for (const auto &a : it.actions)
            actions << QVariantMap{{QStringLiteral("id"), a.id}, {QStringLiteral("title"), a.title}};
        rows << QVariantMap{{QStringLiteral("index"), i},
                            {QStringLiteral("title"), it.title},
                            {QStringLiteral("subtitle"), it.subtitle},
                            {QStringLiteral("group"), it.group},
                            {QStringLiteral("icon"), icon},
                            {QStringLiteral("kind"), int(it.kind)},
                            {QStringLiteral("actions"), actions}};
    }
    m_results = rows;
    Q_EMIT resultsChanged();
}

void Launcher::activate(int index, const QString &actionId)
{
    if (index < 0 || index >= m_items.size())
        return;
    const ActivateResult r = m_engine->activate(m_items[index], actionId);
    switch (r.status) {
    case ActivateResult::Done:
        hide();
        break;
    case ActivateResult::OpenUrl:
        hide();
        Q_EMIT openUrl(r.url, m_query);
        break;
    case ActivateResult::Error:
        appendMessage(QStringLiteral("error"), r.error);
        break;
    }
}

void Launcher::searchWeb(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    hide();
    Q_EMIT openUrl(SearchEngines::current(&m_config).searchUrlFor(text), text);
}

void Launcher::askAbout(const QImage &image, const QString &contextText)
{
    newChat();
    if (!image.isNull()) {
        m_attachmentImage = image;
        m_images->setAttachment(image);
        m_attachmentUrl = QStringLiteral("image://launcher/attach/%1").arg(++m_attachmentGen);
    }
    m_contextText = contextText.trimmed();
    Q_EMIT attachmentChanged();
    setMode(QStringLiteral("ai"));
    m_query.clear();
    Q_EMIT queryChanged();
    show();
}

void Launcher::askAi(const QString &text)
{
    if (!aiEnabled())
        return;
    const bool hasContext = !m_attachmentImage.isNull() || !m_contextText.isEmpty();
    const QString question = text.trimmed().isEmpty() && hasContext ? tr("What is this? Explain briefly.") : text.trimmed();
    if (question.isEmpty())
        return;
    setMode(QStringLiteral("ai"));
    // Recognised/selected text grounds the model (small VLMs misread text).
    QString prompt = question;
    if (!m_contextText.isEmpty())
        prompt += QStringLiteral("\n\n") + (m_attachmentImage.isNull() ? tr("Selected text:") : tr("Text recognised in the image:"))
            + QStringLiteral("\n\"\"\"\n") + m_contextText + QStringLiteral("\n\"\"\"");
    AiMessage user{AiMessage::User, prompt, {}};
    if (!m_attachmentImage.isNull())
        user.images << m_attachmentImage;
    appendMessage(QStringLiteral("user"), question, m_attachmentUrl, m_contextText);
    if (m_thread.messages.isEmpty())
        m_thread.append(AiMessage{AiMessage::System, QString::fromUtf8(kSystemPrompt), {}});
    m_thread.append(user);
    // The image is sent with the message it belongs to; follow-ups reuse the thread.
    m_attachmentImage = QImage();
    m_attachmentUrl.clear();
    m_contextText.clear();
    Q_EMIT attachmentChanged();
    m_query.clear();
    Q_EMIT queryChanged();

    QList<AiMessage> conversation = m_thread.toConversation();
    // History keeps only thumbnails; send the full-resolution image for this turn.
    if (!user.images.isEmpty() && !conversation.isEmpty())
        conversation.last().images = user.images;
    m_ai.setSettings(aiSettingsFromConfig(m_config));
    appendMessage(QStringLiteral("assistant"), QString());
    m_streamText.clear();
    m_ai.ask(conversation);
    Q_EMIT aiBusyChanged();
}

void Launcher::stopAi()
{
    if (!m_ai.isBusy())
        return;
    m_ai.abort();
    m_streamFlush.stop();
    setLastAssistantText(m_streamText, false);
    if (!m_streamText.isEmpty()) {
        m_thread.append(AiMessage{AiMessage::Assistant, m_streamText, {}});
        saveThread();
    }
    Q_EMIT aiBusyChanged();
}

void Launcher::newChat()
{
    stopAi();
    m_thread = AiThread{};
    m_messages.clear();
    Q_EMIT messagesChanged();
    removeAttachment();
}

void Launcher::openThread(const QString &id)
{
    const auto thread = m_history.load(id);
    if (!thread)
        return;
    stopAi();
    m_thread = *thread;
    m_messages.clear();
    for (const auto &m : std::as_const(m_thread.messages)) {
        if (m.role == QLatin1String("system"))
            continue;
        const QString thumb = m.thumbnails.isEmpty() ? QString() : QStringLiteral("data:image/jpeg;base64,") + m.thumbnails.first();
        m_messages << QVariantMap{{QStringLiteral("role"), m.role}, {QStringLiteral("text"), m.text},
                                  {QStringLiteral("image"), thumb}, {QStringLiteral("streaming"), false}};
    }
    Q_EMIT messagesChanged();
    setMode(QStringLiteral("ai"));
}

void Launcher::deleteThread(const QString &id)
{
    m_history.remove(id);
    if (m_thread.id == id)
        newChat();
    Q_EMIT threadsChanged();
}

QVariantList Launcher::threads() const
{
    QVariantList out;
    const auto list = m_history.list();
    for (const auto &t : list)
        out << QVariantMap{{QStringLiteral("id"), t.id}, {QStringLiteral("title"), t.title}, {QStringLiteral("updated"), t.updated}};
    return out;
}

void Launcher::removeAttachment()
{
    if (m_attachmentUrl.isEmpty() && m_attachmentImage.isNull() && m_contextText.isEmpty())
        return;
    m_attachmentImage = QImage();
    m_attachmentUrl.clear();
    m_contextText.clear();
    Q_EMIT attachmentChanged();
}

void Launcher::copy(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

void Launcher::startLens()
{
    hide();
    // Give the compositor a frame to unmap us so we are not in the capture.
    QTimer::singleShot(160, this, &Launcher::lensRequested);
}

void Launcher::openSettings()
{
    hide();
    Q_EMIT settingsRequested();
}

void Launcher::appendMessage(const QString &role, const QString &text, const QString &image, const QString &quote)
{
    m_messages << QVariantMap{{QStringLiteral("role"), role}, {QStringLiteral("text"), text},
                              {QStringLiteral("image"), image}, {QStringLiteral("quote"), quote},
                              {QStringLiteral("streaming"), role == QLatin1String("assistant")}};
    Q_EMIT messagesChanged();
}

void Launcher::setLastAssistantText(const QString &text, bool streaming)
{
    for (qsizetype i = m_messages.size() - 1; i >= 0; --i) {
        QVariantMap m = m_messages[i].toMap();
        if (m.value(QStringLiteral("role")) == QLatin1String("assistant")) {
            m[QStringLiteral("text")] = text;
            m[QStringLiteral("streaming")] = streaming;
            m_messages[i] = m;
            Q_EMIT messagesChanged();
            return;
        }
    }
}

void Launcher::saveThread()
{
    if (m_history.isEnabled()) {
        m_history.save(m_thread);
        Q_EMIT threadsChanged();
    }
}

}  // namespace selsearch
