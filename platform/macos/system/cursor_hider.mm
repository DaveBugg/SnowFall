#include "system/cursor_hider.h"

#import <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>

#include <atomic>

#include "diagnostics/log.h"

namespace snow::mac {
namespace {

using CGSConnectionID = int;
using DefaultConnectionFn = CGSConnectionID (*)(void);
using SetConnectionPropertyFn = CGError (*)(CGSConnectionID, CGSConnectionID, CFStringRef, CFTypeRef);
using CursorIsVisibleFn = boolean_t (*)(void);

std::atomic<int> g_hidden{0};   // outstanding CGDisplayHideCursor calls of ours (0 or 1)
bool g_resolved = false, g_available = false, g_propertySet = false;
DefaultConnectionFn g_connection = nullptr;
SetConnectionPropertyFn g_setProperty = nullptr;
CursorIsVisibleFn g_isVisible = nullptr;

void resolve() {
    if (g_resolved) return;
    g_resolved = true;
    g_connection = reinterpret_cast<DefaultConnectionFn>(dlsym(RTLD_DEFAULT, "_CGSDefaultConnection"));
    if (!g_connection) g_connection = reinterpret_cast<DefaultConnectionFn>(dlsym(RTLD_DEFAULT, "CGSMainConnectionID"));
    g_setProperty = reinterpret_cast<SetConnectionPropertyFn>(dlsym(RTLD_DEFAULT, "CGSSetConnectionProperty"));
    // CGCursorIsVisible is public but deprecated; looked up the same way to verify the hide.
    g_isVisible = reinterpret_cast<CursorIsVisibleFn>(dlsym(RTLD_DEFAULT, "CGCursorIsVisible"));
    g_available = g_connection && g_setProperty;
    if (!g_available)
        SNOW_LOGW("cursor: private CGS cursor API not found (connection=%d, property=%d)", g_connection ? 1 : 0,
                  g_setProperty ? 1 : 0);
}

}  // namespace

bool CursorHider::available() {
    resolve();
    return g_available;
}

bool CursorHider::hide() {
    if (g_hidden.load() > 0) return true;
    if (!available()) return false;
    if (!g_propertySet) {
        const CGSConnectionID conn = g_connection();
        const CGError err = g_setProperty(conn, conn, CFSTR("SetsCursorInBackground"), kCFBooleanTrue);
        if (err != kCGErrorSuccess) {
            SNOW_LOGW("cursor: SetsCursorInBackground failed (%d)", static_cast<int>(err));
            return false;
        }
        g_propertySet = true;
    }
    const CGError err = CGDisplayHideCursor(kCGDirectMainDisplay);
    if (err != kCGErrorSuccess) {
        SNOW_LOGW("cursor: CGDisplayHideCursor failed (%d)", static_cast<int>(err));
        return false;
    }
    g_hidden.store(1);
    if (g_isVisible && g_isVisible()) {
        // The call "succeeded" but the window server kept the cursor: undo and report.
        CGDisplayShowCursor(kCGDirectMainDisplay);
        g_hidden.store(0);
        SNOW_LOGW("cursor: hiding had no effect (cursor still visible)");
        return false;
    }
    SNOW_LOGI("cursor: system cursor hidden");
    return true;
}

void CursorHider::show() {
    if (g_hidden.exchange(0) > 0) {
        CGDisplayShowCursor(kCGDirectMainDisplay);
        SNOW_LOGI("cursor: system cursor shown");
    }
}

bool CursorHider::hidden() { return g_hidden.load() > 0; }
int CursorHider::balance() { return g_hidden.load(); }

void CursorHider::emergencyShow() {
    if (g_hidden.exchange(0) > 0) CGDisplayShowCursor(kCGDirectMainDisplay);
}

}  // namespace snow::mac
