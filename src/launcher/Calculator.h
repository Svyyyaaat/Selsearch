// SPDX-License-Identifier: MIT
#pragma once

#include <QLocale>
#include <QString>

#include <optional>

namespace selsearch::calc {

// A hand-written recursive-descent evaluator; nothing is ever passed to a
// script engine. Grammar (loosest binding first):
//   input   := expr | expr unit (in|to|as|into) unit | expr (in|to) (hex|dec|bin|oct)
//   expr    := term (('+'|'-') term)*         a ± b% means a·(1 ± b/100)
//   term    := unary (('*'|'/'|'%'|"of"|implicit) unary)*
//   unary   := ('-'|'+') unary | power
//   power   := postfix ('^' unary)?           right associative, -2^2 = -4
//   postfix := primary '%'?                   percent when no operand follows
//   primary := number | constant | func '(' args ')' | func unary | '(' expr ')'
// Numbers: 12, 1.5, .5, 1e-3, 0x1f, 0b101, 0o17. Constants: pi, e, tau.
// Functions: sqrt cbrt sin cos tan asin acos atan ln log log2 exp abs round
// floor ceil min max (radians for trig).
struct Result {
    double value = 0;
    QString unit;        // target unit symbol for conversions, else empty
    QString title;       // "3.106855961 mi", "0xff", "1234567.5"
    QString grouped;     // thousands-grouped value for the subtitle
    QString copyText;    // full precision, no grouping, no unit
};

// nullopt when `input` is not clearly math: a plain number, a lone word or
// constant, unknown identifiers, incomplete input, or a non-finite result.
std::optional<Result> evaluate(const QString &input, const QLocale &locale = QLocale());

// Thousands-grouped decimal for display ("1,234,567.89" in en).
QString formatGrouped(double value, const QLocale &locale, int significant = 12);
// Shortest exact-enough representation for copying (15 significant digits).
QString formatPlain(double value, int significant = 15);

}  // namespace selsearch::calc
