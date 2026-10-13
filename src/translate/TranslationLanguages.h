// SPDX-License-Identifier: MIT
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace selsearch {

class Config;

struct TranslationLanguage {
    QString code;         // ISO 639-1
    QString englishName;  // "German"
    QString nativeName;   // "Deutsch"
};

// Target languages offered in the language picker, sorted by English name.
const QList<TranslationLanguage> &translationLanguages();
std::optional<TranslationLanguage> translationLanguage(const QString &code);
// English name for a code, or the code itself when unknown.
QString languageEnglishName(const QString &code);

// "de-AT" -> "de", "deu"/"ger" -> "de", "iw" -> "he", "zh-Hans" -> "zh".
// Returns an empty string for "", "auto" and garbage.
QString normalizeLanguageCode(const QString &code);

// First UI language of the system that we can translate into, else "en".
QString systemLanguageCode();
// translate.target, or systemLanguageCode() when empty.
QString effectiveTargetLanguage(const Config &config);

// Recently used target languages, most recent first (config translate.recent, max 3).
constexpr int kMaxRecentLanguages = 3;
QStringList recentTranslationLanguages(const Config &config);
void addRecentTranslationLanguage(Config &config, const QString &code);

}  // namespace selsearch
