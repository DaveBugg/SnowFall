# Development guide

## Layout

```
core/                     portable C++20 — no OS headers (enforced by review)
  core/                   data contracts: settings.h, layout.h, snapshot.h, input.h, skins.h, types.h
  simulation/             World facade (world.h), weather, particles, chunks, fireworks
  surfaces/               surface registry, heightmaps, rectangle geometry (occlusion)
  interaction/            tools, snowballs & snowmen, snowplow, icicles, splats
  settings/               JSON parser/writer, settings mapping + migrations + sanitize(), hotkeys
  diagnostics/            logger interface (log.h) + portable stderr backend
  tests/                  tiny test harness (snow_test.h) and core unit tests
platform/windows/         Windows 11 implementation
  app/                    entry point, AppHost, SimRunner, input, governor, cursor, UI (app/ui)
  tracker/                WindowTracker → DesktopLayout
  rendering/d3d11/        OverlaySystem: per-monitor transparent overlays, HLSL SDF shaders
  settings/               SettingsStore file I/O, autostart
  diagnostics/            file logger
  installer/ packaging/   SnowFallSetup.exe, portable zip, MSIX
  tests/                  Windows-only unit tests and integration tests/demos
  poc/                    the original overlay proof of concept
platform/macos/           port plan
```

Data flow: the platform publishes a `DesktopLayout` (monitors + windows in Z-order, physical
pixels) and `InputEvent`s; the simulation thread advances `snow::World` at a fixed 1/60 s and
publishes immutable `FrameSnapshot`s; the renderer draws the latest snapshot. All coordinates
are physical virtual-desktop pixels; settings in "px at 96 DPI" are multiplied by `dpiScale`.

## Build (Windows)

Visual Studio 2022 Build Tools (MSVC x64), Windows SDK 10.0.22621+, CMake ≥ 3.24, Ninja.

```bat
build.bat build Release                       :: all targets into build\
build.bat build Release snow_sim_tests        :: one target
```

`build.bat` locates MSVC via vswhere, configures with Ninja and builds. Sources are globbed per
module, so adding a `.cpp` needs no CMake edit (re-run the build to re-glob).

## Tests

Run from the build directory:

```bat
snow_sim_tests && snow_settings_tests && snow_settings_win_tests
tracker_dump --selftest
render_demo --selftest
ui_demo --selftest
e2e_smoke <full path to SnowFall.exe>
```

Pixel-based self-tests create their own windows and read pixels only inside them. A running
SnowFall instance can drop snow onto those test windows and make pixel checks flaky — close
the app (tray → Exit) before running `render_demo`, `ui_demo` and `e2e_smoke`.

`render_demo --gallery`, `--gallery2`, `--gallery3` render every sprite into PNGs for visual review.

## Conventions

- C++20, warnings as errors in spirit (`/W4` clean), no third-party dependencies.
- UTF-8 sources; user-visible strings in Russian and English.
- Public headers of modules are contracts — keep them source-compatible, put state behind `Impl`.
- Line endings are normalised by `.gitattributes`; formatting by `.editorconfig`.
