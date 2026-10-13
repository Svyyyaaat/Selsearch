// SPDX-License-Identifier: MIT
#pragma once

#include "ai/AiClient.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>

class QQmlEngine;
class QQuickWindow;

namespace selsearch {

class Config;
class OcrService;

// Backs the settings window: option lists, model status and connection tests.
// Values themselves are read and written straight through Config.
class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *config READ configObject CONSTANT)
    Q_PROPERTY(QVariantList searchEngines READ searchEngines CONSTANT)
    Q_PROPERTY(QVariantList imageEngines READ imageEngines CONSTANT)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    Q_PROPERTY(QStringList aiModels READ aiModels NOTIFY aiModelsChanged)
    Q_PROPERTY(QString aiStatus READ aiStatus NOTIFY aiStatusChanged)
    Q_PROPERTY(QString ocrStatus READ ocrStatus NOTIFY ocrStatusChanged)
    Q_PROPERTY(bool ocrDownloading READ ocrDownloading NOTIFY ocrStatusChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(bool welcome READ welcome NOTIFY welcomeChanged)
public:
    Settings(Config &config, OcrService &ocr, QObject *parent = nullptr);
    void setEngine(QQmlEngine *engine);

    QObject *configObject() const;
    QVariantList searchEngines() const;
    QVariantList imageEngines() const;
    QVariantList languages() const;
    QStringList aiModels() const { return m_aiModels; }
    QString aiStatus() const { return m_aiStatus; }
    QString ocrStatus() const;
    bool ocrDownloading() const { return m_ocrDownloading; }
    QString version() const;
    QString configPath() const;

    Q_INVOKABLE void show();
    // First start: show the welcome page once (marker in $XDG_STATE_HOME).
    void showWelcomeIfFirstRun();
    bool welcome() const { return m_welcome; }
    Q_INVOKABLE void finishWelcome();
    Q_INVOKABLE void testAi();
    Q_INVOKABLE void downloadOcrModels();
    Q_INVOKABLE void openConfigFile();
    Q_INVOKABLE QString hyprlandSnippet() const;
    Q_INVOKABLE void copy(const QString &text);

Q_SIGNALS:
    void aiModelsChanged();
    void aiStatusChanged();
    void ocrStatusChanged();
    void welcomeChanged();

private:
    Config &m_config;
    OcrService &m_ocr;
    QPointer<QQmlEngine> m_qml;
    QQuickWindow *m_window = nullptr;
    AiClient m_ai;
    QStringList m_aiModels;
    QString m_aiStatus;
    bool m_ocrDownloading = false;
    bool m_welcome = false;
    QString m_ocrProgress;
};

}  // namespace selsearch
