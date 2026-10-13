// SPDX-License-Identifier: MIT
#pragma once

#include <QString>
#include <QStringView>

namespace selsearch {

// Lower-case and strip diacritics ("Chromé" -> "chrome", "Straße" -> "strasse")
// so matching is case- and accent-insensitive. Cheap for ASCII input.
QString foldForMatch(QStringView text);

// fzf-like scoring of `pattern` against `text`. Both must already be folded
// with foldForMatch(). Returns a negative value when there is no match.
// Tiers, best first, so a better tier always beats a worse one:
//   exact (1000) > prefix (~900) > word-boundary substring (~700)
//   > substring (~500) > subsequence (100..450, more for word starts and
//   consecutive runs, less for gaps).
// Within a tier shorter texts score slightly higher.
double fuzzyScore(QStringView pattern, QStringView text);

// Convenience: folds both sides first.
inline double fuzzyScoreRaw(const QString &pattern, const QString &text)
{
    return fuzzyScore(foldForMatch(pattern), foldForMatch(text));
}

}  // namespace selsearch
