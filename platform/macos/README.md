# SnowFall for macOS — port plan

Not implemented yet. The portable `core/` (simulation, drifts, tools, snowmen, settings model)
is written without OS headers and is meant to build on macOS as is
(`cmake -S . -B build && cmake --build build` builds the core and its unit tests); the CI
workflow builds and tests it on macOS and Linux. The macOS layer has to provide what
`platform/windows` provides:

| Piece | Windows | macOS plan |
|---|---|---|
| Overlay | per-monitor `WS_EX_LAYERED|TRANSPARENT` window + D3D11 + DirectComposition | borderless `NSWindow` per screen, level above normal windows, `ignoresMouseEvents = true`, transparent `CAMetalLayer` |
| Renderer | HLSL SDF sprite + drift shaders | port the shaders to Metal Shading Language (same sprite/drift model, `FrameSnapshot` unchanged) |
| Window tracking → `DesktopLayout` | `EnumWindows` + WinEvent hooks | `CGWindowListCopyWindowInfo` (bounds and Z-order need no permission), polled ~30 Hz; optional Accessibility observers for instant updates |
| Taskbar surface | `Shell_TrayWnd` | Dock and menu-bar frames (`NSScreen.visibleFrame` vs `frame`) |
| Interaction input | `WH_MOUSE_LL` only while the mode is active | `CGEventTap` while the mode is active (needs the Accessibility permission; explain it on first use) |
| Hide system cursor | temporary blank system cursors | `CGDisplayHideCursor` while the tool cursor is shown |
| Tray + quick panel | notification-area icon, themed popup | `NSStatusItem` with a popover |
| Settings window | Win32 + custom draw + Direct2D | SwiftUI (same settings model and JSON) |
| Storage | `%LOCALAPPDATA%\SnowFall` | `~/Library/Application Support/SnowFall` |
| Autostart | HKCU Run | `SMAppService.mainApp` (login item) |
| Distribution | installer / MSIX | signed + notarized `.dmg` (Apple Developer account) |

Prerequisites already done in the repository: OS-free core, logger and file I/O behind
interfaces, settings JSON shared by both platforms.
