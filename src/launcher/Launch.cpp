// SPDX-License-Identifier: MIT
#include "Launch.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace selsearch {

QStringList terminalCommand()
{
    // $TERMINAL may carry arguments ("foot -a scratch"); the -e convention is
    // the most widely supported way to pass a command.
    const QString env = qEnvironmentVariable("TERMINAL").trimmed();
    if (!env.isEmpty()) {
        QStringList parts = QProcess::splitCommand(env);
        if (!parts.isEmpty() && !QStandardPaths::findExecutable(parts.first()).isEmpty()) {
            const QString base = QFileInfo(parts.first()).fileName();
            if (base == QLatin1String("foot") || base == QLatin1String("kitty") || base == QLatin1String("footclient"))
                return parts;  // these take the command directly
            if (base == QLatin1String("gnome-terminal"))
                return parts << QStringLiteral("--");
            if (base == QLatin1String("wezterm"))
                return parts << QStringLiteral("start") << QStringLiteral("--");
            return parts << QStringLiteral("-e");
        }
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("xdg-terminal-exec")).isEmpty())
        return {QStringLiteral("xdg-terminal-exec")};

    static const QList<QStringList> fallbacks = {
        {QStringLiteral("foot")},
        {QStringLiteral("kitty")},
        {QStringLiteral("alacritty"), QStringLiteral("-e")},
        {QStringLiteral("wezterm"), QStringLiteral("start"), QStringLiteral("--")},
        {QStringLiteral("ghostty"), QStringLiteral("-e")},
        {QStringLiteral("konsole"), QStringLiteral("-e")},
        {QStringLiteral("gnome-terminal"), QStringLiteral("--")},
        {QStringLiteral("xfce4-terminal"), QStringLiteral("-x")},
        {QStringLiteral("xterm"), QStringLiteral("-e")},
    };
    for (const QStringList &cmd : fallbacks) {
        if (!QStandardPaths::findExecutable(cmd.first()).isEmpty())
            return cmd;
    }
    return {};
}

// systemd unit names allow [A-Za-z0-9:_.\-]; '-' separates the name
// components of app scopes, so escape everything else to '_'.
static QString unitComponent(const QString &hint)
{
    QString s = hint;
    if (s.endsWith(QLatin1String(".desktop")))
        s.chop(8);
    for (QChar &c : s) {
        if (!(c.isLetterOrNumber() && c.unicode() < 0x80) && c != QLatin1Char('_') && c != QLatin1Char('.') && c != QLatin1Char(':'))
            c = QLatin1Char('_');
    }
    return s.left(100);
}

QStringList detachedCommand(const QStringList &argv, const QString &unitHint, bool useSystemd)
{
    if (!useSystemd)
        return argv;
    QStringList cmd{QStringLiteral("systemd-run"), QStringLiteral("--user"), QStringLiteral("--scope"),
                    QStringLiteral("--slice=app.slice"), QStringLiteral("--quiet"), QStringLiteral("--collect")};
    // app-<launcher>-<app id>-<random>.scope, per systemd's desktop environment
    // conventions, so tools like systemd-cgls show which app it is.
    const QString component = unitComponent(unitHint.isEmpty() ? QFileInfo(argv.value(0)).fileName() : unitHint);
    if (!component.isEmpty()) {
        cmd << QStringLiteral("--unit=app-selsearch-%1-%2")
                   .arg(component)
                   .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    }
    cmd << QStringLiteral("--") << argv;
    return cmd;
}

static bool haveSystemdUserManager()
{
    if (QStandardPaths::findExecutable(QStringLiteral("systemd-run")).isEmpty())
        return false;
    // The user manager's private socket exists only when it actually runs;
    // without it systemd-run --user fails after we've already detached.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return !runtime.isEmpty() && QFileInfo::exists(runtime + QStringLiteral("/systemd/private"));
}

bool launchDetached(const QStringList &argv, const QString &workingDir, const QString &unitHint, QString *error)
{
    if (argv.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty command");
        return false;
    }
    const QStringList cmd = detachedCommand(argv, unitHint, haveSystemdUserManager());

    QProcess proc;
    proc.setProgram(cmd.first());
    proc.setArguments(cmd.mid(1));
    QString dir = workingDir;
    if (dir.startsWith(QLatin1Char('~')))
        dir = QDir::homePath() + dir.mid(1);
    proc.setWorkingDirectory(!dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QDir::homePath());
    proc.setStandardInputFile(QProcess::nullDevice());
    // New session so the app does not share our process group or controlling
    // terminal, and no inherited fds or signal dispositions leak into it.
    proc.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession
                                  | QProcess::UnixProcessFlag::ResetSignalHandlers
                                  | QProcess::UnixProcessFlag::CloseFileDescriptors
                                  | QProcess::UnixProcessFlag::IgnoreSigPipe);
    if (!proc.startDetached()) {
        if (error)
            *error = proc.errorString();
        return false;
    }
    return true;
}

}  // namespace selsearch
