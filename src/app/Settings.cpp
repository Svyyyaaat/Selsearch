// SPDX-License-Identifier: MIT
#include "Settings.h"

#include "core/Config.h"
#if __has_include("ocr/ModelStore.h")
#include "ocr/ModelStore.h"
#define SELSEARCH_HAVE_MODELSTORE 1
#endif
#include "ocr/OcrService.h"
#include "search/ImageSearch.h"
#include "search/SearchEngines.h"
#include "translate/TranslationLanguages.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QUrl>

Q_LOGGING_CATEGORY(lcSettings, "selsearch.settings")

namespace selsearch {

Settings::Settings(Config &config, OcrService &ocr, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_ocr(ocr)
{
    connect(&m_ai, &AiClient::modelsListed, this, [this](const QStringList &models) {
        m_aiModels = models;
        m_aiStatus = models.isEmpty() ? tr("Connected, but the server lists no models.")
                                      : tr("Connected · %n model(s) available", nullptr, int(models.size()));
        Q_EMIT aiModelsChanged();
        Q_EMIT aiStatusChanged();
    });
    connect(&m_ai, &AiClient::modelsFailed, this, [this](const QString &error) {
        m_aiStatus = error;
        Q_EMIT aiStatusChanged();
    });
}

void Settings::setEngine(QQmlEngine *engine)
{
    m_qml = engine;
}

QObject *Settings::configObject() const
{
    return &m_config;
}

QVariantList Settings::searchEngines() const
{
    QVariantList out;
    for (const auto &e : SearchEngines::builtin())
        out << QVariantMap{{QStringLiteral("id"), e.id}, {QStringLiteral("name"), e.name}};
    out << QVariantMap{{QStringLiteral("id"), QStringLiteral("searxng")}, {QStringLiteral("name"), QStringLiteral("SearXNG (your instance)")}};
    out << QVariantMap{{QStringLiteral("id"), QStringLiteral("custom")}, {QStringLiteral("name"), tr("Custom URL")}};
    return out;
}

QVariantList Settings::imageEngines() const
{
    QVariantList out;
    for (const auto &e : ImageSearch::engines())
        out << QVariantMap{{QStringLiteral("id"), e.id}, {QStringLiteral("name"), e.name}};
    return out;
}

QVariantList Settings::languages() const
{
    QVariantList out;
    for (const auto &l : translationLanguages())
        out << QVariantMap{{QStringLiteral("code"), l.code},
                           {QStringLiteral("name"), l.nativeName == l.englishName ? l.englishName
                                                                                 : QStringLiteral("%1 · %2").arg(l.englishName, l.nativeName)}};
    return out;
}

QString Settings::ocrStatus() const
{
    if (m_ocrDownloading)
        return m_ocrProgress.isEmpty() ? tr("Downloading models…") : m_ocrProgress;
    const QStringList engines = m_ocr.availableEngines();
#ifdef SELSEARCH_HAVE_MODELSTORE
    const QStringList missing = ModelStore::missingFor(effectiveOcrLanguages(m_config));
#else
    const QStringList missing;
#endif
    if (engines.contains(QLatin1String("paddle")) && missing.isEmpty())
        return tr("PP-OCRv5 ready for %1").arg(effectiveOcrLanguages(m_config).join(QStringLiteral(", ")));
    if (!missing.isEmpty())
        return tr("PP-OCRv5 models missing for your languages (%1). Tesseract is used meanwhile.")
            .arg(effectiveOcrLanguages(m_config).join(QStringLiteral(", ")));
    return engines.isEmpty() ? tr("No OCR engine available") : tr("Using %1").arg(engines.join(QStringLiteral(", ")));
}

QString Settings::version() const
{
    return QStringLiteral(SELSEARCH_VERSION);
}

QString Settings::configPath() const
{
    return m_config.path();
}

void Settings::show()
{
    if (!m_window && m_qml) {
        QQmlComponent component(m_qml, QUrl(QStringLiteral("qrc:/qml/SettingsWindow.qml")));
        QObject *obj = component.createWithInitialProperties({{QStringLiteral("settings"), QVariant::fromValue(this)}});
        m_window = qobject_cast<QQuickWindow *>(obj);
        if (!m_window) {
            qCWarning(lcSettings) << component.errorString();
            delete obj;
            return;
        }
    }
    if (m_window) {
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
    }
}

static QString welcomeMarker()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/selsearch/welcomed");
}

void Settings::showWelcomeIfFirstRun()
{
    if (QFileInfo::exists(welcomeMarker()))
        return;
    m_welcome = true;
    Q_EMIT welcomeChanged();
    show();
}

void Settings::finishWelcome()
{
    QDir().mkpath(QFileInfo(welcomeMarker()).absolutePath());
    QFile f(welcomeMarker());
    if (f.open(QIODevice::WriteOnly))
        f.write("1\n");
    m_welcome = false;
    Q_EMIT welcomeChanged();
}

void Settings::testAi()
{
    m_ai.setSettings(aiSettingsFromConfig(m_config));
    m_aiStatus = tr("Connecting…");
    Q_EMIT aiStatusChanged();
    m_ai.listModels();
}

void Settings::downloadOcrModels()
{
#ifdef SELSEARCH_HAVE_MODELSTORE
    if (m_ocrDownloading)
        return;
    auto *store = new ModelStore(this);
    m_ocrDownloading = true;
    Q_EMIT ocrStatusChanged();
    connect(store, &ModelStore::progress, this, [this](qint64 received, qint64 total) {
        m_ocrProgress = total > 0 ? tr("Downloading models… %1 / %2 MB").arg(received / 1048576).arg(total / 1048576)
                                  : tr("Downloading models…");
        Q_EMIT ocrStatusChanged();
    });
    connect(store, &ModelStore::finished, this, [this, store] {
        m_ocrDownloading = false;
        m_ocrProgress.clear();
        Q_EMIT ocrStatusChanged();
        store->deleteLater();
    });
    connect(store, &ModelStore::failed, this, [this, store](const QString &error) {
        m_ocrDownloading = false;
        m_ocrProgress = error;
        Q_EMIT ocrStatusChanged();
        store->deleteLater();
    });
    store->ensure(effectiveOcrLanguages(m_config));
#endif
}

void Settings::openConfigFile()
{
    if (!QFileInfo::exists(m_config.path()))
        m_config.save();
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_config.path()));
}

QString Settings::hyprlandSnippet() const
{
    return QStringLiteral(
        "-- Selsearch (Hyprland 0.55+, ~/.config/hypr/hyprland.lua)\n"
        "hl.bind(\"ALT + Space\", hl.dsp.exec_cmd(\"selsearch launcher\"))\n"
        "hl.bind(\"SUPER + S\", hl.dsp.exec_cmd(\"selsearch lens\"))\n"
        "hl.bind(\"SUPER + SHIFT + S\", hl.dsp.exec_cmd(\"selsearch lens --text\"))\n"
        "-- The overlay shows a frozen screen: no open/close animation.\n"
        "hl.layer_rule({ match = { namespace = \"selsearch-lens\" }, name = \"selsearch-lens\", no_anim = true })\n"
        "-- Start the daemon with the session (skip if you enabled selsearch.service).\n"
        "hl.on(\"hyprland.start\", function() hl.exec_cmd(\"selsearchd\") end)\n");
}

void Settings::copy(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

}  // namespace selsearch
