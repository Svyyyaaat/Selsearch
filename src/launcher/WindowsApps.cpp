// SPDX-License-Identifier: MIT
#include "WindowsApps.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commoncontrols.h>

namespace selsearch::winshell {

QStringList startMenuDirs()
{
    QStringList out;
    const QString suffix = QStringLiteral("/Microsoft/Windows/Start Menu/Programs");
    // Per-user first: an app installed for this user overrides the same
    // shortcut path in the all-users menu.
    for (const char *var : {"APPDATA", "ProgramData"}) {
        const QString base = qEnvironmentVariable(var);
        if (!base.isEmpty())
            out << QDir::cleanPath(QDir::fromNativeSeparators(base) + suffix);
    }
    return out;
}

std::optional<DesktopEntry> shortcutEntry(const QString &root, const QString &path)
{
    const QFileInfo fi(path);
    const QString name = fi.completeBaseName();
    if (name.isEmpty() || name.contains(QLatin1String("uninstall"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("deinstall"), Qt::CaseInsensitive))
        return std::nullopt;
    DesktopEntry e;
    e.id = QDir(root).relativeFilePath(path);
    e.filePath = fi.absoluteFilePath();
    e.name = name;
    // The folder ("Accessories", "Microsoft Office Tools") as a hint.
    const QString folder = QFileInfo(e.id).path();
    if (folder != QLatin1String("."))
        e.comment = folder;
    e.icon = iconId(e.filePath);
    return e;
}

QString iconId(const QString &path)
{
    return QStringLiteral("winicon:") + QString::fromLatin1(path.toUtf8().toHex());
}

bool isIconId(const QString &id)
{
    return id.startsWith(QLatin1String("winicon:"));
}

QString iconPath(const QString &id)
{
    if (!isIconId(id))
        return {};
    return QString::fromUtf8(QByteArray::fromHex(id.mid(8).toLatin1()));
}

namespace {

// IID_IImageList, spelled out so no extra import library is needed.
const GUID kIImageList = {0x46eb5926, 0x582e, 0x4017, {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};

void ensureCom()
{
    // The shell's icon APIs want COM on the calling thread (QML loads icons
    // on its own threads). Left initialised for the thread's lifetime.
    thread_local const bool initialised = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    (void)initialised;
}

}  // namespace

QImage shellIcon(const QString &path, const QSize &size)
{
    ensureCom();
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    SHFILEINFOW info = {};
    if (!SHGetFileInfoW(native.c_str(), 0, &info, sizeof info, SHGFI_SYSICONINDEX))
        return {};
    // 48 px ("extra large") looks right at launcher sizes; 32 px is too small
    // on high-DPI screens, and the 256 px list pads old icons.
    const int list = qMax(size.width(), size.height()) > 32 ? SHIL_EXTRALARGE : SHIL_LARGE;
    QImage image;
    IImageList *images = nullptr;
    if (SUCCEEDED(SHGetImageList(list, kIImageList, reinterpret_cast<void **>(&images))) && images) {
        HICON icon = nullptr;
        if (SUCCEEDED(images->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
            image = QImage::fromHICON(icon);
            DestroyIcon(icon);
        }
        images->Release();
    }
    if (!image.isNull() && size.isValid() && image.size() != size)
        image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

bool open(const QString &path, QString *error)
{
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    const std::wstring dir = QDir::toNativeSeparators(QFileInfo(path).absolutePath()).toStdWString();
    const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL));
    if (rc > 32)
        return true;
    if (error)
        *error = QStringLiteral("could not open %1 (error %2)").arg(QDir::toNativeSeparators(path)).arg(qint64(rc));
    return false;
}

bool reveal(const QString &path)
{
    ensureCom();
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(native.c_str());
    if (!item)
        return open(QFileInfo(path).absolutePath());
    const bool ok = SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
    ILFree(item);
    return ok || open(QFileInfo(path).absolutePath());
}

}  // namespace selsearch::winshell
