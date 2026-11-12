#include "hotkey_edit.h"

#include <commctrl.h>

namespace snow::ui {
namespace {

constexpr UINT_PTR kSubclassId = 0x484B4544;   // 'HKED'
int g_modOverride = -1;

unsigned currentModifiers() {
    if (g_modOverride >= 0) return static_cast<unsigned>(g_modOverride);
    unsigned mods = 0;
    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
    return mods;
}

bool isModifierKey(UINT vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

std::string keyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_ESCAPE: return "Esc";
        case VK_RETURN: return "Enter";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_PAUSE: return "Pause";
        default: return {};
    }
}

void notifyParent(HWND edit) {
    SendMessageW(GetParent(edit), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(edit), HKN_CHANGED),
                 reinterpret_cast<LPARAM>(edit));
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = static_cast<UINT>(wp);
            if (isModifierKey(vk)) return 0;
            const unsigned mods = currentModifiers();
            if (vk == VK_F4 && mods == MOD_ALT) break;   // Alt+F4 keeps closing the window
            hotkeyEditHandleKey(hwnd, vk, mods);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (static_cast<UINT>(wp) == VK_F4) break;
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
        case WM_UNICHAR:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO:
        case WM_CONTEXTMENU:
            return 0;   // the text is only ever produced by the capture logic
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, subclassProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

std::string formatHotkeyText(unsigned mods, UINT vk) {
    mods &= (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    if (mods == 0) return {};
    const std::string key = keyName(vk);
    if (key.empty()) return {};
    std::string out;
    if (mods & MOD_CONTROL) out += "Ctrl+";
    if (mods & MOD_ALT) out += "Alt+";
    if (mods & MOD_SHIFT) out += "Shift+";
    if (mods & MOD_WIN) out += "Win+";
    return out + key;
}

HWND createHotkeyEdit(HINSTANCE instance, HWND parent, int id) {
    HWND edit = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (edit) SetWindowSubclass(edit, subclassProc, kSubclassId, 0);
    return edit;
}

bool isHotkeyEdit(HWND hwnd) {
    DWORD_PTR data = 0;
    return hwnd && GetWindowSubclass(hwnd, subclassProc, kSubclassId, &data) != FALSE;
}

void setHotkeyEditText(HWND edit, const std::string& text) {
    const std::wstring wide(text.begin(), text.end());   // canonical hotkey text is ASCII
    SetWindowTextW(edit, wide.c_str());
    SendMessageW(edit, EM_SETSEL, wide.size(), wide.size());
}

std::string getHotkeyEditText(HWND edit) {
    wchar_t buf[64] = {};
    GetWindowTextW(edit, buf, ARRAYSIZE(buf));
    std::string out;
    for (const wchar_t* p = buf; *p; ++p) out += static_cast<char>(*p & 0x7F);
    return out;
}

bool hotkeyEditHandleKey(HWND edit, UINT vk, unsigned mods) {
    mods &= (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    std::string text;
    if (mods == 0) {
        if (vk != VK_BACK && vk != VK_DELETE) return false;   // bare keys are never hotkeys
    } else {
        text = formatHotkeyText(mods, vk);
        if (text.empty()) return false;
    }
    if (text == getHotkeyEditText(edit)) return false;
    setHotkeyEditText(edit, text);
    notifyParent(edit);
    return true;
}

void setHotkeyModifierOverride(int mods) { g_modOverride = mods; }

}  // namespace snow::ui
