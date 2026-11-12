# SnowFall for macOS — port plan

Status: not started. The portable `core/` (simulation, drifts, tools, snowmen, snowplow,
settings model/JSON) already builds and passes its unit tests on macOS in CI.

## Decisions

| Topic | Decision |
|---|---|
| Target | macOS 13 Ventura and newer, universal binary (Apple Silicon first-class, Intel built too). Test machine: M2. |
| Languages | Objective-C++ for the platform layer (talks to the C++ core directly); **SwiftUI** for the settings window and the menu-bar quick panel, bridged through a small Objective-C++ façade. |
| Rendering | Metal (`CAMetalLayer`), shaders ported from the HLSL sprite/drift SDF shaders. |
| Build | CMake (Xcode or Ninja generator), `.app` bundle with `LSUIElement` (no Dock icon). |
| Signing | No Apple Developer account for now: ad-hoc signed, not notarized. First launch: right-click → Open (documented in the README / release notes). |
| Distribution | `.dmg` built in CI and attached to GitHub releases. |

## Architecture mapping

| Piece | Windows | macOS |
|---|---|---|
| Overlay | per-monitor layered click-through window + D3D11 + DirectComposition | one borderless `NSWindow` per `NSScreen`: level above normal windows (below the menu bar/Dock menus), `ignoresMouseEvents = YES`, `collectionBehavior = canJoinAllSpaces | stationary | fullScreenAuxiliary | ignoresCycle`, transparent `CAMetalLayer` |
| Frame pacing | vsync Present | `CVDisplayLink` (macOS 13) / `CADisplayLink` (14+) per screen |
| Window tracking → `DesktopLayout` | `EnumWindows` + WinEvent hooks | `CGWindowListCopyWindowInfo(onScreenOnly)` on a background queue at ~30 Hz (bounds, layer, owner PID, Z-order; window titles are never requested, so no Screen Recording permission). Diff against the last layout, publish only on change; faster polling while a window is being dragged (mouse button down). |
| Coordinates | physical pixels, `dpiScale` per monitor | CG global coordinates (top-left origin, points) are used as-is; `dpiScale = 1` for simulation sizes; the renderer draws at `backingScaleFactor` for crisp output |
| "Taskbar" surface | `Shell_TrayWnd` | the Dock (bottom/left/right, auto-hide aware: derived from `NSScreen.frame` vs `visibleFrame` and the Dock's window from the window list); screen bottom when the Dock is hidden or on another side |
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

- The system cursor stays visible in interaction mode (the tool is drawn next to it).
- Window tracking is polled, so a drift may trail a fast-dragged window by a frame or two.
- Without notarization, Gatekeeper asks for confirmation on the first launch.
- Snow is not drawn over the menu bar, Notification Center, Mission Control or the lock screen.
