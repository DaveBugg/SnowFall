// Diagnostics side-door for WindowTracker, used by tests/integration/tracker (tracker_dump)
// and the diagnostics panel. Not part of the fixed public contract in window_tracker.h.
//
// All functions refer to the tracker that is currently started on the CALLING thread
// (there is at most one started tracker per thread) and return zero/false when none is.
// Only class names and executable names are exposed - never window titles.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

#include "core/types.h"

namespace snow {

struct TrackerDebugStats {
    std::uint64_t events = 0;     // WinEvents that passed the filter since start()
    std::uint64_t resyncs = 0;    // passes (full enumerations + fast passes)
    std::uint64_t fastPasses = 0; // passes that only re-read moved windows (no EnumWindows)
    bool lastFast = false;
    std::uint64_t published = 0;  // layouts handed to the callback
    std::uint64_t totalUs = 0;    // time spent in resync passes (callback time excluded)
    std::uint64_t maxUs = 0;
    std::uint64_t lastUs = 0;
    // Breakdown of the last pass: monitor refresh, EnumWindows + classification,
    // fullscreen detection. `lastFull` tells whether it was a full-refresh
    // (reconciliation) pass, which also re-queries slowly changing facts.
    std::uint64_t lastMonitorsUs = 0, lastEnumUs = 0, lastFullscreenUs = 0;
    bool lastFull = false;
    std::uint64_t topLevelSeen = 0;   // top-level windows enumerated in the last pass
};

TrackerDebugStats trackerDebugStats();
void trackerDebugResetStats();
// 0 when the window is unknown to the tracker.
WindowId trackerDebugIdForWindow(HWND hwnd);
// Window class and executable base name of a tracked id.
bool trackerDebugDescribe(WindowId id, std::wstring& className, std::wstring& exeName);

}  // namespace snow
