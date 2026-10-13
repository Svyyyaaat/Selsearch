// SPDX-License-Identifier: MIT
#include "KeyNames.h"

#include <QStringList>

namespace selsearch {
namespace {

struct Parts {
    bool ctrl = false, alt = false, shift = false, meta = false;
    QString key;       // xkb keysym name ("space", "s", "F5")
    QString keyUpper;  // display / Hyprland style ("Space", "S", "F5")
};

Parts split(const QKeySequence &keys)
{
    Parts p;
    if (keys.isEmpty())
        return p;
    const QKeyCombination combo = keys[0];
    const auto mods = combo.keyboardModifiers();
    p.ctrl = mods & Qt::ControlModifier;
    p.alt = mods & Qt::AltModifier;
    p.shift = mods & Qt::ShiftModifier;
    p.meta = mods & Qt::MetaModifier;
    const Qt::Key key = combo.key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        p.key = QChar(QLatin1Char('a' + (key - Qt::Key_A)));
        p.keyUpper = p.key.toUpper();
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        p.key = p.keyUpper = QChar(QLatin1Char('0' + (key - Qt::Key_0)));
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        p.key = p.keyUpper = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    } else {
        static const QList<std::tuple<Qt::Key, const char *, const char *>> names = {
            {Qt::Key_Space, "space", "Space"},     {Qt::Key_Return, "Return", "Return"},
            {Qt::Key_Tab, "Tab", "Tab"},           {Qt::Key_Print, "Print", "Print"},
            {Qt::Key_Escape, "Escape", "Escape"},  {Qt::Key_Slash, "slash", "slash"},
            {Qt::Key_Period, "period", "period"},  {Qt::Key_Comma, "comma", "comma"},
            {Qt::Key_Semicolon, "semicolon", "semicolon"}, {Qt::Key_Minus, "minus", "minus"},
            {Qt::Key_Equal, "equal", "equal"},     {Qt::Key_Insert, "Insert", "Insert"},
            {Qt::Key_Home, "Home", "Home"},        {Qt::Key_End, "End", "End"},
        };
        for (const auto &[k, xkb, upper] : names)
            if (k == key) {
                p.key = QString::fromLatin1(xkb);
                p.keyUpper = QString::fromLatin1(upper);
            }
        if (p.key.isEmpty())
            p.key = p.keyUpper = QKeySequence(key).toString(QKeySequence::PortableText);
    }
    return p;
}

}  // namespace

QString toXdgTrigger(const QKeySequence &keys)
{
    const Parts p = split(keys);
    if (p.key.isEmpty())
        return {};
    QStringList out;
    if (p.ctrl) out << QStringLiteral("CTRL");
    if (p.alt) out << QStringLiteral("ALT");
    if (p.shift) out << QStringLiteral("SHIFT");
    if (p.meta) out << QStringLiteral("LOGO");
    out << p.key;
    return out.join(QLatin1Char('+'));
}

QString toHyprlandKeys(const QKeySequence &keys)
{
    const Parts p = split(keys);
    if (p.key.isEmpty())
        return {};
    QStringList out;
    if (p.meta) out << QStringLiteral("SUPER");
    if (p.ctrl) out << QStringLiteral("CTRL");
    if (p.alt) out << QStringLiteral("ALT");
    if (p.shift) out << QStringLiteral("SHIFT");
    out << (p.keyUpper.size() == 1 ? p.keyUpper : p.key.toLower() == QLatin1String("space") ? QStringLiteral("Space") : p.keyUpper);
    return out.join(QStringLiteral(" + "));
}

QString toSwayKeys(const QKeySequence &keys)
{
    const Parts p = split(keys);
    if (p.key.isEmpty())
        return {};
    QStringList out;
    if (p.meta) out << QStringLiteral("Mod4");
    if (p.ctrl) out << QStringLiteral("Ctrl");
    if (p.alt) out << QStringLiteral("Mod1");
    if (p.shift) out << QStringLiteral("Shift");
    out << p.key;
    return out.join(QLatin1Char('+'));
}

QString toDisplayKeys(const QKeySequence &keys)
{
    const Parts p = split(keys);
    if (p.key.isEmpty())
        return {};
    QStringList out;
    if (p.meta) out << QStringLiteral("Super");
    if (p.ctrl) out << QStringLiteral("Ctrl");
    if (p.alt) out << QStringLiteral("Alt");
    if (p.shift) out << QStringLiteral("Shift");
    out << p.keyUpper;
    return out.join(QStringLiteral(" + "));
}

}  // namespace selsearch
