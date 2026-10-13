// SPDX-License-Identifier: MIT
//
// Global hotkeys on Windows: RegisterHotKey on a hidden message-only window.
// WM_HOTKEY goes to our own window procedure, so hotkeys keep working while
// Qt runs a modal loop (menus, window moves). Receiving the hotkey also gives
// this process the right to bring its windows to the foreground.
//
// Alt+Space normally opens the active window's system menu; a registered
// hotkey takes priority over it while we hold it (as in PowerToys Run).

#include "GlobalShortcuts.h"

#include <QHash>
#include <QLoggingCategory>
#include <QMetaObject>

#include <windows.h>

Q_LOGGING_CATEGORY(lcWinKeys, "selsearch.shortcuts")

namespace selsearch {
namespace {

const wchar_t kWindowClass[] = L"SelsearchHotkeys";

// Virtual-key code for a Qt key, or 0 when there is none.
UINT virtualKey(Qt::Key key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return UINT('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return UINT('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return UINT(VK_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Print: return VK_SNAPSHOT;
    case Qt::Key_Pause: return VK_PAUSE;
    case Qt::Key_ScrollLock: return VK_SCROLL;
    case Qt::Key_Menu: return VK_APPS;
    default: break;
    }
    // Punctuation and other printable keys: ask the current keyboard layout.
    if (key > 0x20 && key < 0x10000) {
        const SHORT vk = VkKeyScanW(wchar_t(key));
        if (vk != -1)
            return UINT(vk & 0xff);
    }
    return 0;
}

UINT modifierFlags(Qt::KeyboardModifiers mods)
{
    UINT flags = MOD_NOREPEAT;
    if (mods & Qt::AltModifier)
        flags |= MOD_ALT;
    if (mods & Qt::ControlModifier)
        flags |= MOD_CONTROL;
    if (mods & Qt::ShiftModifier)
        flags |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)
        flags |= MOD_WIN;
    return flags;
}

class WindowsShortcuts final : public GlobalShortcuts {
public:
    explicit WindowsShortcuts(QObject *parent)
        : GlobalShortcuts(parent)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = &WindowsShortcuts::windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);  // fails harmlessly when already registered
        m_hwnd = CreateWindowExW(0, kWindowClass, L"Selsearch hotkeys", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (m_hwnd)
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        else
            qCWarning(lcWinKeys) << "could not create the hotkey window, error" << GetLastError();
    }

    ~WindowsShortcuts() override
    {
        unregisterAll();
        if (m_hwnd)
            DestroyWindow(m_hwnd);
    }

    bool isValid() const { return m_hwnd != nullptr; }
    Kind kind() const override { return Kind::InApp; }
    QString backendName() const override { return QStringLiteral("windows"); }

    void setShortcuts(const QList<Shortcut> &shortcuts) override
    {
        unregisterAll();
        int nextId = 1;
        for (const Shortcut &s : shortcuts) {
            if (s.keys.isEmpty())
                continue;  // unbound
            const QKeyCombination combo = s.keys[0];
            const UINT vk = virtualKey(combo.key());
            if (vk == 0 || s.keys.count() > 1) {
                Q_EMIT failed(s.id, QStringLiteral("%1 cannot be used as a global shortcut").arg(s.keys.toString()));
                continue;
            }
            const int id = nextId++;
            if (!RegisterHotKey(m_hwnd, id, modifierFlags(combo.keyboardModifiers()), vk)) {
                const DWORD err = GetLastError();
                const QString reason = err == ERROR_HOTKEY_ALREADY_REGISTERED
                    ? QStringLiteral("%1 is already used by Windows or another app").arg(s.keys.toString(QKeySequence::NativeText))
                    : QStringLiteral("could not register %1 (error %2)").arg(s.keys.toString(QKeySequence::NativeText)).arg(err);
                qCWarning(lcWinKeys) << s.id << reason;
                Q_EMIT failed(s.id, reason);
                continue;
            }
            m_ids.insert(id, s.id);
        }
    }

private:
    void unregisterAll()
    {
        for (auto it = m_ids.cbegin(); it != m_ids.cend(); ++it)
            UnregisterHotKey(m_hwnd, it.key());
        m_ids.clear();
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_HOTKEY) {
            if (auto *self = reinterpret_cast<WindowsShortcuts *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                const QString id = self->m_ids.value(int(wParam));
                // Leave the window procedure before the app opens windows.
                if (!id.isEmpty())
                    QMetaObject::invokeMethod(self, [self, id] { Q_EMIT self->activated(id); }, Qt::QueuedConnection);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND m_hwnd = nullptr;
    QHash<int, QString> m_ids;  // hotkey id -> shortcut id
};

}  // namespace

std::unique_ptr<GlobalShortcuts> createWindowsShortcuts(QObject *parent)
{
    auto s = std::make_unique<WindowsShortcuts>(parent);
    if (!s->isValid())
        return nullptr;
    return s;
}

}  // namespace selsearch
