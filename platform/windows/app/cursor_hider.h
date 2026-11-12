// CursorHider: while interaction mode is active the tool sprite drawn by the overlay IS the
// cursor, so the system cursor is made invisible.
//
// There is no API to hide the cursor over other processes' windows, so the standard system
// cursors are temporarily replaced with a blank one (SetSystemCursor) and the user's cursor
// scheme is reloaded afterwards (SystemParametersInfo(SPI_SETCURSORS)). This is session-only
// state (never written to the registry). Safety nets so the cursor can never stay invisible:
//   * restore() on leaving the mode, on shutdown, from the unhandled-exception filter and from
//     WM_ENDSESSION;
//   * a marker file is written while hidden; the next start restores the cursors if a previous
//     run was killed while hidden (restoreIfMarked()).
// UI thread only.
#pragma once
#include <windows.h>

#include <string>

namespace snow {

class CursorHider {
public:
    static void setMarkerPath(std::wstring path);
    static void restoreIfMarked();   // call once at startup
    static void hide();
    static void restore();
    static bool hidden();
    // For crash handlers: restores without touching the file system or allocating.
    static void emergencyRestore();
};

}  // namespace snow
