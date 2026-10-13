// SPDX-License-Identifier: MIT
#pragma once

#include <QString>
#include <QStringList>

namespace selsearch {

// The argv prefix that runs a command inside the user's terminal emulator:
// $TERMINAL (with -e), xdg-terminal-exec, then the first installed of foot,
// kitty, alacritty, wezterm, ghostty, konsole, gnome-terminal, xterm.
// Empty when no terminal could be found.
QStringList terminalCommand();

// Start `argv` fully detached from the daemon so it survives it.
// With a systemd user manager the process goes into its own transient scope
// under app.slice (systemd-run --user --scope), like desktop shells do;
// otherwise a plain detached process in a new session (setsid).
// `unitHint` names the scope (e.g. the desktop file id).
bool launchDetached(const QStringList &argv, const QString &workingDir = {},
                    const QString &unitHint = {}, QString *error = nullptr);

// The argv that launchDetached() would exec (exposed for tests).
QStringList detachedCommand(const QStringList &argv, const QString &unitHint, bool useSystemd);

}  // namespace selsearch
