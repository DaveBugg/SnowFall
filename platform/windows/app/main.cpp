// SnowFall — entry point: single instance, DPI awareness, logging, AppHost.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <dbghelp.h>

#include <cwchar>
#include <shellapi.h>

#include <string>

#include "app/app_host.h"
#include "app/cursor_hider.h"
#include "diagnostics/log.h"
#include "settings/settings_store.h"

namespace {
wchar_t g_dumpDir[MAX_PATH * 2] = {};   // filled at startup: no allocation inside the filter

// On a crash: never leave the user with an invisible cursor, and write a small local
// minidump (our own process only, never sent anywhere) next to the log for diagnosis.
LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    snow::CursorHider::emergencyRestore();
    if (g_dumpDir[0]) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t path[MAX_PATH * 2 + 64];
        swprintf_s(path, L"%s\\crash-%04u%02u%02u-%02u%02u%02u.dmp", g_dumpDir, st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                              info ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(crashFilter);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    snow::LaunchOptions opt;
    bool exitOther = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--autostart") opt.autostart = true;
            else if (a == L"--settings") opt.openSettings = true;
            else if (a == L"--exit") exitOther = true;
            else if (a == L"--data-dir" && i + 1 < argc) {
                snow::SettingsStore::setDataDirectoryOverride(argv[++i]);
                opt.customDataDir = true;
            }
        }
        LocalFree(argv);
    }

    // Single instance (per user session). A second launch opens the running instance's settings.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SnowFall.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(snow::AppHost::kHostClass, nullptr)) {
            if (exitOther) {
                PostMessageW(other, snow::AppHost::kMsgRemote, 2, 0);
            } else if (!opt.autostart) {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(other, snow::AppHost::kMsgRemote, 1, 0);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (exitOther) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    snow::log::init(snow::SettingsStore::dataDirectory() + L"\\snowfall.log");
    wcsncpy_s(g_dumpDir, snow::SettingsStore::dataDirectory().c_str(), _TRUNCATE);

    int rc = 1;
    {
        snow::AppHost app;
        if (app.initialize(instance, opt)) {
            rc = app.run();
        } else {
            SNOW_LOGE("initialization failed");
            MessageBoxW(nullptr, L"SnowFall не удалось запустить. Подробности в журнале.", L"SnowFall",
                        MB_ICONERROR);
        }
    }
    snow::log::shutdown();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return rc;
}
