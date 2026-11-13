// Hiding the system cursor from a background (accessory) app — interaction.hideSystemCursor.
//
// Public API alone cannot do it: CGDisplayHideCursor only works for the frontmost app. The
// well-known private CoreGraphics connection property "SetsCursorInBackground" lifts that
// restriction. The private symbols (_CGSDefaultConnection / CGSMainConnectionID and
// CGSSetConnectionProperty) are resolved at run time with dlsym, so the binary still links and
// runs if Apple removes them; hide() then fails and the app draws the tool around the visible
// arrow instead (CursorDraw::Hold).
//
// CGDisplayHideCursor hides the cursor on ALL displays (the display argument is ignored) and
// is counted: every successful hide() is balanced by exactly one CGDisplayShowCursor.
// Main thread, except emergencyShow() (crash / signal handlers).
#pragma once

namespace snow::mac {

class CursorHider {
public:
    // false when the private API is missing (logged once).
    static bool available();
    // Hides the cursor (idempotent: at most one outstanding hide). false: not possible — the
    // cursor stays visible; the reason is logged.
    static bool hide();
    // Shows it again if we hid it (idempotent).
    static void show();
    static bool hidden();
    // Outstanding hides of ours (0 or 1): the self-test checks it is 0 at the end.
    static int balance();
    // Async-signal-tolerant best effort for the crash / SIGTERM / SIGINT handlers.
    static void emergencyShow();
};

}  // namespace snow::mac
