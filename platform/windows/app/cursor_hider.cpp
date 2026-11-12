#include "app/cursor_hider.h"

#include <atomic>
#include <cstring>

#include "diagnostics/log.h"

namespace snow {
namespace {
std::wstring g_marker;
std::atomic<bool> g_hidden{false};

// Standard system cursor ids (OCR_*): normal, ibeam, wait, cross, up, size*, no, hand,
// appstarting, help, pin, person.
constexpr DWORD kCursorIds[] = {32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645,
                                32646, 32648, 32649, 32650, 32651, 32671, 32672};

HCURSOR blankCursor() {
    const int w = GetSystemMetrics(SM_CXCURSOR), h = GetSystemMetrics(SM_CYCURSOR);
    const int bytes = ((w + 15) / 16) * 2 * h;
    static BYTE andMask[256 * 256 / 8];
    static BYTE xorMask[256 * 256 / 8];
    if (bytes <= 0 || bytes > static_cast<int>(sizeof andMask)) return nullptr;
    memset(andMask, 0xFF, static_cast<size_t>(bytes));   // AND 1: keep the screen pixel
    memset(xorMask, 0x00, static_cast<size_t>(bytes));   // XOR 0: change nothing
    return CreateCursor(GetModuleHandleW(nullptr), 0, 0, w, h, andMask, xorMask);
}

void reloadScheme() { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); }
}  // namespace

void CursorHider::setMarkerPath(std::wstring path) { g_marker = std::move(path); }

void CursorHider::restoreIfMarked() {
    if (g_marker.empty()) return;
    if (GetFileAttributesW(g_marker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    SNOW_LOGW("cursor: previous run ended while the cursor was hidden; restoring the cursor scheme");
    reloadScheme();
    DeleteFileW(g_marker.c_str());
}

void CursorHider::hide() {
    if (g_hidden.exchange(true)) return;
    if (!g_marker.empty()) {
        HANDLE f = CreateFileW(g_marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    int replaced = 0;
    for (DWORD id : kCursorIds) {
        HCURSOR blank = blankCursor();   // SetSystemCursor takes ownership of the handle
        if (blank && SetSystemCursor(blank, id)) {
            ++replaced;
        } else if (blank) {
            DestroyCursor(blank);
        }
    }
    SNOW_LOGI("cursor: hidden (%d system cursors replaced)", replaced);
    if (replaced == 0) {
        SNOW_LOGW("cursor: SetSystemCursor failed (error %lu); the system cursor stays visible", GetLastError());
        restore();
    }
}

void CursorHider::restore() {
    if (!g_hidden.exchange(false)) return;
    reloadScheme();
    if (!g_marker.empty()) DeleteFileW(g_marker.c_str());
    SNOW_LOGI("cursor: restored");
}

bool CursorHider::hidden() { return g_hidden.load(); }

void CursorHider::emergencyRestore() {
    if (g_hidden.exchange(false)) reloadScheme();
}

}  // namespace snow
