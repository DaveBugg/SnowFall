# SnowFall for macOS — port plan

Status: phases 1-4 implemented (branch `mac/app`), built and self-tested in CI on
macos-latest; awaiting the first manual test on the M2 (checklist at the end).

## Decisions

| Topic | Decision |
|---|---|
| Target | macOS 13 Ventura and newer, universal binary (Apple Silicon first-class, Intel built too). Test machine: M2. |
| Languages | Objective-C++ for the platform layer (talks to the C++ core directly); **SwiftUI** for the settings window and the menu-bar quick panel, bridged through a small Objective-C++ façade. |
| Rendering | Metal (`CAMetalLayer`), shaders ported from the HLSL sprite/drift SDF shaders. |
| Build | CMake (Xcode or Ninja generator), `.app` bundle with `LSUIElement` (no Dock icon). |
| Signing | No Apple Developer account for now: ad-hoc signed, not notarized. First launch: macOS 15+ — "Done" in the blocked dialog, then System Settings → Privacy & Security → Open Anyway; macOS 13–14 — right-click → Open (or `xattr -dr com.apple.quarantine /Applications/SnowFall.app`). |
| Distribution | `.dmg` built in CI and attached to GitHub releases. |

## Architecture mapping

| Piece | Windows | macOS |
|---|---|---|
| Overlay | per-monitor layered click-through window + D3D11 + DirectComposition | one borderless `NSWindow` per `NSScreen`: level above normal windows (below the menu bar/Dock menus), `ignoresMouseEvents = YES`, `collectionBehavior = canJoinAllSpaces | stationary | fullScreenAuxiliary | ignoresCycle`, transparent `CAMetalLayer` |
| Frame pacing | vsync Present | one render thread for all screens, woken by a `CVDisplayLink`; 30/60 FPS cap on top |
| Window tracking → `DesktopLayout` | `EnumWindows` + WinEvent hooks | `CGWindowListCopyWindowInfo(onScreenOnly)` on a background queue at ~30 Hz (bounds, layer, owner PID, Z-order; window titles are never requested, so no Screen Recording permission). Diff against the last layout, publish only on change; faster polling while a window is being dragged (mouse button down). |
| Coordinates | physical pixels, `dpiScale` per monitor | CG global coordinates (top-left origin, points) are used as-is; `dpiScale = 1` for simulation sizes; the renderer draws at `backingScaleFactor` for crisp output |
| "Taskbar" surface | `Shell_TrayWnd` | a bottom Dock (height from `NSScreen.frame` vs `visibleFrame`; width/centre from the Accessibility API when trusted, otherwise estimated from the Dock preferences). The rectangle stops 1 pt above the screen bottom so the screen-bottom drift continues left and right of the Dock. Left/right/auto-hidden Dock: screen bottom only |
| Fullscreen auto-pause | foreground window covers the monitor | frontmost app's window covers its screen / the screen is in a fullscreen Space |
| Interaction input | `WH_MOUSE_LL` only while the mode is on | `CGEventTap` only while the mode is on (needs the Accessibility permission — requested with an explanation the first time the user enters interaction mode) |
| Hold chord | `GetAsyncKeyState` polling | `NSEvent.modifierFlags` polling (no permission needed) |
| Hide system cursor | temporary blank system cursors | not possible for a background app with public APIs → the tool sprite is drawn next to the cursor |
| Hotkeys | `RegisterHotKey` | Carbon `RegisterEventHotKey` (no permission) |
| Tray + quick panel | notification-area icon + themed popup | `NSStatusItem` + popover hosting the SwiftUI quick panel |
| Settings window | Win32 + custom draw + Direct2D | SwiftUI, same three themes, live preview (the core `World` rendered into a small Metal view), countdown, presets |
| Settings / log files | `%LOCALAPPDATA%\SnowFall` | `~/Library/Application Support/SnowFall`, log in `~/Library/Logs/SnowFall` |
| Autostart | HKCU Run | `SMAppService.mainApp` (login item) |
| Sleep / lock / battery | WM_POWERBROADCAST, WTS | `NSWorkspace` sleep/wake + screen lock notifications, `IOPowerSources` |
| Crash safety | minidump | `NSSetUncaughtExceptionHandler` + signal handler writing a short report next to the log |

## Phases

1. **Core preparation** (on Windows, no Mac needed): move OS-independent pieces out of
   `platform/windows/app` into `core/`: the simulation thread (`SimRunner`, std::thread +
   condition variable instead of Win32 events), `PerformanceGovernor`, the RU/EN string table
   (shared by both UIs), skin names. Windows keeps working unchanged; all tests stay green.
2. **Snow on screen (MVP)**: overlay windows per screen, Metal renderer (drifts + sprites),
   window tracker, Dock surface, menu-bar item with Pause / Weather / Clear / Quit, settings
   load/save, log, auto-pause. CI builds `SnowFall.app` and a `.dmg` artifact.
   → first test on the M2: snow falls, settles on windows and the Dock, follows dragged windows,
   no clicks are blocked, CPU/GPU usage.
3. **Interaction**: event tap + Accessibility onboarding, tools (snowball, mitten, shovel),
   snowmen, snowplow on the Dock, icicles, hotkeys, umbrella cursor, festive effects.
4. **Settings and polish**: SwiftUI settings window (three themes, preview, presets,
   exclusions), quick panel, login item, release `.dmg` in GitHub Releases, README section.

Each phase ends with a build in CI and a checklist for a manual test on the M2 (screenshots +
the log file are enough for remote diagnosis).

## Building and running

```sh
# universal app + dmg (what CI ships), Xcode generator
cmake -S . -B build -G Xcode && cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure      # core + mac_unit + mac_selftest
# quicker local loop (host architecture only)
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja
build-ninja/SnowFall.app/Contents/MacOS/SnowFall --selftest
```

The root CMake sets `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`, and `arm64;x86_64` for the Xcode
generator (CMake cannot build multi-arch Swift with Ninja). Unix Makefiles still work but
build the app without the SwiftUI module (menu-only fallback).

Command line: `SnowFall [--data-dir <dir>] [--settings] [--exit] [--selftest]`. A second
launch (or a double-click while running) opens the settings of the running instance.

Files: settings `~/Library/Application Support/SnowFall/settings.json` (atomic write,
`.corrupt` kept), log `~/Library/Logs/SnowFall/snowfall.log` (256 KB + one previous file; 2 MB with «Detailed log»), crash reports
`crash-*.txt` next to the log (the 5 newest are kept). With `--data-dir` everything lives in that directory.

### Shell layout (`platform/macos`)

| Path | What |
|---|---|
| `app/` | `main.mm` (single instance, logging, NSApplication), `AppHost` (wiring, governor, settings, hotkeys, menu), `SFAppActionsImpl` (bridge), status item, `InteractionController` (event tap), preview view, strings, icon art, `selftest.mm` |
| `overlay/` | `OverlaySystem`: per-screen windows, `CAMetalLayer`, render thread |
| `render/` | `MetalRenderer` (shared by overlays, preview, self-test) and `msl_translate` — the Windows HLSL (`platform/windows/rendering/d3d11/shaders.h`) is translated to MSL at start-up, so both platforms draw from one shader source; `drift_mesh.cpp` is shared too |
| `tracker/` | `desktop_model` (pure: coordinates, Dock geometry, classification, layout builder) and `WindowTracker` (CGWindowList poller) |
| `system/` | settings store, Carbon hotkeys + key map, login item, power/lock/sleep events, crash handler, Accessibility helpers |
| `diagnostics/` | logger backend (`snow_diag` on macOS) |
| `tests/` | `snow_mac_tests` (ctest `mac_unit`) |
| `packaging/` | `Info.plist.in`, `make_dmg.sh` (`make_dmg` target, part of ALL) |
| `tools/` | `snow_icon_gen`: renders `AppIcon.icns` at build time |

What CI verifies: compilation (universal), core + mac unit tests, and `--selftest`, which
compiles the translated shaders on the runner's GPU and checks pixels of offscreen renders
(transparency, drift colour, scissor clipping, all 81 sprite variants non-empty, 1x and 2x),
creates the overlay windows and presents frames, runs the window tracker, the simulation
thread, Carbon hotkeys, the settings store, and finally starts the whole `AppHost` with the
SwiftUI module and opens the settings window. It never reads the screen.

## UI module (`platform/macos/ui`)

SwiftUI settings window + quick panel behind the contract in `bridge/SFBridge.h`
(static Swift library `snowfall_ui`, module `SnowFallUI`). The app shell consumes it with:

```cmake
# platform/macos/CMakeLists.txt — languages must be enabled HERE (the directory common to the
# app target and the UI), and the build needs -G Ninja or -G Xcode (Makefiles cannot do Swift).
enable_language(OBJC OBJCXX Swift)
add_subdirectory(ui)
# ... add_executable(SnowFall ...), its compile options ...
snowfall_ui_attach(SnowFall)   # links snowfall_ui (-ObjC, AppKit/SwiftUI, bridge include dir) and keeps
                               # the root's -Wall / SNOW_VERSION="…" away from the swiftc link step
```

The app finds the UI only by name: `NSClassFromString(@"SFUIFactory")` (conforms to
`SFUIFactoryProtocol`), so no generated Swift header is needed. Settings JSON goes in and out
whole; keys the UI does not model are preserved. Hotkeys are stored in the core grammar
(`Ctrl+Alt+Shift+Win+K`, see `core/settings/hotkey.h`) and shown as Ctrl / Option / Shift / Cmd.

`snowfall_ui_demo --selftest <dir>` (built in the standalone configuration
`cmake -S platform/macos/ui -B build-ui -G Ninja`) runs the UI against a fake app, checks what
reaches `-applySettingsJSON:` and renders every page in every theme to PNG; CI
(`.github/workflows/mac-ui.yml`) uploads them as the `snowfall-ui-selftest` artifact.

## Known limitations on macOS

- Clicking the snow needs the Accessibility permission (event tap). Without it interaction mode
  is "view only": the tool follows the pointer, clicks reach the apps.
- Without the Accessibility permission the Dock width is estimated from its preferences.
- Window tracking is polled, so a drift may trail a fast-dragged window by a frame or two.
- Without notarization, Gatekeeper asks for confirmation on the first launch.
- Snow is not drawn over the menu bar, Notification Center, Mission Control or the lock screen.
- Tool cursor: by default the system arrow stays visible and the tool is drawn around it
  ("the tool holds the cursor": mitten holding the arrow tip, shovel blade on the tip, snowball
  on the tip, reach ring centred on the tip). `interaction.hideSystemCursor` (experimental)
  hides the arrow like on Windows with the private CoreGraphics property
  "SetsCursorInBackground" (resolved with dlsym; if it is missing or has no effect the app falls
  back to the default for the session and diagnostics report `cursorMode: replace-unavailable`).
  The hide applies to all displays at once; the arrow is shown again over the menu bar and the
  Dock (they cover the snow), on leaving the mode, on sleep / lock / user switch, on quit,
  crash, SIGTERM/SIGINT, and by a 1 s watchdog. Menus of other apps also cover the tool, so
  while they are open in Replace mode the pointer can be hard to see.
