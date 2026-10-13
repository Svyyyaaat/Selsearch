// SPDX-License-Identifier: MIT
#include "OcrEngine.h"

#include <algorithm>

namespace selsearch {

QString OcrLine::text() const
{
    QStringList parts;
    parts.reserve(words.size());
    for (const auto &w : words)
        parts << w.text;
    return parts.join(QLatin1Char(' '));
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
        for (int k = i; k < j; ++k)
            out << lines[k];
        i = j;
    }
    lines = std::move(out);
}

}  // namespace selsearch
