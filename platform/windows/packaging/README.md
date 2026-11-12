# Packaging SnowFall

Three distributions are built from the same `SnowFall.exe` (static CRT, no dependencies):

| Package | Command (repo root, cmd.exe) | Output |
|---|---|---|
| Portable zip | `platform\windows\packaging\make_portable.bat` | `dist\SnowFall-0.1.0-portable-x64.zip` |
| Per-user installer | `build.bat build Release SnowFallSetup` | `build\SnowFallSetup.exe` |
| MSIX (unsigned) | `platform\windows\packaging\make_msix.bat [build-dir]` | `dist\SnowFall-0.1.0-x64.msix` |

From Git Bash use `cmd //c ".\\build.bat build Release SnowFallSetup"` and
`cmd //c "platform\\windows\\packaging\\make_msix.bat build"`.
Requirements: VS 2022 Build Tools (MSVC x64), CMake ≥ 3.24, Ninja, Windows 10/11 SDK
(`makeappx.exe`, `signtool.exe`). Nothing is downloaded.

All artwork (installer icon, MSIX logos) is produced at build time by `snow_art.exe`
(`platform/windows/packaging/art/snow_art.cpp`), which renders the app's own procedural snowflake badge
(`platform/windows/app/ui/app_icon.cpp`); the app and installer ship no binary art assets (the PNGs in `docs/images` are only README screenshots).

## Portable zip

`make_portable.bat` builds Release into `build\` and zips `SnowFall.exe`, `README.md` and
`Start portable.cmd` (starts the app with `--data-dir "%~dp0data"`, so settings stay next to the exe).

## Installer — `SnowFallSetup.exe`

Neither Inno Setup nor WiX is available on the build machine, so the installer is a small
Win32 program (`platform/windows/installer/setup_main.cpp`). The CMake target `SnowFallSetup` depends on
`SnowFall` and embeds the built `SnowFall.exe` as an `RCDATA` resource (generated
`build\packaging\setup_payload.rc`). To embed a prebuilt exe instead:
`cmake -S . -B build -DSNOW_SETUP_PAYLOAD=C:/path/SnowFall.exe`.

* Per-user, never elevates (`asInvoker`), Per-Monitor-V2 DPI aware, Russian UI with English
  fallback (by `GetUserDefaultUILanguage`).
* Wizard: welcome text, install folder (default `%LOCALAPPDATA%\Programs\SnowFall`, folder
  picker), check boxes «Создать ярлык в меню Пуск» (on), «Ярлык на рабочем столе» (off),
  «Запускать вместе с Windows» (on), «Запустить после установки» (on); progress; done.
* What it writes:
  * `<dir>\SnowFall.exe`, `<dir>\SnowFallSetup.exe` (copy of itself, used as the uninstaller);
  * `%APPDATA%\Microsoft\Windows\Start Menu\Programs\SnowFall.lnk` (optional), `Desktop\SnowFall.lnk` (optional);
  * `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `SnowFall` =
    `"<dir>\SnowFall.exe" --autostart` (same format as `platform/windows/settings/autostart.cpp`, so the
    app's own «Автозапуск» toggle recognises it);
  * `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\SnowFall`: `DisplayName`,
    `DisplayVersion` 0.1.0, `Publisher`, `InstallLocation`, `DisplayIcon`, `UninstallString`
    (`"<dir>\SnowFallSetup.exe" --uninstall`), `QuietUninstallString`, `InstallDate`,
    `EstimatedSize`, `NoModify`, `NoRepair`.
* A running SnowFall **started from the install folder** is asked to exit
  (`FindWindow("SnowFallHost")` + `PostMessage(WM_APP+120, 2)`), waited for 10 s, then the user
  is asked to close it (Retry/Cancel). A copy running from somewhere else (portable/dev build) is
  left alone; if «Запустить после установки» is on, the wizard asks whether to close that copy
  (default: No; silent mode: never) — otherwise the new copy would just hand over to it.
* Re-running the installer updates in place (the registered `InstallLocation` is the default).
* Diagnostics (paths only) are appended to `%TEMP%\SnowFallSetup.log`.

Command line:

```
SnowFallSetup.exe                                   wizard
SnowFallSetup.exe /S [/D=<dir>]                     silent install with the defaults above
    /NOSTARTMENU  /DESKTOP  /NOAUTOSTART  /NOLAUNCH     change the silent defaults
SnowFallSetup.exe --uninstall [/S] [/DELSETTINGS] [/D=<dir>]
```

`/D=` must be the last argument; it may contain spaces, either unquoted (`/D=C:\My Apps\SnowFall`)
or quoted as a whole (`"/D=C:\My Apps\SnowFall"`). Exit codes: 0 ok, 1 failed,
2 cancelled / bad arguments, 3 the running app did not exit.

### Migration from "Windows Snow" (the product's previous name)

Installing SnowFall (wizard or `/S`) detects the previous installation: the key
`HKCU\…\Uninstall\WindowsSnow`, or, if that key is missing, `WindowsSnow.exe` in the old default
folder `%LOCALAPPDATA%\Programs\WindowsSnow`. The wizard then shows «Найдена предыдущая версия
Windows Snow — она будет удалена, настройки сохранятся». If the old Run value `WindowsSnow`
exists, «Запускать вместе с Windows» stays on (unless `/NOAUTOSTART`). Before installing, the setup
removes the old installation itself, without running the old uninstaller:
* asks a running old app **started from the old folder** to exit (`FindWindow("WindowsSnowHost")` +
  `PostMessage(WM_APP+120, 2)`), waits, then offers Retry/Cancel. Old copies running from elsewhere are left alone;
* deletes `WindowsSnow.exe`, `WindowsSnowSetup.exe` and the folder (if it is then empty);
* deletes `Windows Snow.lnk` in Start menu Programs and on the Desktop, **only if the shortcut
  points into the old folder**;
* deletes the Run value `WindowsSnow` if it points into the old folder;
* deletes `HKCU\…\Uninstall\WindowsSnow`, unless the old exe could not be deleted (then the entry
  stays, so the leftovers can still be removed from Settings → Apps).

`%LOCALAPPDATA%\WindowsSnow` (old settings and log) is **never** touched: SnowFall copies
`settings.json` from it on first start. The old names are in the `legacy` namespace in
`platform/windows/installer/setup_main.cpp`.

Hidden test switches redirect the detection to a fake old install. If any of them is given, the
fallback to the real old folder is disabled:
`/LEGACYKEY=<Uninstall subkey name>`, `/LEGACYRUN=<Run value name>`,
`/LEGACYLINKDIR=<folder searched for "Windows Snow.lnk" instead of Start menu/Desktop>`,
`/LEGACYDIR=<old folder used when the key is missing>`.

Uninstall (`Settings → Apps`, or `--uninstall`): confirmation page with «Удалить также
настройки и журнал» (off by default; silent mode keeps settings unless `/DELSETTINGS`), closes the
app, removes the two files, the folder if it is empty, the shortcuts and the Run value **only if
they point at this installation**, and the Uninstall key. Because the uninstaller is itself in the
install folder, it copies itself to `%TEMP%\SnowFallUninstall-<pid>.exe`, starts that copy
(which waits on an inherited handle of the original process) and exits; the temporary copy
deletes itself through a hidden `cmd.exe` loop when it finishes
(`MoveFileEx(…, MOVEFILE_DELAY_UNTIL_REBOOT)` is only a fallback — it needs admin rights to work).
Settings in `%LOCALAPPDATA%\SnowFall` are not touched unless requested.

## MSIX

`platform\windows\packaging\make_msix.bat [build-dir]` builds `SnowFall` and `snow_art`, stages
`dist\msix-stage\` (`AppxManifest.xml` from `packaging\msix\`, `SnowFall.exe`,
`Assets\*.png` generated by `snow_art --msix`) and runs
`makeappx pack /h SHA256` → `dist\SnowFall-0.1.0-x64.msix`. Set `SNOW_EXE=<path>` to package a
prebuilt exe. The package is **unsigned**: Windows will not install it until it is signed.

Manifest: `Identity Name="SnowFall" Publisher="CN=SnowFall Dev"`, x64,
`EntryPoint="Windows.FullTrustApplication"`, capability `runFullTrust`, plus a
`windows.startupTask` (disabled by default; the user can enable it in Settings → Apps → Startup).
Keep `Version` in sync with `CMakeLists.txt`.

Packaged-app caveats: HKCU writes of an MSIX app are virtualized, so the app's own
«Автозапуск» (Run key) has no effect there — use the startup task; `%LOCALAPPDATA%\SnowFall` is
redirected to the package's private `LocalCache`.

## Code signing

Nothing in this repository creates, imports or installs certificates. To sign:

1. Obtain a code-signing certificate (an OV/EV certificate from a CA for public distribution;
   for internal testing a self-signed one created **by you** with
   `New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=SnowFall Dev" -CertStoreLocation Cert:\CurrentUser\My`,
   which then has to be trusted on the test machine — `Trusted People` for MSIX).
2. The certificate subject must equal the manifest `Publisher` (`CN=SnowFall Dev`); change both
   together for a real certificate.
3. Sign (SDK `signtool.exe`, `x64` folder):

```bat
signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 build\SnowFall.exe
rem rebuild SnowFallSetup so it embeds the signed exe, then:
signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 build\SnowFallSetup.exe
signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 dist\SnowFall-0.1.0-x64.msix
```

(`/a` picks the best certificate from the store; use `/f cert.pfx /p …` or `/sha1 <thumbprint>` to be
explicit. Timestamping contacts the CA's server — run it on a machine where that is allowed.)
Sign `SnowFall.exe` **before** packing the MSIX and before building the installer, so the
embedded/packaged copies are signed too.

## Soak test — `soak_test.exe`

`platform/windows/tests/integration/soak/soak_test.cpp`, target `soak_test`
(`build.bat build Release SnowFall soak_test`).

```
soak_test.exe [--exe <SnowFall.exe>] [--minutes 480] [--windows 10] [--rect l,t,r,b]
              [--sample-sec 60] [--seed N] [--topmost] [--stop-existing]
```

* Starts `SnowFall.exe` (default: next to `soak_test.exe`) with its own temporary
  `--data-dir` and blizzard settings.
* Creates N of its own captioned windows in the rect (default: bottom-right quarter of the
  primary monitor) and keeps dragging / moving / resizing / minimizing / restoring /
  closing+recreating them without ever activating them (no focus stealing). Other programs'
  windows are never touched or read.
* Every 20–60 s sends a remote command: clear snow, snowplow, pause (unpaused after 3–10 s),
  or an interaction-mode episode of 1–2.5 s at most every 3–7 min (that mode hides the system
  cursor; the test verifies through the app's `cursor-hidden.marker` that it is off again, and
  switches it off on Ctrl+C / console close too).
* Every `--sample-sec` writes a row to `soak_<timestamp>.csv` next to the exe: private bytes,
  working set, handles, GDI/USER objects, threads, CPU % (whole machine and one core), host
  window response time (`SendMessageTimeout(WM_NULL)`, 5 s), window ops, remote commands.
* At the end it asks the app to exit and checks exit code 0 and that no host/overlay windows
  remain, then prints (and saves as `soak_<timestamp>_summary.txt`) the summary: start/end/peak
  memory, least-squares slope of private bytes over the last 75 % of the run (MB/h),
  handle/GDI/USER growth over the same window (the t=0 sample is taken during app start-up), CPU, hangs. The app log is kept as `soak_<timestamp>_app.log`.
* Exit code 1 if the slope > 2 MB/h, handles grew by > 200, the app died, hung (two missed
  answers in a row) or did not exit cleanly; 2 for usage errors.
* Only one SnowFall can run per session: if one is already running the test refuses to
  start, unless `--stop-existing` is given (then it asks that copy to exit first; restart it
  yourself afterwards).

The acceptance run is 8 hours (`--minutes 480`, the default). Short runs (e.g. `--minutes 10`)
validate the harness; their slope includes warm-up effects and is not a leak verdict.
