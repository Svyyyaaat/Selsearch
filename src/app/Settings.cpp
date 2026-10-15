// SPDX-License-Identifier: MIT
#include "Settings.h"

#include "core/Config.h"
#include "ocr/ModelStore.h"
#include "ocr/OcrService.h"
#include "platform/KeyNames.h"
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
    // Keep the OCR status live while languages/engine are edited.
    connect(&m_config, &Config::changed, this, [this](const QString &key) {
        if (key.startsWith(QLatin1String("ocr.")))
            Q_EMIT ocrStatusChanged();
    });
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

static QString modelLabel(const QString &id)
{
    if (id.startsWith(QLatin1String("eslav")))
        return QObject::tr("Russian/Ukrainian/Belarusian");
    if (id.startsWith(QLatin1String("cyrillic")))
        return QObject::tr("Cyrillic");
    if (id.startsWith(QLatin1String("latin")))
        return QObject::tr("Latin-script");
    if (id.startsWith(QLatin1String("PP-OCRv5_mobile_rec")))
        return QObject::tr("Chinese/Japanese/English");
    return id.section(QLatin1Char('_'), 0, 0);
}

QString Settings::ocrStatus() const
{
    if (m_ocrDownloading)
        return m_ocrProgress.isEmpty() ? tr("Downloading…") : m_ocrProgress;
    const QStringList langs = effectiveOcrLanguages(m_config);
    const QString langText = langs.join(QStringLiteral(", "));
    ModelStore store;
    const QStringList missing = store.missingFor(langs);
    const QString rec = ModelStore::bestRecognitionModel(langs);
    QString line;
    if (missing.isEmpty()) {
        line = tr("✓ Installed. Reading %1 with the %2 model.").arg(langText, modelLabel(rec));
    } else {
        qint64 bytes = 0;
        for (const QString &id : missing)
            if (const ModelSpec *spec = ModelStore::findSpec(id))
                for (const auto &f : spec->files)
                    bytes += f.size;
        line = tr("Not installed for %1 — Download fetches %2 MB. Until then Tesseract is used.")
                   .arg(langText).arg(qMax<qint64>(1, bytes / 1048576));
    }
    // One recognition model per scan: say which languages it won't cover.
    if (const ModelSpec *spec = ModelStore::findSpec(rec)) {
        QStringList uncovered;
        for (const QString &l : langs)
            if (!spec->languages.contains(l))
                uncovered << l;
        if (!uncovered.isEmpty())
            line += QLatin1Char(' ') + tr("Note: %1 needs a different model; put the language you scan most first.").arg(uncovered.join(QStringLiteral(", ")));
    }
    if (!m_ocrProgress.isEmpty())
        line += QLatin1Char(' ') + m_ocrProgress;  // last error
    return line;
}

bool Settings::ocrInstalled() const
{
    return ModelStore().missingFor(effectiveOcrLanguages(m_config)).isEmpty();
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
    if (m_ocrDownloading)
        return;
    auto *store = new ModelStore(this);
    m_ocrDownloading = true;
    Q_EMIT ocrStatusChanged();
    connect(store, &ModelStore::progress, this, [this](qint64 received, qint64 total) {
        m_ocrProgress = total > 0 ? tr("Downloading… %1 / %2 MB").arg(received / 1048576.0, 0, 'f', 1).arg(total / 1048576.0, 0, 'f', 1)
                                  : tr("Downloading…");
        m_ocrFraction = total > 0 ? double(received) / double(total) : 0.0;
        Q_EMIT ocrStatusChanged();
    });
    m_ocrProgress.clear();
    connect(store, &ModelStore::finished, this, [this, store] {
        m_ocrDownloading = false;
        m_ocrProgress.clear();
        Q_EMIT ocrStatusChanged();
        store->deleteLater();
    });
    connect(store, &ModelStore::failed, this, [this, store](const QString &error) {
        m_ocrDownloading = false;
        m_ocrProgress = tr("Download failed: %1").arg(error);
        Q_EMIT ocrStatusChanged();
        store->deleteLater();
    });
    store->ensure(effectiveOcrLanguages(m_config));
}

void Settings::openConfigFile()
{
    if (!QFileInfo::exists(m_config.path()))
        m_config.save();
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_config.path()));
}

void Settings::setNativeKeyCapture(bool on)
{
    if (m_nativeKeyCapture == on)
        return;
    m_nativeKeyCapture = on;
    Q_EMIT shortcutStateChanged();
}

void Settings::setShortcutError(const QString &id, const QString &reason)
{
    if (m_shortcutErrors.value(id).toString() == reason)
        return;
    if (reason.isEmpty())
        m_shortcutErrors.remove(id);
    else
        m_shortcutErrors.insert(id, reason);
    Q_EMIT shortcutStateChanged();
}

QString Settings::shortcutMode() const
{
#ifdef Q_OS_WIN
    return QStringLiteral("inapp");
#else
    if (qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE"))
        return QStringLiteral("hyprland");
    if (qEnvironmentVariableIsSet("SWAYSOCK"))
        return QStringLiteral("sway");
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (desktop.contains(QLatin1String("kde")) || desktop.contains(QLatin1String("gnome")))
        return QStringLiteral("portal");
    return QStringLiteral("other");
#endif
}

QString Settings::sequenceFor(int key, int modifiers) const
{
    const auto mods = Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QString Settings::displayKeys(const QString &sequence) const
{
    return toDisplayKeys(QKeySequence(sequence, QKeySequence::PortableText));
}

static QKeySequence shortcutKeys(const Config &c, const char *key)
{
    return QKeySequence(c.string(QString::fromLatin1(key)), QKeySequence::PortableText);
}

QString Settings::hyprlandSnippet() const
{
    QString s = QStringLiteral("-- Selsearch (Hyprland 0.55+, ~/.config/hypr/hyprland.lua)\n");
    const auto bind = [&](const char *key, const char *cmd) {
        const QString keys = toHyprlandKeys(shortcutKeys(m_config, key));
        if (!keys.isEmpty())
            s += QStringLiteral("hl.bind(\"%1\", hl.dsp.exec_cmd(\"%2\"))\n").arg(keys, QString::fromLatin1(cmd));
    };
    bind("shortcuts.launcher", "selsearch launcher");
    bind("shortcuts.lens", "selsearch lens");
    bind("shortcuts.lens_text", "selsearch lens --text");
    s += QStringLiteral(
        "-- Selsearch's surfaces use these layer namespaces, so your layer animations\n"
        "-- apply to them: selsearch-lens, selsearch-launcher, selsearch-results.\n"
        "-- Optional, to make the lens appear instantly instead:\n"
        "-- hl.layer_rule({ match = { namespace = \"selsearch-lens\" }, name = \"selsearch-lens\", no_anim = true })\n"
        "-- Start the daemon with the session (skip if you enabled selsearch.service).\n"
        "hl.on(\"hyprland.start\", function() hl.exec_cmd(\"selsearchd\") end)\n");
    return s;
}

QString Settings::swaySnippet() const
{
    QString s = QStringLiteral("# Selsearch (sway / i3-style config)\n");
    const auto bind = [&](const char *key, const char *cmd) {
        const QString keys = toSwayKeys(shortcutKeys(m_config, key));
        if (!keys.isEmpty())
            s += QStringLiteral("bindsym %1 exec %2\n").arg(keys, QString::fromLatin1(cmd));
    };
    bind("shortcuts.launcher", "selsearch launcher");
    bind("shortcuts.lens", "selsearch lens");
    bind("shortcuts.lens_text", "selsearch lens --text");
    s += QStringLiteral("exec selsearchd\n");
    return s;
}

void Settings::copy(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

}  // namespace selsearch
