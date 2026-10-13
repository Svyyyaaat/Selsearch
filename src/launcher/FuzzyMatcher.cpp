// SPDX-License-Identifier: MIT
#include "FuzzyMatcher.h"

#include <QVarLengthArray>

#include <algorithm>

namespace selsearch {

QString foldForMatch(QStringView text)
{
    bool ascii = true;
    for (QChar c : text) {
        if (c.unicode() >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii)
        return text.toString().toLower();

    // Decompose, drop combining marks, then case-fold. A few letters have no
    // decomposition but an obvious ASCII spelling.
    const QString decomposed = text.toString().normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (QChar c : decomposed) {
        switch (c.category()) {
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
            continue;
        default:
            break;
        }
        switch (c.unicode()) {
        case 0x00DF: out += QStringLiteral("ss"); continue;  // ß
        case 0x00E6: case 0x00C6: out += QStringLiteral("ae"); continue;
        case 0x0153: case 0x0152: out += QStringLiteral("oe"); continue;
        case 0x00F8: case 0x00D8: out += QLatin1Char('o'); continue;
        case 0x0142: case 0x0141: out += QLatin1Char('l'); continue;
        case 0x0111: case 0x0110: out += QLatin1Char('d'); continue;
        case 0x0131: out += QLatin1Char('i'); continue;  // dotless i
        case 0x2014: case 0x2013: out += QLatin1Char('-'); continue;
        default: break;
        }
        out += c.toCaseFolded();
    }
    return out;
}

static bool isSeparator(QChar c)
{
    return !c.isLetterOrNumber();
}

static bool isBoundary(QStringView text, qsizetype i)
{
    return i == 0 || isSeparator(text[i - 1]);
}

double fuzzyScore(QStringView pattern, QStringView text)
{
    const qsizetype m = pattern.size();
    const qsizetype n = text.size();
    if (m == 0)
        return 0;
    if (m > n)
        return -1;

    // Small length penalty so "Files" ranks over "Files and Folders Manager"
    // within the same tier, without ever crossing a tier.
    const double lengthPenalty = std::min<double>(60.0, double(n - m) * 1.5);

    if (n == m && pattern == text)
        return 1000;
    if (text.startsWith(pattern))
        return 900 - lengthPenalty;

    qsizetype first = -1;
    for (qsizetype pos = text.indexOf(pattern); pos >= 0; pos = text.indexOf(pattern, pos + 1)) {
        if (first < 0)
            first = pos;
        if (isBoundary(text, pos))
            return 700 - lengthPenalty - std::min<double>(40.0, double(pos));
    }
    if (first >= 0)
        return 500 - lengthPenalty - std::min<double>(40.0, double(first));

    // Subsequence. Forward pass finds the earliest end of a match, backward
    // pass from there finds the tightest start (the fzf v1 trick), then score
    // the chosen positions.
    qsizetype pi = 0;
    qsizetype end = -1;
    for (qsizetype i = 0; i < n; ++i) {
        if (text[i] == pattern[pi] && ++pi == m) {
            end = i;
            break;
        }
    }
    if (end < 0)
        return -1;
    // The backward pass yields the right-most alignment within the window
    // (which tends to land on word starts: "vsc" -> Visual Studio Code); the
    // forward rescan yields the left-most one. Score both, keep the better.
    QVarLengthArray<qsizetype, 32> backward(m);
    pi = m - 1;
    for (qsizetype i = end; i >= 0 && pi >= 0; --i) {
        if (text[i] == pattern[pi])
            backward[pi--] = i;
    }
    const qsizetype start = backward[0];
    QVarLengthArray<qsizetype, 32> forward(m);
    pi = 0;
    for (qsizetype i = start; i <= end && pi < m; ++i) {
        if (text[i] == pattern[pi])
            forward[pi++] = i;
    }

    auto scorePositions = [&](const QVarLengthArray<qsizetype, 32> &pos) {
        double score = 0;
        int run = 0;
        for (qsizetype k = 0; k < m; ++k) {
            const qsizetype i = pos[k];
            double s = 10;
            if (isBoundary(text, i))
                s += 12;
            if (k > 0 && i == pos[k - 1] + 1) {
                ++run;
                s += 6 + std::min(run, 4) * 2;
            } else {
                run = 0;
                if (k > 0)
                    s -= std::min<double>(12.0, 2.0 + double(i - pos[k - 1] - 1) * 0.5);
            }
            score += s;
        }
        return score;
    };
    double score = std::max(scorePositions(forward), scorePositions(backward));
    // Normalise by pattern length so long patterns don't escape the tier.
    score = 100 + score * (8.0 / std::max<qsizetype>(m, 8));
    score -= std::min<double>(30.0, double(start) * 0.5) + lengthPenalty * 0.5;
    return std::clamp(score, 1.0, 450.0);
}

}  // namespace selsearch
