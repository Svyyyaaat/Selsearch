// SPDX-License-Identifier: MIT
#pragma once

#include "ai/AiClient.h"
#include "ai/AiHistory.h"
#include "launcher/Provider.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <memory>

class QQmlEngine;
class QQuickWindow;

namespace selsearch {

class Config;
class LauncherEngine;
class SuggestionFetcher;

// Serves the launcher's attached image and app icons to QML.
class LauncherImages : public QQuickImageProvider {
public:
    LauncherImages();
    void setAttachment(const QImage &image);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QImage m_attachment;
};

// The Alt+Space window: search apps, files, the web, a calculator; or chat
// with the configured AI about a lens selection.
class Launcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(QStringList suggestions READ suggestions NOTIFY suggestionsChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)  // "search" | "ai"
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(bool aiBusy READ aiBusy NOTIFY aiBusyChanged)
    Q_PROPERTY(QString aiModel READ aiModel NOTIFY settingsChanged)
    Q_PROPERTY(QString aiRemoteHost READ aiRemoteHost NOTIFY settingsChanged)  // empty = local
    Q_PROPERTY(bool aiEnabled READ aiEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString engineName READ engineName NOTIFY settingsChanged)
    Q_PROPERTY(QString attachment READ attachment NOTIFY attachmentChanged)
    Q_PROPERTY(QString contextText READ contextText NOTIFY attachmentChanged)
    Q_PROPERTY(QVariantList threads READ threads NOTIFY threadsChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY settingsChanged)
public:
    Launcher(Config &config, QObject *parent = nullptr);
    ~Launcher() override;

    void setEngine(QQmlEngine *engine);

    bool shown() const { return m_shown; }
    QString query() const { return m_query; }
    QVariantList results() const { return m_results; }
    QStringList suggestions() const { return m_suggestions; }
    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    QVariantList messages() const { return m_messages; }
    bool aiBusy() const { return m_ai.isBusy(); }
    QString aiModel() const;
    QString aiRemoteHost() const;
    bool aiEnabled() const;
    QString engineName() const;
    QString attachment() const { return m_attachmentUrl; }
    QString contextText() const { return m_contextText; }
    QVariantList threads() const;
    QColor accent() const;

    // Opens in AI mode with an image and/or text from the lens. The text
    // (recognised or selected) is sent as context with the next question.
    void askAbout(const QImage &image, const QString &contextText);

    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void setQuery(const QString &text);
    Q_INVOKABLE void activate(int index, const QString &actionId = {});
    Q_INVOKABLE void searchWeb(const QString &text);
    Q_INVOKABLE void askAi(const QString &text);
    Q_INVOKABLE void stopAi();
    Q_INVOKABLE void newChat();
    Q_INVOKABLE void openThread(const QString &id);
    Q_INVOKABLE void deleteThread(const QString &id);
    Q_INVOKABLE void removeAttachment();
    Q_INVOKABLE void copy(const QString &text);
    Q_INVOKABLE void startLens();
    Q_INVOKABLE void openSettings();

Q_SIGNALS:
    void shownChanged();
    void queryChanged();
    void resultsChanged();
    void suggestionsChanged();
    void modeChanged();
    void messagesChanged();
    void aiBusyChanged();
    void settingsChanged();
    void attachmentChanged();
    void threadsChanged();
    void focusInput();
    void openUrl(const QUrl &url, const QString &title);
    void lensRequested();
    void settingsRequested();

private:
    void onResults(const QList<LauncherItem> &items, quint64 generation);
    void appendMessage(const QString &role, const QString &text, const QString &image = {}, const QString &quote = {});
    void setLastAssistantText(const QString &text, bool streaming);
    void saveThread();

    Config &m_config;
    std::unique_ptr<LauncherEngine> m_engine;
    std::unique_ptr<SuggestionFetcher> m_suggest;
    AiClient m_ai;
    AiHistory m_history;
    AiThread m_thread;
    QPointer<QQmlEngine> m_qml;
    QQuickWindow *m_window = nullptr;
    LauncherImages *m_images = nullptr;  // owned by the QML engine
    bool m_shown = false;
    QString m_query;
    QString m_mode = QStringLiteral("search");
    QList<LauncherItem> m_items;
    QVariantList m_results;
    QStringList m_suggestions;
    QVariantList m_messages;
    QImage m_attachmentImage;
    QString m_attachmentUrl;
    QString m_contextText;
    int m_attachmentGen = 0;
    QString m_streamText;
    QTimer m_streamFlush;
};

}  // namespace selsearch
