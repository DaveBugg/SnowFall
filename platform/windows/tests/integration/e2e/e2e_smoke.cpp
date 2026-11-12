// End-to-end smoke test of the real SnowFall.exe.
//
//   e2e_smoke.exe <path\to\SnowFall.exe>
//
// Starts the app with its own temporary data directory (blizzard settings), creates OWN test
// windows (a magenta backdrop and a captioned window in front of it) and checks — by reading
// pixels ONLY inside the own magenta backdrop — that:
//   1. snow accumulates on the test window's top edge (drift visible above the edge);
//   2. the overlay stays click-through over the drift;
//   3. moving the window 300 px moves the same drift (acceptance criterion 3);
//   4. minimize hides the drift, restore brings it back (criterion 4);
//   5. remote "clear snow" removes it (criterion 7);
//   6. closing the window leaves no hanging drift (criterion 4);
//   7. app CPU usage stays low; 8. --exit closes the app and all overlays (criterion 12).
#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <string>

#include "core/settings.h"
#include "settings/settings_store.h"

namespace {
int g_fail = 0;
void report(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    MSG m;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
}

LRESULT CALLBACK solidProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(h, GWLP_USERDATA)));
        FillRect(reinterpret_cast<HDC>(wp), &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND makeWindow(const wchar_t* cls, DWORD style, DWORD ex, RECT r, COLORREF color) {
    HWND h = CreateWindowExW(ex, cls, L"Snow e2e", style, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, static_cast<LONG_PTR>(color));
    InvalidateRect(h, nullptr, TRUE);
    return h;
}

RECT frame(HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    return r;
}

// Fraction of "snow-like" pixels (bright, not magenta) in rows [y0, y1] across [x0, x1).
// Only called for coordinates inside our own magenta backdrop window.
double snowFraction(int x0, int x1, int y0, int y1) {
    HDC dc = GetDC(nullptr);
    int snow = 0, total = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x < x1; x += 4) {
            const COLORREF c = GetPixel(dc, x, y);
            const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
            ++total;
            if (g > 150 && r > 150 && b > 150) ++snow;   // magenta has g == 0
        }
    }
    ReleaseDC(nullptr, dc);
    return total ? static_cast<double>(snow) / total : 0;
}

double cpuSeconds(HANDLE p) {
    FILETIME c, e, k, u;
    GetProcessTimes(p, &c, &e, &k, &u);
    auto s = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
    return s(k) + s(u);
}
}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        printf("usage: e2e_smoke <SnowFall.exe>\n");
        return 2;
    }
    std::wstring exe(argv[1], argv[1] + strlen(argv[1]));

    // Own data directory with fast-accumulating settings.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"SnowFallE2E_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    snow::Settings s;
    s.weather.preset = snow::WeatherPreset::Blizzard;
    s.weather.intensity = 100;
    s.weather.wind = 0;
    s.weather.gustiness = 0;
    s.accumulation.maxHeightPxAt96Dpi = 30;
    s.accumulation.onScreenBottom = false;
    s.accumulation.onTaskbar = false;
    s.performance.pauseOnFullscreen = false;
    snow::SettingsStore(dir + L"\\settings.json").save(s);

    WNDCLASSW wc{};
    wc.lpfnWndProc = solidProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnowE2EWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // Backdrop (magenta, topmost popup) and the captioned test window in front of it.
    HWND bg = makeWindow(L"SnowE2EWindow", WS_POPUP | WS_VISIBLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                         RECT{60, 60, 1460, 900}, RGB(255, 0, 255));
    HWND win = makeWindow(L"SnowE2EWindow", WS_OVERLAPPEDWINDOW | WS_VISIBLE, WS_EX_TOPMOST,
                          RECT{200, 420, 700, 700}, RGB(60, 60, 70));
    SetWindowPos(win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    pump(500);

    std::wstring cmd = L"\"" + exe + L"\" --autostart --data-dir \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("FAIL  cannot start %s\n", argv[1]);
        return 1;
    }
    pump(1500);
    HWND host = FindWindowW(L"SnowFallHost", nullptr);
    report(host != nullptr, "app started (host window found)");

    // ---- 1. accumulation ------------------------------------------------------------------
    auto edgeSnow = [&](HWND w) {
        RECT r = frame(w);
        DWORD_PTR dummy = 0;
        (void)dummy;
        return snowFraction(r.left + 60, r.right - 60, r.top - 3, r.top - 2);
    };
    double f = 0;
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 60000) {
        pump(500);
        f = edgeSnow(win);
        if (f > 0.85) break;
    }
    printf("      drift coverage above the edge: %.0f%% after %.1f s\n", f * 100, (GetTickCount64() - t0) / 1000.0);
    report(f > 0.85, "1. snow accumulates on the window's top edge");

    // ---- 2. click-through ----------------------------------------------------------------
    RECT r = frame(win);
    POINT p{(r.left + r.right) / 2, r.top - 3};
    report(WindowFromPoint(p) == bg, "2. drift area is click-through (WindowFromPoint = backdrop)");

    // ---- 3. move by 300 px ---------------------------------------------------------------
    const double before = f;
    SetWindowPos(win, nullptr, r.left + 300, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump(150);
    const double moved = edgeSnow(win);
    const double old = snowFraction(r.left + 60, r.left + 290, r.top - 3, r.top - 2);
    printf("      after move: new edge %.0f%%, vacated area %.0f%%\n", moved * 100, old * 100);
    report(moved > 0.85 && moved > before - 0.1, "3a. drift moved with the window within 150 ms");
    report(old < 0.1, "3b. no drift left at the old position");

    // ---- 4. minimize / restore -----------------------------------------------------------
    r = frame(win);
    ShowWindow(win, SW_MINIMIZE);
    pump(600);
    const double minimized = snowFraction(r.left + 60, r.right - 60, r.top - 3, r.top - 2);
    ShowWindow(win, SW_RESTORE);
    SetWindowPos(win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    pump(800);
    const double restored = edgeSnow(win);
    printf("      minimized %.0f%%, restored %.0f%%\n", minimized * 100, restored * 100);
    report(minimized < 0.1, "4a. minimized window: drift hidden");
    report(restored > 0.8, "4b. restored window: same drift back");

    // ---- 7. CPU (blizzard, interaction off) ----------------------------------------------
    const double c0 = cpuSeconds(pi.hProcess);
    pump(5000);
    const double cpu = (cpuSeconds(pi.hProcess) - c0) / 5.0 * 100.0;
    SYSTEM_INFO sys;
    GetSystemInfo(&sys);
    PROCESS_MEMORY_COUNTERS_EX pm{sizeof pm};
    GetProcessMemoryInfo(pi.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    printf("      app CPU: %.1f%% of one core = %.2f%% of the machine (%lu logical CPUs); private %.1f MB\n", cpu,
           cpu / sys.dwNumberOfProcessors, sys.dwNumberOfProcessors, pm.PrivateUsage / 1048576.0);
    report(cpu / sys.dwNumberOfProcessors < 3.0, "7a. average CPU <= 3% (blizzard)");
    report(pm.PrivateUsage < 150ull * 1048576, "7b. memory <= 150 MB");

    // ---- 5. clear ------------------------------------------------------------------------
    PostMessageW(host, WM_APP + 120, 3, 0);
    pump(300);
    const double cleared = edgeSnow(win);
    printf("      after clear: %.0f%%\n", cleared * 100);
    report(cleared < 0.2, "5. clear removes the drift");

    // ---- 6. close the window -------------------------------------------------------------
    pump(6000);   // let some snow build up again
    r = frame(win);
    DestroyWindow(win);
    pump(1500);
    const double closed = snowFraction(r.left + 60, r.right - 60, r.top - 3, r.top - 2);
    printf("      after close: %.0f%%\n", closed * 100);
    report(closed < 0.1, "6. closed window leaves no hanging drift");

    // ---- 9. system cursor hidden in interaction mode, always restored ---------------------
    const std::wstring marker = dir + L"\\cursor-hidden.marker";
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    PostMessageW(host, WM_APP + 120, 6, 0);   // interaction on
    pump(500);
    report(exists(marker), "9a. interaction mode on: system cursor hidden (marker present)");
    PostMessageW(host, WM_APP + 120, 6, 0);   // off
    pump(500);
    report(!exists(marker), "9b. interaction mode off: cursor restored (marker removed)");
    PostMessageW(host, WM_APP + 120, 6, 0);   // on again, then the process is killed
    pump(500);
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    report(exists(marker), "9c. killed while hidden: marker left behind");
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("FAIL  cannot restart the app\n");
        return 1;
    }
    pump(2000);
    report(!exists(marker), "9d. next start restores the cursor scheme (marker cleared)");
    host = FindWindowW(L"SnowFallHost", nullptr);

    // ---- 8. exit -------------------------------------------------------------------------
    PostMessageW(host, WM_APP + 120, 2, 0);
    const DWORD w = WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    report(w == WAIT_OBJECT_0 && code == 0, "8a. app exits cleanly with code 0");
    report(FindWindowW(L"SnowFallOverlay", nullptr) == nullptr && FindWindowW(L"SnowFallHost", nullptr) == nullptr,
           "8b. no overlay/host windows remain");
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(bg);

    DeleteFileW((dir + L"\\settings.json").c_str());
    DeleteFileW((dir + L"\\snowfall.log").c_str());
    RemoveDirectoryW(dir.c_str());
    printf("\n%s (%d failure(s))\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS", g_fail);
    return g_fail ? 1 : 0;
}
