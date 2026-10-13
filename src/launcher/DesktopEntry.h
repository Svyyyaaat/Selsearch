// SPDX-License-Identifier: MIT
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace selsearch {

// A parsed freedesktop.org Desktop Entry (Type=Application), localised for one
// locale. Spec: https://specifications.freedesktop.org/desktop-entry-spec/latest/
struct DesktopAction {
    QString id;    // the action identifier from Actions=
    QString name;  // localised
    QString icon;
    QString exec;
};

struct DesktopEntry {
    QString id;        // desktop file id, e.g. "org.kde.dolphin.desktop" or "kde-foo.desktop"
    QString filePath;  // absolute path of the .desktop file
    QString name;
    QString genericName;
    QString comment;
    QStringList keywords;
    QString icon;
    QString exec;
    QString tryExec;
    QString workingDir;  // Path=
    bool terminal = false;
    bool hidden = false;     // Hidden=true: treat as deleted (also masks lower-priority ids)
    bool noDisplay = false;
    QStringList onlyShowIn;
    QStringList notShowIn;
    QList<DesktopAction> actions;

    // Hidden, NoDisplay, OnlyShowIn/NotShowIn against `desktops` (the
    // colon-separated $XDG_CURRENT_DESKTOP list) and TryExec.
    bool shouldShow(const QStringList &desktops) const;
};

namespace desktopentry {

// Locale lookup keys for `locale` (e.g. "sr_RS.UTF-8@latin"), most specific
// first, per the spec: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang.
QStringList localeKeys(const QString &locale);
// The messages locale from LC_ALL / LC_MESSAGES / LANG.
QString currentLocale();
// $XDG_CURRENT_DESKTOP split on ':'.
QStringList currentDesktops();

// Desktop file id for `filePath` found under the applications dir `root`:
// the path relative to root with '/' replaced by '-'.
QString desktopFileId(const QString &root, const QString &filePath);

// Parse a .desktop file. Returns nullopt for unreadable files, files without
// a [Desktop Entry] group, and anything that is not Type=Application.
// Hidden entries ARE returned (hidden = true) so they can mask other dirs.
std::optional<DesktopEntry> parseFile(const QString &filePath, const QString &id, const QString &locale);
std::optional<DesktopEntry> parse(const QByteArray &contents, const QString &id, const QString &locale);

// Undo the general string escapes (\s \n \t \r \\).
QString unescapeValue(QStringView value);
// Split a string list value on unescaped ';' (also unescaping "\;").
QStringList splitList(QStringView value);

// Split an Exec value (already passed through unescapeValue) into argv using
// the spec's quoting rules: space separated, double-quoted args may contain
// spaces, and inside quotes \" \` \$ \\ are escapes. Returns an empty list and
// sets *ok = false on unbalanced quotes.
QStringList splitExec(const QString &exec, bool *ok = nullptr);

// Expand field codes in an already split argv:
//   %f %u      -> first of `files` (arg dropped when there is none)
//   %F %U      -> all of `files` as separate args (dropped when empty)
//   %i         -> "--icon" "<Icon>" (dropped when Icon is empty)
//   %c         -> the localised Name
//   %k         -> the .desktop file path
//   %%         -> %
//   deprecated %d %D %n %N %v %m and unknown codes are removed.
QStringList expandFieldCodes(const QStringList &argv, const DesktopEntry &entry,
                             const QStringList &files = {});

// Is `program` an executable path, or found on $PATH?
bool isExecutableAvailable(const QString &program);

}  // namespace desktopentry
}  // namespace selsearch
