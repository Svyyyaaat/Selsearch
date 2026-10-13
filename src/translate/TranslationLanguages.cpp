// SPDX-License-Identifier: MIT
#include "TranslationLanguages.h"

#include "core/Config.h"

#include <QLocale>

namespace selsearch {

const QList<TranslationLanguage> &translationLanguages()
{
    static const QList<TranslationLanguage> list = [] {
        struct Row {
            const char *code, *english, *native;
        };
        static const Row rows[] = {
            {"af", "Afrikaans", "Afrikaans"},
            {"sq", "Albanian", "Shqip"},
            {"am", "Amharic", "አማርኛ"},
            {"ar", "Arabic", "العربية"},
            {"hy", "Armenian", "Հայերեն"},
            {"az", "Azerbaijani", "Azərbaycanca"},
            {"eu", "Basque", "Euskara"},
            {"be", "Belarusian", "Беларуская"},
            {"bn", "Bengali", "বাংলা"},
            {"bs", "Bosnian", "Bosanski"},
            {"bg", "Bulgarian", "Български"},
            {"ca", "Catalan", "Català"},
            {"zh", "Chinese", "中文"},
            {"hr", "Croatian", "Hrvatski"},
            {"cs", "Czech", "Čeština"},
            {"da", "Danish", "Dansk"},
            {"nl", "Dutch", "Nederlands"},
            {"en", "English", "English"},
            {"eo", "Esperanto", "Esperanto"},
            {"et", "Estonian", "Eesti"},
            {"tl", "Filipino", "Filipino"},
            {"fi", "Finnish", "Suomi"},
            {"fr", "French", "Français"},
            {"gl", "Galician", "Galego"},
            {"ka", "Georgian", "ქართული"},
            {"de", "German", "Deutsch"},
            {"el", "Greek", "Ελληνικά"},
            {"gu", "Gujarati", "ગુજરાતી"},
            {"he", "Hebrew", "עברית"},
            {"hi", "Hindi", "हिन्दी"},
            {"hu", "Hungarian", "Magyar"},
            {"is", "Icelandic", "Íslenska"},
            {"id", "Indonesian", "Bahasa Indonesia"},
            {"ga", "Irish", "Gaeilge"},
            {"it", "Italian", "Italiano"},
            {"ja", "Japanese", "日本語"},
            {"kn", "Kannada", "ಕನ್ನಡ"},
            {"kk", "Kazakh", "Қазақ тілі"},
            {"km", "Khmer", "ខ្មែរ"},
            {"ko", "Korean", "한국어"},
            {"ky", "Kyrgyz", "Кыргызча"},
            {"lo", "Lao", "ລາວ"},
            {"lv", "Latvian", "Latviešu"},
            {"lt", "Lithuanian", "Lietuvių"},
            {"mk", "Macedonian", "Македонски"},
            {"ms", "Malay", "Bahasa Melayu"},
            {"ml", "Malayalam", "മലയാളം"},
            {"mt", "Maltese", "Malti"},
            {"mr", "Marathi", "मराठी"},
            {"mn", "Mongolian", "Монгол"},
            {"ne", "Nepali", "नेपाली"},
            {"nb", "Norwegian", "Norsk bokmål"},
            {"fa", "Persian", "فارسی"},
            {"pl", "Polish", "Polski"},
            {"pt", "Portuguese", "Português"},
            {"pa", "Punjabi", "ਪੰਜਾਬੀ"},
            {"ro", "Romanian", "Română"},
            {"ru", "Russian", "Русский"},
            {"sr", "Serbian", "Српски"},
            {"sk", "Slovak", "Slovenčina"},
            {"sl", "Slovenian", "Slovenščina"},
            {"es", "Spanish", "Español"},
            {"sw", "Swahili", "Kiswahili"},
            {"sv", "Swedish", "Svenska"},
            {"ta", "Tamil", "தமிழ்"},
            {"te", "Telugu", "తెలుగు"},
            {"th", "Thai", "ไทย"},
            {"tr", "Turkish", "Türkçe"},
            {"uk", "Ukrainian", "Українська"},
            {"ur", "Urdu", "اردو"},
            {"uz", "Uzbek", "Oʻzbekcha"},
            {"vi", "Vietnamese", "Tiếng Việt"},
            {"cy", "Welsh", "Cymraeg"},
        };
        QList<TranslationLanguage> out;
        for (const Row &r : rows)
            out.append({QString::fromLatin1(r.code), QString::fromUtf8(r.english), QString::fromUtf8(r.native)});
        return out;
    }();
    return list;
}

std::optional<TranslationLanguage> translationLanguage(const QString &code)
{
    const QString c = normalizeLanguageCode(code);
    for (const TranslationLanguage &l : translationLanguages())
        if (l.code == c)
            return l;
    return std::nullopt;
}

QString languageEnglishName(const QString &code)
{
    if (auto l = translationLanguage(code))
        return l->englishName;
    const QString c = normalizeLanguageCode(code);
    const QLocale::Language lang = QLocale::codeToLanguage(c);
    if (lang != QLocale::AnyLanguage && lang != QLocale::C)
        return QLocale::languageToString(lang);
    return code;
}

QString normalizeLanguageCode(const QString &code)
{
    QString c = code.trimmed().toLower().replace(QLatin1Char('_'), QLatin1Char('-'));
    c = c.section(QLatin1Char('-'), 0, 0);
    if (c.isEmpty() || c == QLatin1String("auto"))
        return {};
    // Legacy and macro-language codes.
    if (c == QLatin1String("iw"))
        return QStringLiteral("he");
    if (c == QLatin1String("in"))
        return QStringLiteral("id");
    if (c == QLatin1String("ji"))
        return QStringLiteral("yi");
    if (c == QLatin1String("no") || c == QLatin1String("nob"))
        return QStringLiteral("nb");
    if (c == QLatin1String("fil"))
        return QStringLiteral("tl");
    if (c == QLatin1String("zho") || c == QLatin1String("chi") || c == QLatin1String("cmn"))
        return QStringLiteral("zh");
    if (c.size() == 2) {
        for (QChar ch : c)
            if (ch < QLatin1Char('a') || ch > QLatin1Char('z'))
                return {};
        return c;
    }
    if (c.size() == 3) {
        const QLocale::Language lang = QLocale::codeToLanguage(c, QLocale::AnyLanguageCode);
        if (lang != QLocale::AnyLanguage && lang != QLocale::C) {
            const QString two = QLocale::languageToCode(lang, QLocale::ISO639Part1);
            if (two.size() == 2)
                return two;
        }
        return c;  // e.g. "hbs": keep, callers may know it
    }
    return {};
}

QString systemLanguageCode()
{
    for (const QString &name : QLocale::system().uiLanguages(QLocale::TagSeparator::Dash)) {
        const QString c = normalizeLanguageCode(name);
        if (!c.isEmpty() && translationLanguage(c))
            return c;
    }
    return QStringLiteral("en");
}

QString effectiveTargetLanguage(const Config &config)
{
    const QString t = normalizeLanguageCode(config.string(QStringLiteral("translate.target")));
    return t.isEmpty() ? systemLanguageCode() : t;
}

QStringList recentTranslationLanguages(const Config &config)
{
    QStringList out;
    for (const QString &code : config.list(QStringLiteral("translate.recent"))) {
        const QString c = normalizeLanguageCode(code);
        if (!c.isEmpty() && !out.contains(c))
            out << c;
        if (out.size() == kMaxRecentLanguages)
            break;
    }
    return out;
}

void addRecentTranslationLanguage(Config &config, const QString &code)
{
    const QString c = normalizeLanguageCode(code);
    if (c.isEmpty())
        return;
    QStringList recent = recentTranslationLanguages(config);
    recent.removeAll(c);
    recent.prepend(c);
    while (recent.size() > kMaxRecentLanguages)
        recent.removeLast();
    if (recent != config.list(QStringLiteral("translate.recent")))
        config.setValue(QStringLiteral("translate.recent"), recent);
}

}  // namespace selsearch
