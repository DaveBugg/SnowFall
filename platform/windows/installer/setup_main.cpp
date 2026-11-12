// SnowFallSetup.exe — per-user installer / uninstaller for SnowFall.
//
// No elevation (asInvoker), no services, no network. Everything goes to the current user's
// profile: %LOCALAPPDATA%\Programs\SnowFall (default), the user's Start menu / desktop,
// HKCU\...\Run (autostart) and HKCU\...\Uninstall\SnowFall ("Apps" in Settings).
//
// Command line:
//   SnowFallSetup.exe                      wizard
//   SnowFallSetup.exe /S [/D=<dir>]        silent install with defaults
//       /NOSTARTMENU /DESKTOP /NOAUTOSTART /NOLAUNCH   change the silent defaults
//   SnowFallSetup.exe --uninstall [/S] [/DELSETTINGS] [/D=<dir>]
// /D= must be the last argument (it may contain spaces, quotes optional).
// Exit codes: 0 ok, 1 failed, 2 cancelled / bad arguments, 3 the running app did not exit.
// Diagnostics (paths only) are appended to %TEMP%\SnowFallSetup.log.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kAppName[] = L"SnowFall";
constexpr wchar_t kVersion[] = L"" SNOW_VERSION;
constexpr wchar_t kExeName[] = L"SnowFall.exe";
constexpr wchar_t kSetupName[] = L"SnowFallSetup.exe";
constexpr wchar_t kLinkName[] = L"SnowFall.lnk";
constexpr wchar_t kHostClass[] = L"SnowFallHost";   // snow::AppHost::kHostClass
constexpr UINT kMsgRemote = WM_APP + 120;              // snow::AppHost::kMsgRemote
constexpr WPARAM kRemoteExit = 2;
constexpr wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowFall";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"SnowFall";        // snow::autostart::kValueName
constexpr int kPayloadId = 101;
constexpr int kIconId = 1;

// ---- legacy: the product was called "Windows Snow" before it was renamed to SnowFall --------
// Exactly what the old WindowsSnowSetup.exe created. Migration removes these (files, folder if
// empty, shortcuts pointing into the old folder, Run value, uninstall key) and never touches the
// old settings folder %LOCALAPPDATA%\WindowsSnow (SnowFall imports settings.json from it).
namespace legacy {
constexpr wchar_t kUninstName[] = L"WindowsSnow";      // HKCU\...\Uninstall\WindowsSnow
constexpr wchar_t kRunValue[] = L"WindowsSnow";        // HKCU\...\Run
constexpr wchar_t kExeName[] = L"WindowsSnow.exe";
constexpr wchar_t kSetupName[] = L"WindowsSnowSetup.exe";
constexpr wchar_t kLinkName[] = L"Windows Snow.lnk";   // Start menu Programs and Desktop
constexpr wchar_t kHostClass[] = L"WindowsSnowHost";   // remote "exit" = PostMessage(WM_APP+120, 2)
}  // namespace legacy
constexpr wchar_t kUninstRoot[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";

enum ExitCode { kOk = 0, kFailed = 1, kCancelled = 2, kAppStillRunning = 3 };

// ---------------------------------------------------------------- strings (RU / EN) --------
bool g_ru = false;

enum class S {
    TitleInstall, TitleUninstall, Subtitle, Welcome, FolderLabel, Browse, BrowseTitle, ChkStartMenu,
    ChkDesktop, ChkAutostart, ChkLaunch, Install, Cancel, Finish, Uninstall, Retry, StClosing, StCopying,
    StShortcuts, StRegistry, StLaunching, StRemoving, StCleaning, DoneInstall, DoneUninstall, FailInstall,
    FailUninstall, UninstallText, ChkDelSettings, AppRunning, OtherInstance, BadPath, NotInstalled,
    WriteFailed, ConfirmCancel, Usage, LegacyNote, StMigrating, AppRunningLegacy,
};

struct Pair {
    const wchar_t* ru;
    const wchar_t* en;
};

const Pair kStrings[] = {
    {L"Установка SnowFall", L"SnowFall Setup"},
    {L"Удаление SnowFall", L"SnowFall Uninstall"},
    {L"Снег на рабочем столе  ·  версия ", L"Snow on your desktop  ·  version "},
    {L"Программа установит SnowFall для текущего пользователя — права администратора не нужны. "
     L"Снег будет падать поверх рабочего стола, оседать на окнах и на панели задач.",
     L"SnowFall will be installed for the current user — no administrator rights needed. "
     L"Snow will fall over your desktop and settle on windows and the taskbar."},
    {L"Папка установки:", L"Install folder:"},
    {L"Обзор…", L"Browse…"},
    {L"Выберите папку для установки SnowFall", L"Choose a folder for SnowFall"},
    {L"Создать ярлык в меню Пуск", L"Create a Start menu shortcut"},
    {L"Ярлык на рабочем столе", L"Desktop shortcut"},
    {L"Запускать вместе с Windows", L"Start with Windows"},
    {L"Запустить после установки", L"Launch after installation"},
    {L"Установить", L"Install"},
    {L"Отмена", L"Cancel"},
    {L"Готово", L"Finish"},
    {L"Удалить", L"Uninstall"},
    {L"Повторить", L"Retry"},
    {L"Закрываем запущенную копию SnowFall…", L"Closing the running SnowFall…"},
    {L"Копируем файлы…", L"Copying files…"},
    {L"Создаём ярлыки…", L"Creating shortcuts…"},
    {L"Записываем параметры…", L"Writing settings…"},
    {L"Запускаем SnowFall…", L"Starting SnowFall…"},
    {L"Удаляем файлы…", L"Removing files…"},
    {L"Удаляем ярлыки и записи реестра…", L"Removing shortcuts and registry entries…"},
    {L"SnowFall установлен.\n\nЗначок-снежинка живёт в области уведомлений (трее): щелчок ставит снег "
     L"на паузу, правая кнопка открывает меню и настройки.\n\nУдалить программу можно в «Параметры → "
     L"Приложения → Установленные приложения».",
     L"SnowFall is installed.\n\nThe snowflake icon lives in the notification area (tray): click it to "
     L"pause the snow, right-click for the menu and settings.\n\nYou can uninstall it from Settings → Apps → "
     L"Installed apps."},
    {L"SnowFall удалён с компьютера.", L"SnowFall has been removed from your computer."},
    {L"Установка не завершена.\n\n", L"Setup did not complete.\n\n"},
    {L"Удаление не завершено.\n\n", L"Uninstall did not complete.\n\n"},
    {L"SnowFall будет удалён из папки:\n", L"SnowFall will be removed from:\n"},
    {L"Удалить также настройки и журнал (%LOCALAPPDATA%\\SnowFall)",
     L"Also delete settings and the log (%LOCALAPPDATA%\\SnowFall)"},
    {L"SnowFall всё ещё работает и не закрылся сам.\n\nЗакройте его через меню значка в трее («Выход») "
     L"и нажмите «Повтор».",
     L"SnowFall is still running and did not close by itself.\n\nClose it from the tray icon menu (Exit) "
     L"and press Retry."},
    {L"Сейчас работает другая копия SnowFall:\n%s\n\nЗакрыть её и запустить установленную?",
     L"Another copy of SnowFall is running:\n%s\n\nClose it and start the installed one?"},
    {L"Укажите полный путь к папке, например C:\\Users\\Имя\\AppData\\Local\\Programs\\SnowFall.",
     L"Enter a full folder path, e.g. C:\\Users\\Name\\AppData\\Local\\Programs\\SnowFall."},
    {L"SnowFall не установлен — удалять нечего.", L"SnowFall is not installed — nothing to remove."},
    {L"Не удалось записать файл:\n%s\n\n%s", L"Cannot write the file:\n%s\n\n%s"},
    {L"Прервать установку SnowFall?", L"Cancel SnowFall setup?"},
    {L"SnowFallSetup.exe [/S] [/NOSTARTMENU] [/DESKTOP] [/NOAUTOSTART] [/NOLAUNCH] [/D=папка]\n"
     L"SnowFallSetup.exe --uninstall [/S] [/DELSETTINGS] [/D=папка]",
     L"SnowFallSetup.exe [/S] [/NOSTARTMENU] [/DESKTOP] [/NOAUTOSTART] [/NOLAUNCH] [/D=folder]\n"
     L"SnowFallSetup.exe --uninstall [/S] [/DELSETTINGS] [/D=folder]"},
    {L"Найдена предыдущая версия Windows Snow — она будет удалена, настройки сохранятся.",
     L"A previous version (Windows Snow) was found — it will be removed; your settings are kept."},
    {L"Удаляем предыдущую версию Windows Snow…", L"Removing the previous version (Windows Snow)…"},
    {L"Предыдущая версия Windows Snow всё ещё работает и не закрылась сама.\n\nЗакройте её через меню значка "
     L"в трее («Выход») и нажмите «Повтор».",
     L"The previous version (Windows Snow) is still running and did not close by itself.\n\nClose it from the "
     L"tray icon menu (Exit) and press Retry."},
};

const wchar_t* T(S id) {
    const Pair& p = kStrings[static_cast<int>(id)];
    return g_ru ? p.ru : p.en;
}

// Replaces the first "%s" with `a` and the second with `b`.
std::wstring format(const wchar_t* fmt, const std::wstring& a, const std::wstring& b = {}) {
    std::wstring out = fmt;
    const std::wstring* args[] = {&a, &b};
    size_t pos = 0;
    for (const std::wstring* arg : args) {
        pos = out.find(L"%s", pos);
        if (pos == std::wstring::npos) break;
        out.replace(pos, 2, *arg);
        pos += arg->size();
    }
    return out;
}

// ---------------------------------------------------------------- log ----------------------
std::wstring g_logPath;

void logf(const wchar_t* fmt, ...) {
    if (g_logPath.empty()) return;
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[2200];
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u [%lu] %ls\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, GetCurrentProcessId(), buf);
    char utf8[6600];
    const int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, nullptr, nullptr);
    if (n <= 1) return;
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
    CloseHandle(f);
}

std::wstring systemMessage(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s + L" (" + std::to_wstring(code) + L")";
}

// ---------------------------------------------------------------- paths --------------------
std::wstring modulePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring parentDir(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring fullPath(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    std::wstring r = (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : p;
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    const std::wstring fa = fullPath(a), fb = fullPath(b);
    return CompareStringOrdinal(fa.c_str(), static_cast<int>(fa.size()), fb.c_str(), static_cast<int>(fb.size()),
                                TRUE) == CSTR_EQUAL;
}

bool fileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isAbsolutePath(const std::wstring& p) {
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return true;
    return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\';
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring defaultInstallDir() {
    std::wstring base = knownFolder(FOLDERID_UserProgramFiles);   // %LOCALAPPDATA%\Programs
    if (base.empty()) base = knownFolder(FOLDERID_LocalAppData) + L"\\Programs";
    return base + L"\\SnowFall";
}

std::wstring dataDir() { return knownFolder(FOLDERID_LocalAppData) + L"\\SnowFall"; }

ULONGLONG fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<ULONGLONG>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

// ---------------------------------------------------------------- registry -----------------
bool regGetString(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& out) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr,
                     &bytes) != ERROR_SUCCESS)
        return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf.data(),
                     &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    out = buf;
    return true;
}

bool regSetString(HKEY key, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool regSetDword(HKEY key, const wchar_t* name, DWORD v) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v) == ERROR_SUCCESS;
}

// Same parsing as platform/windows/settings/autostart.cpp: the quoted part, or up to the first space.
std::wstring exeFromCommandLine(const std::wstring& cmd) {
    const size_t begin = cmd.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return {};
    if (cmd[begin] == L'"') {
        const size_t end = cmd.find(L'"', begin + 1);
        return cmd.substr(begin + 1, end == std::wstring::npos ? end : end - begin - 1);
    }
    const size_t end = cmd.find_first_of(L" \t", begin);
    return cmd.substr(begin, end == std::wstring::npos ? end : end - begin);
}

bool runValuePointsTo(const std::wstring& exe) {
    std::wstring cmd;
    return regGetString(HKEY_CURRENT_USER, kRunKey, kRunValue, cmd) && samePath(exeFromCommandLine(cmd), exe);
}

bool setAutostart(const std::wstring& exe) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return false;
    const bool ok = regSetString(key, kRunValue, L"\"" + exe + L"\" --autostart");   // autostart::commandLineFor
    RegCloseKey(key);
    return ok;
}

// Removes the Run value only when it starts OUR exe (a dev/portable copy's autostart is kept).
void removeAutostartIfOurs(const std::wstring& exe) {
    if (!runValuePointsTo(exe)) return;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    const LSTATUS rc = RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);
    logf(L"autostart: removed HKCU Run\\%ls (rc %ld)", kRunValue, static_cast<long>(rc));
}

// ---------------------------------------------------------------- shortcuts ----------------
bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& iconFile, int iconIndex) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(parentDir(target).c_str());
    sl->SetDescription(g_ru ? L"Снег на рабочем столе" : L"Snow on your desktop");
    sl->SetIconLocation(iconFile.c_str(), iconIndex);
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)));
    if (ok) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

std::wstring shortcutTarget(const std::wstring& lnk) {
    std::wstring r;
    if (!fileExists(lnk)) return r;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return r;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        wchar_t buf[MAX_PATH * 2] = {};
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ)) &&
            SUCCEEDED(sl->GetPath(buf, static_cast<int>(std::size(buf)), nullptr, SLGP_RAWPATH)))
            r = buf;
        pf->Release();
    }
    sl->Release();
    return r;
}

void removeShortcutIfOurs(const std::wstring& lnk, const std::wstring& exe) {
    if (!fileExists(lnk)) return;
    if (!samePath(shortcutTarget(lnk), exe)) {
        logf(L"shortcut kept (points elsewhere): %ls", lnk.c_str());
        return;
    }
    const BOOL ok = DeleteFileW(lnk.c_str());
    logf(L"shortcut removed: %ls (%ls)", lnk.c_str(), ok ? L"ok" : L"FAILED");
}

std::wstring startMenuLink() { return knownFolder(FOLDERID_Programs) + L"\\" + kLinkName; }
std::wstring desktopLink() { return knownFolder(FOLDERID_Desktop) + L"\\" + kLinkName; }

// ---------------------------------------------------------------- running app --------------
struct RunningApp {
    HWND host = nullptr;
    DWORD pid = 0;
    std::wstring exe;   // empty if it could not be queried
};

bool findRunningApp(RunningApp& out, const wchar_t* hostClass = kHostClass) {
    out = {};
    out.host = FindWindowW(hostClass, nullptr);
    if (!out.host) return false;
    GetWindowThreadProcessId(out.host, &out.pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, out.pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.exe.assign(buf, n);
        CloseHandle(p);
    }
    return true;
}

// Posts the "exit" remote command (same as `SnowFall.exe --exit`) and waits for the process.
bool requestAppExit(const RunningApp& app, DWORD timeoutMs) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, app.pid);
    PostMessageW(app.host, kMsgRemote, kRemoteExit, 0);
    bool gone = false;
    if (p) {
        gone = WaitForSingleObject(p, timeoutMs) == WAIT_OBJECT_0;
        CloseHandle(p);
    } else {
        const ULONGLONG end = GetTickCount64() + timeoutMs;
        while (IsWindow(app.host) && GetTickCount64() < end) Sleep(100);
        gone = !IsWindow(app.host);
    }
    logf(L"asked the running app (pid %lu) to exit: %ls", app.pid, gone ? L"exited" : L"still running");
    return gone;
}

// ---------------------------------------------------------------- UI bridge ----------------
constexpr UINT WM_APP_STATUS = WM_APP + 1;   // wParam = percent, lParam = new std::wstring*
constexpr UINT WM_APP_ASK = WM_APP + 2;      // wParam = MB_ flags, lParam = const std::wstring*; returns ID*
constexpr UINT WM_APP_DONE = WM_APP + 3;     // wParam = exit code, lParam = new std::wstring* (error)

struct Ui {
    bool silent = true;
    HWND hwnd = nullptr;

    void status(int pct, const wchar_t* text) const {
        logf(L"[%d%%] %ls", pct, text);
        if (!silent && hwnd) PostMessageW(hwnd, WM_APP_STATUS, static_cast<WPARAM>(pct),
                                          reinterpret_cast<LPARAM>(new std::wstring(text)));
    }
    int ask(const std::wstring& text, UINT flags, int silentAnswer) const {
        if (silent || !hwnd) return silentAnswer;
        return static_cast<int>(SendMessageW(hwnd, WM_APP_ASK, flags, reinterpret_cast<LPARAM>(&text)));
    }
};

struct Options {
    bool silent = false;
    bool uninstall = false;
    bool dirGiven = false;
    std::wstring dir;
    bool startMenu = true;
    bool desktop = false;
    bool autostart = true;
    bool launch = true;
    bool deleteSettings = false;
    HANDLE waitFor = nullptr;   // relaunched uninstaller: handle of the original process
    bool help = false;
    bool autostartGiven = false;   // /NOAUTOSTART was passed
    // Legacy detection. Hidden test switches /LEGACYKEY= /LEGACYRUN= /LEGACYLINKDIR= /LEGACYDIR=
    // point it at a fake old install so the migration can be tested without touching a real one.
    std::wstring legacyKey = legacy::kUninstName;
    std::wstring legacyRun = legacy::kRunValue;
    std::wstring legacyLinkDir;    // empty: the real Start menu Programs and Desktop folders
    bool legacyTest = false;       // any /LEGACY*= switch given: no fallback to the real old folder
    std::wstring legacyDir;        // /LEGACYDIR=: fallback old folder used when the key is missing
};

// Makes sure no SnowFall started from `dir` is running. Instances started from elsewhere
// (portable or development builds) are left alone and returned in `other`.
int ensureAppClosed(const std::wstring& dir, const Ui& ui, RunningApp* other, const wchar_t* hostClass = kHostClass) {
    const bool old = hostClass != kHostClass;
    for (int attempt = 0;; ++attempt) {
        RunningApp app;
        if (!findRunningApp(app, hostClass)) return kOk;
        if (app.exe.empty() || !samePath(parentDir(app.exe), dir)) {
            logf(L"another %ls is running from %ls — left alone", old ? L"Windows Snow" : kAppName,
                 app.exe.empty() ? L"?" : app.exe.c_str());
            if (other) *other = app;
            return kOk;
        }
        ui.status(5, old ? T(S::StMigrating) : T(S::StClosing));
        if (requestAppExit(app, 10000)) {
            Sleep(300);   // let the image unmap
            continue;
        }
        if (ui.ask(old ? T(S::AppRunningLegacy) : T(S::AppRunning), MB_RETRYCANCEL | MB_ICONWARNING, IDCANCEL) !=
            IDRETRY)
            return kAppStillRunning;
        (void)attempt;
    }
}

bool writePayload(const std::wstring& dest, std::wstring& err) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadId), RT_RCDATA);
    HGLOBAL g = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || size < 2 || memcmp(data, "MZ", 2) != 0) {
        err = L"payload resource missing";
        return false;
    }
    const std::wstring tmp = dest + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = format(T(S::WriteFailed), tmp, systemMessage(GetLastError()));
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, size, &written, nullptr) && written == size && FlushFileBuffers(f);
    const DWORD e = GetLastError();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        err = format(T(S::WriteFailed), tmp, systemMessage(e));
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD me = GetLastError();
        DeleteFileW(tmp.c_str());
        err = format(T(S::WriteFailed), dest, systemMessage(me));
        return false;
    }
    logf(L"wrote %ls (%lu bytes)", dest.c_str(), size);
    return true;
}

// Retries `op` while the user agrees (interactive) — silent mode fails at once.
template <class F>
bool withRetry(const Ui& ui, std::wstring& err, F op) {
    for (;;) {
        err.clear();
        if (op(err)) return true;
        logf(L"error: %ls", err.c_str());
        if (ui.ask(err, MB_RETRYCANCEL | MB_ICONERROR, IDCANCEL) != IDRETRY) return false;
    }
}

bool launchApp(const std::wstring& exe) {
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    const std::wstring dir = parentDir(exe);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        logf(L"launch failed: %ls", systemMessage(GetLastError()).c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf(L"launched %ls", exe.c_str());
    return true;
}

// Icon for shortcuts / "Apps": the app's own icon if it has one, else this setup's badge icon.
std::wstring iconLocation(const std::wstring& exe, const std::wstring& setup, int& index) {
    index = 0;
    if (ExtractIconExW(exe.c_str(), -1, nullptr, nullptr, 0) > 0) return exe;
    return setup;
}

int migrateLegacy(const Options& o, const Ui& ui);

int doInstall(const Options& o, const Ui& ui, std::wstring& err) {
    const std::wstring dir = fullPath(o.dir);
    const std::wstring exe = dir + L"\\" + kExeName;
    const std::wstring setupCopy = dir + L"\\" + kSetupName;
    const std::wstring self = modulePath();
    logf(L"install to %ls (startmenu %d, desktop %d, autostart %d, launch %d, silent %d)", dir.c_str(), o.startMenu,
         o.desktop, o.autostart, o.launch, o.silent);

    RunningApp other;
    const int closed = ensureAppClosed(dir, ui, &other);
    if (closed != kOk) {
        err = T(S::AppRunning);
        return closed;
    }

    const int migrated = migrateLegacy(o, ui);
    if (migrated != kOk) {
        err = T(S::AppRunningLegacy);
        return migrated;
    }

    ui.status(20, T(S::StCopying));
    const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
        err = format(T(S::WriteFailed), dir, systemMessage(static_cast<DWORD>(rc)));
        return kFailed;
    }
    if (!withRetry(ui, err, [&](std::wstring& e) { return writePayload(exe, e); })) return kFailed;
    ui.status(40, T(S::StCopying));
    if (!samePath(self, setupCopy)) {
        if (!withRetry(ui, err, [&](std::wstring& e) {
                if (CopyFileW(self.c_str(), setupCopy.c_str(), FALSE)) return true;
                e = format(T(S::WriteFailed), setupCopy, systemMessage(GetLastError()));
                return false;
            }))
            return kFailed;
        logf(L"copied the uninstaller to %ls", setupCopy.c_str());
    }

    ui.status(60, T(S::StShortcuts));
    int iconIndex = 0;
    const std::wstring icon = iconLocation(exe, setupCopy, iconIndex);
    const std::wstring sm = startMenuLink(), dt = desktopLink();
    if (o.startMenu) logf(L"start menu shortcut %ls: %ls", sm.c_str(), createShortcut(sm, exe, icon, iconIndex) ? L"ok" : L"FAILED");
    else removeShortcutIfOurs(sm, exe);
    if (o.desktop) logf(L"desktop shortcut %ls: %ls", dt.c_str(), createShortcut(dt, exe, icon, iconIndex) ? L"ok" : L"FAILED");
    else removeShortcutIfOurs(dt, exe);

    ui.status(75, T(S::StRegistry));
    if (o.autostart) logf(L"autostart HKCU Run\\%ls: %ls", kRunValue, setAutostart(exe) ? L"set" : L"FAILED");
    else removeAutostartIfOurs(exe);

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        err = L"HKCU\\" + std::wstring(kUninstKey);
        return kFailed;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t date[16];
    swprintf_s(date, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
    const ULONGLONG kb = (fileSize(exe) + fileSize(setupCopy) + 1023) / 1024;
    bool ok = regSetString(key, L"DisplayName", kAppName) && regSetString(key, L"DisplayVersion", kVersion) &&
              regSetString(key, L"Publisher", L"SnowFall") && regSetString(key, L"InstallLocation", dir) &&
              regSetString(key, L"DisplayIcon", icon + L"," + std::to_wstring(iconIndex)) &&
              regSetString(key, L"UninstallString", L"\"" + setupCopy + L"\" --uninstall") &&
              regSetString(key, L"QuietUninstallString", L"\"" + setupCopy + L"\" --uninstall /S") &&
              regSetString(key, L"InstallDate", date) &&
              regSetDword(key, L"EstimatedSize", static_cast<DWORD>(kb)) && regSetDword(key, L"NoModify", 1) &&
              regSetDword(key, L"NoRepair", 1);
    RegCloseKey(key);
    logf(L"uninstall entry HKCU\\%ls: %ls", kUninstKey, ok ? L"written" : L"FAILED");
    if (!ok) {
        err = L"HKCU\\" + std::wstring(kUninstKey);
        return kFailed;
    }

    if (o.launch) {
        ui.status(90, T(S::StLaunching));
        RunningApp now2;
        bool canStart = true;
        if (findRunningApp(now2)) {
            canStart = false;
            // Default button "No": another (portable/dev) copy is only closed on an explicit "Yes".
            if (ui.ask(format(T(S::OtherInstance), now2.exe.empty() ? L"?" : now2.exe),
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, IDNO) == IDYES)
                canStart = requestAppExit(now2, 10000);
            else
                logf(L"launch skipped: another instance is running");
        }
        if (canStart) launchApp(exe);
    }
    (void)other;
    ui.status(100, T(S::DoneInstall));
    return kOk;
}

bool deleteWithRetries(const std::wstring& p, int tries) {
    for (int i = 0; i < tries; ++i) {
        if (!fileExists(p)) return true;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str())) return true;
        Sleep(250);
    }
    return !fileExists(p);
}

// Recursive delete that never follows junctions/symlinks (they are removed as links).
void deleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) deleteTree(p);
                else RemoveDirectoryW(p.c_str());
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const BOOL ok = RemoveDirectoryW(dir.c_str());
    logf(L"removed %ls: %ls", dir.c_str(), ok ? L"ok" : L"not empty / failed");
}

// ---------------------------------------------------------------- legacy migration ---------
struct LegacyInstall {
    bool found = false;      // the old uninstall key (or the old exe in the old default folder) exists
    std::wstring dir;        // its InstallLocation (may be empty)
    bool runValue = false;   // the old Run value exists
};

std::wstring legacyKeyPath(const Options& o) { return kUninstRoot + o.legacyKey; }

LegacyInstall detectLegacy(const Options& o) {
    LegacyInstall l;
    HKEY key = nullptr;
    if (!o.legacyKey.empty() &&
        RegOpenKeyExW(HKEY_CURRENT_USER, legacyKeyPath(o).c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        l.found = true;
        std::wstring loc;
        if (regGetString(HKEY_CURRENT_USER, legacyKeyPath(o).c_str(), L"InstallLocation", loc) && isAbsolutePath(loc))
            l.dir = fullPath(loc);
    }
    // Fallback: an old install whose uninstall key is gone (default folder of the old installer).
    if (l.dir.empty()) {
        const std::wstring fallback = !o.legacyDir.empty() ? o.legacyDir
                                      : o.legacyTest   ? std::wstring()
                                                       : knownFolder(FOLDERID_UserProgramFiles) + L"\\WindowsSnow";
        if (!fallback.empty() && fileExists(fallback + L"\\" + legacy::kExeName)) {
            l.found = true;
            l.dir = fullPath(fallback);
        }
    }
    std::wstring cmd;
    l.runValue = !o.legacyRun.empty() && regGetString(HKEY_CURRENT_USER, kRunKey, o.legacyRun.c_str(), cmd);
    return l;
}

// Removes the old "Windows Snow" installation the way its own uninstaller would, silently.
// Settings (%LOCALAPPDATA%\WindowsSnow) are kept. Only the old app's closing can stop the new
// install; if the old exe cannot be deleted the old uninstall entry is kept so it stays removable.
int migrateLegacy(const Options& o, const Ui& ui) {
    const LegacyInstall l = detectLegacy(o);
    if (!l.found) return kOk;
    logf(L"legacy: found (key HKCU\\%ls, folder %ls)", legacyKeyPath(o).c_str(), l.dir.empty() ? L"-" : l.dir.c_str());
    ui.status(10, T(S::StMigrating));
    bool filesGone = true;
    if (!l.dir.empty()) {
        const int closed = ensureAppClosed(l.dir, ui, nullptr, legacy::kHostClass);
        if (closed != kOk) return closed;
        const std::wstring oldExe = l.dir + L"\\" + legacy::kExeName;
        for (const std::wstring& f : {oldExe, oldExe + L".new", l.dir + L"\\" + legacy::kSetupName}) {
            if (!fileExists(f)) continue;
            const bool ok = deleteWithRetries(f, 8);
            logf(L"legacy: delete %ls: %ls", f.c_str(), ok ? L"ok" : L"FAILED");
            if (!ok && f == oldExe) filesGone = false;
        }
        const BOOL dirOk = RemoveDirectoryW(l.dir.c_str());   // only if empty
        logf(L"legacy: folder %ls: %ls", l.dir.c_str(), dirOk ? L"removed" : L"kept (not empty or in use)");

        const std::wstring sm = o.legacyLinkDir.empty() ? knownFolder(FOLDERID_Programs) : o.legacyLinkDir;
        const std::wstring dt = o.legacyLinkDir.empty() ? knownFolder(FOLDERID_Desktop) : o.legacyLinkDir;
        for (const std::wstring& folder : {sm, dt}) {
            const std::wstring lnk = folder + L"\\" + legacy::kLinkName;
            if (fileExists(lnk)) {
                const std::wstring target = shortcutTarget(lnk);
                if (!target.empty() && samePath(parentDir(target), l.dir)) {
                    const BOOL ok = DeleteFileW(lnk.c_str());
                    logf(L"legacy: shortcut %ls removed: %ls", lnk.c_str(), ok ? L"ok" : L"FAILED");
                } else {
                    logf(L"legacy: shortcut %ls kept (points elsewhere)", lnk.c_str());
                }
            }
            if (sm == dt) break;
        }
        std::wstring cmd;
        if (l.runValue && regGetString(HKEY_CURRENT_USER, kRunKey, o.legacyRun.c_str(), cmd)) {
            if (samePath(parentDir(exeFromCommandLine(cmd)), l.dir)) {
                HKEY key = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                    const LSTATUS rc = RegDeleteValueW(key, o.legacyRun.c_str());
                    RegCloseKey(key);
                    logf(L"legacy: Run\\%ls removed (rc %ld)", o.legacyRun.c_str(), static_cast<long>(rc));
                }
            } else {
                logf(L"legacy: Run\\%ls kept (points elsewhere)", o.legacyRun.c_str());
            }
        }
    }
    if (filesGone) {
        const LSTATUS rc = RegDeleteTreeW(HKEY_CURRENT_USER, legacyKeyPath(o).c_str());
        if (rc == ERROR_SUCCESS) RegDeleteKeyW(HKEY_CURRENT_USER, legacyKeyPath(o).c_str());
        logf(L"legacy: uninstall key %ls (rc %ld)", rc == ERROR_FILE_NOT_FOUND ? L"was absent" : L"removed",
             static_cast<long>(rc));
    } else {
        logf(L"legacy: old exe still present - uninstall key kept");
    }
    return kOk;
}

int doUninstall(const Options& o, const Ui& ui, std::wstring& err) {
    const std::wstring dir = fullPath(o.dir);
    const std::wstring exe = dir + L"\\" + kExeName;
    const std::wstring setupCopy = dir + L"\\" + kSetupName;
    const std::wstring self = modulePath();
    logf(L"uninstall from %ls (delete settings %d, silent %d)", dir.c_str(), o.deleteSettings, o.silent);

    const int closed = ensureAppClosed(dir, ui, nullptr);
    if (closed != kOk) {
        err = T(S::AppRunning);
        return closed;
    }

    ui.status(25, T(S::StRemoving));
    if (!withRetry(ui, err, [&](std::wstring& e) {
            if (deleteWithRetries(exe, 8)) return true;
            e = format(T(S::WriteFailed), exe, systemMessage(GetLastError()));
            return false;
        }))
        return kFailed;
    logf(L"removed %ls", exe.c_str());
    deleteWithRetries(exe + L".new", 1);
    if (fileExists(setupCopy)) {
        if (samePath(self, setupCopy) || !deleteWithRetries(setupCopy, 40)) {
            // Still mapped (relaunch impossible): best effort removal at the next reboot.
            const BOOL later = MoveFileExW(setupCopy.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            logf(L"cannot delete %ls now; delete-at-reboot: %ls", setupCopy.c_str(), later ? L"scheduled" : L"FAILED");
        } else {
            logf(L"removed %ls", setupCopy.c_str());
        }
    }
    RemoveDirectoryW(dir.c_str());   // only if empty: never touch files we did not create
    logf(L"install dir %ls: %ls", dir.c_str(), GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES ? L"removed" : L"kept (not empty)");

    ui.status(60, T(S::StCleaning));
    removeShortcutIfOurs(startMenuLink(), exe);
    removeShortcutIfOurs(desktopLink(), exe);
    removeAutostartIfOurs(exe);
    std::wstring loc;
    if (!regGetString(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", loc) || samePath(loc, dir)) {
        const LSTATUS rc = RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
        if (rc == ERROR_SUCCESS) RegDeleteKeyW(HKEY_CURRENT_USER, kUninstKey);
        logf(L"uninstall entry removed (rc %ld)", static_cast<long>(rc));
    } else {
        logf(L"uninstall entry kept: it belongs to %ls", loc.c_str());
    }

    if (o.deleteSettings) {
        ui.status(85, T(S::StCleaning));
        deleteTree(dataDir());
    } else {
        logf(L"settings kept in %ls", dataDir().c_str());
    }
    ui.status(100, T(S::DoneUninstall));
    return kOk;
}

// The relaunched uninstaller lives in %TEMP%; remove it once this process has exited.
void scheduleSelfDelete() {
    const std::wstring self = modulePath();
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring cmdExe = std::wstring(sys) + L"\\cmd.exe";
    std::wstring cmd = L"\"" + cmdExe + L"\" /d /s /c \"for /l %i in (1,1,30) do (\"" + sys +
                       L"\\PING.EXE\" -n 2 127.0.0.1 >nul & del /f /q \"" + self + L"\" >nul 2>&1 & if not exist \"" +
                       self + L"\" exit /b 0)\"";
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmdExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS,
                       nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logf(L"temporary uninstaller %ls will be deleted after exit", self.c_str());
    } else {
        MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

// Copies this exe to %TEMP% and runs the copy (which can then delete the installed setup).
bool relaunchFromTemp(const Options& o) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    const std::wstring copy = std::wstring(tmp) + L"SnowFallUninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    if (!CopyFileW(modulePath().c_str(), copy.c_str(), FALSE)) return false;
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE, TRUE, 0)) {
        DeleteFileW(copy.c_str());
        return false;
    }
    std::wstring cmd = L"\"" + copy + L"\" --uninstall --wait-handle " +
                       std::to_wstring(reinterpret_cast<uintptr_t>(self));
    if (o.silent) cmd += L" /S";
    if (o.deleteSettings) cmd += L" /DELSETTINGS";
    cmd += L" /D=" + fullPath(o.dir);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, tmp, &si, &pi);
    CloseHandle(self);
    if (!ok) {
        DeleteFileW(copy.c_str());
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf(L"relaunched the uninstaller from %ls", copy.c_str());
    return true;
}

Options parseCommandLine() {
    Options o;
    std::wstring raw = GetCommandLineW();
    // NSIS style (/D=... unquoted, last) and also a quoted "/D=..." argument.
    for (const wchar_t* tag : {L" /D=", L" /d=", L" \"/D=", L" \"/d="}) {
        const size_t p = raw.find(tag);
        if (p != std::wstring::npos) {
            std::wstring d = raw.substr(p + wcslen(tag));
            raw.resize(p);
            while (!d.empty() && (d.back() == L' ' || d.back() == L'"')) d.pop_back();
            while (!d.empty() && (d.front() == L' ' || d.front() == L'"')) d.erase(0, 1);
            o.dir = d;
            o.dirGiven = !d.empty();
            break;
        }
    }
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(raw.c_str(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            auto is = [&](const wchar_t* s) { return _wcsicmp(a.c_str(), s) == 0; };
            if (is(L"/S")) o.silent = true;
            else if (is(L"--uninstall") || is(L"/uninstall")) o.uninstall = true;
            else if (is(L"/NOSTARTMENU")) o.startMenu = false;
            else if (is(L"/DESKTOP")) o.desktop = true;
            else if (is(L"/NOAUTOSTART")) {
                o.autostart = false;
                o.autostartGiven = true;
            }
            else if (_wcsnicmp(a.c_str(), L"/LEGACYKEY=", 11) == 0) {
                o.legacyKey = a.substr(11);
                o.legacyTest = true;
            }
            else if (_wcsnicmp(a.c_str(), L"/LEGACYRUN=", 11) == 0) {
                o.legacyRun = a.substr(11);
                o.legacyTest = true;
            }
            else if (_wcsnicmp(a.c_str(), L"/LEGACYLINKDIR=", 15) == 0) {
                o.legacyLinkDir = fullPath(a.substr(15));
                o.legacyTest = true;
            }
            else if (_wcsnicmp(a.c_str(), L"/LEGACYDIR=", 11) == 0) {
                o.legacyDir = fullPath(a.substr(11));
                o.legacyTest = true;
            }
            else if (is(L"/NOLAUNCH")) o.launch = false;
            else if (is(L"/DELSETTINGS")) o.deleteSettings = true;
            else if (is(L"--wait-handle") && i + 1 < argc)
                o.waitFor = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[++i], nullptr, 10)));
            else o.help = true;
        }
        LocalFree(argv);
    }
    return o;
}

// Install location for --uninstall: /D=, the registered InstallLocation, or this exe's folder.
bool resolveUninstallDir(Options& o) {
    if (o.dirGiven) return true;
    std::wstring loc;
    if (regGetString(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", loc) && !loc.empty()) {
        o.dir = loc;
        return true;
    }
    const std::wstring here = parentDir(modulePath());
    if (fileExists(here + L"\\" + kExeName)) {
        o.dir = here;
        return true;
    }
    return false;
}

// ================================================================ wizard window ============
enum Ctl {
    IDC_WELCOME = 200, IDC_FOLDER_LABEL, IDC_FOLDER, IDC_BROWSE, IDC_CHK_START, IDC_CHK_DESKTOP, IDC_CHK_AUTO,
    IDC_CHK_LAUNCH, IDC_STATUS, IDC_PROGRESS, IDC_DONE, IDC_UNINST_TEXT, IDC_CHK_DELSET, IDC_LEGACY_NOTE,
};
enum class Page { Welcome, Uninstall, Progress, Done };

struct Wizard {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    Options opt;
    Page page = Page::Welcome;
    UINT dpi = 96;
    HFONT font = nullptr, fontTitle = nullptr, fontSub = nullptr;
    HBITMAP header = nullptr;
    int headerW = 0, headerH = 0;
    HICON badge = nullptr, iconBig = nullptr, iconSmall = nullptr;
    HBRUSH white = nullptr;
    bool busy = false;
    bool legacyFound = false;   // show the "previous version will be removed" note
    int exitCode = kCancelled;
    std::thread worker;

    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }
    HWND ctl(int id) const { return GetDlgItem(hwnd, id); }
};

Wizard g_w;
constexpr int kClientW = 560, kClientH = 430, kHeaderH = 96, kFooterH = 60, kMargin = 28;
constexpr int kLegacyNoteH = 30;   // extra client height when the legacy note is shown
int clientH() { return kClientH + (g_w.legacyFound ? kLegacyNoteH : 0); }

void destroyDpiResources() {
    for (HFONT* f : {&g_w.font, &g_w.fontTitle, &g_w.fontSub}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    if (g_w.header) DeleteObject(g_w.header);
    g_w.header = nullptr;
    if (g_w.badge) DestroyIcon(g_w.badge);
    g_w.badge = nullptr;
}

HFONT makeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, static_cast<int>(g_w.dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

// Header art: a deep-blue winter-night gradient with soft falling snow (rendered per pixel).
void buildHeader() {
    const int w = g_w.px(kClientW), h = g_w.px(kHeaderH);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    g_w.header = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_w.headerW = w;
    g_w.headerH = h;
    if (!g_w.header || !bits) return;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float t = 0.65f * static_cast<float>(x) / static_cast<float>(w) + 0.35f * static_cast<float>(y) / static_cast<float>(h);
            const float r = 18 + (52 - 18) * t, g = 52 + (128 - 52) * t, b = 128 + (214 - 128) * t;
            px[static_cast<size_t>(y) * w + x] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                                                 static_cast<uint32_t>(b);
        }
    }
    unsigned seed = 1229u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) % 10000u) / 10000.0f;
    };
    for (int i = 0; i < 90; ++i) {
        const float cx = rnd() * static_cast<float>(w), cy = rnd() * static_cast<float>(h);
        const float rad = (0.8f + rnd() * 1.8f) * static_cast<float>(g_w.dpi) / 96.0f;
        const float alpha = 0.35f + 0.55f * rnd() * (0.4f + 0.6f * cx / static_cast<float>(w));
        for (int y = static_cast<int>(cy - rad - 1); y <= static_cast<int>(cy + rad + 1); ++y) {
            for (int x = static_cast<int>(cx - rad - 1); x <= static_cast<int>(cx + rad + 1); ++x) {
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
                float cov = rad + 0.5f - std::sqrt(dx * dx + dy * dy);
                cov = (cov < 0 ? 0 : cov > 1 ? 1 : cov) * alpha;
                if (cov <= 0) continue;
                uint32_t& p = px[static_cast<size_t>(y) * w + x];
                auto ch = [&](int s) {
                    const float c = static_cast<float>((p >> s) & 0xFF);
                    return static_cast<uint32_t>(c + (255 - c) * cov) << s;
                };
                p = ch(16) | ch(8) | ch(0);
            }
        }
    }
}

void createDpiResources() {
    destroyDpiResources();
    g_w.font = makeFont(9, FW_NORMAL);
    g_w.fontTitle = makeFont(17, FW_SEMIBOLD);
    g_w.fontSub = makeFont(10, FW_NORMAL);
    buildHeader();
    LoadIconWithScaleDown(g_w.inst, MAKEINTRESOURCEW(kIconId), g_w.px(60), g_w.px(60), &g_w.badge);
}

void layout() {
    const int W = g_w.px(kClientW), H = g_w.px(clientH()), m = g_w.px(kMargin);
    const int inner = W - 2 * m;
    auto place = [&](int id, int x, int y, int w, int h) {
        if (HWND c = g_w.ctl(id)) {
            SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_w.font), FALSE);
        }
    };
    int y = g_w.px(kHeaderH + 20);
    place(IDC_WELCOME, m, y, inner, g_w.px(56));
    place(IDC_UNINST_TEXT, m, y, inner, g_w.px(70));
    place(IDC_DONE, m, y, inner, g_w.px(170));
    place(IDC_CHK_DELSET, m, y + g_w.px(84), inner, g_w.px(40));
    place(IDC_STATUS, m, y + g_w.px(40), inner, g_w.px(22));
    place(IDC_PROGRESS, m, y + g_w.px(68), inner, g_w.px(16));
    y += g_w.px(62);
    if (g_w.legacyFound) {
        place(IDC_LEGACY_NOTE, m, y, inner, g_w.px(kLegacyNoteH - 6));
        y += g_w.px(kLegacyNoteH);
    }
    y += g_w.px(6);
    place(IDC_FOLDER_LABEL, m, y, inner, g_w.px(20));
    y += g_w.px(22);
    const int bw = g_w.px(96);
    place(IDC_FOLDER, m, y, inner - bw - g_w.px(8), g_w.px(28));
    place(IDC_BROWSE, m + inner - bw, y, bw, g_w.px(28));
    y += g_w.px(42);
    for (int id : {IDC_CHK_START, IDC_CHK_DESKTOP, IDC_CHK_AUTO, IDC_CHK_LAUNCH}) {
        place(id, m, y, inner, g_w.px(24));
        y += g_w.px(27);
    }
    const int by = H - g_w.px(kFooterH) + g_w.px(15), bh = g_w.px(30), bwid = g_w.px(112);
    place(IDCANCEL, W - m - bwid, by, bwid, bh);
    place(IDOK, W - m - 2 * bwid - g_w.px(10), by, bwid, bh);
    InvalidateRect(g_w.hwnd, nullptr, TRUE);
}

void showPage(Page p) {
    g_w.page = p;
    auto vis = [](int id, bool on) { ShowWindow(g_w.ctl(id), on ? SW_SHOW : SW_HIDE); };
    const bool welcome = p == Page::Welcome;
    vis(IDC_LEGACY_NOTE, welcome && g_w.legacyFound);
    for (int id : {IDC_WELCOME, IDC_FOLDER_LABEL, IDC_FOLDER, IDC_BROWSE, IDC_CHK_START, IDC_CHK_DESKTOP, IDC_CHK_AUTO,
                   IDC_CHK_LAUNCH})
        vis(id, welcome);
    vis(IDC_UNINST_TEXT, p == Page::Uninstall);
    vis(IDC_CHK_DELSET, p == Page::Uninstall);
    vis(IDC_STATUS, p == Page::Progress);
    vis(IDC_PROGRESS, p == Page::Progress);
    vis(IDC_DONE, p == Page::Done);
    HWND ok = g_w.ctl(IDOK), cancel = g_w.ctl(IDCANCEL);
    switch (p) {
        case Page::Welcome: SetWindowTextW(ok, T(S::Install)); break;
        case Page::Uninstall: SetWindowTextW(ok, T(S::Uninstall)); break;
        case Page::Progress: break;
        case Page::Done: SetWindowTextW(ok, T(S::Finish)); break;
    }
    ShowWindow(ok, p == Page::Progress ? SW_HIDE : SW_SHOW);
    ShowWindow(cancel, p == Page::Welcome || p == Page::Uninstall ? SW_SHOW : SW_HIDE);
    EnableWindow(cancel, TRUE);
    if (p != Page::Progress) SetFocus(ok);
}

HWND addCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, g_w.hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_w.inst, nullptr);
}

void createControls() {
    addCtl(L"STATIC", T(S::Welcome), SS_LEFT | SS_NOPREFIX, IDC_WELCOME);
    addCtl(L"STATIC", T(S::LegacyNote), SS_LEFT | SS_NOPREFIX, IDC_LEGACY_NOTE);
    addCtl(L"STATIC", T(S::FolderLabel), SS_LEFT, IDC_FOLDER_LABEL);
    addCtl(L"EDIT", g_w.opt.dir.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, IDC_FOLDER, WS_EX_CLIENTEDGE);
    addCtl(L"BUTTON", T(S::Browse), WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE);
    const struct {
        int id;
        S text;
        bool on;
    } checks[] = {{IDC_CHK_START, S::ChkStartMenu, g_w.opt.startMenu},
                  {IDC_CHK_DESKTOP, S::ChkDesktop, g_w.opt.desktop},
                  {IDC_CHK_AUTO, S::ChkAutostart, g_w.opt.autostart},
                  {IDC_CHK_LAUNCH, S::ChkLaunch, g_w.opt.launch}};
    for (const auto& c : checks) {
        HWND h = addCtl(L"BUTTON", T(c.text), WS_TABSTOP | BS_AUTOCHECKBOX, c.id);
        SendMessageW(h, BM_SETCHECK, c.on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    addCtl(L"STATIC", (T(S::UninstallText) + fullPath(g_w.opt.dir)).c_str(), SS_LEFT | SS_NOPREFIX, IDC_UNINST_TEXT);
    addCtl(L"BUTTON", T(S::ChkDelSettings), WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, IDC_CHK_DELSET);
    addCtl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS);
    HWND bar = addCtl(PROGRESS_CLASSW, L"", 0, IDC_PROGRESS);
    SendMessageW(bar, PBM_SETRANGE32, 0, 100);
    addCtl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_DONE);
    addCtl(L"BUTTON", T(S::Install), WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    addCtl(L"BUTTON", T(S::Cancel), WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
}

void browseFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD fl = 0;
    dlg->GetOptions(&fl);
    dlg->SetOptions(fl | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(T(S::BrowseTitle));
    wchar_t cur[MAX_PATH * 2] = {};
    GetWindowTextW(g_w.ctl(IDC_FOLDER), cur, static_cast<int>(std::size(cur)));
    std::wstring start = cur;
    while (!start.empty() && GetFileAttributesW(start.c_str()) == INVALID_FILE_ATTRIBUTES) start = parentDir(start);
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    if (SUCCEEDED(dlg->Show(g_w.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                std::wstring p = fullPath(path);
                const std::wstring leaf = p.substr(p.find_last_of(L'\\') + 1);
                if (_wcsicmp(leaf.c_str(), L"SnowFall") != 0) p += (p.back() == L'\\' ? L"" : L"\\") + std::wstring(L"SnowFall");
                SetWindowTextW(g_w.ctl(IDC_FOLDER), p.c_str());
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void startWork() {
    Options o = g_w.opt;
    if (!o.uninstall) {
        wchar_t buf[MAX_PATH * 2] = {};
        GetWindowTextW(g_w.ctl(IDC_FOLDER), buf, static_cast<int>(std::size(buf)));
        o.dir = buf;
        while (!o.dir.empty() && o.dir.back() == L' ') o.dir.pop_back();
        while (!o.dir.empty() && o.dir.front() == L' ') o.dir.erase(0, 1);
        if (!isAbsolutePath(o.dir)) {
            MessageBoxW(g_w.hwnd, T(S::BadPath), T(S::TitleInstall), MB_ICONWARNING);
            SetFocus(g_w.ctl(IDC_FOLDER));
            return;
        }
        auto checked = [](int id) { return SendMessageW(g_w.ctl(id), BM_GETCHECK, 0, 0) == BST_CHECKED; };
        o.startMenu = checked(IDC_CHK_START);
        o.desktop = checked(IDC_CHK_DESKTOP);
        o.autostart = checked(IDC_CHK_AUTO);
        o.launch = checked(IDC_CHK_LAUNCH);
    } else {
        o.deleteSettings = SendMessageW(g_w.ctl(IDC_CHK_DELSET), BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    g_w.opt = o;
    g_w.busy = true;
    showPage(Page::Progress);
    SendMessageW(g_w.ctl(IDC_PROGRESS), PBM_SETPOS, 0, 0);
    HWND hwnd = g_w.hwnd;
    g_w.worker = std::thread([o, hwnd] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Ui ui{false, hwnd};
        std::wstring err;
        const int rc = o.uninstall ? doUninstall(o, ui, err) : doInstall(o, ui, err);
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_DONE, static_cast<WPARAM>(rc), reinterpret_cast<LPARAM>(new std::wstring(err)));
    });
}

void paint(HDC dc) {
    RECT rc;
    GetClientRect(g_w.hwnd, &rc);
    const int hh = g_w.px(kHeaderH), fh = g_w.px(kFooterH);
    RECT body{0, hh, rc.right, rc.bottom - fh};
    FillRect(dc, &body, g_w.white);
    RECT foot{0, rc.bottom - fh, rc.right, rc.bottom};
    HBRUSH fb = CreateSolidBrush(RGB(243, 245, 249));
    FillRect(dc, &foot, fb);
    DeleteObject(fb);
    RECT line{0, rc.bottom - fh, rc.right, rc.bottom - fh + std::max(1, g_w.px(1))};
    HBRUSH lb = CreateSolidBrush(RGB(222, 227, 235));
    FillRect(dc, &line, lb);
    DeleteObject(lb);
    if (g_w.header) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g_w.header);
        BitBlt(dc, 0, 0, g_w.headerW, g_w.headerH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    const int is = g_w.px(60);
    if (g_w.badge) DrawIconEx(dc, g_w.px(kMargin) - g_w.px(4), (hh - is) / 2, g_w.badge, is, is, 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ of = SelectObject(dc, g_w.fontTitle);
    const int tx = g_w.px(kMargin) + is + g_w.px(10);
    RECT tr{tx, g_w.px(20), rc.right - g_w.px(kMargin), g_w.px(56)};
    DrawTextW(dc, kAppName, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g_w.fontSub);
    SetTextColor(dc, RGB(214, 232, 252));
    const std::wstring sub = g_w.opt.uninstall ? std::wstring(T(S::TitleUninstall)) : T(S::Subtitle) + std::wstring(kVersion);
    RECT sr{tx, g_w.px(56), rc.right - g_w.px(kMargin), g_w.px(80)};
    DrawTextW(dc, sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, RGB(255, 255, 255));
            SetTextColor(dc, reinterpret_cast<HWND>(lp) == g_w.ctl(IDC_LEGACY_NOTE) ? RGB(16, 92, 176) : RGB(32, 36, 44));
            return reinterpret_cast<LRESULT>(g_w.white);
        }
        case WM_DPICHANGED: {
            g_w.dpi = HIWORD(wp);
            createDpiResources();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    if (g_w.page == Page::Done) DestroyWindow(h);
                    else if (g_w.page == Page::Welcome || g_w.page == Page::Uninstall) startWork();
                    return 0;
                case IDCANCEL:
                    if (g_w.busy) return 0;
                    if (g_w.page == Page::Done) DestroyWindow(h);
                    else if (g_w.page == Page::Uninstall ||
                             MessageBoxW(h, T(S::ConfirmCancel), T(S::TitleInstall), MB_YESNO | MB_ICONQUESTION) == IDYES)
                        DestroyWindow(h);
                    return 0;
                case IDC_BROWSE: browseFolder(); return 0;
            }
            break;
        case WM_CLOSE:
            SendMessageW(h, WM_COMMAND, IDCANCEL, 0);
            return 0;
        case WM_APP_STATUS: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            if (static_cast<int>(wp) < 100) SetWindowTextW(g_w.ctl(IDC_STATUS), s->c_str());
            SendMessageW(g_w.ctl(IDC_PROGRESS), PBM_SETPOS, wp, 0);
            delete s;
            return 0;
        }
        case WM_APP_ASK: {
            const auto* s = reinterpret_cast<const std::wstring*>(lp);
            return MessageBoxW(h, s->c_str(), g_w.opt.uninstall ? T(S::TitleUninstall) : T(S::TitleInstall),
                               static_cast<UINT>(wp));
        }
        case WM_APP_DONE: {
            auto* err = reinterpret_cast<std::wstring*>(lp);
            if (g_w.worker.joinable()) g_w.worker.join();
            g_w.busy = false;
            g_w.exitCode = static_cast<int>(wp);
            std::wstring text;
            if (g_w.exitCode == kOk) text = g_w.opt.uninstall ? T(S::DoneUninstall) : T(S::DoneInstall);
            else text = (g_w.opt.uninstall ? T(S::FailUninstall) : T(S::FailInstall)) + *err;
            delete err;
            SetWindowTextW(g_w.ctl(IDC_DONE), text.c_str());
            showPage(Page::Done);
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int runWizard(HINSTANCE inst, const Options& o) {
    g_w.inst = inst;
    g_w.opt = o;
    g_w.white = CreateSolidBrush(RGB(255, 255, 255));
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SnowFallSetupWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(kIconId), GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), &g_w.iconBig);
    LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(kIconId), GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), &g_w.iconSmall);
    wc.hIcon = g_w.iconBig;
    wc.hIconSm = g_w.iconSmall;
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    g_w.hwnd = CreateWindowExW(0, wc.lpszClassName, o.uninstall ? T(S::TitleUninstall) : T(S::TitleInstall), style,
                               CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!g_w.hwnd) return kFailed;

    // Size for the DPI of the monitor under the cursor, centred in its work area.
    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(g_w.hwnd, nullptr, mi.rcWork.left, mi.rcWork.top, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE);
    g_w.dpi = GetDpiForWindow(g_w.hwnd);
    RECT r{0, 0, g_w.px(kClientW), g_w.px(clientH())};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_w.dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    SetWindowPos(g_w.hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - ww) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);

    createDpiResources();
    createControls();
    layout();
    showPage(o.uninstall ? Page::Uninstall : Page::Welcome);
    ShowWindow(g_w.hwnd, SW_SHOW);
    SetForegroundWindow(g_w.hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_w.hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (g_w.worker.joinable()) g_w.worker.join();
    destroyDpiResources();
    if (g_w.iconBig) DestroyIcon(g_w.iconBig);
    if (g_w.iconSmall) DestroyIcon(g_w.iconSmall);
    DeleteObject(g_w.white);
    return g_w.exitCode;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    {
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) g_logPath = std::wstring(tmp) + L"SnowFallSetup.log";
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Options o = parseCommandLine();
    logf(L"SnowFallSetup %ls started: %ls", kVersion, GetCommandLineW());
    int rc = kOk;
    if (o.help) {
        if (!o.silent) MessageBoxW(nullptr, T(S::Usage), kAppName, MB_ICONINFORMATION);
        rc = kCancelled;
    } else if (o.uninstall) {
        if (o.waitFor) {   // relaunched copy: wait until the original setup exe is unlocked
            WaitForSingleObject(o.waitFor, 15000);
            CloseHandle(o.waitFor);
        }
        if (!resolveUninstallDir(o)) {
            logf(L"nothing to uninstall");
            if (!o.silent) MessageBoxW(nullptr, T(S::NotInstalled), T(S::TitleUninstall), MB_ICONINFORMATION);
            rc = kFailed;
        } else if (!o.waitFor && samePath(parentDir(modulePath()), o.dir) && relaunchFromTemp(o)) {
            rc = kOk;   // the temporary copy does the work
        } else {
            if (o.silent) {
                std::wstring err;
                rc = doUninstall(o, Ui{}, err);
            } else {
                rc = runWizard(inst, o);
            }
            if (o.waitFor) scheduleSelfDelete();
        }
    } else {
        if (!o.dirGiven) {
            std::wstring loc;   // reinstall/update: default to the existing location
            o.dir = regGetString(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", loc) && !loc.empty() ? loc
                                                                                                         : defaultInstallDir();
        }
        const LegacyInstall legacyInstall = detectLegacy(o);
        if (legacyInstall.found) {
            logf(L"previous version (Windows Snow) detected; it will be migrated");
            if (legacyInstall.runValue && !o.autostartGiven) o.autostart = true;   // keep starting with Windows
            g_w.legacyFound = true;
        }
        if (o.silent) {
            if (!isAbsolutePath(o.dir)) {
                logf(L"bad /D path");
                rc = kCancelled;
            } else {
                std::wstring err;
                rc = doInstall(o, Ui{}, err);
                if (rc != kOk) logf(L"install failed: %ls", err.c_str());
            }
        } else {
            rc = runWizard(inst, o);
        }
    }
    logf(L"exit code %d", rc);
    CoUninitialize();
    return rc;
}
