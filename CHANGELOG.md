# Changelog

## 0.1.3 — 2026-11-14

- **Updates from GitHub Releases (Windows and macOS), opt-in.** On first start the app asks once
  «Check for updates?» — one request a day for the latest version number, no data about you.
  Settings → About → Updates: toggle, «Check now», «Update», «What's new», release page. A badge in
  the settings header and a tray / menu-bar item appear when a new version is out.
- Updates install only files listed in `SHA256SUMS` signed with the release key (ECDSA P-256);
  downloads come only from this repository's GitHub releases. Windows runs the installer silently
  and keeps the autostart / shortcut choices; macOS checks the code signature, swaps the app and
  restarts it. Without the update permission the app makes no network requests at all.
- Releases now ship `SHA256SUMS` and `SHA256SUMS.sig`.

## 0.1.2 — 2026-11-14

- **macOS (first test build, macOS 13+, universal Apple Silicon + Intel):** `SnowFall-0.1.2.dmg`.
  Snow on windows and the Dock, tools, snowmen, snowplow, icicles, festive effects, SwiftUI settings
  window with three themes and the menu-bar quick panel. Tool cursor drawn around the arrow by
  default; «Hide the system cursor (experimental)» makes the tool replace it like on Windows.
- Log: compact by default (256 KB + one previous file), «Detailed log» toggle on the
  Performance page (debug records, 2 MB); only the 5 newest crash reports are kept.
- Builds made from a commit other than the release tag show the version as `0.1.2+<commit>`.
- Fix: with monitors stacked vertically, flakes of the lower monitor flickered for a moment
  under the taskbar of the upper one (they started a few pixels above their own monitor).
- Windows: the simulation thread is now shared with the macOS port (no behaviour change intended).

## 0.1.1 — 2026-11-12

- Settings: the live preview label now shows the real speed (×4); user-preset chips no longer
  cut long names, and the quick panel shows three presets without a scrollbar.
- README: clean English screenshots (Russian ones in README.ru), screenshots of all three themes.
- CI: GitHub Actions updated to Node 24 versions.

## 0.1.0 — 2026-11-12

First public version for Windows 11 x64.

- Snow on window tops, the taskbar and the screen bottom; drifts move with windows, hide behind
  overlapping windows, shed in chunks on a jerk; natural lumpy caps, icicles, thaw.
- Weather presets Light / Normal / Blizzard / Thaw, wind and gusts, umbrella cursor.
- Tools: snowball (roll, throw, splat on windows, stack snowmen), mitten (sculpt snowmen, brush),
  shovel (scoop and fling, knock snowmen over).
- Snowmen with 12 skins, falling snowmen (configurable), realistic breakup by plow and shovel.
- Toy snowplow for the taskbar, garland, New Year fireworks.
- Settings window with three themes and live preview, tray quick panel, user presets,
  per-application exclusions, Russian and English UI.
- Snowfall on every monitor (stacked monitors), snowplows on every taskbar with joined routes
  for side-by-side taskbars; snowballs knock icicles down.
- Per-user installer with migration from the earlier "Windows Snow" builds, portable zip
  (an unsigned MSIX can be built locally).
