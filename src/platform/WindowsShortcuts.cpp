// SPDX-License-Identifier: MIT
//
// Global hotkeys on Windows: RegisterHotKey on a hidden message-only window.
// WM_HOTKEY goes to our own window procedure, so hotkeys keep working while
// Qt runs a modal loop (menus, window moves). Receiving the hotkey also gives
// this process the right to bring its windows to the foreground.
//
// Alt+Space normally opens the active window's system menu; a registered
// hotkey takes priority over it while we hold it (as in PowerToys Run).
//
// Releasing Alt (or Win) with no other key seen in between opens the
// focused window's menu bar (or the Start menu). The hotkey's own key never
// reaches anyone, so on every hotkey with Alt/Win we inject an unassigned
// key while the modifier is still down, like AutoHotkey's menu mask.
//
// The Settings key recorder uses a low-level keyboard hook instead of Qt key
// events: that sees combinations Windows or other apps would act on first
// (Alt+Space, Win+…, keys already registered) and swallows them.

#include "GlobalShortcuts.h"

#include <QHash>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QTimer>

#include <windows.h>

Q_LOGGING_CATEGORY(lcWinKeys, "selsearch.shortcuts")

namespace selsearch {
namespace {

const wchar_t kWindowClass[] = L"SelsearchHotkeys";

// Unassigned virtual key used as the menu mask (AutoHotkey uses it too).
constexpr WORD kMaskKey = 0xE8;

struct NamedKey {
    Qt::Key key;
    UINT vk;
};
constexpr NamedKey kNamedKeys[] = {
    {Qt::Key_Space, VK_SPACE},   {Qt::Key_Tab, VK_TAB},          {Qt::Key_Return, VK_RETURN},
    {Qt::Key_Enter, VK_RETURN},  {Qt::Key_Escape, VK_ESCAPE},    {Qt::Key_Backspace, VK_BACK},
    {Qt::Key_Insert, VK_INSERT}, {Qt::Key_Delete, VK_DELETE},    {Qt::Key_Home, VK_HOME},
    {Qt::Key_End, VK_END},       {Qt::Key_PageUp, VK_PRIOR},     {Qt::Key_PageDown, VK_NEXT},
    {Qt::Key_Left, VK_LEFT},     {Qt::Key_Right, VK_RIGHT},      {Qt::Key_Up, VK_UP},
    {Qt::Key_Down, VK_DOWN},     {Qt::Key_Print, VK_SNAPSHOT},   {Qt::Key_Pause, VK_PAUSE},
    {Qt::Key_ScrollLock, VK_SCROLL}, {Qt::Key_Menu, VK_APPS},
};

// Virtual-key code for a Qt key, or 0 when there is none.
UINT virtualKey(Qt::Key key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return UINT('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return UINT('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return UINT(VK_F1 + (key - Qt::Key_F1));
    for (const NamedKey &k : kNamedKeys)
        if (k.key == key)
            return k.vk;
    // Punctuation and other printable keys: ask the current keyboard layout.
    if (key > 0x20 && key < 0x10000) {
        const SHORT vk = VkKeyScanW(wchar_t(key));
        if (vk != -1)
            return UINT(vk & 0xff);
    }
    return 0;
}

// The inverse of virtualKey(), or 0.
Qt::Key qtKey(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return Qt::Key(Qt::Key_A + int(vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return Qt::Key(Qt::Key_0 + int(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F24)
        return Qt::Key(Qt::Key_F1 + int(vk - VK_F1));
    for (const NamedKey &k : kNamedKeys)
        if (k.vk == vk)
            return k.key;
    // Punctuation (OEM keys): the character on the current layout.
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fff;
    if (ch > 0x20)
        return Qt::Key(QChar(char16_t(ch)).toUpper().unicode());
    return Qt::Key(0);
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
        m_captureTimeout.setSingleShot(true);
        QObject::connect(&m_captureTimeout, &QTimer::timeout, this, [this] {
            setCapturingKeys(false);
            Q_EMIT keysCaptured(QStringLiteral("Esc"));  // ends the recording in the UI
        });
        if (m_hwnd)
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        else
            qCWarning(lcWinKeys) << "could not create the hotkey window, error" << GetLastError();
    }

    ~WindowsShortcuts() override
    {
        setCapturingKeys(false);
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

    bool canCaptureKeys() const override { return true; }

    void setCapturingKeys(bool on) override
    {
        if (on && !s_hook) {
            s_capturer = this;
            s_mods = {};
            s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &WindowsShortcuts::keyboardHook, GetModuleHandleW(nullptr), 0);
            if (!s_hook)
                qCWarning(lcWinKeys) << "could not install the keyboard hook, error" << GetLastError();
            // Never keep the whole keyboard for long, whatever the UI does.
            m_captureTimeout.start(20000);
        } else if (!on && s_hook) {
            UnhookWindowsHookEx(s_hook);
            s_hook = nullptr;
            s_capturer = nullptr;
            m_captureTimeout.stop();
        }
    }

private:
    static LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam)
    {
        const auto *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
        if (code != HC_ACTION || !s_capturer || (k->flags & LLKHF_INJECTED))
            return CallNextHookEx(nullptr, code, wParam, lParam);
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        // Swallowed keys don't update the key state; track modifiers here.
        Qt::KeyboardModifier mod = Qt::NoModifier;
        switch (k->vkCode) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: mod = Qt::ShiftModifier; break;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: mod = Qt::ControlModifier; break;
        case VK_MENU: case VK_LMENU: case VK_RMENU: mod = Qt::AltModifier; break;
        case VK_LWIN: case VK_RWIN: mod = Qt::MetaModifier; break;
        default: break;
        }
        if (mod != Qt::NoModifier) {
            s_mods.setFlag(mod, down);
        } else if (down) {
            if (const Qt::Key key = qtKey(k->vkCode)) {
                const QString seq = QKeySequence(QKeyCombination(s_mods, key)).toString(QKeySequence::PortableText);
                WindowsShortcuts *self = s_capturer;
                // Leave the hook quickly (Windows drops slow hooks).
                QMetaObject::invokeMethod(self, [self, seq] { Q_EMIT self->keysCaptured(seq); }, Qt::QueuedConnection);
            }
        }
        return 1;  // swallow: no menus, no Start menu, no other hotkeys
    }

    void unregisterAll()
    {
        for (auto it = m_ids.cbegin(); it != m_ids.cend(); ++it)
            UnregisterHotKey(m_hwnd, it.key());
        m_ids.clear();
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_HOTKEY) {
            if (LOWORD(lParam) & (MOD_ALT | MOD_WIN)) {
                INPUT mask[2] = {};
                mask[0].type = mask[1].type = INPUT_KEYBOARD;
                mask[0].ki.wVk = mask[1].ki.wVk = kMaskKey;
                mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, mask, sizeof(INPUT));
            }
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
    QTimer m_captureTimeout;

    static inline HHOOK s_hook = nullptr;
    static inline WindowsShortcuts *s_capturer = nullptr;
    static inline Qt::KeyboardModifiers s_mods;
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
