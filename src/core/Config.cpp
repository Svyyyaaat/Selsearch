// SPDX-License-Identifier: MIT
#include "Config.h"

#include <QDir>
#include <QJSValue>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

#include <sstream>

namespace selsearch {

struct Config::Impl {
    toml::table table;
};

const QVariantMap &Config::defaults()
{
    static const QVariantMap d = {
        // Appearance
        {QStringLiteral("general.theme"), QStringLiteral("signal")},  // signal | mono | aurora
        {QStringLiteral("general.accent"), QString()},                // "#rrggbb", empty = built-in palette
        {QStringLiteral("general.follow_portal_accent"), false},
        {QStringLiteral("general.color_scheme"), QStringLiteral("auto")},  // auto | light | dark
        {QStringLiteral("general.autostart"), false},  // Windows: start with Windows (Linux: selsearch.service)

        // Lens overlay
        {QStringLiteral("lens.on_select"), QStringLiteral("toolbar")},  // toolbar | search_image | search_text | ask
        {QStringLiteral("lens.recognize_text"), true},
        {QStringLiteral("lens.snap_to_windows"), true},
        {QStringLiteral("lens.show_hints"), true},

        // Search
        {QStringLiteral("search.engine"), QStringLiteral("duckduckgo")},
        {QStringLiteral("search.custom_url"), QString()},  // used when engine = "custom"; %s = query
        {QStringLiteral("search.searxng_url"), QStringLiteral("https://searx.be")},  // used when engine = "searxng"
        {QStringLiteral("search.image_engine"), QStringLiteral("google_lens")},
        {QStringLiteral("search.suggestions"), false},  // as-you-type suggestions from the engine
        {QStringLiteral("search.open_in"), QStringLiteral("panel")},  // panel | browser
        {QStringLiteral("search.persist_cookies"), false},

        // OCR
        {QStringLiteral("ocr.engine"), QStringLiteral("auto")},  // auto | paddle | tesseract
        {QStringLiteral("ocr.languages"), QStringList()},        // empty = system languages + en
        {QStringLiteral("ocr.unload_after_s"), 60},              // 0 = keep loaded

        // Translation
        {QStringLiteral("translate.backend"), QStringLiteral("auto")},  // auto | web | bergamot | libretranslate | llm
        {QStringLiteral("translate.web_service"), QStringLiteral("google")},  // google | deepl (backend "web", or auto fallback)
        {QStringLiteral("translate.target"), QString()},                // empty = system language
        {QStringLiteral("translate.libretranslate_url"), QStringLiteral("http://localhost:5000")},
        {QStringLiteral("translate.libretranslate_key"), QString()},
        {QStringLiteral("translate.recent"), QStringList()},  // recently used targets, newest first (max 3)

        // AI
        {QStringLiteral("ai.enabled"), true},
        {QStringLiteral("ai.endpoint"), QStringLiteral("http://localhost:11434/v1")},
        {QStringLiteral("ai.model"), QStringLiteral("qwen3-vl:4b")},
        {QStringLiteral("ai.api_key"), QString()},
        {QStringLiteral("ai.api_key_env"), QString()},  // read the key from this env var instead
        {QStringLiteral("ai.history"), true},

        // Shortcuts (Qt key-sequence syntax). Applied in-app on Windows and
        // through the GlobalShortcuts portal (KDE/GNOME); on Hyprland/sway
        // they feed the generated compositor config.
        {QStringLiteral("shortcuts.launcher"), QStringLiteral("Alt+Space")},
#ifdef Q_OS_WIN
        // Win+S (Search) and Win+Shift+S (Snipping Tool) belong to Windows,
        // which refuses to let an app register them.
        {QStringLiteral("shortcuts.lens"), QStringLiteral("Alt+Shift+S")},
        {QStringLiteral("shortcuts.lens_text"), QStringLiteral("Alt+Shift+T")},
#else
        {QStringLiteral("shortcuts.lens"), QStringLiteral("Meta+S")},
        {QStringLiteral("shortcuts.lens_text"), QStringLiteral("Meta+Shift+S")},
#endif

        // Launcher
        {QStringLiteral("launcher.apps"), true},
        {QStringLiteral("launcher.files"), true},
        {QStringLiteral("launcher.file_roots"), QStringList{QStringLiteral("~")}},
        {QStringLiteral("launcher.calculator"), true},
        {QStringLiteral("launcher.max_results"), 8},
    };
    return d;
}

QString Config::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/selsearch/config.toml");
}

Config::Config(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
}

Config::~Config() = default;

static QVariant fromNode(const toml::node &node)
{
    if (auto s = node.as_string())
        return QString::fromStdString(s->get());
    if (auto i = node.as_integer())
        return QVariant::fromValue<qlonglong>(i->get());
    if (auto f = node.as_floating_point())
        return f->get();
    if (auto b = node.as_boolean())
        return b->get();
    if (auto a = node.as_array()) {
        QStringList out;
        for (const auto &el : *a) {
            if (auto s = el.as_string())
                out << QString::fromStdString(s->get());
        }
        return out;
    }
    return {};
}

static void insertValue(toml::table &table, const std::string &key, const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::Bool:
        table.insert_or_assign(key, value.toBool());
        break;
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        table.insert_or_assign(key, static_cast<int64_t>(value.toLongLong()));
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        table.insert_or_assign(key, value.toDouble());
        break;
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        toml::array arr;
        for (const auto &s : value.toStringList())
            arr.push_back(s.toStdString());
        table.insert_or_assign(key, std::move(arr));
        break;
    }
    default:
        table.insert_or_assign(key, value.toString().toStdString());
    }
}

bool Config::load(const QString &path)
{
    m_path = path.isEmpty() ? defaultPath() : path;
    m_error.clear();
    d->table = toml::table{};
    QFile f(m_path);
    if (!f.exists()) {
        Q_EMIT reloaded();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = f.errorString();
        return false;
    }
    const QByteArray data = f.readAll();
    auto result = toml::parse(std::string_view(data.constData(), size_t(data.size())), m_path.toStdString());
    if (!result) {
        std::ostringstream os;
        os << result.error();
        m_error = QString::fromStdString(os.str());
        Q_EMIT reloaded();
        return false;
    }
    d->table = std::move(result).table();
    Q_EMIT reloaded();
    return true;
}

bool Config::save() const
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    std::ostringstream os;
    os << "# Selsearch configuration. Only changed keys are stored here;\n"
          "# see `selsearch config --defaults` for every key and its default.\n\n";
    os << d->table << "\n";
    f.write(QByteArray::fromStdString(os.str()));
    return f.commit();
}

QVariant Config::value(const QString &key) const
{
    const auto node = d->table.at_path(key.toStdString());
    if (node) {
        const QVariant v = fromNode(*node.node());
        const QVariant def = defaults().value(key);
        if (v.isValid()) {
            // Keep the default's type so callers get what they expect.
            if (def.isValid() && def.typeId() == QMetaType::QStringList && v.typeId() == QMetaType::QString)
                return QStringList{v.toString()};
            return v;
        }
    }
    return defaults().value(key);
}

void Config::setValue(const QString &key, const QVariant &input)
{
    // Values from QML arrive wrapped in QJSValue (e.g. a JS array of strings).
    QVariant value = input;
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        value = value.value<QJSValue>().toVariant();
    if (value.typeId() == QMetaType::QVariantList)
        value = value.toStringList();
    const QStringList parts = key.split(QLatin1Char('.'));
    toml::table *t = &d->table;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const std::string part = parts[i].toStdString();
        auto *child = t->get_as<toml::table>(part);
        if (!child) {
            t->insert_or_assign(part, toml::table{});
            child = t->get_as<toml::table>(part);
        }
        t = child;
    }
    insertValue(*t, parts.last().toStdString(), value);
    save();
    Q_EMIT changed(key);
}

void Config::reset(const QString &key)
{
    const QStringList parts = key.split(QLatin1Char('.'));
    toml::table *t = &d->table;
    for (int i = 0; i < parts.size() - 1 && t; ++i)
        t = t->get_as<toml::table>(parts[i].toStdString());
    if (t)
        t->erase(parts.last().toStdString());
    save();
    Q_EMIT changed(key);
}

QStringList effectiveOcrLanguages(const Config &config)
{
    QStringList langs = config.list(QStringLiteral("ocr.languages"));
    if (langs.isEmpty()) {
        for (const QString &name : QLocale::system().uiLanguages(QLocale::TagSeparator::Dash)) {
            const QString code = name.section(QLatin1Char('-'), 0, 0).toLower();
            if (!code.isEmpty() && code != QLatin1String("c") && !langs.contains(code))
                langs << code;
        }
    }
    if (!langs.contains(QLatin1String("en")))
        langs << QStringLiteral("en");
    return langs;
}

}  // namespace selsearch
