// WindowTracker implementation. See window_tracker.h for the contract.
//
// Design
//   * A hidden top-level tool window (never shown) receives WM_DISPLAYCHANGE /
//     WM_SETTINGCHANGE broadcasts, the reconciliation WM_TIMER and our private
//     "resync now" message. (Message-only windows do not get broadcasts.)
//   * Out-of-context WinEvent hooks only filter and set a dirty flag; the real work is a
//     full EnumWindows pass ("resync") that runs from the helper window's message handler.
//   * Coalescing: the first event after an idle period posts the resync message right
//     away; while events keep arriving a resync runs at most once per kCoalesceUs. The
//     trailing edge is driven by a high-resolution waitable timer observed by a
//     thread-pool wait whose only job is to PostMessage back to the UI thread (WM_TIMER
//     is too coarse: ~15.6 ms granularity).
//   * A layout is published only when it differs from the previously published one.
//
// Privacy: only class names, styles, rectangles, DWM attributes and executable names are
// read. No window text, no pixels, no injection (all hooks are WINEVENT_OUTOFCONTEXT).
#include "tracker/window_tracker.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include "diagnostics/log.h"
#include "tracker/window_tracker_debug.h"

namespace snow {
namespace {

// ---- tunables ---------------------------------------------------------------------
constexpr std::int64_t kCoalesceUs = 8000;      // min spacing between event-driven resyncs
constexpr UINT kReconcileMs = 1000;             // fallback full reconciliation period
constexpr size_t kMaxFastMoved = 8;             // more moving windows than this -> full pass
constexpr UINT kMsgResync = WM_APP + 0x51;
constexpr UINT_PTR kTimerReconcile = 1;
constexpr UINT_PTR kTimerCoalesceFallback = 2;  // only if the thread-pool timer is unavailable

// Sizes below are "px at 96 DPI" and get multiplied by the window's dpiScale.
constexpr float kAppMinW = 160, kAppMinH = 80;        // caption-less window must be this big to be App
constexpr float kCaptionAppMinW = 48, kCaptionAppMinH = 24;  // captioned window smaller than this is an Occluder
constexpr int kMinVisiblePx = 4;                      // anything thinner is a helper/marker window: skip
constexpr float kLayeredThinPx = 12;                  // layered strips thinner than this are glow/shadow frames
constexpr int kMaxSanePx = 65536;                     // absurd rect guard

const wchar_t kHelperClass[] = L"SnowFallTrackerHelper";

// ---- class-name table (the ONLY place with hard-coded class names) -----------------
enum class ClassRole : std::uint8_t {
    Normal,    // decide by styles/size
    Skip,      // never listed (no visible content of its own, or it is the desktop)
    Taskbar,   // WindowKind::Taskbar
    Occluder,  // always WindowKind::Occluder when visible and uncloaked
    App,       // always WindowKind::App when visible and uncloaked
};

struct ClassRule {
    const wchar_t* name;
    ClassRole role;
    bool shell;  // shell/system surface: never counts as a "fullscreen application"
};

const ClassRule kClassRules[] = {
    // Desktop / wallpaper hosts.
    {L"Progman", ClassRole::Skip, true},
    {L"WorkerW", ClassRole::Skip, true},
    // Taskbars (primary and secondary monitors). Geometry always comes from the live rect.
    {L"Shell_TrayWnd", ClassRole::Taskbar, true},
    {L"Shell_SecondaryTrayWnd", ClassRole::Taskbar, true},
    // IME / text-input helper windows: no content of their own.
    {L"IME", ClassRole::Skip, false},
    {L"MSCTFIME UI", ClassRole::Skip, false},
    // Invisible shell plumbing: edge-swipe input strips, immersive shell managers,
    // foreground staging, tab proxies, light-dismiss catcher, lock-screen backstops.
    {L"EdgeUiInputTopWndClass", ClassRole::Skip, true},
    {L"EdgeUiInputWndClass", ClassRole::Skip, true},
    {L"ApplicationManager_ImmersiveShellWindow", ClassRole::Skip, true},
    {L"ApplicationManager_DesktopShellWindow", ClassRole::Skip, true},
    {L"ForegroundStaging", ClassRole::Skip, true},
    {L"Windows.Internal.Shell.TabProxyWindow", ClassRole::Skip, false},
    {L"Shell_LightDismissOverlay", ClassRole::Skip, true},
    {L"LockScreenBackstopFrame", ClassRole::Skip, true},
    {L"LockScreenInputOcclusionFrame", ClassRole::Skip, true},
    {L"Internet Explorer_Hidden", ClassRole::Skip, false},
    // Classic drop shadows of menus/tooltips (translucent strips) and icon titles.
    {L"SysShadow", ClassRole::Skip, false},
    {L"#32772", ClassRole::Skip, false},
    // Menus, tooltips, drop-down lists, classic task switcher.
    {L"#32768", ClassRole::Occluder, false},
    {L"tooltips_class32", ClassRole::Occluder, false},
    {L"ComboLBox", ClassRole::Occluder, false},
    {L"#32771", ClassRole::Occluder, true},
    // Shell surfaces (Start, Search, notification centre, quick settings, task view,
    // taskbar thumbnails, tray overflow). They are cloaked when closed, so they only
    // occlude while actually on screen.
    {L"Windows.UI.Core.CoreWindow", ClassRole::Occluder, true},
    {L"XamlExplorerHostIslandWindow", ClassRole::Occluder, true},
    {L"MultitaskingViewFrame", ClassRole::Occluder, true},
    {L"TaskListThumbnailWnd", ClassRole::Occluder, true},
    {L"TopLevelWindowForOverflowXamlIsland", ClassRole::Occluder, true},
    {L"NotifyIconOverflowWindow", ClassRole::Occluder, true},
    {L"ControlCenterWindow", ClassRole::Occluder, true},
    {L"Shell_InputSwitchTopLevelWindow", ClassRole::Occluder, true},
    // XAML / WinUI popups (menus, flyouts, teaching tips).
    {L"Xaml_WindowedPopupClass", ClassRole::Occluder, false},
    {L"Microsoft.UI.Content.PopupWindowSiteBridge", ClassRole::Occluder, false},
    {L"PopupHost", ClassRole::Occluder, false},
    // UWP application frame: a real app window even though it has no WS_CAPTION.
    {L"ApplicationFrameWindow", ClassRole::App, false},
};

const ClassRule* findClassRule(const std::wstring& cls) {
    for (const ClassRule& r : kClassRules)
        if (cls == r.name) return &r;
    return nullptr;
}

// ---- small helpers -----------------------------------------------------------------
std::int64_t nowUs() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq * 1000000 + c.QuadPart % freq * 1000000 / freq;
}

RectI toRectI(const RECT& r) { return RectI{r.left, r.top, r.right, r.bottom}; }

bool covers(const RectI& outer, const RectI& inner) {
    return outer.l <= inner.l && outer.t <= inner.t && outer.r >= inner.r && outer.b >= inner.b;
}

std::wstring lowerBaseName(std::wstring s) {
    const size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s.erase(0, slash + 1);
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool isTopLevel(HWND hwnd) { return GetAncestor(hwnd, GA_ROOT) == hwnd; }

bool isCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// DWM extended frame bounds (the visible frame, physical px), falling back to
// GetWindowRect. Returns false when no usable rectangle is available.
bool frameBounds(HWND hwnd, RectI& out) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || r.right <= r.left ||
        r.bottom <= r.top) {
        if (!GetWindowRect(hwnd, &r)) return false;
    }
    out = toRectI(r);
    if (out.empty()) return false;
    if (out.width() > kMaxSanePx || out.height() > kMaxSanePx) return false;
    return true;
}

// Type-erased access for the diagnostics side-door (Impl itself is private).
struct DebugAccess {
    virtual ~DebugAccess() = default;
    virtual TrackerDebugStats debugStats() const = 0;
    virtual void debugResetStats() = 0;
    virtual WindowId debugIdFor(HWND hwnd) const = 0;
    virtual bool debugDescribe(WindowId id, std::wstring& cls, std::wstring& exe) const = 0;
};
thread_local DebugAccess* t_debug = nullptr;

struct WinEq {
    bool operator()(const WindowInfo& a, const WindowInfo& b) const {
        return a.id == b.id && a.bounds == b.bounds && a.kind == b.kind && a.dpiScale == b.dpiScale &&
               a.cornerRadiusPx == b.cornerRadiusPx && a.maximized == b.maximized && a.snowAllowed == b.snowAllowed;
    }
};

}  // namespace

// =====================================================================================
struct WindowTracker::Impl final : DebugAccess {
    // Static facts about one HWND, cached until the HWND dies.
    struct Entry {
        WindowId id = 0;
        DWORD pid = 0;
        std::wstring cls;
        std::wstring exe;        // lower-case base name, empty when the process cannot be opened
        ClassRole role = ClassRole::Normal;
        bool shell = false;
        bool excluded = false;   // exe is in TrackerOptions::excludedProcesses
        bool wasApp = false;     // has been published as App at least once
        int cornerPref = -1;     // DWM_WINDOW_CORNER_PREFERENCE, -1 = not queried
        std::uint64_t seenGen = 0;
    };
    struct Monitor {
        HMONITOR handle = nullptr;
        MonitorInfo info;
    };

    bool running = false;
    HINSTANCE instance = nullptr;
    HWND helper = nullptr;
    HWND desktop = nullptr;
    DWORD selfPid = GetCurrentProcessId();
    std::vector<HWINEVENTHOOK> hooks;
    HANDLE coalesceTimer = nullptr;
    PTP_WAIT coalesceWait = nullptr;

    LayoutCallback callback;
    std::shared_ptr<const DesktopLayout> published = std::make_shared<DesktopLayout>();
    std::uint64_t revision = 0;

    std::unordered_set<std::wstring> excluded;   // lower-case base names
    std::unordered_set<HWND> ignored;
    std::unordered_set<HWND> ownApp;

    std::unordered_map<HWND, Entry> entries;
    std::unordered_map<DWORD, std::wstring> exeByPid;
    WindowId nextId = 1;
    std::uint64_t gen = 0;

    std::vector<Monitor> monitors;
    RectI virtualScreen;
    bool monitorsDirty = true;
    bool fullRefresh = true;        // re-query slowly changing facts (corner preference, QUNS)
    bool cornerPrefSupported = true;

    HWND lastForeground = nullptr;
    bool qunsFullscreen = false;

    bool dirty = false;
    // Fast path: when every event since the last pass was a LOCATIONCHANGE of a few
    // already-listed windows, only those windows are re-read instead of enumerating
    // everything (EnumWindows + ~40 DWM queries cost 0.5-1 ms while a window is being
    // dragged, because the whole desktop is busy reacting to the move).
    bool needFull = true;
    std::vector<HWND> moved;         // windows with pending LOCATIONCHANGE
    std::vector<HWND> listedHwnds;   // parallel to scratchWindows (== published->windows)
    bool scheduled = false;
    bool inResync = false;
    std::int64_t lastResyncUs = 0;

    // Scratch buffers reused between passes.
    std::vector<WindowInfo> scratchWindows;
    std::vector<WindowId> scratchHidden;

    TrackerDebugStats stats;

    static thread_local Impl* active;

    // ---- DebugAccess --------------------------------------------------------------
    TrackerDebugStats debugStats() const override { return stats; }
    void debugResetStats() override { stats = {}; }
    WindowId debugIdFor(HWND hwnd) const override {
        auto it = entries.find(hwnd);
        return it != entries.end() ? it->second.id : 0;
    }
    bool debugDescribe(WindowId id, std::wstring& cls, std::wstring& exe) const override {
        for (const auto& kv : entries) {
            if (kv.second.id == id) {
                cls = kv.second.cls;
                exe = kv.second.exe;
                return true;
            }
        }
        return false;
    }

    // ---- lifecycle ----------------------------------------------------------------
    bool start(HINSTANCE inst, LayoutCallback cb);
    void stop();

    // ---- scheduling ---------------------------------------------------------------
    void schedule(bool full = true);
    void onResyncMessage();
    void onWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild);

    // ---- enumeration --------------------------------------------------------------
    void refreshMonitors();
    const Monitor* monitorOf(HWND hwnd);
    Entry& entryFor(HWND hwnd, DWORD pid);
    const std::wstring& exeForPid(DWORD pid);
    void applyExclusion(Entry& e) const { e.excluded = !e.exe.empty() && excluded.count(e.exe) != 0; }
    bool appStyle(const Entry& e, HWND hwnd, LONG_PTR style, LONG_PTR ex) const;
    float cornerRadius(Entry& e, HWND hwnd, LONG_PTR style, float dpiScale);
    enum class Verdict { Skip, Hidden, Listed };
    Verdict classify(HWND hwnd, WindowInfo& out);
    void visit(HWND hwnd);
    bool fastPass();
    bool computeFullscreen();
    void resync(bool forcePublish);

    static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lp);
    static BOOL CALLBACK enumMonitorsProc(HMONITOR mon, HDC, LPRECT, LPARAM lp);
    static void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                      DWORD);
    static void CALLBACK coalesceWaitProc(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WAIT, TP_WAIT_RESULT);
    static LRESULT CALLBACK helperProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

thread_local WindowTracker::Impl* WindowTracker::Impl::active = nullptr;

// ---- lifecycle ------------------------------------------------------------------------
bool WindowTracker::Impl::start(HINSTANCE inst, LayoutCallback cb) {
    if (running) stop();
    if (active != nullptr) {
        SNOW_LOGE("WindowTracker: another tracker is already running on this thread");
        return false;
    }
    instance = inst ? inst : GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = helperProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHelperClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        SNOW_LOGE("WindowTracker: RegisterClassEx failed (%lu)", GetLastError());
        return false;
    }
    // Hidden top-level tool window: receives broadcasts, never shown, never activated.
    helper = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHelperClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, instance, nullptr);
    if (!helper) {
        SNOW_LOGE("WindowTracker: helper window creation failed (%lu)", GetLastError());
        return false;
    }
    SetWindowLongPtrW(helper, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    desktop = GetDesktopWindow();

    // Trailing-edge coalescing timer (optional; WM_TIMER is the fallback).
    coalesceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,
                                           TIMER_ALL_ACCESS);
    if (!coalesceTimer) coalesceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (coalesceTimer) coalesceWait = CreateThreadpoolWait(coalesceWaitProc, helper, nullptr);

    // WinEvent hooks. All out-of-context: callbacks are delivered to this thread's
    // message loop, nothing is injected into other processes. Own-process events are
    // NOT skipped (the settings window is tracked); filtering happens in onWinEvent.
    // EVENT_OBJECT_CREATE is deliberately not hooked: it fires for every child control,
    // and a top-level window only matters once it is shown (EVENT_OBJECT_SHOW).
    struct Range {
        DWORD lo, hi;
    };
    const Range ranges[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER},  // DESTROY, SHOW, HIDE, REORDER
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (const Range& r : ranges) {
        HWINEVENTHOOK h = SetWinEventHook(r.lo, r.hi, nullptr, winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (h)
            hooks.push_back(h);
        else
            SNOW_LOGW("WindowTracker: SetWinEventHook(0x%lX-0x%lX) failed", r.lo, r.hi);
    }
    SetTimer(helper, kTimerReconcile, kReconcileMs, nullptr);

    callback = std::move(cb);
    active = this;
    t_debug = this;
    running = true;
    stats = {};
    monitorsDirty = true;
    fullRefresh = true;
    dirty = false;
    scheduled = false;
    SNOW_LOGI("WindowTracker: started, %zu hooks", hooks.size());
    resync(true);
    return running;  // the callback may have called stop()
}

void WindowTracker::Impl::stop() {
    if (!running && !helper) return;
    running = false;
    for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
    hooks.clear();
    if (coalesceWait) {
        SetThreadpoolWait(coalesceWait, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(coalesceWait, TRUE);
        CloseThreadpoolWait(coalesceWait);
        coalesceWait = nullptr;
    }
    if (coalesceTimer) {
        CancelWaitableTimer(coalesceTimer);
        CloseHandle(coalesceTimer);
        coalesceTimer = nullptr;
    }
    if (helper) {
        KillTimer(helper, kTimerReconcile);
        KillTimer(helper, kTimerCoalesceFallback);
        SetWindowLongPtrW(helper, GWLP_USERDATA, 0);
        DestroyWindow(helper);
        helper = nullptr;
    }
    if (active == this) {
        active = nullptr;
        t_debug = nullptr;
    }
    callback = nullptr;
    // Ids are session-unique: after a restart every window gets a fresh id.
    entries.clear();
    exeByPid.clear();
    moved.clear();
    listedHwnds.clear();
    scratchWindows.clear();
    needFull = true;
    dirty = scheduled = false;
    SNOW_LOGI("WindowTracker: stopped");
}

// ---- scheduling -----------------------------------------------------------------------
void WindowTracker::Impl::schedule(bool full) {
    dirty = true;
    if (full) needFull = true;
    if (scheduled || !running) return;
    scheduled = true;
    const std::int64_t elapsed = nowUs() - lastResyncUs;
    if (elapsed >= kCoalesceUs) {
        PostMessageW(helper, kMsgResync, 0, 0);  // idle -> react immediately
        return;
    }
    const std::int64_t waitUs = kCoalesceUs - elapsed;
    if (coalesceWait) {
        LARGE_INTEGER due;
        due.QuadPart = -(waitUs * 10);  // relative, 100 ns units
        SetWaitableTimer(coalesceTimer, &due, 0, nullptr, nullptr, FALSE);
        SetThreadpoolWait(coalesceWait, coalesceTimer, nullptr);
    } else {
        SetTimer(helper, kTimerCoalesceFallback, static_cast<UINT>(std::max<std::int64_t>(1, waitUs / 1000)), nullptr);
    }
}

void WindowTracker::Impl::onResyncMessage() {
    scheduled = false;
    if (!running || !dirty || inResync) return;
    resync(false);
}

void CALLBACK WindowTracker::Impl::coalesceWaitProc(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WAIT, TP_WAIT_RESULT) {
    // Thread-pool thread: do nothing except wake the UI thread.
    PostMessageW(static_cast<HWND>(ctx), kMsgResync, 0, 0);
}

void CALLBACK WindowTracker::Impl::winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                                DWORD, DWORD) {
    if (Impl* self = active) self->onWinEvent(event, hwnd, idObject, idChild);
}

// Must stay trivial: filter, flag, schedule. The vast majority of events (cursor, caret,
// child controls, invisible windows) are rejected by the first two or three tests.
void WindowTracker::Impl::onWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild) {
    if (!running || !hwnd) return;  // cursor/caret events have no hwnd
    if (event == EVENT_OBJECT_REORDER) {
        // Z-order changes of top-level windows are reported on their container (the
        // desktop window); list views etc. also fire REORDER for their items -> reject.
        if (hwnd != desktop && !isTopLevel(hwnd)) return;
    } else {
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
        if (event == EVENT_OBJECT_DESTROY) {
            // Only windows we know matter; the id is dropped right here so that a
            // recycled HWND value can never inherit it.
            auto it = entries.find(hwnd);
            if (it == entries.end()) return;
            if (!inResync) entries.erase(it);
        } else if (event == EVENT_OBJECT_HIDE) {
            if (entries.find(hwnd) == entries.end()) return;
        } else {
            if (!isTopLevel(hwnd)) return;
            if (event == EVENT_OBJECT_LOCATIONCHANGE && !IsWindowVisible(hwnd)) return;
        }
    }
    if (hwnd == helper || ignored.count(hwnd) != 0) return;
    ++stats.events;
    if (event == EVENT_OBJECT_LOCATIONCHANGE && moved.size() < kMaxFastMoved) {
        if (std::find(moved.begin(), moved.end(), hwnd) == moved.end()) moved.push_back(hwnd);
        schedule(false);
    } else {
        schedule(true);
    }
}

LRESULT CALLBACK WindowTracker::Impl::helperProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->running) {
        switch (msg) {
            case kMsgResync:
                self->onResyncMessage();
                return 0;
            case WM_TIMER:
                if (wp == kTimerReconcile) {
                    self->monitorsDirty = true;
                    self->fullRefresh = true;
                    self->schedule();
                } else if (wp == kTimerCoalesceFallback) {
                    KillTimer(hwnd, kTimerCoalesceFallback);
                    self->onResyncMessage();
                }
                return 0;
            case WM_DISPLAYCHANGE:   // resolution / monitor hot-plug
            case WM_SETTINGCHANGE:   // work area, taskbar position
            case WM_DPICHANGED:
            case WM_DEVICECHANGE:
                self->monitorsDirty = true;
                self->fullRefresh = true;
                self->schedule();
                break;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- monitors -------------------------------------------------------------------------
BOOL CALLBACK WindowTracker::Impl::enumMonitorsProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    Impl* self = reinterpret_cast<Impl*>(lp);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    Monitor m;
    m.handle = mon;
    m.info.id = static_cast<int>(self->monitors.size());
    m.info.bounds = toRectI(mi.rcMonitor);
    m.info.work = toRectI(mi.rcWork);
    m.info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
    m.info.dpiScale = static_cast<float>(dx) / 96.0f;
    if (!m.info.bounds.empty()) self->monitors.push_back(m);
    return TRUE;
}

void WindowTracker::Impl::refreshMonitors() {
    monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, enumMonitorsProc, reinterpret_cast<LPARAM>(this));
    virtualScreen = RectI{};
    bool first = true;
    for (const Monitor& m : monitors) {
        if (first) {
            virtualScreen = m.info.bounds;
            first = false;
        } else {
            virtualScreen.l = std::min(virtualScreen.l, m.info.bounds.l);
            virtualScreen.t = std::min(virtualScreen.t, m.info.bounds.t);
            virtualScreen.r = std::max(virtualScreen.r, m.info.bounds.r);
            virtualScreen.b = std::max(virtualScreen.b, m.info.bounds.b);
        }
    }
    monitorsDirty = false;
}

const WindowTracker::Impl::Monitor* WindowTracker::Impl::monitorOf(HWND hwnd) {
    HMONITOR h = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (const Monitor& m : monitors)
        if (m.handle == h) return &m;
    // Unknown handle: the monitor set changed under us. Refresh once and retry.
    refreshMonitors();
    for (const Monitor& m : monitors)
        if (m.handle == h) return &m;
    return nullptr;
}

// ---- per-window cache -----------------------------------------------------------------
const std::wstring& WindowTracker::Impl::exeForPid(DWORD pid) {
    auto it = exeByPid.find(pid);
    if (it != exeByPid.end()) return it->second;
    std::wstring name;
    // PROCESS_QUERY_LIMITED_INFORMATION works for most processes without elevation;
    // when it does not (protected/elevated) the name stays empty => snow allowed.
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD len = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) name = lowerBaseName(std::wstring(path, len));
        CloseHandle(proc);
    }
    return exeByPid.emplace(pid, std::move(name)).first->second;
}

WindowTracker::Impl::Entry& WindowTracker::Impl::entryFor(HWND hwnd, DWORD pid) {
    auto it = entries.find(hwnd);
    if (it != entries.end()) {
        if (it->second.pid == pid) return it->second;
        entries.erase(it);  // HWND value recycled by another process: new identity
    }
    Entry e;
    e.id = nextId++;
    e.pid = pid;
    wchar_t cls[256];
    const int n = GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    e.cls.assign(cls, n > 0 ? static_cast<size_t>(n) : 0);
    if (const ClassRule* rule = findClassRule(e.cls)) {
        e.role = rule->role;
        e.shell = rule->shell;
    }
    e.exe = exeForPid(pid);
    applyExclusion(e);
    return entries.emplace(hwnd, std::move(e)).first->second;
}

// "Application-like" by class and styles only (size is checked by the caller because a
// minimized window has no meaningful size).
bool WindowTracker::Impl::appStyle(const Entry& e, HWND hwnd, LONG_PTR style, LONG_PTR ex) const {
    if (e.role == ClassRole::App) return true;
    if (e.role != ClassRole::Normal) return false;
    const bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    // Tool windows (floating palettes, OSDs) and never-activating windows (on-screen
    // keyboards, volume popups, notification toasts) are not applications.
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0 && !appWindow) return false;
    if ((style & WS_CAPTION) == WS_CAPTION) return true;
    // Caption-less from here on (custom-chrome apps, borderless games, splash screens,
    // but also every kind of popup).
    if ((ex & WS_EX_TOPMOST) != 0 && !appWindow) return false;  // always-on-top utility popup
    // Owned popup = dropdown / flyout / custom menu / owned tooltip of another window.
    if ((style & WS_POPUP) != 0 && !appWindow && GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    return true;
}

float WindowTracker::Impl::cornerRadius(Entry& e, HWND hwnd, LONG_PTR style, float dpiScale) {
    // DWMWA_WINDOW_CORNER_PREFERENCE (33): 0 default, 1 do-not-round, 2 round, 3 round-small.
    // Queried once per window and refreshed by the 1 s reconciliation pass. Some builds
    // reject reading this attribute; then the Windows 11 defaults below are used.
    if (cornerPrefSupported && (e.cornerPref < 0 || fullRefresh)) {
        int pref = 0;
        const HRESULT hr = DwmGetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
        if (SUCCEEDED(hr)) {
            e.cornerPref = pref;
        } else {
            e.cornerPref = 0;
            if (hr == E_INVALIDARG) cornerPrefSupported = false;
        }
    }
    switch (e.cornerPref) {
        case 1: return 0.0f;
        case 2: return 8.0f * dpiScale;
        case 3: return 4.0f * dpiScale;
        default: break;
    }
    // Default policy: Windows 11 rounds framed windows; bare WS_POPUP rectangles without
    // caption and sizing frame (borderless games, splash screens) stay square.
    if (e.role != ClassRole::App && (style & WS_CAPTION) != WS_CAPTION && (style & WS_THICKFRAME) == 0) return 0.0f;
    return 8.0f * dpiScale;
}

// ---- classification of one top-level window ---------------------------------------------
void WindowTracker::Impl::visit(HWND hwnd) {
    ++stats.topLevelSeen;
    WindowInfo w;
    switch (classify(hwnd, w)) {
        case Verdict::Listed:
            scratchWindows.push_back(w);
            listedHwnds.push_back(hwnd);
            break;
        case Verdict::Hidden:
            scratchHidden.push_back(w.id);
            break;
        case Verdict::Skip: {
            // A taskbar that still exists but is momentarily invisible/cloaked (e.g. while
            // the Start menu or Search is open) must not look "closed": that would delete its
            // drift. Report it as hidden so its snow is kept and comes back.
            auto it = entries.find(hwnd);
            if (it != entries.end() && it->second.role == ClassRole::Taskbar) {
                it->second.seenGen = gen;
                scratchHidden.push_back(it->second.id);
            }
            break;
        }
    }
}

WindowTracker::Impl::Verdict WindowTracker::Impl::classify(HWND hwnd, WindowInfo& w) {
    if (!IsWindowVisible(hwnd)) return Verdict::Skip;  // rejects the bulk of top-level windows
    if (hwnd == helper || ignored.count(hwnd) != 0) return Verdict::Skip;
    DWORD pid = 0;
    if (GetWindowThreadProcessId(hwnd, &pid) == 0 || pid == 0) return Verdict::Skip;

    Entry& e = entryFor(hwnd, pid);
    e.seenGen = gen;
    if (e.role == ClassRole::Skip) return Verdict::Skip;
    // Own windows are invisible to the tracker unless explicitly opted in.
    if (pid == selfPid && ownApp.count(hwnd) == 0) return Verdict::Skip;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    // Click-through overlays of other applications: no surface, no occlusion.
    if ((ex & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT)) return Verdict::Skip;
    if (ex & WS_EX_LAYERED) {
        COLORREF key = 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        // Fails for UpdateLayeredWindow (per-pixel alpha) windows; those are handled below.
        if (GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) && (flags & LWA_ALPHA)) {
            if (alpha == 0) return Verdict::Skip;  // fully transparent
        }
    }

    const bool iconic = (style & WS_MINIMIZE) != 0;
    const bool hiddenAlive = iconic || isCloaked(hwnd);  // cloaked: other virtual desktop, suspended UWP

    if (e.role == ClassRole::Taskbar) {
        if (hiddenAlive) return Verdict::Skip;
        RectI b;
        if (!frameBounds(hwnd, b) || !b.intersects(virtualScreen)) return Verdict::Skip;
        const Monitor* mon = monitorOf(hwnd);
        w.id = e.id;
        w.bounds = b;  // live rect: an auto-hidden taskbar reports its sliver / off-screen part as is
        w.kind = WindowKind::Taskbar;
        w.dpiScale = mon ? mon->info.dpiScale : 1.0f;
        w.snowAllowed = !e.excluded;
        return Verdict::Listed;
    }

    const bool styleApp = appStyle(e, hwnd, style, ex);
    if (hiddenAlive) {
        // Keep the snow of minimized / cloaked application windows.
        w.id = e.id;
        return (styleApp || e.wasApp) ? Verdict::Hidden : Verdict::Skip;
    }

    RectI b;
    if (!frameBounds(hwnd, b)) return Verdict::Skip;  // no trustworthy rect -> never invent a surface
    if (!b.intersects(virtualScreen)) {
        // Parked completely off-screen (some apps do this instead of hiding).
        w.id = e.id;
        return (styleApp || e.wasApp) ? Verdict::Hidden : Verdict::Skip;
    }
    if (b.width() < kMinVisiblePx || b.height() < kMinVisiblePx) return Verdict::Skip;  // 1x1 helper windows etc.

    const Monitor* mon = monitorOf(hwnd);
    const float dpi = mon ? mon->info.dpiScale : 1.0f;
    const bool hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    const bool coversMonitor = mon && covers(b, mon->info.bounds);

    if (!hasCaption && e.role == ClassRole::Normal) {
        if (ex & WS_EX_LAYERED) {
            // Glow / shadow frames around custom-chrome windows (VS, Office...): thin
            // translucent strips that must not cut holes into the snow.
            if (static_cast<float>(std::min(b.width(), b.height())) < kLayeredThinPx * dpi) return Verdict::Skip;
            // A caption-less layered window spanning a whole monitor is in practice a
            // translucent overlay (annotation tools, dimmers, other desktop effects).
            if (coversMonitor) return Verdict::Skip;
        }
        // Composition-only (no redirection surface) always-on-top / non-activating window
        // spanning a whole monitor: GPU overlay of another tool, content mostly transparent.
        if (coversMonitor && (ex & WS_EX_NOREDIRECTIONBITMAP) != 0 &&
            (ex & (WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0)
            return Verdict::Skip;
    }

    const float wPx = static_cast<float>(b.width()), hPx = static_cast<float>(b.height());
    bool app = styleApp;
    if (app && e.role != ClassRole::App) {
        if (hasCaption)
            app = wPx >= kCaptionAppMinW * dpi && hPx >= kCaptionAppMinH * dpi;
        else
            app = wPx >= kAppMinW * dpi && hPx >= kAppMinH * dpi;
    }

    w.id = e.id;
    w.bounds = b;
    w.dpiScale = dpi;
    w.snowAllowed = !e.excluded;
    if (app) {
        e.wasApp = true;
        w.kind = WindowKind::App;
        w.maximized = (style & WS_MAXIMIZE) != 0;
        // Square corners when maximized or when the window fills its monitor (fullscreen).
        w.cornerRadiusPx = (w.maximized || coversMonitor) ? 0.0f : cornerRadius(e, hwnd, style, dpi);
    } else {
        w.kind = WindowKind::Occluder;
    }
    return Verdict::Listed;
}

// Re-reads only the windows that reported a LOCATIONCHANGE. Returns false when anything
// beyond geometry may have changed, in which case the caller runs a full pass.
bool WindowTracker::Impl::fastPass() {
    for (HWND hwnd : moved) {
        const auto it = std::find(listedHwnds.begin(), listedHwnds.end(), hwnd);
        if (it == listedHwnds.end()) return false;  // not listed before: might be now
        WindowInfo& slot = scratchWindows[static_cast<size_t>(it - listedHwnds.begin())];
        WindowInfo w;
        if (classify(hwnd, w) != Verdict::Listed || w.id != slot.id || w.kind != slot.kind) return false;
        slot = w;
    }
    return true;
}


BOOL CALLBACK WindowTracker::Impl::enumWindowsProc(HWND hwnd, LPARAM lp) {
    reinterpret_cast<Impl*>(lp)->visit(hwnd);
    return TRUE;
}

// ---- fullscreen detection ----------------------------------------------------------------
bool WindowTracker::Impl::computeFullscreen() {
    HWND fg = GetForegroundWindow();
    bool rectFullscreen = false;
    DWORD pid = 0;
    if (fg && fg != helper && ignored.count(fg) == 0 && IsWindowVisible(fg) &&
        GetWindowThreadProcessId(fg, &pid) != 0 && pid != 0 && (pid != selfPid || ownApp.count(fg) != 0)) {
        const Entry& e = entryFor(fg, pid);
        const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
        // The desktop, taskbar and shell hosts (Start, lock screen, task view) cover the
        // monitor too but are not "applications".
        if (!e.shell && e.role != ClassRole::Skip && (style & WS_MINIMIZE) == 0 && !isCloaked(fg)) {
            RectI b;
            MONITORINFO mi{sizeof(mi)};
            if (frameBounds(fg, b) && GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) {
                // A maximized captioned window covers only the work area - or the whole
                // monitor when the taskbar auto-hides; neither is fullscreen. Borderless and
                // exclusive fullscreen windows drop the caption (or are not zoomed).
                const bool maximizedFramed = (style & WS_MAXIMIZE) != 0 && (style & WS_CAPTION) == WS_CAPTION;
                rectFullscreen = covers(b, toRectI(mi.rcMonitor)) && !maximizedFramed;
            }
        }
    }
    // The shell's own opinion, refreshed only when the foreground window changes or on the
    // reconciliation pass (the call is comparatively slow).
    if (fg != lastForeground || fullRefresh) {
        lastForeground = fg;
        QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
        qunsFullscreen = SUCCEEDED(SHQueryUserNotificationState(&state)) &&
                         (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE);
        // QUNS_BUSY ("a fullscreen app is running") alone is too vague - Explorer reports
        // it for some of its own surfaces - so it only counts together with the rect test,
        // which is already sufficient by itself.
    }
    return rectFullscreen || qunsFullscreen;
}

// ---- the resync pass -----------------------------------------------------------------------
void WindowTracker::Impl::resync(bool forcePublish) {
    const std::int64_t t0 = nowUs();
    inResync = true;
    dirty = false;
    stats.lastFull = fullRefresh;
    stats.topLevelSeen = 0;
    const bool tryFast = !needFull && !forcePublish && !monitorsDirty && !fullRefresh;
    if (monitorsDirty) refreshMonitors();
    const std::int64_t tMon = nowUs();
    const bool fast = tryFast && fastPass();
    if (fast) {
        ++stats.fastPasses;
    } else {
        ++gen;
        scratchWindows.clear();
        listedHwnds.clear();
        scratchHidden.clear();
        EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(this));  // Z-order, front to back
        // While the Start menu / Search is open, Windows 11 temporarily leaves Shell_TrayWnd
        // out of EnumWindows although the taskbar stays alive and on screen. A taskbar that
        // was not enumerated but still exists is reported as hidden (never as closed), so
        // its drift is kept.
        for (auto& kv : entries) {
            if (kv.second.role == ClassRole::Taskbar && kv.second.seenGen != gen && IsWindow(kv.first)) {
                kv.second.seenGen = gen;
                scratchHidden.push_back(kv.second.id);
            }
        }
    }
    stats.lastFast = fast;
    moved.clear();
    needFull = false;
    const std::int64_t tEnum = nowUs();
    const bool fullscreen = computeFullscreen();
    const std::int64_t tFs = nowUs();
    stats.lastMonitorsUs = static_cast<std::uint64_t>(tMon - t0);
    stats.lastEnumUs = static_cast<std::uint64_t>(tEnum - tMon);
    stats.lastFullscreenUs = static_cast<std::uint64_t>(tFs - tEnum);

    // Purge identities of dead HWNDs (those not seen as visible in this pass and no
    // longer valid). Hidden-but-existing windows keep their id.
    bool erased = false;
    for (auto it = entries.begin(); !fast && it != entries.end();) {
        if (it->second.seenGen != gen && !IsWindow(it->first)) {
            it = entries.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }
    if (erased) {
        // Drop exe names of pids without windows so a recycled pid is looked up again.
        for (auto it = exeByPid.begin(); it != exeByPid.end();) {
            const DWORD pid = it->first;
            const bool used = std::any_of(entries.begin(), entries.end(),
                                          [pid](const auto& kv) { return kv.second.pid == pid; });
            it = used ? std::next(it) : exeByPid.erase(it);
        }
    }
    std::sort(scratchHidden.begin(), scratchHidden.end());
    fullRefresh = false;

    const DesktopLayout& old = *published;
    bool changed = forcePublish || old.fullscreenAppActive != fullscreen || old.hiddenWindows != scratchHidden ||
                   old.monitors.size() != monitors.size() || old.windows.size() != scratchWindows.size();
    if (!changed) {
        for (size_t i = 0; i < monitors.size() && !changed; ++i) changed = !(old.monitors[i] == monitors[i].info);
        changed = changed || !std::equal(old.windows.begin(), old.windows.end(), scratchWindows.begin(), WinEq{});
    }

    std::shared_ptr<const DesktopLayout> toPublish;
    if (changed) {
        auto layout = std::make_shared<DesktopLayout>();
        layout->revision = ++revision;
        layout->monitors.reserve(monitors.size());
        for (const Monitor& m : monitors) layout->monitors.push_back(m.info);
        layout->windows = scratchWindows;
        layout->hiddenWindows = scratchHidden;
        layout->fullscreenAppActive = fullscreen;
        published = layout;
        toPublish = std::move(layout);
    }

    const std::int64_t t1 = nowUs();
    lastResyncUs = t1;
    const std::uint64_t us = static_cast<std::uint64_t>(t1 - t0);
    ++stats.resyncs;
    stats.totalUs += us;
    stats.lastUs = us;
    stats.maxUs = std::max(stats.maxUs, us);
    inResync = false;

    if (toPublish) {
        ++stats.published;
        // Copy: the callback may call stop(), which clears `callback`.
        if (LayoutCallback cb = callback) cb(std::move(toPublish));
    }
}

// =====================================================================================
WindowTracker::WindowTracker() : impl_(std::make_unique<Impl>()) {}
WindowTracker::~WindowTracker() { stop(); }

bool WindowTracker::start(HINSTANCE instance, LayoutCallback cb) { return impl_->start(instance, std::move(cb)); }
void WindowTracker::stop() { impl_->stop(); }

void WindowTracker::setOptions(const TrackerOptions& options) {
    impl_->excluded.clear();
    for (const std::wstring& name : options.excludedProcesses) {
        std::wstring n = lowerBaseName(name);
        if (!n.empty()) impl_->excluded.insert(std::move(n));
    }
    for (auto& kv : impl_->entries) impl_->applyExclusion(kv.second);
    impl_->schedule();
}

void WindowTracker::setIgnoredWindows(const std::vector<HWND>& windows) {
    impl_->ignored.clear();
    impl_->ignored.insert(windows.begin(), windows.end());
    impl_->schedule();
}

void WindowTracker::setOwnAppWindows(const std::vector<HWND>& windows) {
    impl_->ownApp.clear();
    impl_->ownApp.insert(windows.begin(), windows.end());
    impl_->schedule();
}

void WindowTracker::requestResync() {
    impl_->monitorsDirty = true;
    impl_->fullRefresh = true;
    impl_->schedule();
}

std::shared_ptr<const DesktopLayout> WindowTracker::current() const { return impl_->published; }

// ---- diagnostics side-door ---------------------------------------------------------------
TrackerDebugStats trackerDebugStats() { return t_debug ? t_debug->debugStats() : TrackerDebugStats{}; }

void trackerDebugResetStats() {
    if (t_debug) t_debug->debugResetStats();
}

WindowId trackerDebugIdForWindow(HWND hwnd) { return t_debug ? t_debug->debugIdFor(hwnd) : 0; }

bool trackerDebugDescribe(WindowId id, std::wstring& className, std::wstring& exeName) {
    return t_debug && t_debug->debugDescribe(id, className, exeName);
}

}  // namespace snow
