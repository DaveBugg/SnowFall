// soak_test: long-running stress test of the real SnowFall.exe (spec §8 "no sustained memory
// growth over 8 hours", §13 criterion 13, §14 "8 hours, constantly moving 10 windows").
//
//   soak_test.exe [--exe <SnowFall.exe>] [--minutes 480] [--windows 10]
//                 [--rect left,top,right,bottom] [--sample-sec 60] [--seed N] [--topmost]
//                 [--stop-existing]
//
// * Starts SnowFall.exe with its OWN --data-dir (blizzard settings, like e2e_smoke).
// * Creates N of its OWN small captioned windows inside the rect (default: the bottom-right
//   quarter of the primary monitor) and keeps moving / resizing / minimizing / restoring /
//   closing+recreating them. Never activates them (no focus stealing) and never touches,
//   reads or enumerates other programs' windows.
// * Occasionally sends remote commands: clear snow, snowplow, pause/unpause, and very short
//   (<= 3 s) interaction-mode episodes — that mode hides the system cursor, so it is always
//   switched off again (verified via the app's cursor-hidden.marker) and on every exit path.
// * Every --sample-sec seconds records private bytes, working set, handles, GDI/USER objects,
//   threads, CPU% and host-window responsiveness into soak_<timestamp>.csv next to this exe.
// * Summary: start/end/peak memory, least-squares slope of private bytes over the last 75%
//   of the run (MB/h), handle/GDI/USER growth over the same window, crash/hang detection. Exit code 1 if
//   slope > 2 MB/h, handles grew > 200, the app died/hung or did not exit cleanly.
//
// Only one SnowFall can run per session (single-instance mutex): if another copy is
// already running the test refuses to start unless --stop-existing is given.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/settings.h"
#include "settings/settings_store.h"

namespace {

constexpr wchar_t kHostClass[] = L"SnowFallHost";
constexpr UINT kMsgRemote = WM_APP + 120;
enum Remote : WPARAM { kSettings = 1, kExit = 2, kClear = 3, kPause = 4, kSnowplow = 5, kInteraction = 6 };

volatile LONG g_stop = 0;
// State needed by the console handler to leave the app in a safe state on a hard close.
volatile LONG g_interactionOn = 0;
HWND g_host = nullptr;

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    // Console closed / logoff / shutdown: we will be killed shortly — restore the cursor and
    // ask the app to exit right now.
    if (g_host) {
        if (g_interactionOn) PostMessageW(g_host, kMsgRemote, kInteraction, 0);
        PostMessageW(g_host, kMsgRemote, kExit, 0);
    }
    Sleep(1500);
    return FALSE;
}

struct Args {
    std::wstring exe;
    double minutes = 480;
    int windows = 10;
    RECT rect{};
    bool rectGiven = false;
    int sampleSec = 60;
    unsigned seed = 0;
    bool topmost = false;
    bool stopExisting = false;
};

// ---------------------------------------------------------------- random -------------------
unsigned g_rng = 1;
unsigned rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
int rndInt(int lo, int hi) { return hi <= lo ? lo : lo + static_cast<int>(rnd() % static_cast<unsigned>(hi - lo + 1)); }
double rnd01() { return static_cast<double>(rnd() % 1000000u) / 1000000.0; }

// ---------------------------------------------------------------- own windows --------------
LRESULT CALLBACK soakProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(h, GWLP_USERDATA)));
        FillRect(reinterpret_cast<HDC>(wp), &rc, b);
        DeleteObject(b);
        return 1;
    }
    if (msg == WM_CLOSE) return 0;   // only the test closes its windows
    return DefWindowProcW(h, msg, wp, lp);
}

struct TestWindow {
    HWND h = nullptr;
    int index = 0;
    int x = 0, y = 0, w = 0, hgt = 0;
    int vx = 0, vy = 0;          // "drag" velocity in px per tick
    ULONGLONG dragUntil = 0;
    bool minimized = false;
};

struct Area {
    RECT r{};
    int w() const { return r.right - r.left; }
    int h() const { return r.bottom - r.top; }
};

void clampInto(TestWindow& t, const Area& a) {
    t.w = std::clamp(t.w, 160, std::max(160, a.w()));
    t.hgt = std::clamp(t.hgt, 110, std::max(110, a.h()));
    t.x = std::clamp(t.x, static_cast<int>(a.r.left), static_cast<int>(std::max(a.r.left, a.r.right - t.w)));
    t.y = std::clamp(t.y, static_cast<int>(a.r.top), static_cast<int>(std::max(a.r.top, a.r.bottom - t.hgt)));
}

const COLORREF kColors[] = {RGB(60, 60, 70), RGB(40, 70, 110), RGB(90, 50, 60), RGB(50, 90, 60), RGB(80, 80, 40),
                            RGB(30, 30, 30), RGB(100, 70, 120), RGB(40, 100, 100)};

void createTestWindow(TestWindow& t, const Area& a, bool topmost) {
    t.w = rndInt(170, std::max(170, std::min(420, a.w() / 2)));
    t.hgt = rndInt(120, std::max(120, std::min(300, a.h() / 2)));
    t.x = rndInt(a.r.left, std::max(static_cast<int>(a.r.left), static_cast<int>(a.r.right) - t.w));
    t.y = rndInt(a.r.top, std::max(static_cast<int>(a.r.top), static_cast<int>(a.r.bottom) - t.hgt));
    clampInto(t, a);
    wchar_t title[64];
    swprintf_s(title, L"Snow soak %d", t.index + 1);
    t.h = CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, L"SnowSoakWindow", title, WS_OVERLAPPEDWINDOW, t.x, t.y, t.w,
                          t.hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(t.h, GWLP_USERDATA, static_cast<LONG_PTR>(kColors[t.index % std::size(kColors)]));
    ShowWindow(t.h, SW_SHOWNOACTIVATE);
    t.minimized = false;
    t.dragUntil = 0;
}

void applyPos(const TestWindow& t, UINT extra = 0) {
    SetWindowPos(t.h, nullptr, t.x, t.y, t.w, t.hgt, SWP_NOZORDER | SWP_NOACTIVATE | extra);
}

void pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void pumpFor(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        pump();
        Sleep(10);
    }
}

// ---------------------------------------------------------------- metrics ------------------
struct Sample {
    double minutes = 0;
    double privateMb = 0, wsMb = 0, peakWsMb = 0;
    DWORD handles = 0, gdi = 0, user = 0, threads = 0;
    double cpuMachine = 0, cpuCore = 0;
    int responseMs = -1;   // -1 = no answer within the timeout (hung)
    unsigned long long ops = 0, remotes = 0;
};

double cpuSeconds(HANDLE p) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(p, &c, &e, &k, &u)) return 0;
    auto s = [](FILETIME f) {
        return static_cast<double>((static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
    };
    return s(k) + s(u);
}

DWORD threadCount(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    DWORD n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ++n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

int hostResponseMs(HWND host) {
    const ULONGLONG t0 = GetTickCount64();
    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &res)) return -1;
    return static_cast<int>(GetTickCount64() - t0);
}

// Least-squares slope (y per x) of the given points.
double slope(const std::vector<double>& x, const std::vector<double>& y) {
    const size_t n = x.size();
    if (n < 2) return 0;
    double mx = 0, my = 0;
    for (size_t i = 0; i < n; ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double num = 0, den = 0;
    for (size_t i = 0; i < n; ++i) {
        num += (x[i] - mx) * (y[i] - my);
        den += (x[i] - mx) * (x[i] - mx);
    }
    return den > 0 ? num / den : 0;
}

bool fileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 1)) - 1, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (s == "--exe") {
            const char* v = next();
            if (!v) return false;
            a.exe = widen(v);
        } else if (s == "--minutes") {
            const char* v = next();
            if (!v) return false;
            a.minutes = atof(v);
        } else if (s == "--windows") {
            const char* v = next();
            if (!v) return false;
            a.windows = std::clamp(atoi(v), 1, 40);
        } else if (s == "--sample-sec") {
            const char* v = next();
            if (!v) return false;
            a.sampleSec = std::clamp(atoi(v), 5, 3600);
        } else if (s == "--seed") {
            const char* v = next();
            if (!v) return false;
            a.seed = static_cast<unsigned>(strtoul(v, nullptr, 10));
        } else if (s == "--rect") {
            const char* v = next();
            long l, t, r, b;
            if (!v || sscanf_s(v, "%ld,%ld,%ld,%ld", &l, &t, &r, &b) != 4 || r - l < 200 || b - t < 150) return false;
            a.rect = RECT{l, t, r, b};
            a.rectGiven = true;
        } else if (s == "--topmost") {
            a.topmost = true;
        } else if (s == "--stop-existing") {
            a.stopExisting = true;
        } else {
            return false;
        }
    }
    return a.minutes > 0;
}

void deleteDirFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printf("usage: soak_test [--exe SnowFall.exe] [--minutes 480] [--windows 10]\n"
               "                 [--rect l,t,r,b] [--sample-sec 60] [--seed N] [--topmost] [--stop-existing]\n");
        return 2;
    }
    if (args.exe.empty()) args.exe = exeDir() + L"\\SnowFall.exe";
    if (!fileExists(args.exe)) {
        printf("FAIL  %ls not found (use --exe)\n", args.exe.c_str());
        return 2;
    }
    g_rng = args.seed ? args.seed : static_cast<unsigned>(GetTickCount64() ^ GetCurrentProcessId()) | 1u;
    printf("seed %u\n", g_rng);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    // ---- single instance --------------------------------------------------------------------
    if (HWND other = FindWindowW(kHostClass, nullptr)) {
        if (!args.stopExisting) {
            printf("FAIL  another SnowFall is already running; exit it first (SnowFall.exe --exit)\n"
                   "      or pass --stop-existing to ask it to exit.\n");
            return 2;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, kMsgRemote, kExit, 0);
        const bool gone = p && WaitForSingleObject(p, 10000) == WAIT_OBJECT_0;
        if (p) CloseHandle(p);
        printf("asked the running SnowFall (pid %lu) to exit: %s\n", pid, gone ? "exited" : "STILL RUNNING");
        if (!gone) return 2;
    }

    // ---- own data dir + blizzard settings ---------------------------------------------------
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"SnowFallSoak_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    snow::Settings s;
    s.weather.preset = snow::WeatherPreset::Blizzard;
    s.weather.intensity = 100;
    s.accumulation.maxHeightPxAt96Dpi = 30;
    s.performance.pauseOnFullscreen = false;
    snow::SettingsStore(dir + L"\\settings.json").save(s);
    const std::wstring marker = dir + L"\\cursor-hidden.marker";

    // ---- test area ----------------------------------------------------------------------------
    Area area;
    if (args.rectGiven) {
        area.r = args.rect;
    } else {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT m = mi.rcMonitor;
        RECT q{(m.left + m.right) / 2, (m.top + m.bottom) / 2, m.right, m.bottom};
        IntersectRect(&area.r, &q, &mi.rcWork);
    }
    printf("area %ld,%ld - %ld,%ld; %d windows; %.1f min; sample every %d s\n", area.r.left, area.r.top, area.r.right,
           area.r.bottom, args.windows, args.minutes, args.sampleSec);

    WNDCLASSW wc{};
    wc.lpfnWndProc = soakProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnowSoakWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    std::vector<TestWindow> wins(static_cast<size_t>(args.windows));
    for (size_t i = 0; i < wins.size(); ++i) {
        wins[i].index = static_cast<int>(i);
        createTestWindow(wins[i], area, args.topmost);
    }
    pumpFor(300);

    // ---- start the app --------------------------------------------------------------------
    std::wstring cmd = L"\"" + args.exe + L"\" --autostart --data-dir \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("FAIL  cannot start %ls (error %lu)\n", args.exe.c_str(), GetLastError());
        return 1;
    }
    HWND host = nullptr;
    for (int i = 0; i < 100 && !host; ++i) {
        pumpFor(100);
        HWND h = FindWindowW(kHostClass, nullptr);
        DWORD pid = 0;
        if (h) GetWindowThreadProcessId(h, &pid);
        if (pid == pi.dwProcessId) host = h;
    }
    if (!host) {
        printf("FAIL  host window of the started app not found\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    g_host = host;
    printf("app started, pid %lu\n", pi.dwProcessId);

    // ---- CSV --------------------------------------------------------------------------------
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring csvPath = exeDir() + L"\\soak_" + stamp + L".csv";
    FILE* csv = _wfopen(csvPath.c_str(), L"w");
    if (csv) {
        fprintf(csv, "local_time,elapsed_min,private_mb,working_set_mb,peak_working_set_mb,handles,gdi_objects,"
                     "user_objects,threads,cpu_machine_pct,cpu_one_core_pct,host_response_ms,window_ops,remote_cmds\n");
        fflush(csv);
    }
    printf("csv: %ls\n", csvPath.c_str());

    SYSTEM_INFO sys;
    GetSystemInfo(&sys);
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG total = static_cast<ULONGLONG>(args.minutes * 60000.0);
    std::vector<Sample> samples;
    double lastCpu = cpuSeconds(pi.hProcess);
    ULONGLONG lastCpuAt = start;
    ULONGLONG nextSample = start;
    ULONGLONG nextAction = start + 200;
    ULONGLONG nextRemote = start + static_cast<ULONGLONG>(rndInt(20, 45)) * 1000;
    ULONGLONG nextInteractionAllowed = start + 90000;   // first episode after 1.5 min
    ULONGLONG interactionOffAt = 0, pauseOffAt = 0, markerCheckAt = 0;
    bool paused = false, interaction = false;
    unsigned long long ops = 0, remotes = 0, interactionEpisodes = 0, markerFixes = 0;
    int hangs = 0, consecutiveHangs = 0;
    bool died = false, hung = false, interrupted = false;
    DWORD diedCode = 0;
    ULONGLONG lastAlive = start;

    auto remote = [&](WPARAM c) {
        PostMessageW(host, kMsgRemote, c, 0);
        ++remotes;
    };
    auto interactionOff = [&]() {
        if (!interaction) return;
        remote(kInteraction);
        interaction = false;
        InterlockedExchange(&g_interactionOn, 0);
        markerCheckAt = GetTickCount64() + 1000;
    };

    for (;;) {
        pump();
        const ULONGLONG now = GetTickCount64();
        if (g_stop) {
            interrupted = true;
            break;
        }
        if (now - start >= total && !interaction) break;

        // -- liveness ------------------------------------------------------------------------
        if (now - lastAlive >= 2000) {
            lastAlive = now;
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(pi.hProcess, &diedCode);
                died = true;
                printf("FAIL  the app exited unexpectedly (code 0x%08lX) at %.1f min\n", diedCode,
                       static_cast<double>(now - start) / 60000.0);
                break;
            }
        }

        // -- window churn ----------------------------------------------------------------------
        for (TestWindow& t : wins) {
            if (t.minimized || now >= t.dragUntil) continue;
            t.x += t.vx;
            t.y += t.vy;
            if (t.x <= area.r.left || t.x + t.w >= area.r.right) t.vx = -t.vx;
            if (t.y <= area.r.top || t.y + t.hgt >= area.r.bottom) t.vy = -t.vy;
            clampInto(t, area);
            applyPos(t);
        }
        if (now >= nextAction) {
            nextAction = now + static_cast<ULONGLONG>(rndInt(120, 400));
            TestWindow& t = wins[rnd() % wins.size()];
            const int r = rndInt(0, 99);
            ++ops;
            if (t.minimized) {
                if (r < 60) {
                    ShowWindow(t.h, SW_SHOWNOACTIVATE);   // restore without activation
                    t.minimized = false;
                }
            } else if (r < 30) {   // drag for a while
                t.vx = rndInt(-9, 9);
                t.vy = rndInt(-6, 6);
                t.dragUntil = now + static_cast<ULONGLONG>(rndInt(400, 2500));
            } else if (r < 45) {   // jump
                t.x = rndInt(area.r.left, area.r.right - t.w);
                t.y = rndInt(area.r.top, area.r.bottom - t.hgt);
                clampInto(t, area);
                applyPos(t);
            } else if (r < 65) {   // resize
                t.w = rndInt(160, std::max(160, std::min(460, area.w() * 2 / 3)));
                t.hgt = rndInt(110, std::max(110, std::min(320, area.h() * 2 / 3)));
                clampInto(t, area);
                applyPos(t);
            } else if (r < 77) {   // minimize
                ShowWindow(t.h, SW_SHOWMINNOACTIVE);
                t.minimized = true;
            } else if (r < 87) {   // close + recreate
                DestroyWindow(t.h);
                createTestWindow(t, area, args.topmost);
            } else {               // raise among the test windows
                SetWindowPos(t.h, args.topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }

        // -- remote commands ----------------------------------------------------------------
        if (interaction && now >= interactionOffAt) interactionOff();
        if (markerCheckAt && now >= markerCheckAt) {
            markerCheckAt = 0;
            if (fileExists(marker)) {   // still hidden: toggle once more and re-check
                printf("WARN  cursor marker still present after interaction off; toggling again\n");
                remote(kInteraction);
                ++markerFixes;
                markerCheckAt = now + 1500;
            }
        }
        if (paused && now >= pauseOffAt) {
            remote(kPause);
            paused = false;
        }
        if (now >= nextRemote && !interaction && !paused) {
            nextRemote = now + static_cast<ULONGLONG>(rndInt(20, 60)) * 1000;
            const int r = rndInt(0, 99);
            if (r < 25 && now >= nextInteractionAllowed && now - start + 5000 < total) {
                remote(kInteraction);   // ON for 1.0..2.5 s only (hides the system cursor)
                interaction = true;
                InterlockedExchange(&g_interactionOn, 1);
                interactionOffAt = now + static_cast<ULONGLONG>(rndInt(1000, 2500));
                nextInteractionAllowed = now + static_cast<ULONGLONG>(rndInt(180, 420)) * 1000;
                ++interactionEpisodes;
            } else if (r < 55) {
                remote(kClear);
            } else if (r < 75) {
                remote(kSnowplow);
            } else {
                remote(kPause);
                paused = true;
                pauseOffAt = now + static_cast<ULONGLONG>(rndInt(3, 10)) * 1000;
            }
        }

        // -- sampling --------------------------------------------------------------------------
        if (now >= nextSample) {
            nextSample += static_cast<ULONGLONG>(args.sampleSec) * 1000;
            Sample sm;
            sm.minutes = static_cast<double>(now - start) / 60000.0;
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            GetProcessMemoryInfo(pi.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
            sm.privateMb = static_cast<double>(pm.PrivateUsage) / 1048576.0;
            sm.wsMb = static_cast<double>(pm.WorkingSetSize) / 1048576.0;
            sm.peakWsMb = static_cast<double>(pm.PeakWorkingSetSize) / 1048576.0;
            GetProcessHandleCount(pi.hProcess, &sm.handles);
            sm.gdi = GetGuiResources(pi.hProcess, GR_GDIOBJECTS);
            sm.user = GetGuiResources(pi.hProcess, GR_USEROBJECTS);
            sm.threads = threadCount(pi.dwProcessId);
            const double c = cpuSeconds(pi.hProcess);
            const double wall = static_cast<double>(now - lastCpuAt) / 1000.0;
            sm.cpuCore = wall > 0.5 ? (c - lastCpu) / wall * 100.0 : 0;
            sm.cpuMachine = sm.cpuCore / static_cast<double>(sys.dwNumberOfProcessors);
            lastCpu = c;
            lastCpuAt = now;
            sm.responseMs = hostResponseMs(host);
            sm.ops = ops;
            sm.remotes = remotes;
            if (sm.responseMs < 0) {
                ++hangs;
                if (++consecutiveHangs >= 2) hung = true;
            } else {
                consecutiveHangs = 0;
            }
            samples.push_back(sm);
            GetLocalTime(&st);
            if (csv) {
                fprintf(csv, "%02u:%02u:%02u,%.2f,%.2f,%.2f,%.2f,%lu,%lu,%lu,%lu,%.2f,%.1f,%d,%llu,%llu\n", st.wHour,
                        st.wMinute, st.wSecond, sm.minutes, sm.privateMb, sm.wsMb, sm.peakWsMb, sm.handles, sm.gdi,
                        sm.user, sm.threads, sm.cpuMachine, sm.cpuCore, sm.responseMs, sm.ops, sm.remotes);
                fflush(csv);
            }
            printf("[%6.1f min] private %6.1f MB  ws %6.1f MB  handles %4lu  gdi %3lu  user %3lu  threads %2lu  "
                   "cpu %4.1f%% (%5.1f%% core)  resp %d ms  ops %llu\n",
                   sm.minutes, sm.privateMb, sm.wsMb, sm.handles, sm.gdi, sm.user, sm.threads, sm.cpuMachine,
                   sm.cpuCore, sm.responseMs, sm.ops);
            if (hung) {
                printf("FAIL  host window did not answer twice in a row (hang)\n");
                break;
            }
        }
        Sleep(15);
    }

    // ---- shutdown: cursor back, unpause, clean exit -------------------------------------------
    bool cleanExit = false;
    DWORD exitCode = 1;
    if (!died) {
        if (interaction) {
            remote(kInteraction);
            interaction = false;
            InterlockedExchange(&g_interactionOn, 0);
            pumpFor(800);
        }
        if (fileExists(marker)) {
            remote(kInteraction);
            pumpFor(800);
        }
        if (paused) remote(kPause);
        PostMessageW(host, kMsgRemote, kExit, 0);
        const DWORD w = WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        cleanExit = w == WAIT_OBJECT_0 && exitCode == 0;
        if (w != WAIT_OBJECT_0) {
            printf("FAIL  the app did not exit within 10 s; terminating\n");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 3000);
        }
    }
    g_host = nullptr;
    pumpFor(300);
    const bool noWindowsLeft = FindWindowW(L"SnowFallOverlay", nullptr) == nullptr && FindWindowW(kHostClass, nullptr) == nullptr;
    for (TestWindow& t : wins) DestroyWindow(t.h);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (csv) fclose(csv);

    // ---- summary ------------------------------------------------------------------------------
    const double ranMin = static_cast<double>(GetTickCount64() - start) / 60000.0;
    std::string out;
    char line[512];
    auto add = [&](const char* fmt, auto... v) {
        snprintf(line, sizeof line, fmt, v...);
        out += line;
        out += "\n";
    };
    add("==== SnowFall soak summary ====");
    add("duration            %.1f min (planned %.1f)%s, %zu samples every %d s", ranMin, args.minutes,
        interrupted ? " INTERRUPTED" : "", samples.size(), args.sampleSec);
    add("window ops          %llu (%d windows), remote commands %llu, interaction episodes %llu (cursor re-fixes %llu)",
        ops, args.windows, remotes, interactionEpisodes, markerFixes);
    double slopeMbH = 0, peakPriv = 0, peakWs = 0, avgCpu = 0, maxCpu = 0;
    long dHandles = 0, dGdi = 0, dUser = 0;
    if (!samples.empty()) {
        const Sample& a = samples.front();
        const Sample& b = samples.back();
        for (const Sample& sm : samples) {
            peakPriv = std::max(peakPriv, sm.privateMb);
            peakWs = std::max(peakWs, sm.wsMb);
            maxCpu = std::max(maxCpu, sm.cpuMachine);
        }
        for (size_t i = 1; i < samples.size(); ++i) avgCpu += samples[i].cpuMachine;
        if (samples.size() > 1) avgCpu /= static_cast<double>(samples.size() - 1);
        std::vector<double> xs, ys;
        const double from = b.minutes * 0.25;
        for (const Sample& sm : samples) {
            if (sm.minutes + 1e-9 >= from) {
                xs.push_back(sm.minutes / 60.0);
                ys.push_back(sm.privateMb);
            }
        }
        slopeMbH = slope(xs, ys);
        // Growth is measured from the first sample of the same window (after warm-up): the
        // t=0 sample is taken while the app is still creating its devices, hooks and overlays.
        const Sample* base = &a;
        for (const Sample& sm : samples) {
            if (sm.minutes + 1e-9 >= from) {
                base = &sm;
                break;
            }
        }
        dHandles = static_cast<long>(b.handles) - static_cast<long>(base->handles);
        dGdi = static_cast<long>(b.gdi) - static_cast<long>(base->gdi);
        dUser = static_cast<long>(b.user) - static_cast<long>(base->user);
        add("private bytes       start %.1f MB, end %.1f MB, peak %.1f MB", a.privateMb, b.privateMb, peakPriv);
        add("working set         start %.1f MB, end %.1f MB, peak %.1f MB", a.wsMb, b.wsMb, peakWs);
        add("private slope       %+.2f MB/h over the last 75%% (%zu samples from %.1f min)", slopeMbH, xs.size(), from);
        add("handles             start %lu; %lu -> %lu after warm-up (%+ld)", a.handles, base->handles, b.handles, dHandles);
        add("GDI objects         start %lu; %lu -> %lu after warm-up (%+ld)", a.gdi, base->gdi, b.gdi, dGdi);
        add("USER objects        start %lu; %lu -> %lu after warm-up (%+ld)", a.user, base->user, b.user, dUser);
        add("threads             %lu -> %lu", a.threads, b.threads);
        add("CPU (whole machine) avg %.2f%%, max %.2f%% (%lu logical CPUs)", avgCpu, maxCpu, sys.dwNumberOfProcessors);
    }
    add("host responsiveness %d missed answer(s)%s", hangs, hung ? " -> HUNG" : "");
    add("process             %s", died ? "DIED" : "alive until the end");
    add("shutdown            %s, overlays/host gone: %s", died ? "n/a" : cleanExit ? "clean (exit code 0)" : "NOT CLEAN",
        noWindowsLeft ? "yes" : "NO");
    if (samples.size() < 4) add("note                fewer than 4 samples: slope is not meaningful");
    if (ranMin < 60) add("note                short run: the 2 MB/h threshold is meant for multi-hour runs");
    const bool fail = died || hung || !cleanExit || !noWindowsLeft || slopeMbH > 2.0 || dHandles > 200 || samples.empty();
    add("RESULT: %s", fail ? "FAIL" : "PASS");
    printf("\n%s", out.c_str());
    const std::wstring sumPath = exeDir() + L"\\soak_" + stamp + L"_summary.txt";
    if (FILE* f = _wfopen(sumPath.c_str(), L"w")) {
        fputs(out.c_str(), f);
        fclose(f);
    }
    // Keep the app's log next to the CSV, then remove the temporary data directory.
    CopyFileW((dir + L"\\snowfall.log").c_str(), (exeDir() + L"\\soak_" + stamp + L"_app.log").c_str(), FALSE);
    deleteDirFiles(dir);
    return fail ? 1 : 0;
}
