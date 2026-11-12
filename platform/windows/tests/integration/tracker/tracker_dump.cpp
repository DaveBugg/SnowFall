// tracker_dump: diagnostic dump and integration self-test of snow::WindowTracker.
//
//   tracker_dump [--seconds N]   print every published layout for N seconds (default 3)
//   tracker_dump --selftest      automated checks, exit code != 0 on failure
//
// Prints window class names and executable names only - never window titles.
#include <windows.h>

#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "tracker/window_tracker.h"
#include "tracker/window_tracker_debug.h"

using namespace snow;

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

double nowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

const char* kindName(WindowKind k) {
    switch (k) {
        case WindowKind::App: return "App";
        case WindowKind::Taskbar: return "Taskbar";
        default: return "Occluder";
    }
}

void printStats() {
    const TrackerDebugStats s = trackerDebugStats();
    std::printf("  stats: events=%llu resyncs=%llu (fast %llu) published=%llu avg=%.1f us max=%llu us last=%llu us\n",
                static_cast<unsigned long long>(s.events), static_cast<unsigned long long>(s.resyncs),
                static_cast<unsigned long long>(s.fastPasses), static_cast<unsigned long long>(s.published),
                s.resyncs ? static_cast<double>(s.totalUs) / static_cast<double>(s.resyncs) : 0.0,
                static_cast<unsigned long long>(s.maxUs), static_cast<unsigned long long>(s.lastUs));
    std::printf("  last pass: %s, %llu top-level windows; monitors=%llu us enum=%llu us fullscreen=%llu us\n",
                s.lastFull ? "full refresh" : s.lastFast ? "fast" : "event", static_cast<unsigned long long>(s.topLevelSeen),
                static_cast<unsigned long long>(s.lastMonitorsUs), static_cast<unsigned long long>(s.lastEnumUs),
                static_cast<unsigned long long>(s.lastFullscreenUs));
}

void printLayout(const DesktopLayout& l) {
    std::printf("=== layout rev %llu: %zu monitors, %zu windows, %zu hidden, fullscreen=%d\n",
                static_cast<unsigned long long>(l.revision), l.monitors.size(), l.windows.size(),
                l.hiddenWindows.size(), l.fullscreenAppActive ? 1 : 0);
    for (const MonitorInfo& m : l.monitors)
        std::printf("  mon %d%s bounds=(%d,%d)-(%d,%d) work=(%d,%d)-(%d,%d) dpi=%.2f\n", m.id, m.primary ? "*" : "",
                    m.bounds.l, m.bounds.t, m.bounds.r, m.bounds.b, m.work.l, m.work.t, m.work.r, m.work.b,
                    m.dpiScale);
    for (const WindowInfo& w : l.windows) {
        std::wstring cls, exe;
        trackerDebugDescribe(w.id, cls, exe);
        std::printf("  #%-4llu %-8s (%6d,%6d) %5dx%-5d dpi=%.2f r=%4.1f max=%d snow=%d  [%s] %s\n",
                    static_cast<unsigned long long>(w.id), kindName(w.kind), w.bounds.l, w.bounds.t,
                    w.bounds.width(), w.bounds.height(), w.dpiScale, w.cornerRadiusPx, w.maximized ? 1 : 0,
                    w.snowAllowed ? 1 : 0, narrow(cls).c_str(), narrow(exe).c_str());
    }
    for (WindowId id : l.hiddenWindows) {
        std::wstring cls, exe;
        trackerDebugDescribe(id, cls, exe);
        std::printf("  #%-4llu hidden   [%s] %s\n", static_cast<unsigned long long>(id), narrow(cls).c_str(),
                    narrow(exe).c_str());
    }
    printStats();
    std::fflush(stdout);
}

// ---- message pumping ------------------------------------------------------------------
void drain() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// Pumps messages until `pred` returns true or the timeout expires.
bool pumpUntil(const std::function<bool()>& pred, int timeoutMs) {
    const double deadline = nowMs() + timeoutMs;
    for (;;) {
        drain();
        if (pred && pred()) return true;
        const double left = deadline - nowMs();
        if (left <= 0) return false;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(left < 5 ? 1 : 5), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
}

// ---- dump mode ------------------------------------------------------------------------
int runDump(int seconds) {
    WindowTracker tracker;
    if (!tracker.start(GetModuleHandleW(nullptr),
                       [](std::shared_ptr<const DesktopLayout> l) { printLayout(*l); })) {
        std::printf("tracker.start failed\n");
        return 2;
    }
    // Layouts are printed as they are published; pass timing is reported once a second.
    for (int i = 0; i < seconds; ++i) {
        pumpUntil(nullptr, 1000);
        printStats();
    }
    std::printf("--- final ");
    printStats();
    tracker.stop();
    return 0;
}

// ---- self-test ------------------------------------------------------------------------
const wchar_t kTestClass[] = L"SnowTrackerSelfTestWnd";
int g_fail = 0, g_pass = 0, g_skip = 0;

void check(const char* name, bool ok) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}
void skip(const char* name, const char* why) {
    std::printf("SKIP  %s (%s)\n", name, why);
    std::fflush(stdout);
    ++g_skip;
}

struct Capture {
    std::shared_ptr<const DesktopLayout> last;
    int count = 0;
};

HWND makeWindow(DWORD style, DWORD ex, int x, int y, int w, int h, int show = SW_SHOWNOACTIVATE) {
    HWND hwnd = CreateWindowExW(ex, kTestClass, L"", style, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    if (hwnd && show >= 0) ShowWindow(hwnd, show);
    return hwnd;
}

RectI dwmBounds(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
    return RectI{r.left, r.top, r.right, r.bottom};
}

const WindowInfo* findWin(const DesktopLayout& l, WindowId id) {
    for (const WindowInfo& w : l.windows)
        if (w.id == id) return &w;
    return nullptr;
}
int indexOf(const DesktopLayout& l, WindowId id) {
    for (size_t i = 0; i < l.windows.size(); ++i)
        if (l.windows[i].id == id) return static_cast<int>(i);
    return -1;
}
bool isHidden(const DesktopLayout& l, WindowId id) {
    for (WindowId h : l.hiddenWindows)
        if (h == id) return true;
    return false;
}

BOOL CALLBACK collectMonitors(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* v = reinterpret_cast<std::vector<MONITORINFO>*>(lp);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) v->push_back(mi);
    return TRUE;
}

double cpuMs() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const ULONGLONG kk = (static_cast<ULONGLONG>(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    const ULONGLONG uu = (static_cast<ULONGLONG>(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return static_cast<double>(kk + uu) / 10000.0;
}

int runSelfTest() {
    const int kEventMs = 400;  // event-driven changes must arrive well before the 1 s reconciliation
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTestClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
    RegisterClassExW(&wc);

    Capture cap;
    WindowTracker tracker;
    auto cb = [&cap](std::shared_ptr<const DesktopLayout> l) {
        cap.last = std::move(l);
        ++cap.count;
    };
    check("start() succeeds and publishes the first layout synchronously",
          tracker.start(GetModuleHandleW(nullptr), cb) && cap.count == 1 && cap.last && cap.last->revision >= 1);
    if (!cap.last) return 1;

    std::vector<HWND> own;
    auto track = [&](HWND h) {
        own.push_back(h);
        tracker.setOwnAppWindows(own);
    };
    auto untrack = [&](HWND h) {
        own.erase(std::remove(own.begin(), own.end(), h), own.end());
        tracker.setOwnAppWindows(own);
    };
    auto idOf = [](HWND h) { return trackerDebugIdForWindow(h); };

    // (9) monitors
    {
        std::vector<MONITORINFO> mons;
        EnumDisplayMonitors(nullptr, nullptr, collectMonitors, reinterpret_cast<LPARAM>(&mons));
        bool ok = mons.size() == cap.last->monitors.size() && !mons.empty();
        int primaries = 0;
        for (size_t i = 0; ok && i < mons.size(); ++i) {
            const MonitorInfo& m = cap.last->monitors[i];
            const RECT& a = mons[i].rcMonitor;
            const RECT& w = mons[i].rcWork;
            ok = m.id == static_cast<int>(i) && m.bounds == RectI{a.left, a.top, a.right, a.bottom} &&
                 m.work == RectI{w.left, w.top, w.right, w.bottom} && m.dpiScale >= 1.0f && m.dpiScale <= 5.0f &&
                 m.primary == ((mons[i].dwFlags & MONITORINFOF_PRIMARY) != 0);
            primaries += m.primary ? 1 : 0;
        }
        check("(9) monitors match EnumDisplayMonitors (bounds, work, primary)", ok && primaries == 1);
    }

    // (8) taskbars
    {
        std::vector<HWND> bars;
        for (const wchar_t* cls : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
            for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, cls, nullptr)) != nullptr;)
                if (IsWindowVisible(h)) bars.push_back(h);
        if (bars.empty()) {
            skip("(8) taskbar", "no visible Shell_TrayWnd on this desktop");
        } else {
            size_t kindCount = 0;
            for (const WindowInfo& w : cap.last->windows) kindCount += w.kind == WindowKind::Taskbar ? 1 : 0;
            bool ok = true;
            size_t expected = 0;
            for (HWND h : bars) {
                const RectI b = dwmBounds(h);
                const WindowInfo* w = findWin(*cap.last, idOf(h));
                bool onScreen = false;
                for (const MonitorInfo& m : cap.last->monitors) onScreen = onScreen || m.bounds.intersects(b);
                if (!onScreen) continue;
                ++expected;
                ok = ok && w && w->kind == WindowKind::Taskbar && w->bounds == b && w->cornerRadiusPx == 0;
            }
            check("(8) exactly one Taskbar-kind window per taskbar, bounds equal the live rect",
                  ok && kindCount == expected && expected >= 1);
        }
    }

    // (1) new captioned window
    HWND w1 = makeWindow(WS_OVERLAPPEDWINDOW, 0, 200, 200, 600, 400);
    track(w1);
    WindowId id1 = 0;
    bool ok = pumpUntil([&] { return (id1 = idOf(w1)) != 0 && findWin(*cap.last, id1) != nullptr; }, kEventMs);
    {
        const WindowInfo* w = ok ? findWin(*cap.last, id1) : nullptr;
        check("(1) new captioned window is listed as App with DWM frame bounds",
              w && w->kind == WindowKind::App && w->bounds == dwmBounds(w1) && w->snowAllowed && !w->maximized &&
                  w->dpiScale >= 1.0f);
        if (w) std::printf("      id=%llu radius=%.1f dpi=%.2f\n", static_cast<unsigned long long>(w->id),
                           w->cornerRadiusPx, w->dpiScale);
    }

    // (2) move by 300 px
    {
        const RectI before = dwmBounds(w1);
        const double t0 = nowMs();
        SetWindowPos(w1, nullptr, 500, 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        double t1 = 0;
        ok = pumpUntil(
            [&] {
                const WindowInfo* w = findWin(*cap.last, id1);
                if (w && w->bounds.l == before.l + 300 && w->bounds.t == before.t &&
                    w->bounds.width() == before.width()) {
                    t1 = nowMs();
                    return true;
                }
                return false;
            },
            1000);
        std::printf("      move latency %.2f ms\n", ok ? t1 - t0 : -1.0);
        check("(2) moving by 300 px publishes same id with shifted bounds within 100 ms", ok && t1 - t0 <= 100.0);
    }

    // (3) Z-order
    HWND w2 = makeWindow(WS_OVERLAPPEDWINDOW, 0, 560, 260, 500, 300);
    track(w2);
    SetWindowPos(w2, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    WindowId id2 = 0;
    ok = pumpUntil(
        [&] {
            id2 = idOf(w2);
            const int i1 = indexOf(*cap.last, id1), i2 = indexOf(*cap.last, id2);
            return id2 != 0 && i1 >= 0 && i2 >= 0 && i2 < i1;
        },
        kEventMs);
    check("(3a) second window on top precedes the first", ok && id2 != id1);
    SetWindowPos(w1, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ok = pumpUntil(
        [&] {
            const int i1 = indexOf(*cap.last, id1), i2 = indexOf(*cap.last, id2);
            return i1 >= 0 && i2 >= 0 && i1 < i2;
        },
        kEventMs);
    check("(3b) raising the first window flips the order (event-driven)", ok);

    // (4) minimize / restore
    ShowWindow(w1, SW_SHOWMINNOACTIVE);
    ok = pumpUntil([&] { return isHidden(*cap.last, id1) && !findWin(*cap.last, id1); }, kEventMs);
    check("(4a) minimized window moves to hiddenWindows", ok);
    ShowWindow(w1, SW_SHOWNOACTIVATE);
    ok = pumpUntil([&] { return !isHidden(*cap.last, id1) && findWin(*cap.last, id1); }, kEventMs);
    check("(4b) restored window is back with the same id", ok && idOf(w1) == id1);

    // (7) excluded process
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring exe = path;
        exe = exe.substr(exe.find_last_of(L"\\/") + 1);
        CharUpperBuffW(exe.data(), static_cast<DWORD>(exe.size()));  // matching must be case-insensitive
        TrackerOptions opt;
        opt.excludedProcesses = {L"definitely_not_running.exe", exe};
        tracker.setOptions(opt);
        ok = pumpUntil(
            [&] {
                const WindowInfo* a = findWin(*cap.last, id1);
                const WindowInfo* b = findWin(*cap.last, id2);
                return a && b && !a->snowAllowed && !b->snowAllowed;
            },
            kEventMs);
        bool othersAllowed = true;
        for (const WindowInfo& w : cap.last->windows)
            if (w.id != id1 && w.id != id2 && !w.snowAllowed) othersAllowed = false;
        check("(7a) excludedProcesses with own exe -> snowAllowed=false for own windows only", ok && othersAllowed);
        tracker.setOptions(TrackerOptions{});
        ok = pumpUntil(
            [&] {
                const WindowInfo* a = findWin(*cap.last, id1);
                return a && a->snowAllowed;
            },
            kEventMs);
        check("(7b) clearing the exclusion restores snowAllowed=true", ok);
    }

    // (6) overlays and tool popups
    {
        HWND overlay = makeWindow(WS_POPUP, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, 100, 100, 400, 300, -1);
        SetLayeredWindowAttributes(overlay, 0, 128, LWA_ALPHA);
        ShowWindow(overlay, SW_SHOWNOACTIVATE);
        HWND tool = makeWindow(WS_POPUP, WS_EX_TOOLWINDOW, 120, 620, 120, 60);
        track(overlay);
        track(tool);
        WindowId idTool = 0;
        ok = pumpUntil([&] { return (idTool = idOf(tool)) != 0 && findWin(*cap.last, idTool); }, kEventMs);
        const WindowInfo* t = findWin(*cap.last, idTool);
        check("(6a) small WS_POPUP tool window is an Occluder",
              ok && t && t->kind == WindowKind::Occluder && t->cornerRadiusPx == 0);
        pumpUntil(nullptr, 50);
        const WindowId idOv = idOf(overlay);
        check("(6b) WS_EX_LAYERED|WS_EX_TRANSPARENT window is not listed",
              !findWin(*cap.last, idOv) && !isHidden(*cap.last, idOv));
        untrack(overlay);
        untrack(tool);
        DestroyWindow(overlay);
        DestroyWindow(tool);
        ok = pumpUntil([&] { return !findWin(*cap.last, idTool); }, kEventMs);
        check("(6c) destroyed tool window disappears", ok);
    }

    // (5) destroy -> id gone; new window gets a new id
    {
        untrack(w2);
        DestroyWindow(w2);
        ok = pumpUntil([&] { return !findWin(*cap.last, id2) && !isHidden(*cap.last, id2); }, kEventMs);
        check("(5a) destroyed window is absent from both lists", ok && idOf(w2) == 0);
        HWND w3 = makeWindow(WS_OVERLAPPEDWINDOW, 0, 560, 260, 500, 300);
        track(w3);
        WindowId id3 = 0;
        ok = pumpUntil([&] { return (id3 = idOf(w3)) != 0 && findWin(*cap.last, id3); }, kEventMs);
        check("(5b) a newly created window gets a different id", ok && id3 != id2 && id3 != id1 && id3 > id2);
        untrack(w3);
        DestroyWindow(w3);
        pumpUntil([&] { return !findWin(*cap.last, id3); }, kEventMs);
    }

    // (10) fullscreen
    {
        const bool before = cap.last->fullscreenAppActive;
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        HWND fs = makeWindow(WS_POPUP, 0, 0, 0, sw, sh, SW_SHOW);
        track(fs);
        SetForegroundWindow(fs);
        pumpUntil([&] { return GetForegroundWindow() == fs; }, 300);
        if (GetForegroundWindow() != fs) {
            skip("(10) fullscreen detection", "OS refused foreground activation for this process");
            untrack(fs);
            DestroyWindow(fs);
        } else if (before) {
            skip("(10) fullscreen detection", "a fullscreen app was already active before the test");
            untrack(fs);
            DestroyWindow(fs);
        } else {
            ok = pumpUntil([&] { return cap.last->fullscreenAppActive; }, kEventMs);
            const WindowInfo* w = findWin(*cap.last, idOf(fs));
            check("(10a) borderless foreground window covering the primary monitor -> fullscreenAppActive",
                  ok && w && w->kind == WindowKind::App && w->cornerRadiusPx == 0);
            untrack(fs);
            DestroyWindow(fs);
            ok = pumpUntil([&] { return !cap.last->fullscreenAppActive; }, 1500);
            check("(10b) fullscreenAppActive is false again after the window is destroyed", ok);
        }
        pumpUntil(nullptr, 100);
    }

    // (11) stop
    {
        tracker.stop();
        tracker.stop();  // idempotent
        const int n = cap.count;
        SetWindowPos(w1, nullptr, 240, 240, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(w1, SW_SHOWMINNOACTIVE);
        ShowWindow(w1, SW_SHOWNOACTIVATE);
        pumpUntil(nullptr, 1300);  // longer than the reconciliation period
        check("(11) no callbacks after stop()", cap.count == n);
        // Only this process's helper counts: a running SnowFall.exe has its own.
        bool ownHelper = false;
        for (HWND h = FindWindowExW(nullptr, nullptr, L"SnowFallTrackerHelper", nullptr); h;
             h = FindWindowExW(nullptr, h, L"SnowFallTrackerHelper", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) ownHelper = true;
        }
        check("(11b) helper window is gone after stop()", !ownHelper);
    }

    // (12) burst: restart, then move a window 500 times
    {
        ok = tracker.start(GetModuleHandleW(nullptr), cb);
        tracker.setOwnAppWindows({w1});
        pumpUntil([&] { return findWin(*cap.last, idOf(w1)) != nullptr; }, kEventMs);
        check("(12a) tracker restarts after stop() and assigns a fresh id", ok && idOf(w1) != 0 && idOf(w1) != id1);
        for (int paced = 0; paced < 2; ++paced) {
            pumpUntil(nullptr, 50);
            trackerDebugResetStats();
            const int n0 = cap.count;
            const double c0 = cpuMs(), t0 = nowMs();
            for (int i = 0; i < 500; ++i) {
                SetWindowPos(w1, nullptr, 200 + (i % 250) * 2, 200 + (i % 100), 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (paced)
                    pumpUntil(nullptr, 2);
                else
                    drain();
            }
            const RectI target = dwmBounds(w1);
            const bool settled = pumpUntil(
                [&] {
                    const WindowInfo* w = findWin(*cap.last, idOf(w1));
                    return w && w->bounds == target;
                },
                kEventMs);
            const double t1 = nowMs(), c1 = cpuMs();
            const TrackerDebugStats s = trackerDebugStats();
            std::printf("      %s burst: 500 moves in %.1f ms wall, %.1f ms process CPU; events=%llu resyncs=%llu (fast %llu) "
                        "layouts=%d; resync avg=%.1f us max=%llu us total=%.2f ms\n",
                        paced ? "paced(2ms)" : "tight", t1 - t0, c1 - c0, static_cast<unsigned long long>(s.events),
                        static_cast<unsigned long long>(s.resyncs),
                        static_cast<unsigned long long>(s.fastPasses), cap.count - n0,
                        s.resyncs ? static_cast<double>(s.totalUs) / static_cast<double>(s.resyncs) : 0.0,
                        static_cast<unsigned long long>(s.maxUs), static_cast<double>(s.totalUs) / 1000.0);
            const double wall = t1 - t0;
            // Coalescing bound: at most one resync per 8 ms (+ a few for edges).
            check(paced ? "(12c) paced burst is coalesced (<= 1 resync / 8 ms) and ends on the final position"
                        : "(12b) tight burst is coalesced (<= 1 resync / 8 ms) and ends on the final position",
                  settled && static_cast<double>(s.resyncs) <= wall / 8.0 + 4.0 && s.resyncs < 500);
        }
        tracker.stop();
    }

    DestroyWindow(w1);
    drain();
    std::printf("\nselftest: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
    bool selftest = false;
    int seconds = 3;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::max(1, std::atoi(argv[++i]));
        } else {
            std::printf("usage: tracker_dump [--seconds N] | --selftest\n");
            return 2;
        }
    }
    return selftest ? runSelfTest() : runDump(seconds);
}
