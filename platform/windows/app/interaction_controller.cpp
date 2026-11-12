#include "app/interaction_controller.h"

#include "app/cursor_hider.h"
#include "runtime/sim_runner.h"
#include "diagnostics/log.h"

namespace snow {
namespace {
InteractionController* g_instance = nullptr;   // one controller per process (hook has no user data)
constexpr UINT kPollMs = 20;
// The chord must be held this long (ignores quick shortcuts such as Ctrl+Alt+S). Short, so
// that "hold keys, then press the mouse" is already captured by the hook.
constexpr ULONGLONG kHoldDelayMs = 60;

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Reads HKCU\Keyboard Layout\Toggle: which chord Windows uses to switch input language /
// keyboard layout. "1" = Alt+Shift, "2" = Ctrl+Shift, "3" = none, "4" = grave accent.
// A missing "Language Hotkey"/"Hotkey" value means the default (Alt+Shift).
void layoutSwitchChords(bool& altShift, bool& ctrlShift) {
    altShift = false;
    ctrlShift = false;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Toggle", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        altShift = true;
        return;
    }
    auto read = [&](const wchar_t* name, const wchar_t* fallback) {
        wchar_t buf[8] = {};
        DWORD size = sizeof buf - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS ||
            type != REG_SZ)
            return std::wstring(fallback);
        return std::wstring(buf);
    };
    std::wstring lang = read(L"Language Hotkey", L"");
    if (lang.empty()) lang = read(L"Hotkey", L"1");
    const std::wstring layout = read(L"Layout Hotkey", L"2");
    RegCloseKey(key);
    for (const auto& v : {lang, layout}) {
        if (v == L"1") altShift = true;
        if (v == L"2") ctrlShift = true;
    }
}
}  // namespace

HoldChord InteractionController::pickChord() {
    // Only relevant when more than one keyboard layout is installed.
    bool altShift = false, ctrlShift = false;
    if (GetKeyboardLayoutList(0, nullptr) > 1) layoutSwitchChords(altShift, ctrlShift);
    if (!altShift) return HoldChord::AltShift;
    if (!ctrlShift) return HoldChord::CtrlShift;
    return HoldChord::CtrlAlt;
}

std::wstring InteractionController::chordName(HoldChord c) {
    switch (c) {
        case HoldChord::AltShift: return L"Alt+Shift";
        case HoldChord::CtrlShift: return L"Ctrl+Shift";
        default: return L"Ctrl+Alt";
    }
}

InteractionController::InteractionController(SimRunner& runner, const SnapshotExchange& snapshots)
    : runner_(runner), snapshots_(snapshots) {}

InteractionController::~InteractionController() { shutdown(); }

void InteractionController::initialize(HWND host, UINT cycleToolMsg) {
    host_ = host;
    cycleMsg_ = cycleToolMsg;
    chord_ = pickChord();
    g_instance = this;
    SetTimer(host_, kTimerId, kPollMs, nullptr);
    SNOW_LOGI("interaction: hold chord = %ls", chordName(chord_).c_str());
}

void InteractionController::shutdown() {
    if (!host_) return;
    KillTimer(host_, kTimerId);
    removeHook();
    CursorHider::restore();
    if (g_instance == this) g_instance = nullptr;
    host_ = nullptr;
}

void InteractionController::setFeatureEnabled(bool enabled) {
    feature_ = enabled;
    if (!enabled) toggled_ = false;
    evaluate();
}

void InteractionController::setHoldKeyEnabled(bool enabled) {
    holdEnabled_ = enabled;
    if (!enabled) held_ = false;
    evaluate();
}

void InteractionController::setSuspended(bool suspended) {
    suspended_ = suspended;
    evaluate();
}

void InteractionController::setToggled(bool on) {
    toggled_ = on && feature_;
    evaluate();
}

void InteractionController::onTimer() {
    bool chord = false;
    if (holdEnabled_ && feature_ && !suspended_) chord = chordDown();
    const ULONGLONG now = GetTickCount64();
    if (!chord) {
        chordSince_ = 0;
        // Keep the mode alive while a captured drag is still in progress.
        if (held_ && !capturing_) {
            held_ = false;
            evaluate();
        }
    } else {
        if (chordSince_ == 0) chordSince_ = now;
        if (!held_ && now - chordSince_ >= kHoldDelayMs) {
            held_ = true;
            evaluate();
        }
    }
}

void InteractionController::evaluate() {
    const bool want = feature_ && !suspended_ && (toggled_ || held_);
    if (want == active_) return;
    active_ = want;
    SNOW_LOGI("interaction mode %s", active_ ? "on" : "off");
    if (active_) {
        CursorHider::hide();   // the overlay's tool sprite replaces the cursor
        installHook();
        POINT pt;
        if (GetCursorPos(&pt)) runner_.setPointer(static_cast<float>(pt.x), static_cast<float>(pt.y));
    } else {
        if (capturing_) {
            POINT pt{};
            GetCursorPos(&pt);
            InputEvent ev;
            ev.type = InputEvent::Type::PointerUp;
            ev.x = static_cast<float>(pt.x);
            ev.y = static_cast<float>(pt.y);
            runner_.postInput(ev);
            capturing_ = false;
        }
        swallowRightUp_ = false;
        removeHook();
        CursorHider::restore();
    }
    runner_.setInteractionMode(active_);
    if (onActiveChanged) onActiveChanged(active_);
}

void InteractionController::installHook() {
    if (hook_) return;
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, hookProc, GetModuleHandleW(nullptr), 0);
    if (!hook_) SNOW_LOGW("interaction: SetWindowsHookEx failed, error %lu", GetLastError());
}

void InteractionController::removeHook() {
    if (!hook_) return;
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
}

bool InteractionController::captureAll() const {
    // `held_` lags the keys by kHoldDelayMs; inside the hook also accept the chord being
    // physically down right now so a quick "keys + click" never leaks to the desktop.
    return blockWhileHeld_ && (held_ || (holdEnabled_ && chordDown()));
}

bool InteractionController::chordDown() const {
    const bool alt = down(VK_MENU), shift = down(VK_SHIFT), ctrl = down(VK_CONTROL),
               win = down(VK_LWIN) || down(VK_RWIN);
    switch (chord_) {
        case HoldChord::AltShift: return alt && shift && !ctrl && !win;
        case HoldChord::CtrlShift: return ctrl && shift && !alt && !win;
        default: return ctrl && alt && !shift && !win;
    }
}

bool InteractionController::hit(POINT pt) const {
    const auto snap = snapshots_.latest();
    if (!snap) return false;
    for (const RectI& r : snap->hitRegions)
        if (r.contains(pt.x, pt.y)) return true;
    return false;
}

LRESULT InteractionController::onMouse(WPARAM msg, const MSLLHOOKSTRUCT& info, bool& swallow) {
    swallow = false;
    const float x = static_cast<float>(info.pt.x), y = static_cast<float>(info.pt.y);
    switch (msg) {
        case WM_MOUSEMOVE:
            runner_.setPointer(x, y);
            break;
        case WM_LBUTTONDOWN:
            if (!capturing_ && (captureAll() || hit(info.pt))) {
                capturing_ = true;
                swallow = true;
                runner_.setPointer(x, y);
                InputEvent ev;
                ev.type = InputEvent::Type::PointerDown;
                ev.x = x;
                ev.y = y;
                runner_.postInput(ev);
            }
            break;
        case WM_LBUTTONUP:
            if (capturing_) {
                capturing_ = false;
                swallow = true;
                InputEvent ev;
                ev.type = InputEvent::Type::PointerUp;
                ev.x = x;
                ev.y = y;
                runner_.postInput(ev);
            }
            break;
        case WM_RBUTTONDOWN:
            if (!capturing_ && (captureAll() || hit(info.pt))) {
                swallow = true;
                swallowRightUp_ = true;
                PostMessageW(host_, cycleMsg_, 0, 0);
            }
            break;
        case WM_RBUTTONUP:
            if (swallowRightUp_) {
                swallowRightUp_ = false;
                swallow = true;
            }
            break;
        default:
            break;
    }
    return 0;
}

LRESULT CALLBACK InteractionController::hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_instance && g_instance->active_) {
        bool swallow = false;
        g_instance->onMouse(wp, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp), swallow);
        if (swallow) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace snow
