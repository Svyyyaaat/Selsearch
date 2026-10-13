// SPDX-License-Identifier: MIT
#include "Calculator.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>

#include <cmath>
#include <numbers>

namespace selsearch::calc {

namespace {

// ---------------------------------------------------------------------------
// Tokens

struct Token {
    enum Type { Number, Ident, Op, LParen, RParen, Comma, End };
    Type type = End;
    double number = 0;
    bool radixLiteral = false;  // 0x / 0b / 0o
    QString text;               // identifier name
    QChar op;                   // + - * / % ^
};

bool isIdentStart(QChar c)
{
    return c.isLetter() || c == QChar(0x00B0) /*°*/ || c == QChar(0x00B5) /*µ*/;
}

std::optional<QList<Token>> tokenize(QStringView s)
{
    QList<Token> out;
    qsizetype i = 0;
    const qsizetype n = s.size();
    while (i < n) {
        const QChar c = s[i];
        if (c.isSpace()) {
            ++i;
            continue;
        }
        Token t;
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && s[i + 1].isDigit())) {
            t.type = Token::Number;
            // Radix literals.
            if (c == QLatin1Char('0') && i + 2 < n) {
                const QChar p = s[i + 1].toLower();
                int base = p == QLatin1Char('x') ? 16 : p == QLatin1Char('b') ? 2 : p == QLatin1Char('o') ? 8 : 0;
                if (base) {
                    qsizetype j = i + 2;
                    while (j < n && (s[j].isLetterOrNumber()))
                        ++j;
                    bool ok = false;
                    const qulonglong v = s.mid(i + 2, j - i - 2).toULongLong(&ok, base);
                    if (!ok)
                        return std::nullopt;
                    t.number = double(v);
                    t.radixLiteral = true;
                    out << t;
                    i = j;
                    continue;
                }
            }
            qsizetype j = i;
            while (j < n && s[j].isDigit())
                ++j;
            if (j < n && s[j] == QLatin1Char('.')) {
                ++j;
                while (j < n && s[j].isDigit())
                    ++j;
            }
            // Exponent only when digits follow, so "2e" stays 2·e.
            if (j < n && (s[j] == QLatin1Char('e') || s[j] == QLatin1Char('E'))) {
                qsizetype k = j + 1;
                if (k < n && (s[k] == QLatin1Char('+') || s[k] == QLatin1Char('-')))
                    ++k;
                if (k < n && s[k].isDigit()) {
                    while (k < n && s[k].isDigit())
                        ++k;
                    j = k;
                }
            }
            bool ok = false;
            t.number = s.mid(i, j - i).toDouble(&ok);
            if (!ok)
                return std::nullopt;
            out << t;
            i = j;
            continue;
        }
        if (isIdentStart(c)) {
            qsizetype j = i + 1;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('_')))
                ++j;
            t.type = Token::Ident;
            t.text = s.mid(i, j - i).toString().toLower();
            out << t;
            i = j;
            continue;
        }
        switch (c.unicode()) {
        case '+': case '-': case '/': case '%': case '^':
            t.type = Token::Op;
            t.op = c;
            break;
        case '*':
            t.type = Token::Op;
            t.op = QLatin1Char('*');
            if (i + 1 < n && s[i + 1] == QLatin1Char('*')) {  // ** = ^
                t.op = QLatin1Char('^');
                ++i;
            }
            break;
        case 0x00D7: case 0x00B7: case 0x22C5:  // × · ⋅
            t.type = Token::Op;
            t.op = QLatin1Char('*');
            break;
        case 0x00F7: case 0x2215:  // ÷ ∕
            t.type = Token::Op;
            t.op = QLatin1Char('/');
            break;
        case 0x2212: case 0x2013:  // − –
            t.type = Token::Op;
            t.op = QLatin1Char('-');
            break;
        case 0x221A:  // √
            t.type = Token::Ident;
            t.text = QStringLiteral("sqrt");
            break;
        case 0x03C0:  // π
            t.type = Token::Ident;
            t.text = QStringLiteral("pi");
            break;
        case 0x03C4:  // τ
            t.type = Token::Ident;
            t.text = QStringLiteral("tau");
            break;
        case '(': t.type = Token::LParen; break;
        case ')': t.type = Token::RParen; break;
        case ',': t.type = Token::Comma; break;
        default:
            return std::nullopt;
        }
        out << t;
        ++i;
    }
    out << Token{};
    return out;
}

// ---------------------------------------------------------------------------
// Parser

struct Value {
    double v = 0;
    bool barePercent = false;  // "b%" with nothing else, for a ± b%
};

using Fn1 = double (*)(double);

const QHash<QString, Fn1> &unaryFunctions()
{
    static const QHash<QString, Fn1> f = {
        {QStringLiteral("sqrt"), [](double x) { return std::sqrt(x); }},
        {QStringLiteral("cbrt"), [](double x) { return std::cbrt(x); }},
        {QStringLiteral("sin"), [](double x) { return std::sin(x); }},
        {QStringLiteral("cos"), [](double x) { return std::cos(x); }},
        {QStringLiteral("tan"), [](double x) { return std::tan(x); }},
        {QStringLiteral("asin"), [](double x) { return std::asin(x); }},
        {QStringLiteral("acos"), [](double x) { return std::acos(x); }},
        {QStringLiteral("atan"), [](double x) { return std::atan(x); }},
        {QStringLiteral("ln"), [](double x) { return std::log(x); }},
        {QStringLiteral("log"), [](double x) { return std::log10(x); }},
        {QStringLiteral("log2"), [](double x) { return std::log2(x); }},
        {QStringLiteral("log10"), [](double x) { return std::log10(x); }},
        {QStringLiteral("exp"), [](double x) { return std::exp(x); }},
        {QStringLiteral("abs"), [](double x) { return std::fabs(x); }},
        {QStringLiteral("round"), [](double x) { return std::round(x); }},
        {QStringLiteral("floor"), [](double x) { return std::floor(x); }},
        {QStringLiteral("ceil"), [](double x) { return std::ceil(x); }},
    };
    return f;
}

const QHash<QString, double> &constants()
{
    static const QHash<QString, double> c = {
        {QStringLiteral("pi"), std::numbers::pi},
        {QStringLiteral("e"), std::numbers::e},
        {QStringLiteral("tau"), 2 * std::numbers::pi},
    };
    return c;
}

class Parser {
public:
    explicit Parser(QList<Token> tokens)
        : m_t(std::move(tokens))
    {
    }

    std::optional<double> parseAll()
    {
        auto v = expr();
        if (!v || !m_ok || peek().type != Token::End)
            return std::nullopt;
        return v->v;
    }
    int operations() const { return m_ops; }
    bool sawRadixLiteral() const { return m_radix; }

private:
    const Token &peek(int ahead = 0) const { return m_t[std::min<qsizetype>(m_pos + ahead, m_t.size() - 1)]; }
    const Token &next() { return m_t[std::min<qsizetype>(m_pos++, m_t.size() - 1)]; }
    bool isOp(QChar op, int ahead = 0) const { return peek(ahead).type == Token::Op && peek(ahead).op == op; }
    bool startsOperand(const Token &t) const
    {
        return t.type == Token::Number || t.type == Token::LParen
            || (t.type == Token::Ident && t.text != QLatin1String("of"));
    }
    bool fail()
    {
        m_ok = false;
        return false;
    }

    std::optional<Value> expr()
    {
        auto lhs = term();
        if (!lhs)
            return std::nullopt;
        Value acc{lhs->v, lhs->barePercent};
        while (isOp(QLatin1Char('+')) || isOp(QLatin1Char('-'))) {
            const bool minus = next().op == QLatin1Char('-');
            auto rhs = term();
            if (!rhs)
                return std::nullopt;
            ++m_ops;
            if (rhs->barePercent)  // 200 + 10% = 220
                acc.v = minus ? acc.v * (1 - rhs->v) : acc.v * (1 + rhs->v);
            else
                acc.v = minus ? acc.v - rhs->v : acc.v + rhs->v;
            acc.barePercent = false;
        }
        return acc;
    }

    std::optional<Value> term()
    {
        auto lhs = unary();
        if (!lhs)
            return std::nullopt;
        Value acc = *lhs;
        for (;;) {
            const Token &t = peek();
            QChar op;
            if (t.type == Token::Op && (t.op == QLatin1Char('*') || t.op == QLatin1Char('/') || t.op == QLatin1Char('%'))) {
                op = next().op;
            } else if (t.type == Token::Ident && t.text == QLatin1String("of")) {
                next();
                op = QLatin1Char('*');  // 20% of 150
            } else if (t.type == Token::Ident || t.type == Token::LParen) {
                op = QLatin1Char('*');  // implicit: 2pi, 3(4+5), 2 sqrt(2)
            } else {
                break;
            }
            auto rhs = unary();
            if (!rhs)
                return std::nullopt;
            ++m_ops;
            if (op == QLatin1Char('*'))
                acc.v *= rhs->v;
            else if (op == QLatin1Char('/'))
                acc.v /= rhs->v;
            else
                acc.v = std::fmod(acc.v, rhs->v);
            acc.barePercent = false;
        }
        return acc;
    }

    std::optional<Value> unary()
    {
        if (isOp(QLatin1Char('-')) || isOp(QLatin1Char('+'))) {
            const bool minus = next().op == QLatin1Char('-');
            auto v = unary();
            if (!v)
                return std::nullopt;
            if (minus)
                v->v = -v->v;
            return v;
        }
        return power();
    }

    std::optional<Value> power()
    {
        auto base = postfix();
        if (!base)
            return std::nullopt;
        if (isOp(QLatin1Char('^'))) {
            next();
            auto exp = unary();
            if (!exp)
                return std::nullopt;
            ++m_ops;
            return Value{std::pow(base->v, exp->v), false};
        }
        return base;
    }

    std::optional<Value> postfix()
    {
        auto v = primary();
        if (!v)
            return std::nullopt;
        // '%' is a percent sign unless an operand follows (then it is modulo,
        // handled in term()).
        if (isOp(QLatin1Char('%')) && !startsOperand(peek(1))) {
            next();
            // A percent only counts as math together with another operation.
            return Value{v->v / 100.0, true};
        }
        return v;
    }

    std::optional<Value> primary()
    {
        const Token t = next();
        switch (t.type) {
        case Token::Number:
            m_radix = m_radix || t.radixLiteral;
            return Value{t.number, false};
        case Token::LParen: {
            auto v = expr();
            if (!v || next().type != Token::RParen) {
                fail();
                return std::nullopt;
            }
            return Value{v->v, false};
        }
        case Token::Ident: {
            if (constants().contains(t.text))
                return Value{constants().value(t.text), false};
            const bool isMinMax = t.text == QLatin1String("min") || t.text == QLatin1String("max");
            if (isMinMax) {
                if (next().type != Token::LParen) {
                    fail();
                    return std::nullopt;
                }
                QList<double> args;
                do {
                    auto a = expr();
                    if (!a)
                        return std::nullopt;
                    args << a->v;
                } while (peek().type == Token::Comma && (next(), true));
                if (next().type != Token::RParen) {
                    fail();
                    return std::nullopt;
                }
                ++m_ops;
                double r = args.first();
                for (double a : args)
                    r = t.text == QLatin1String("min") ? std::min(r, a) : std::max(r, a);
                return Value{r, false};
            }
            auto fn = unaryFunctions().constFind(t.text);
            if (fn == unaryFunctions().constEnd()) {
                fail();  // unknown word: not math
                return std::nullopt;
            }
            std::optional<Value> arg;
            if (peek().type == Token::LParen) {
                next();
                arg = expr();
                if (!arg || next().type != Token::RParen) {
                    fail();
                    return std::nullopt;
                }
            } else {
                arg = unary();  // sqrt 16, √2
                if (!arg)
                    return std::nullopt;
            }
            ++m_ops;
            return Value{(*fn)(arg->v), false};
        }
        default:
            fail();
            return std::nullopt;
        }
    }

    QList<Token> m_t;
    qsizetype m_pos = 0;
    bool m_ok = true;
    int m_ops = 0;
    bool m_radix = false;
};

struct EvalResult {
    double value;
    int ops;
    bool radix;
};

std::optional<EvalResult> evalExpression(QStringView s)
{
    auto tokens = tokenize(s);
    if (!tokens || tokens->size() <= 1)
        return std::nullopt;
    Parser p(std::move(*tokens));
    auto v = p.parseAll();
    if (!v || !std::isfinite(*v))
        return std::nullopt;
    return EvalResult{*v, p.operations(), p.sawRadixLiteral()};
}

// ---------------------------------------------------------------------------
// Units. Values convert to a base unit with base = (v + offset) * factor.

enum class Dim { Length, Mass, Temperature, Data, Time, Volume };

struct Unit {
    Dim dim;
    QString symbol;
    double factor;
    double offset = 0;
};

struct UnitTables {
    QHash<QString, Unit> exact;  // case-sensitive aliases
    QHash<QString, Unit> folded; // lower-case aliases
};

const UnitTables &units()
{
    static const UnitTables t = [] {
        UnitTables u;
        auto add = [&u](Dim dim, const char *symbol, double factor, std::initializer_list<const char *> aliases,
                        double offset = 0) {
            const Unit unit{dim, QString::fromUtf8(symbol), factor, offset};
            u.folded.insert(QString::fromUtf8(symbol).toLower(), unit);
            for (const char *a : aliases)
                u.folded.insert(QString::fromUtf8(a).toLower(), unit);
        };
        // Length (metre)
        add(Dim::Length, "nm", 1e-9, {"nanometer", "nanometers", "nanometre", "nanometres"});
        add(Dim::Length, "µm", 1e-6, {"um", "micrometer", "micrometers", "micron", "microns"});
        add(Dim::Length, "mm", 1e-3, {"millimeter", "millimeters", "millimetre", "millimetres"});
        add(Dim::Length, "cm", 1e-2, {"centimeter", "centimeters", "centimetre", "centimetres"});
        add(Dim::Length, "m", 1, {"meter", "meters", "metre", "metres"});
        add(Dim::Length, "km", 1e3, {"kilometer", "kilometers", "kilometre", "kilometres"});
        add(Dim::Length, "in", 0.0254, {"inch", "inches", "\""});
        add(Dim::Length, "ft", 0.3048, {"foot", "feet", "'"});
        add(Dim::Length, "yd", 0.9144, {"yard", "yards"});
        add(Dim::Length, "mi", 1609.344, {"mile", "miles"});
        add(Dim::Length, "nmi", 1852, {"nauticalmile", "nauticalmiles"});
        // Mass (kilogram)
        add(Dim::Mass, "mg", 1e-6, {"milligram", "milligrams"});
        add(Dim::Mass, "g", 1e-3, {"gram", "grams"});
        add(Dim::Mass, "kg", 1, {"kilogram", "kilograms", "kilo", "kilos"});
        add(Dim::Mass, "t", 1e3, {"tonne", "tonnes"});
        add(Dim::Mass, "oz", 0.028349523125, {"ounce", "ounces"});
        add(Dim::Mass, "lb", 0.45359237, {"lbs", "pound", "pounds"});
        add(Dim::Mass, "st", 6.35029318, {"stone", "stones"});
        // Temperature (kelvin)
        add(Dim::Temperature, "°C", 1, {"c", "celsius", "degc", "°c"}, 273.15);
        add(Dim::Temperature, "°F", 5.0 / 9.0, {"f", "fahrenheit", "degf", "°f"}, 459.67);
        add(Dim::Temperature, "K", 1, {"k", "kelvin", "kelvins"});
        // Data (byte). Lower-case kb/mb/gb mean bytes, as people type them.
        add(Dim::Data, "B", 1, {"b", "byte", "bytes"});
        add(Dim::Data, "bit", 0.125, {"bits"});
        add(Dim::Data, "kB", 1e3, {"kb", "kilobyte", "kilobytes"});
        add(Dim::Data, "MB", 1e6, {"mb", "megabyte", "megabytes"});
        add(Dim::Data, "GB", 1e9, {"gb", "gigabyte", "gigabytes"});
        add(Dim::Data, "TB", 1e12, {"tb", "terabyte", "terabytes"});
        add(Dim::Data, "PB", 1e15, {"pb", "petabyte", "petabytes"});
        add(Dim::Data, "KiB", 1024.0, {"kib", "kibibyte", "kibibytes"});
        add(Dim::Data, "MiB", 1024.0 * 1024, {"mib", "mebibyte", "mebibytes"});
        add(Dim::Data, "GiB", 1024.0 * 1024 * 1024, {"gib", "gibibyte", "gibibytes"});
        add(Dim::Data, "TiB", 1024.0 * 1024 * 1024 * 1024, {"tib", "tebibyte", "tebibytes"});
        add(Dim::Data, "kbit", 125, {"kbits", "kilobit", "kilobits"});
        add(Dim::Data, "Mbit", 125e3, {"mbits", "megabit", "megabits"});
        add(Dim::Data, "Gbit", 125e6, {"gbits", "gigabit", "gigabits"});
        // Exact-case bit units: "Mb" is megabits only when typed like that.
        u.exact.insert(QStringLiteral("Kb"), Unit{Dim::Data, QStringLiteral("kbit"), 125});
        u.exact.insert(QStringLiteral("Mb"), Unit{Dim::Data, QStringLiteral("Mbit"), 125e3});
        u.exact.insert(QStringLiteral("Gb"), Unit{Dim::Data, QStringLiteral("Gbit"), 125e6});
        u.exact.insert(QStringLiteral("Tb"), Unit{Dim::Data, QStringLiteral("Tbit"), 125e9});
        // Time (second)
        add(Dim::Time, "ms", 1e-3, {"millisecond", "milliseconds", "msec"});
        add(Dim::Time, "s", 1, {"sec", "secs", "second", "seconds"});
        add(Dim::Time, "min", 60, {"mins", "minute", "minutes"});
        add(Dim::Time, "h", 3600, {"hr", "hrs", "hour", "hours"});
        add(Dim::Time, "d", 86400, {"day", "days"});
        add(Dim::Time, "wk", 604800, {"week", "weeks"});
        add(Dim::Time, "mo", 2629746, {"month", "months"});  // 1/12 Gregorian year
        add(Dim::Time, "yr", 31556952, {"y", "year", "years"});
        // Volume (litre)
        add(Dim::Volume, "ml", 1e-3, {"milliliter", "milliliters", "millilitre", "millilitres"});
        add(Dim::Volume, "l", 1, {"liter", "liters", "litre", "litres"});
        add(Dim::Volume, "gal", 3.785411784, {"gallon", "gallons"});
        add(Dim::Volume, "qt", 0.946352946, {"quart", "quarts"});
        add(Dim::Volume, "pt", 0.473176473, {"pint", "pints"});
        add(Dim::Volume, "cup", 0.2365882365, {"cups"});
        add(Dim::Volume, "floz", 0.0295735295625, {"fl.oz"});
        add(Dim::Volume, "tbsp", 0.01478676478125, {"tablespoon", "tablespoons"});
        add(Dim::Volume, "tsp", 0.00492892159375, {"teaspoon", "teaspoons"});
        return u;
    }();
    return t;
}

std::optional<Unit> findUnit(const QString &name)
{
    const UnitTables &u = units();
    auto e = u.exact.constFind(name);
    if (e != u.exact.constEnd())
        return *e;
    auto f = u.folded.constFind(name.toLower());
    if (f != u.folded.constEnd())
        return *f;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Formatting

QString trimZeros(QString s)
{
    if (s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

QString radixString(double v, int base)
{
    const bool neg = v < 0;
    const auto mag = qulonglong(std::fabs(v));
    QString prefix = base == 16 ? QStringLiteral("0x") : base == 2 ? QStringLiteral("0b") : base == 8 ? QStringLiteral("0o") : QString();
    return (neg ? QStringLiteral("-") : QString()) + prefix + QString::number(mag, base);
}

}  // namespace

QString formatPlain(double value, int significant)
{
    if (value == 0)
        return QStringLiteral("0");
    const double mag = std::fabs(value);
    if (mag >= 1e15 || mag < 1e-6)
        return QString::number(value, 'g', significant);
    const int intDigits = mag >= 1 ? int(std::floor(std::log10(mag))) + 1 : 0;
    int decimals = std::max(0, significant - intDigits);
    if (mag < 1)  // count leading zeros after the point as non-significant
        decimals = std::min(significant + int(-std::floor(std::log10(mag))) - 1, 20);
    return trimZeros(QString::number(value, 'f', decimals));
}

QString formatGrouped(double value, const QLocale &locale, int significant)
{
    QString plain = formatPlain(value, significant);
    if (plain.contains(QLatin1Char('e')))
        return plain;
    QString sign;
    if (plain.startsWith(QLatin1Char('-'))) {
        sign = locale.negativeSign();
        plain.remove(0, 1);
    }
    QString intPart = plain.section(QLatin1Char('.'), 0, 0);
    const QString frac = plain.section(QLatin1Char('.'), 1);
    const QString sep = locale.groupSeparator();
    for (qsizetype i = intPart.size() - 3; i > 0; i -= 3)
        intPart.insert(i, sep);
    return sign + intPart + (frac.isEmpty() ? QString() : locale.decimalPoint() + frac);
}

std::optional<Result> evaluate(const QString &rawInput, const QLocale &locale)
{
    QString input = rawInput.trimmed();
    if (input.startsWith(QLatin1Char('=')))
        input = input.mid(1).trimmed();
    if (input.endsWith(QLatin1Char('=')))
        input.chop(1);
    if (input.isEmpty() || input.size() > 256)
        return std::nullopt;
    // Dates and version-like strings are not subtraction or decimals.
    static const QRegularExpression notMath(QStringLiteral(R"(^\d{4}-\d{1,2}-\d{1,2}$|^\d+\.\d+\.\d+)"));
    if (notMath.match(input).hasMatch())
        return std::nullopt;

    Result r;

    // Conversions: "<lhs> (in|to|as|into) <rhs>", trying the rightmost
    // separator first so "5 in to cm" means inches.
    static const QRegularExpression sep(QStringLiteral(R"(\s+(?:in|to|as|into|->|→)(?=\s))"), QRegularExpression::CaseInsensitiveOption);
    QList<QRegularExpressionMatch> seps;
    for (auto it = sep.globalMatch(input); it.hasNext();)
        seps.prepend(it.next());
    for (const QRegularExpressionMatch &m : std::as_const(seps)) {
        const QString lhs = input.left(m.capturedStart()).trimmed();
        const QString rhs = input.mid(m.capturedEnd()).trimmed();
        const QString target = rhs.toLower();

        static const QHash<QString, int> bases = {
            {QStringLiteral("hex"), 16}, {QStringLiteral("hexadecimal"), 16},
            {QStringLiteral("dec"), 10}, {QStringLiteral("decimal"), 10},
            {QStringLiteral("bin"), 2}, {QStringLiteral("binary"), 2},
            {QStringLiteral("oct"), 8}, {QStringLiteral("octal"), 8},
        };
        if (bases.contains(target)) {
            auto v = evalExpression(lhs);
            if (!v || std::fabs(v->value) >= 9.2e18 || v->value != std::floor(v->value))
                return std::nullopt;
            const int base = bases.value(target);
            r.value = v->value;
            r.copyText = base == 10 ? QString::number(qlonglong(v->value)) : radixString(v->value, base);
            r.title = r.copyText;
            r.grouped = base == 10 ? formatGrouped(v->value, locale) : r.copyText;
            return r;
        }

        auto to = findUnit(rhs);
        if (!to)
            continue;
        // Split the trailing unit off the left side: "5 km", "100f", "3 GiB".
        static const QRegularExpression unitTail(QStringLiteral(R"(^(.*?)\s*([A-Za-z°µ"'][A-Za-z.°µ"']*)$)"));
        const auto um = unitTail.match(lhs);
        if (!um.hasMatch())
            continue;
        auto from = findUnit(um.captured(2));
        if (!from || from->dim != to->dim)
            continue;
        auto v = evalExpression(um.captured(1));
        if (!v)
            continue;
        const double base = (v->value + from->offset) * from->factor;
        const double out = base / to->factor - to->offset;
        if (!std::isfinite(out))
            return std::nullopt;
        r.value = out;
        r.unit = to->symbol;
        r.copyText = formatPlain(out);
        r.title = formatPlain(out, 10) + QLatin1Char(' ') + to->symbol;
        r.grouped = formatGrouped(out, locale, 10) + QLatin1Char(' ') + to->symbol;
        return r;
    }

    auto v = evalExpression(input);
    if (!v)
        return std::nullopt;
    // A lone number or constant is not a calculation, but a lone 0x/0b/0o
    // literal is answered with its decimal value.
    if (v->ops == 0 && !v->radix)
        return std::nullopt;
    r.value = v->value;
    r.copyText = formatPlain(v->value);
    r.title = formatPlain(v->value, 12);
    r.grouped = formatGrouped(v->value, locale);
    return r;
}

}  // namespace selsearch::calc
