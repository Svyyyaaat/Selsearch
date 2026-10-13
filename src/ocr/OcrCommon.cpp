// SPDX-License-Identifier: MIT
#include "OcrEngine.h"

#include <QTextBoundaryFinder>

#include <algorithm>

#ifdef SELSEARCH_HAVE_ICU
#include <unicode/brkiter.h>
#include <unicode/locid.h>
#include <unicode/unistr.h>
#elif defined(SELSEARCH_HAVE_WINDOWS_ICU)
#include <icu.h>  // Windows 10 1903+: the C API of the system's icu.dll
#endif

namespace selsearch {

QString OcrLine::text() const
{
    QString out;
    for (const auto &w : words) {
        if (!out.isEmpty() && w.spaceBefore)
            out += QLatin1Char(' ');
        out += w.text;
    }
    return out;
}

QString OcrResult::text() const
{
    QStringList parts;
    parts.reserve(lines.size());
    for (const auto &l : lines)
        parts << l.text();
    return parts.join(QLatin1Char('\n'));
}

void sortReadingOrder(QList<OcrLine> &lines)
{
    for (auto &line : lines) {
        if (line.words.isEmpty())
            continue;
        QRectF box = line.words.first().box;
        for (const auto &w : line.words)
            box = box.united(w.box);
        line.box = box;
    }
    lines.removeIf([](const OcrLine &l) { return l.words.isEmpty(); });

    // Group lines into rows: two lines are on the same row when their vertical
    // centres are within half the smaller height. Rows go top-to-bottom, lines
    // inside a row go left-to-right (multi-column UIs keep columns side by side).
    std::stable_sort(lines.begin(), lines.end(), [](const OcrLine &a, const OcrLine &b) {
        return a.box.center().y() < b.box.center().y();
    });
    QList<OcrLine> out;
    out.reserve(lines.size());
    int i = 0;
    while (i < lines.size()) {
        int j = i + 1;
        const qreal cy = lines[i].box.center().y();
        while (j < lines.size()) {
            const qreal h = std::min(lines[i].box.height(), lines[j].box.height());
            if (std::abs(lines[j].box.center().y() - cy) > h * 0.5)
                break;
            ++j;
        }
        std::stable_sort(lines.begin() + i, lines.begin() + j, [](const OcrLine &a, const OcrLine &b) {
            return a.box.left() < b.box.left();
        });
        // Detectors often split one visual line at wide gaps ("Total ..... $7.50",
        // "$ cmd --flag"). Re-join pieces on the same row that are close
        // together; far-apart pieces (columns) stay separate lines.
        for (int k = i; k < j; ++k) {
            if (!out.isEmpty() && k > i) {
                OcrLine &prev = out.last();
                const qreal h = std::min(prev.box.height(), lines[k].box.height());
                const qreal gap = lines[k].box.left() - prev.box.right();
                if (gap < h * 2.5) {
                    prev.words += lines[k].words;
                    prev.box = prev.box.united(lines[k].box);
                    continue;
                }
            }
            out << lines[k];
        }
        i = j;
    }
    lines = std::move(out);
}

// ---- CJK word segmentation --------------------------------------------------

static bool isCjkChar(char32_t c)
{
    return (c >= 0x3040 && c <= 0x30FF)     // hiragana, katakana
        || (c >= 0x31F0 && c <= 0x31FF)     // katakana extensions
        || (c >= 0x3400 && c <= 0x4DBF)     // CJK ext. A
        || (c >= 0x4E00 && c <= 0x9FFF)     // CJK unified
        || (c >= 0xF900 && c <= 0xFAFF)     // compatibility ideographs
        || (c >= 0xFF66 && c <= 0xFF9F)     // half-width katakana
        || (c >= 0x20000 && c <= 0x2FA1F);  // ext. B+
}

bool containsCjk(QStringView text)
{
    for (char32_t c : text.toUcs4())
        if (isCjkChar(c))
            return true;
    return false;
}

// Relative advance of a character: full-width glyphs ~1, Latin/half-width ~0.55.
static qreal charWeight(QChar c)
{
    const char16_t u = c.unicode();
    if (c.isSurrogate())
        return 0.5;  // each half of a pair; together one full-width glyph
    if (isCjkChar(u) || (u >= 0x3000 && u <= 0x303F) || (u >= 0xFF01 && u <= 0xFF60) || (u >= 0xFFE0 && u <= 0xFFE6))
        return 1.0;
    if (c.isSpace())
        return 0.35;
    return 0.55;
}

struct Segment {
    int start;
    int length;
    bool isWord;  // false: punctuation / symbols (attached to the previous word)
};

static QList<Segment> wordSegments(const QString &text)
{
    QList<Segment> out;
#ifdef SELSEARCH_HAVE_ICU
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> it(icu::BreakIterator::createWordInstance(icu::Locale::getJapanese(), status));
    if (U_SUCCESS(status) && it) {
        const icu::UnicodeString ustr(reinterpret_cast<const char16_t *>(text.utf16()), int32_t(text.size()));
        it->setText(ustr);
        int32_t start = it->first();
        for (int32_t end = it->next(); end != icu::BreakIterator::DONE; start = end, end = it->next())
            out << Segment{start, end - start, it->getRuleStatus() != UBRK_WORD_NONE};
        return out;
    }
#elif defined(SELSEARCH_HAVE_WINDOWS_ICU)
    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator *it = ubrk_open(UBRK_WORD, "ja", reinterpret_cast<const UChar *>(text.utf16()), int32_t(text.size()), &status);
    if (U_SUCCESS(status) && it) {
        int32_t start = ubrk_first(it);
        for (int32_t end = ubrk_next(it); end != UBRK_DONE; start = end, end = ubrk_next(it))
            out << Segment{start, end - start, ubrk_getRuleStatus(it) != UBRK_WORD_NONE};
        ubrk_close(it);
        return out;
    }
    if (it)
        ubrk_close(it);
#endif
    // Fallback (UAX #29): katakana runs stay together, kanji split per character.
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    int start = 0;
    while (finder.toNextBoundary() != -1) {
        const int end = finder.position();
        if (end > start) {
            bool word = false;
            for (int i = start; i < end; ++i)
                word = word || text[i].isLetterOrNumber();
            out << Segment{start, end - start, word};
        }
        start = end;
    }
    return out;
}

void segmentCjkWords(QList<OcrLine> &lines)
{
    for (auto &line : lines) {
        QList<OcrWord> out;
        for (const auto &w : std::as_const(line.words)) {
            if (!containsCjk(w.text)) {
                out << w;
                continue;
            }
            // Merge punctuation and spaces into the preceding word.
            QList<Segment> segs;
            for (const auto &seg : wordSegments(w.text)) {
                const QString piece = w.text.mid(seg.start, seg.length);
                if (piece.trimmed().isEmpty() && !segs.isEmpty()) {
                    segs.last().length += seg.length;
                    continue;
                }
                if (!seg.isWord && !segs.isEmpty())
                    segs.last().length += seg.length;
                else
                    segs << seg;
            }
            // ICU's dictionary chops long katakana runs (loanwords, brand
            // names) into odd pieces; keep a katakana run together instead.
            auto allKatakana = [&](const Segment &sg) {
                for (int i = sg.start; i < sg.start + sg.length; ++i) {
                    const char16_t u = w.text[i].unicode();
                    if (!((u >= 0x30A0 && u <= 0x30FF) || (u >= 0x31F0 && u <= 0x31FF) || (u >= 0xFF66 && u <= 0xFF9F)))
                        return false;
                }
                return sg.length > 0;
            };
            QList<Segment> merged;
            for (const auto &sg : std::as_const(segs)) {
                if (!merged.isEmpty() && allKatakana(merged.last()) && allKatakana(sg)
                    && merged.last().start + merged.last().length == sg.start)
                    merged.last().length += sg.length;
                else
                    merged << sg;
            }
            segs = merged;
            if (segs.size() <= 1) {
                out << w;
                continue;
            }
            qreal total = 0;
            for (QChar c : w.text)
                total += charWeight(c);
            qreal x = w.box.left();
            const qreal unit = total > 0 ? w.box.width() / total : 0;
            for (int i = 0; i < segs.size(); ++i) {
                const QString piece = w.text.mid(segs[i].start, segs[i].length);
                qreal width = 0;
                for (QChar c : piece)
                    width += charWeight(c) * unit;
                OcrWord part;
                part.text = piece.trimmed();
                part.box = QRectF(x, w.box.top(), width, w.box.height());
                part.confidence = w.confidence;
                part.spaceBefore = i == 0 ? w.spaceBefore : piece.at(0).isSpace() || w.text.at(segs[i].start - 1).isSpace();
                x += width;
                if (!part.text.isEmpty())
                    out << part;
            }
        }
        line.words = out;
    }
}

}  // namespace selsearch
