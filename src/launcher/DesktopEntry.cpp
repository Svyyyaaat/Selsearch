// SPDX-License-Identifier: MIT
#include "DesktopEntry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>

namespace selsearch {

using Group = QHash<QString, QString>;  // raw "Key" or "Key[locale]" -> raw value

namespace desktopentry {

QStringList localeKeys(const QString &locale)
{
    // lang_COUNTRY.ENCODING@MODIFIER; the encoding part is ignored.
    QString lang = locale;
    QString modifier;
    QString country;
    const qsizetype at = lang.indexOf(QLatin1Char('@'));
    if (at >= 0) {
        modifier = lang.mid(at + 1);
        lang.truncate(at);
    }
    const qsizetype dot = lang.indexOf(QLatin1Char('.'));
    if (dot >= 0)
        lang.truncate(dot);
    const qsizetype us = lang.indexOf(QLatin1Char('_'));
    if (us >= 0) {
        country = lang.mid(us + 1);
        lang.truncate(us);
    }
    if (lang.isEmpty() || lang == QLatin1String("C") || lang == QLatin1String("POSIX"))
        return {};

    QStringList keys;
    if (!country.isEmpty() && !modifier.isEmpty())
        keys << lang + QLatin1Char('_') + country + QLatin1Char('@') + modifier;
    if (!country.isEmpty())
        keys << lang + QLatin1Char('_') + country;
    if (!modifier.isEmpty())
        keys << lang + QLatin1Char('@') + modifier;
    keys << lang;
    return keys;
}

QString currentLocale()
{
    for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty())
            return v;
    }
    return QStringLiteral("C");
}

QStringList currentDesktops()
{
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts);
}

QString desktopFileId(const QString &root, const QString &filePath)
{
    QString rel = QDir(root).relativeFilePath(filePath);
    rel.replace(QLatin1Char('/'), QLatin1Char('-'));
    return rel;
}

QString unescapeValue(QStringView value)
{
    QString out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value[i];
        if (c != QLatin1Char('\\') || i + 1 >= value.size()) {
            out += c;
            continue;
        }
        const QChar n = value[++i];
        switch (n.unicode()) {
        case 's': out += QLatin1Char(' '); break;
        case 'n': out += QLatin1Char('\n'); break;
        case 't': out += QLatin1Char('\t'); break;
        case 'r': out += QLatin1Char('\r'); break;
        case '\\': out += QLatin1Char('\\'); break;
        default:
            // Unknown escapes (\" \$ \; ...) are kept for the next layer
            // (Exec quoting or list splitting).
            out += QLatin1Char('\\');
            out += n;
        }
    }
    return out;
}

QStringList splitList(QStringView value)
{
    QStringList out;
    QString cur;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value[i];
        if (c == QLatin1Char('\\') && i + 1 < value.size() && value[i + 1] == QLatin1Char(';')) {
            cur += QLatin1Char(';');
            ++i;
        } else if (c == QLatin1Char(';')) {
            out << unescapeValue(cur).trimmed();
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.trimmed().isEmpty())
        out << unescapeValue(cur).trimmed();
    out.removeAll(QString());
    return out;
}

QStringList splitExec(const QString &exec, bool *ok)
{
    QStringList argv;
    QString cur;
    bool inArg = false;
    bool quoted = false;
    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec[i];
        if (quoted) {
            if (c == QLatin1Char('\\') && i + 1 < exec.size()) {
                const QChar n = exec[i + 1];
                if (n == QLatin1Char('"') || n == QLatin1Char('`') || n == QLatin1Char('$') || n == QLatin1Char('\\')) {
                    cur += n;
                    ++i;
                    continue;
                }
            }
            if (c == QLatin1Char('"'))
                quoted = false;
            else
                cur += c;
            continue;
        }
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n')) {
            if (inArg) {
                argv << cur;
                cur.clear();
                inArg = false;
            }
            continue;
        }
        inArg = true;
        if (c == QLatin1Char('"'))
            quoted = true;
        else if (c == QLatin1Char('\\') && i + 1 < exec.size())
            cur += exec[++i];  // tolerate shell-style escapes outside quotes
        else
            cur += c;
    }
    if (quoted) {
        if (ok)
            *ok = false;
        return {};
    }
    if (inArg)
        argv << cur;
    if (ok)
        *ok = true;
    return argv;
}

QStringList expandFieldCodes(const QStringList &argv, const DesktopEntry &entry, const QStringList &files)
{
    QStringList out;
    for (const QString &arg : argv) {
        // Codes that stand alone and expand to zero or more whole arguments.
        if (arg == QLatin1String("%F") || arg == QLatin1String("%U")) {
            out << files;
            continue;
        }
        if (arg == QLatin1String("%f") || arg == QLatin1String("%u")) {
            if (!files.isEmpty())
                out << files.first();
            continue;
        }
        if (arg == QLatin1String("%i")) {
            if (!entry.icon.isEmpty())
                out << QStringLiteral("--icon") << entry.icon;
            continue;
        }
        if (!arg.contains(QLatin1Char('%'))) {
            out << arg;
            continue;
        }
        // Codes embedded in a larger argument.
        QString expanded;
        for (qsizetype i = 0; i < arg.size(); ++i) {
            const QChar c = arg[i];
            if (c != QLatin1Char('%') || i + 1 >= arg.size()) {
                expanded += c;
                continue;
            }
            switch (arg[++i].unicode()) {
            case '%': expanded += QLatin1Char('%'); break;
            case 'c': expanded += entry.name; break;
            case 'k': expanded += entry.filePath; break;
            case 'f': case 'u': case 'F': case 'U':
                if (!files.isEmpty())
                    expanded += files.first();
                break;
            case 'i': expanded += entry.icon; break;
            default: break;  // deprecated or unknown: removed
            }
        }
        // An argument that consisted only of removed codes disappears.
        if (!expanded.isEmpty() || arg.isEmpty())
            out << expanded;
    }
    return out;
}

bool isExecutableAvailable(const QString &program)
{
    if (program.isEmpty())
        return false;
    if (program.startsWith(QLatin1Char('/'))) {
        const QFileInfo fi(program);
        return fi.isFile() && fi.isExecutable();
    }
    return !QStandardPaths::findExecutable(program).isEmpty();
}

static QString localized(const Group &g, const QString &key, const QStringList &locales)
{
    for (const QString &l : locales) {
        auto it = g.constFind(key + QLatin1Char('[') + l + QLatin1Char(']'));
        if (it != g.constEnd())
            return *it;
    }
    return g.value(key);
}

static bool boolValue(const Group &g, const QString &key)
{
    const QString v = g.value(key).trimmed();
    return v == QLatin1String("true") || v == QLatin1String("1");
}

std::optional<DesktopEntry> parse(const QByteArray &contents, const QString &id, const QString &locale)
{
    QHash<QString, Group> groups;
    Group *current = nullptr;
    const QString text = QString::fromUtf8(contents);
    for (QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            const QString name = line.mid(1, line.size() - 2).toString();
            // The first occurrence of a group wins; duplicates are invalid.
            current = groups.contains(name) ? nullptr : &groups[name];
            continue;
        }
        if (!current)
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed().toString();
        if (!current->contains(key))
            current->insert(key, line.mid(eq + 1).trimmed().toString());
    }

    auto mainIt = groups.constFind(QStringLiteral("Desktop Entry"));
    if (mainIt == groups.constEnd())
        return std::nullopt;
    const Group &g = *mainIt;
    const QString type = g.value(QStringLiteral("Type"), QStringLiteral("Application"));
    if (type != QLatin1String("Application"))
        return std::nullopt;

    const QStringList locales = localeKeys(locale);
    DesktopEntry e;
    e.id = id;
    e.name = unescapeValue(localized(g, QStringLiteral("Name"), locales));
    e.genericName = unescapeValue(localized(g, QStringLiteral("GenericName"), locales));
    e.comment = unescapeValue(localized(g, QStringLiteral("Comment"), locales));
    e.keywords = splitList(localized(g, QStringLiteral("Keywords"), locales));
    e.icon = unescapeValue(localized(g, QStringLiteral("Icon"), locales));
    e.exec = unescapeValue(g.value(QStringLiteral("Exec")));
    e.tryExec = unescapeValue(g.value(QStringLiteral("TryExec")));
    e.workingDir = unescapeValue(g.value(QStringLiteral("Path")));
    e.terminal = boolValue(g, QStringLiteral("Terminal"));
    e.hidden = boolValue(g, QStringLiteral("Hidden"));
    e.noDisplay = boolValue(g, QStringLiteral("NoDisplay"));
    e.onlyShowIn = splitList(g.value(QStringLiteral("OnlyShowIn")));
    e.notShowIn = splitList(g.value(QStringLiteral("NotShowIn")));

    // Entries without Name or Exec are invalid (D-Bus-only activation is not
    // supported); keep them only when hidden so they still mask other dirs.
    if ((e.name.isEmpty() || e.exec.isEmpty()) && !e.hidden)
        return std::nullopt;

    for (const QString &actionId : splitList(g.value(QStringLiteral("Actions")))) {
        auto it = groups.constFind(QStringLiteral("Desktop Action ") + actionId);
        if (it == groups.constEnd())
            continue;
        DesktopAction a;
        a.id = actionId;
        a.name = unescapeValue(localized(*it, QStringLiteral("Name"), locales));
        a.icon = unescapeValue(localized(*it, QStringLiteral("Icon"), locales));
        a.exec = unescapeValue(it->value(QStringLiteral("Exec")));
        if (!a.name.isEmpty() && !a.exec.isEmpty())
            e.actions << a;
    }
    return e;
}

std::optional<DesktopEntry> parseFile(const QString &filePath, const QString &id, const QString &locale)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    // Desktop files are small; refuse anything absurd.
    if (f.size() > 1024 * 1024)
        return std::nullopt;
    auto e = parse(f.readAll(), id, locale);
    if (e)
        e->filePath = QFileInfo(filePath).absoluteFilePath();
    return e;
}

}  // namespace desktopentry

bool DesktopEntry::shouldShow(const QStringList &desktops) const
{
    if (hidden || noDisplay)
        return false;
    if (!onlyShowIn.isEmpty()) {
        bool any = false;
        for (const QString &d : desktops) {
            if (onlyShowIn.contains(d, Qt::CaseInsensitive)) {
                any = true;
                break;
            }
        }
        if (!any)
            return false;
    }
    for (const QString &d : desktops) {
        if (notShowIn.contains(d, Qt::CaseInsensitive))
            return false;
    }
    if (!tryExec.isEmpty() && !desktopentry::isExecutableAvailable(tryExec))
        return false;
    return true;
}

}  // namespace selsearch
