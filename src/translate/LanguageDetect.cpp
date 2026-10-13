// SPDX-License-Identifier: MIT
#include "LanguageDetect.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>

namespace selsearch {

namespace {

struct LatinProfile {
    const char *code;
    const char *stopwords;  // space separated, UTF-8
    const char *letters;    // characteristic letters (UTF-8), each adds kLetterWeight
    const char *trigrams;   // space separated, UTF-8; matched inside words
};

constexpr double kStopwordWeight = 1.0;
constexpr double kLetterWeight = 0.6;
constexpr double kTrigramWeight = 0.25;

// Stopwords are the most frequent function words; characteristic letters are
// the ones a language uses and its look-alikes mostly do not.
const std::array<LatinProfile, 16> kLatin = {{
    {"en",
     "the and of to is in that it for with was on are this be you have not at by from they we an or which has "
     "will can what there their were been would do does i he she my your our it's don't hello hi please thanks "
     "thank all but if so about just more out up new get how when who why where here",
     "", "the ing and ion tio ent her hat thi wit"},
    {"de",
     "der die das und ist nicht ein eine einen zu den von mit sich des auf für im dem auch es ich wir sie sind "
     "wird oder aber wie nach bei noch kann über nur werden hat haben war mehr diese dieser",
     "äöüß", "sch ein ich und che ung eit cht den gen"},
    {"fr",
     "le la les et est un une des du de que qui dans pour pas ne sur au avec ce il elle nous vous sont mais ou "
     "plus par cette être à aux ces leur je c'est n'est l'",
     "éèêàçùâîôœë", "ent les que ais eau ion our tio ell eur"},
    {"es",
     "el la los las y es un una de que en por con para no del se al lo como más pero su está son muy este esta "
     "también hay ser fue han sus le me mi yo",
     "ñ¿¡áíóú", "que ión ado ent los las con aci era nte"},
    {"it",
     "il lo la gli le e è un una di che in per con non del della dei delle si sono ma come più questo questa "
     "anche al nel alla ho ci sei io mi ha hanno essere",
     "àèìòù", "che zio ell gli are ent one ion del lla"},
    {"pt",
     "o a os as e é um uma de que em para com não do da dos das no na se por mais mas como são está também ao "
     "você muito foi tem eu ele ela seu sua",
     "ãõçâêôáéíóú", "ção ões nho lho que ent ado ica men ara"},
    {"nl",
     "de het een en is van dat niet in op te met voor zijn er maar ook aan als bij wordt nog dit die wat hij ze "
     "wij heeft naar zich worden geen kan door",
     "", "ijk aar oor een sch van gen der ver oed"},
    {"pl",
     "i w na z nie się jest to że do o jak ale co tak od po przez dla są jego już czy tylko może który która "
     "oraz było jak ich jej mnie też",
     "ąęłśźżńćó", "nie prz ych owa ego wie ani rze czy szy"},
    {"cs",
     "a v na se je že s z do o to jak ale by jsou není jsem pro tak které který také nebo jeho být už při od po "
     "jsme bylo jako jen když",
     "řěůčšžýáíéďťň", "pro ost ení ých ova ter sti ske nos pře"},
    {"sv",
     "och i att det som en ett är på för med av till den inte har de om jag vi var men så kan eller från vid ska "
     "hon han sig mycket",
     "åäö", "och för att det som ade nde ing ter lig"},
    {"tr",
     "ve bir bu da de için ile çok ne gibi daha olarak ama var yok ben sen biz siz değil mi her kadar sonra "
     "olan bunu ki şey",
     "ğışçöüİ", "lar ler bir ini eri yor ası dir ara mak"},
    {"vi",
     "và của là có không được trong cho một những các với này người đã để khi từ như đến tôi bạn rất cũng sẽ "
     "nhiều làm",
     "ơưđăạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ", "ngư ông ười ành ước"},
    {"ro",
     "și în de la cu nu este o un pe care mai sau sunt din pentru ce se a fost dar", "ăâîșțşţ",
     "are ului ție ate est"},
    {"hu",
     "a az és hogy nem is egy van meg de ez csak már mint vagy volt kell még ha el", "őűáéíóöú",
     "egy ett tet nak nek ség"},
    {"fi",
     "ja on ei se että oli hän ovat kun mutta tai ole myös niin kuin joka mitä tämä", "äö",
     "ise sta ist tta lla ssa"},
    {"da",
     "og i at det er en til på som med for af den ikke har de jeg vi var men så kan eller fra ved skal", "æøå",
     "der ede ige for lig"},
}};

struct Prepared {
    QString code;
    QSet<QString> stopwords;
    QHash<uint, double> letters;
    QStringList trigrams;
};

const QList<Prepared> &prepared()
{
    static const QList<Prepared> list = [] {
        QList<Prepared> out;
        for (const auto &p : kLatin) {
            Prepared pr;
            pr.code = QString::fromLatin1(p.code);
            for (const QString &w : QString::fromUtf8(p.stopwords).split(QLatin1Char(' '), Qt::SkipEmptyParts))
                pr.stopwords.insert(w);
            for (uint c : QString::fromUtf8(p.letters).toLower().toUcs4())
                pr.letters.insert(c, kLetterWeight);
            pr.trigrams = QString::fromUtf8(p.trigrams).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            out << pr;
        }
        return out;
    }();
    return list;
}

DetectedLanguage scoreLatin(const QString &lower)
{
    // Tokenise into words (letters and apostrophes).
    QStringList words;
    QString cur;
    for (const QChar c : lower) {
        if (c.isLetter() || c == QLatin1Char('\'') || c == QChar(0x2019)) {
            cur += c == QChar(0x2019) ? QLatin1Char('\'') : c;
        } else if (!cur.isEmpty()) {
            words << cur;
            cur.clear();
        }
    }
    if (!cur.isEmpty())
        words << cur;

    const QList<uint> chars = lower.toUcs4();
    struct Score {
        QString code;
        double score;
    };
    QList<Score> scores;
    for (const Prepared &p : prepared()) {
        double s = 0;
        for (const QString &w : words) {
            if (p.stopwords.contains(w))
                s += kStopwordWeight;
            else if (w.contains(QLatin1Char('\''))) {
                // "c'est", "l'homme": match the elided prefix too.
                const QString head = w.section(QLatin1Char('\''), 0, 0) + QLatin1Char('\'');
                if (p.stopwords.contains(head) || p.stopwords.contains(w.section(QLatin1Char('\''), 0, 0)))
                    s += kStopwordWeight * 0.5;
            }
        }
        double letterScore = 0;
        for (uint c : chars)
            letterScore += p.letters.value(c, 0.0);
        s += std::min(letterScore, 6.0 + words.size() * 0.3);
        for (const QString &w : words) {
            if (w.size() < 3)
                continue;
            for (const QString &t : p.trigrams)
                if (w.contains(t))
                    s += kTrigramWeight;
        }
        scores.append({p.code, s});
    }
    std::stable_sort(scores.begin(), scores.end(), [](const Score &a, const Score &b) { return a.score > b.score; });
    const double best = scores[0].score;
    const double second = scores.size() > 1 ? scores[1].score : 0.0;
    if (best <= 0.0)
        return {QStringLiteral("en"), 0.1};
    const double margin = (best - second) / best;
    const double evidence = std::min(1.0, best / 4.0);
    return {scores[0].code, std::clamp(evidence * (0.4 + 0.6 * margin), 0.05, 0.99)};
}

bool in(uint c, std::initializer_list<uint> set) { return std::find(set.begin(), set.end(), c) != set.end(); }

DetectedLanguage detectCyrillic(const QString &lower, const QList<uint> &chars, int total)
{
    int uk = 0, iDot = 0, be = 0, sr = 0, mk = 0, kk = 0, kyMn = 0, ng = 0, hardSign = 0, ruOnly = 0;
    for (uint c : chars) {
        if (in(c, {0x457, 0x454, 0x491}))  // ї є ґ
            ++uk;
        else if (c == 0x456)  // і
            ++iDot;
        else if (c == 0x45E)  // ў
            ++be;
        else if (in(c, {0x452, 0x45B}))  // ђ ћ
            sr += 2;
        else if (in(c, {0x45F, 0x458, 0x459, 0x45A}))  // џ ј љ њ (sr + mk)
            ++sr;
        else if (in(c, {0x453, 0x45C, 0x455}))  // ѓ ќ ѕ
            ++mk;
        else if (in(c, {0x4D9, 0x493, 0x49B, 0x4B1}))  // ә ғ қ ұ
            ++kk;
        else if (in(c, {0x4E9, 0x4AF}))  // ө ү
            ++kyMn;
        else if (c == 0x4A3)  // ң
            ++ng;
        else if (c == 0x44A)  // ъ
            ++hardSign;
        else if (in(c, {0x44B, 0x44D, 0x451}))  // ы э ё
            ++ruOnly;
    }
    const double sure = 0.95;
    if (kk > 0)
        return {QStringLiteral("kk"), sure};
    if (kyMn > 0)
        return {ng > 0 ? QStringLiteral("ky") : QStringLiteral("mn"), 0.7};
    if (be > 0)
        return {QStringLiteral("be"), sure};
    if (uk > 0)
        return {QStringLiteral("uk"), sure};
    if (iDot > 0)
        return {ruOnly > 0 ? QStringLiteral("be") : QStringLiteral("uk"), 0.85};
    if (mk > 0)
        return {QStringLiteral("mk"), 0.9};
    if (sr > 0)
        return {QStringLiteral("sr"), 0.9};

    // Russian vs Bulgarian: ъ is common in Bulgarian, ы/э/ё do not exist there.
    static const QSet<QString> bgWords = {
        QStringLiteral("е"), QStringLiteral("са"), QStringLiteral("ще"), QStringLiteral("това"),
        QStringLiteral("които"), QStringLiteral("който"), QStringLiteral("която"), QStringLiteral("тя"),
        QStringLiteral("със"), QStringLiteral("към"), QStringLiteral("във"), QStringLiteral("беше"),
        QStringLiteral("бил"), QStringLiteral("но"), QStringLiteral("ли"), QStringLiteral("тези"),
        QStringLiteral("има"), QStringLiteral("много"), QStringLiteral("за"), QStringLiteral("от")};
    static const QSet<QString> ruWords = {
        QStringLiteral("что"), QStringLiteral("это"), QStringLiteral("как"), QStringLiteral("он"),
        QStringLiteral("она"), QStringLiteral("они"), QStringLiteral("был"), QStringLiteral("была"),
        QStringLiteral("было"), QStringLiteral("есть"), QStringLiteral("его"), QStringLiteral("только"),
        QStringLiteral("уже"), QStringLiteral("нет"), QStringLiteral("я"), QStringLiteral("так"),
        QStringLiteral("все"), QStringLiteral("мы"), QStringLiteral("вы"), QStringLiteral("из")};
    double bg = hardSign * (total > 0 && double(hardSign) / total > 0.01 ? 1.0 : 0.4);
    double ru = ruOnly * 1.5;
    for (const QString &w : lower.split(QRegularExpression(QStringLiteral("[^\\p{L}]+")), Qt::SkipEmptyParts)) {
        if (bgWords.contains(w))
            bg += 1;
        if (ruWords.contains(w))
            ru += 1;
    }
    if (bg > ru)
        return {QStringLiteral("bg"), std::clamp(0.5 + 0.1 * (bg - ru), 0.5, 0.9)};
    return {QStringLiteral("ru"), ru > 0 ? std::clamp(0.6 + 0.1 * (ru - bg), 0.6, 0.95) : 0.6};
}

DetectedLanguage detectArabic(const QList<uint> &chars)
{
    int fa = 0, ur = 0, arOnly = 0, faForms = 0;
    for (uint c : chars) {
        if (in(c, {0x67E, 0x686, 0x698, 0x6AF}))  // پ چ ژ گ
            ++fa;
        else if (in(c, {0x679, 0x688, 0x691, 0x6BA, 0x6D2, 0x6C1}))  // ٹ ڈ ڑ ں ے ہ
            ++ur;
        else if (in(c, {0x6A9, 0x6CC}))  // ک ی (Persian / Urdu forms)
            ++faForms;
        else if (in(c, {0x629, 0x64A, 0x643}))  // ة ي ك
            ++arOnly;
    }
    if (ur > 0)
        return {QStringLiteral("ur"), 0.85};
    if (fa > 0 || faForms > arOnly)
        return {QStringLiteral("fa"), fa > 0 ? 0.9 : 0.7};
    return {QStringLiteral("ar"), 0.9};
}

}  // namespace

DetectedLanguage detectLanguage(const QString &text)
{
    const QString lower = text.toLower();
    const QList<uint> chars = lower.toUcs4();

    QHash<int, int> scripts;
    int letters = 0;
    int kana = 0, hangul = 0, han = 0;
    for (uint c : chars) {
        if (!QChar::isLetter(c))
            continue;
        ++letters;
        const QChar::Script s = QChar::script(c);
        switch (s) {
        case QChar::Script_Hiragana:
        case QChar::Script_Katakana:
            ++kana;
            ++scripts[QChar::Script_Han];  // count CJK as one script
            break;
        case QChar::Script_Hangul:
            ++hangul;
            ++scripts[QChar::Script_Han];
            break;
        case QChar::Script_Han:
            ++han;
            ++scripts[QChar::Script_Han];
            break;
        default:
            ++scripts[s];
        }
    }
    if (letters == 0)
        return {};

    int dominant = QChar::Script_Unknown;
    int count = 0;
    for (auto it = scripts.cbegin(); it != scripts.cend(); ++it) {
        if (it.value() > count) {
            count = it.value();
            dominant = it.key();
        }
    }
    const double share = double(count) / letters;
    auto withShare = [share](DetectedLanguage d) {
        d.confidence = std::clamp(d.confidence * (0.5 + 0.5 * share), 0.0, 0.99);
        return d;
    };

    switch (dominant) {
    case QChar::Script_Latin:
        return withShare(scoreLatin(lower));
    case QChar::Script_Cyrillic:
        return withShare(detectCyrillic(lower, chars, count));
    case QChar::Script_Han:
        if (kana > 0 && kana * 20 >= count && kana >= hangul)
            return withShare({QStringLiteral("ja"), 0.95});
        if (hangul > 0 && hangul >= han)
            return withShare({QStringLiteral("ko"), 0.95});
        return withShare({QStringLiteral("zh"), 0.9});
    case QChar::Script_Arabic:
        return withShare(detectArabic(chars));
    case QChar::Script_Hebrew:
        return withShare({QStringLiteral("he"), 0.9});
    case QChar::Script_Greek:
        return withShare({QStringLiteral("el"), 0.95});
    case QChar::Script_Thai:
        return withShare({QStringLiteral("th"), 0.95});
    case QChar::Script_Devanagari:
        return withShare({QStringLiteral("hi"), 0.8});
    case QChar::Script_Georgian:
        return withShare({QStringLiteral("ka"), 0.95});
    case QChar::Script_Armenian:
        return withShare({QStringLiteral("hy"), 0.95});
    case QChar::Script_Bengali:
        return withShare({QStringLiteral("bn"), 0.9});
    case QChar::Script_Tamil:
        return withShare({QStringLiteral("ta"), 0.95});
    case QChar::Script_Telugu:
        return withShare({QStringLiteral("te"), 0.95});
    case QChar::Script_Kannada:
        return withShare({QStringLiteral("kn"), 0.95});
    case QChar::Script_Malayalam:
        return withShare({QStringLiteral("ml"), 0.95});
    case QChar::Script_Gujarati:
        return withShare({QStringLiteral("gu"), 0.95});
    case QChar::Script_Gurmukhi:
        return withShare({QStringLiteral("pa"), 0.95});
    case QChar::Script_Khmer:
        return withShare({QStringLiteral("km"), 0.95});
    case QChar::Script_Lao:
        return withShare({QStringLiteral("lo"), 0.95});
    case QChar::Script_Ethiopic:
        return withShare({QStringLiteral("am"), 0.8});
    case QChar::Script_Myanmar:
        return withShare({QStringLiteral("my"), 0.9});
    case QChar::Script_Sinhala:
        return withShare({QStringLiteral("si"), 0.95});
    default:
        return {};
    }
}

DetectedLanguage detectLanguage(const QStringList &segments)
{
    return detectLanguage(segments.join(QLatin1Char('\n')));
}

}  // namespace selsearch
