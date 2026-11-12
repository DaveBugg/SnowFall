// `SnowFall --selftest`: end-to-end checks that need the real frameworks (run by ctest in CI,
// which has a WindowServer session but no user): settings store, Metal shader compilation,
// offscreen rendering with pixel checks (our own texture only — the screen is never read),
// overlay windows + render thread, window tracker, simulation thread, Carbon hotkeys.
// Prints one line per check; returns 0 when nothing failed (unavailable Metal = SKIP).
#pragma once

namespace snow::mac {
int runSelfTest();
}
