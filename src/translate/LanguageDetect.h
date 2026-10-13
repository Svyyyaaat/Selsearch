// SPDX-License-Identifier: MIT
#pragma once

#include <QString>
#include <QStringList>

namespace selsearch {

struct DetectedLanguage {
    QString code;             // ISO 639-1, empty when the text has no letters
    double confidence = 0.0;  // 0..1
    bool isValid() const { return !code.isEmpty(); }
};

// Small offline language guesser, good enough to pick a translation model:
//  - non-Latin scripts by script, refined by characteristic letters
//    (Cyrillic: uk/be/sr/mk/bg/kk/ru; CJK: ja/ko/zh; Arabic: fa/ur/ar; he, el,
//    th, hi, ka, hy, bn, ta, ...);
//  - Latin script by stopwords + characteristic letters + a few trigrams:
//    en de fr es it pt nl pl cs sv tr vi (and a little ro hu fi da).
// Latin text with no evidence at all is reported as "en" with low confidence.
DetectedLanguage detectLanguage(const QString &text);

// Detects over the concatenation of all segments.
DetectedLanguage detectLanguage(const QStringList &segments);

}  // namespace selsearch
