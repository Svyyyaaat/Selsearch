// SPDX-License-Identifier: MIT
//
// Window list for snap-to-window on Windows: the visible top-level windows
// from EnumWindows (which walks them in Z order, topmost first), with their
// visible frame from DWM, converted to Qt's logical coordinates.

#include "Compositor.h"

#include "capture/Capture.h"

#include <QGuiApplication>
#include <QHash>
#include <QMutex>
#include <QScreen>

#include <windows.h>
#include <dwmapi.h>

#include <iterator>
#include <optional>
#include <string>

namespace selsearch {
namespace {

struct ScreenInfo {
    QString name;  // screenName()
    QRect logical;
};

QRect physicalRect(const RECT &r)
{
    return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
}

class WindowsCompositor final : public Compositor {
public:
    WindowsCompositor()
    {
        // QScreen is only safe to read on the GUI thread, while the lens asks
        // from a worker: keep a copy of what is needed, refreshed on changes.
        refreshScreens();
        if (qGuiApp) {
            auto watch = [this](QScreen *s) {
                QObject::connect(s, &QScreen::geometryChanged, &m_context, [this] { refreshScreens(); });
            };
            for (QScreen *s : QGuiApplication::screens())
                watch(s);
            QObject::connect(qGuiApp, &QGuiApplication::screenAdded, &m_context, [this, watch](QScreen *s) {
                watch(s);
                refreshScreens();
            });
            QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, &m_context, [this] { refreshScreens(); });
        }
    }

    QString id() const override { return QStringLiteral("windows"); }

    QList<WindowInfo> visibleWindows() override
    {
        struct Walk {
            WindowsCompositor *self;
            DWORD ownPid;
            HWND foreground;
            QHash<DWORD, QString> exeNames;
            QList<WindowInfo> out;
        } walk{this, GetCurrentProcessId(), GetAncestor(GetForegroundWindow(), GA_ROOT), {}, {}};
        EnumWindows(
            [](HWND hwnd, LPARAM data) -> BOOL {
                auto &w = *reinterpret_cast<Walk *>(data);
                if (auto info = w.self->describe(hwnd, w.ownPid, w.exeNames)) {
                    info->focused = hwnd == w.foreground;
                    w.out.append(*info);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&walk));
        return walk.out;
    }

    QString focusedOutput() override
    {
        HWND fg = GetForegroundWindow();
        if (!fg)
            return {};
        return screenFor(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)).name;
    }

private:
    void refreshScreens()
    {
        QList<ScreenInfo> list;
        for (QScreen *s : QGuiApplication::screens())
            list.append({screenName(s), s->geometry()});
        QMutexLocker lock(&m_mutex);
        m_screens = list;
    }

    // The Qt screen showing `monitor`: same top-left corner (Qt keeps screen
    // origins in physical pixels and scales only their size).
    ScreenInfo screenFor(HMONITOR monitor, QRect *physical = nullptr) const
    {
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        if (!monitor || !GetMonitorInfoW(monitor, &mi))
            return {};
        const QRect phys = physicalRect(mi.rcMonitor);
        if (physical)
            *physical = phys;
        QMutexLocker lock(&m_mutex);
        for (const ScreenInfo &s : m_screens)
            if (s.logical.topLeft() == phys.topLeft())
                return s;
        return {};
    }

    // Physical rect -> logical, using the scale of the monitor it is mostly on.
    QRect toLogical(const RECT &r) const
    {
        QRect monitorPhys;
        const ScreenInfo s = screenFor(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &monitorPhys);
        const QRect phys = physicalRect(r);
        if (s.logical.isEmpty() || monitorPhys.isEmpty())
            return phys;
        const qreal scale = qreal(monitorPhys.width()) / s.logical.width();
        const QPointF origin = monitorPhys.topLeft();
        const QPointF tl = origin + (QPointF(phys.topLeft()) - origin) / scale;
        return QRectF(tl, QSizeF(phys.size()) / scale).toAlignedRect();
    }

    std::optional<WindowInfo> describe(HWND hwnd, DWORD ownPid, QHash<DWORD, QString> &exeNames) const
    {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
            return std::nullopt;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ownPid)
            return std::nullopt;
        // Cloaked: UWP apps on other virtual desktops, suspended apps, etc.
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
            return std::nullopt;
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW)  // tooltips, floating palettes, the shell's own bits
            return std::nullopt;
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        static const wchar_t *const shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
        for (const wchar_t *c : shell)
            if (wcscmp(cls, c) == 0)
                return std::nullopt;

        // The visible frame, without the invisible resize borders of Win10+.
        RECT r = {};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && !GetWindowRect(hwnd, &r))
            return std::nullopt;
        if (r.right - r.left < 8 || r.bottom - r.top < 8)
            return std::nullopt;

        WindowInfo info;
        const int len = GetWindowTextLengthW(hwnd);
        if (len > 0) {
            std::wstring title(size_t(len) + 1, L'\0');
            const int n = GetWindowTextW(hwnd, title.data(), len + 1);
            info.title = QString::fromWCharArray(title.data(), n);
        }
        auto exe = exeNames.constFind(pid);
        if (exe == exeNames.constEnd())
            exe = exeNames.insert(pid, exeName(pid));
        info.appId = *exe;
        info.geometry = toLogical(r);
        return info;
    }

    static QString exeName(DWORD pid)
    {
        QString name;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH * 2];
            DWORD size = DWORD(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                name = QString::fromWCharArray(path, int(size));
                name = name.mid(name.lastIndexOf(QLatin1Char('\\')) + 1);
                if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
                    name.chop(4);
            }
            CloseHandle(process);
        }
        return name;
    }

    QObject m_context;  // owns the screen-change connections
    mutable QMutex m_mutex;  // guards m_screens
    QList<ScreenInfo> m_screens;
};

}  // namespace

std::unique_ptr<Compositor> createWindowsCompositor()
{
    return std::make_unique<WindowsCompositor>();
}

}  // namespace selsearch
