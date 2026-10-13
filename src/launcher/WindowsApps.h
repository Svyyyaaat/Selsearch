// SPDX-License-Identifier: MIT
#pragma once

// Windows shell helpers for the launcher: Start Menu shortcuts, shell icons,
// opening files the way Explorer does. Only built on Windows.

#include "DesktopEntry.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace selsearch::winshell {

// The per-user and all-users Start Menu "Programs" folders.
QStringList startMenuDirs();

// An app entry for a Start Menu shortcut (.lnk / .url) found under `root`.
// id: the path relative to root ("Accessories/Paint.lnk"), name: the file's
// base name, icon: iconId(path), filePath: the shortcut. Uninstall links and
// links to help/readme documents are skipped (nullopt).
std::optional<DesktopEntry> shortcutEntry(const QString &root, const QString &path);

// Launcher icon ids ("winicon:<hex utf-8 path>") for files whose icon comes
// from the shell. Hex keeps any path intact inside an image:// URL.
QString iconId(const QString &path);
bool isIconId(const QString &id);
QString iconPath(const QString &id);

// The shell's icon for a file, folder or shortcut (the target's icon for a
// .lnk), scaled to `size`. Safe on any thread.
QImage shellIcon(const QString &path, const QSize &size);

// ShellExecute "open": starts shortcuts and programs, opens documents in
// their default app.
bool open(const QString &path, QString *error = nullptr);
// Opens Explorer with `path` selected.
bool reveal(const QString &path);

}  // namespace selsearch::winshell
