// SPDX-License-Identifier: MIT
//
// Windows screen capture: one GDI BitBlt per monitor from the desktop DC, at
// physical resolution.
//
// Coordinates: the process is Per-Monitor-V2 DPI aware (Qt 6 sets this at
// startup), so GDI and GetMonitorInfo speak physical pixels. Qt's logical
// screen geometry keeps each screen's top-left corner in physical pixels and
// only scales the size (QHighDpi::fromNativeScreenGeometry), so a ScreenLayout
// entry is matched to its monitor by that corner.
//
// QScreen::grabWindow(0) would also return physical pixels, but it goes
// through QPixmap, which belongs on the GUI thread; grabAll() runs on a
// worker so the overlay can be prepared meanwhile.

#include "Capture.h"

#include <QElapsedTimer>

#include <windows.h>

namespace selsearch {
namespace {

struct Monitor {
    QRect rect;      // physical, virtual-desktop coordinates
    QString device;  // \\.\DISPLAY1
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof info;
    if (GetMonitorInfoW(monitor, &info)) {
        const RECT &r = info.rcMonitor;
        reinterpret_cast<QList<Monitor> *>(data)->append(
            {QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1)), QString::fromWCharArray(info.szDevice)});
    }
    return TRUE;
}

QList<Monitor> monitors()
{
    QList<Monitor> out;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&out));
    return out;
}

// Copies `rect` (physical) of the desktop into an opaque RGB32 image.
QImage grabRect(const QRect &rect, QString *error)
{
    const int w = rect.width(), h = rect.height();
    HDC screen = GetDC(nullptr);
    if (!screen) {
        *error = QStringLiteral("GetDC failed");
        return {};
    }
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    QImage image;
    if (mem && bitmap && bits) {
        HGDIOBJ old = SelectObject(mem, bitmap);
        // CAPTUREBLT includes layered windows (menus, tooltips, some apps).
        if (BitBlt(mem, 0, 0, w, h, screen, rect.x(), rect.y(), SRCCOPY | CAPTUREBLT)) {
            GdiFlush();
            image = QImage(w, h, QImage::Format_RGB32);
            const auto *src = static_cast<const quint32 *>(bits);
            for (int y = 0; y < h; ++y) {
                auto *dst = reinterpret_cast<quint32 *>(image.scanLine(y));
                const quint32 *row = src + qsizetype(y) * w;
                // GDI leaves the alpha byte undefined; RGB32 wants 0xff.
                for (int x = 0; x < w; ++x)
                    dst[x] = row[x] | 0xff000000u;
            }
        } else {
            *error = QStringLiteral("BitBlt failed (error %1)").arg(GetLastError());
        }
        SelectObject(mem, old);
    } else {
        *error = QStringLiteral("could not allocate a %1×%2 bitmap").arg(w).arg(h);
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (mem)
        DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return image;
}

class WindowsCapture final : public CaptureBackend {
public:
    QString id() const override { return QStringLiteral("gdi"); }
    bool isAvailable() override { return true; }

    CaptureResult grabAll(const QList<ScreenLayout> &layout) override
    {
        QElapsedTimer timer;
        timer.start();
        CaptureResult result;
        result.backend = id();
        // Physical pixels even in a process that is not DPI aware (tools).
        const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const QList<Monitor> all = monitors();

        QList<QPair<QString, QRect>> targets;
        if (layout.isEmpty()) {
            for (const Monitor &m : all)
                targets.append({m.device, m.rect});
        } else {
            for (const ScreenLayout &s : layout) {
                const Monitor *match = nullptr;
                for (const Monitor &m : all)
                    if (m.rect.topLeft() == s.geometry.topLeft())
                        match = &m;
                if (!match) {  // should not happen; take the monitor under the corner
                    for (const Monitor &m : all)
                        if (m.rect.contains(s.geometry.topLeft()))
                            match = &m;
                }
                if (match)
                    targets.append({s.name, match->rect});
            }
        }
        for (const auto &[name, rect] : std::as_const(targets)) {
            QString error;
            QImage image = grabRect(rect, &error);
            if (image.isNull()) {
                result.error = QStringLiteral("%1: %2").arg(name, error);
                break;
            }
            result.outputs.append({name, std::move(image)});
        }
        if (result.outputs.isEmpty() && result.error.isEmpty())
            result.error = QStringLiteral("no monitors found");
        if (previous)
            SetThreadDpiAwarenessContext(previous);
        result.elapsedMs = timer.elapsed();
        return result;
    }
};

}  // namespace

std::unique_ptr<CaptureBackend> createWindowsCaptureBackend()
{
    return std::make_unique<WindowsCapture>();
}

std::unique_ptr<CaptureBackend> createCaptureBackend(const QString &)
{
    return createWindowsCaptureBackend();
}

}  // namespace selsearch
