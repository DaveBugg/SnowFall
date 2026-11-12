// Themed settings window (hybrid: real Win32 controls painted by the Skin + Direct2D decorations).
#include "settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#include "compositor.h"
#include "core/skins.h"
#include "countdown.h"
#include "d2d_paint.h"
#include "decor.h"
#include "hotkey_edit.h"
#include "process_picker.h"
#include "settings_ids.h"
#include "skin.h"
#include "snow_preview.h"
#include "strings.h"
#include "theme.h"
#include "tray_icon.h"
#include "ui_util.h"
#include "user_presets.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef SNOW_VERSION
#define SNOW_VERSION "dev"
#endif

namespace snow {

using namespace snow::ui;

namespace {

const wchar_t kWindowClass[] = L"SnowFallSettings";
const wchar_t kPageClass[] = L"SnowFallSettingsPage";
const wchar_t kDecoClass[] = L"SnowFallDeco";

constexpr UINT_PTR kDiagTimerId = 1;
constexpr UINT_PTR kAnimTimerId = 2;
constexpr UINT kDiagTimerMs = 500;
constexpr UINT kAnimTimerMs = 50;   // 20 fps: smooth enough for falling snow, ~3 % CPU
constexpr UINT_PTR kChipsSubclassId = 0x43484950;   // 'CHIP'

// Layout, DIPs.
constexpr int kHeaderH = 64;
constexpr int kNavW = 196;
constexpr int kPageW = 560;
constexpr int kPageH = 484;
constexpr int kRightW = 280;
constexpr int kPillH = 34;
constexpr int kBottomH = 32;
constexpr int kEditH = 18;

UINT g_dpiOverride = 0;

enum class FontKind { Normal, Heading, Title };
enum class ValueKind { Percent, Px, Wind, Minutes, EveryMinutes, Snowmen };

struct Ctl {
    HWND hwnd = nullptr;
    int id = -1;
    int page = -1;                    // 0..6 page; -1 main window (positioned by layoutMain); -2 header child
    int x = 0, y = 0, w = 0, h = 0;   // DIPs (pages only)
    int strId = -1;
    FontKind font = FontKind::Normal;
    bool keepSize = false;
};

struct Field {   // rounded input frame painted by the page behind an EDIT / LISTBOX
    int page;
    int x, y, w, h;
    HWND inner;
};

struct BoolField {
    int id;
    bool& (*ref)(Settings&);
};
const BoolField kBoolFields[] = {
    {IDC_SKY_PER_MONITOR, [](Settings& s) -> bool& { return s.weather.skyPerMonitor; }},
    {IDC_ACC_ENABLED, [](Settings& s) -> bool& { return s.accumulation.enabled; }},
    {IDC_ACC_WINDOWS, [](Settings& s) -> bool& { return s.accumulation.onWindows; }},
    {IDC_ACC_TASKBAR, [](Settings& s) -> bool& { return s.accumulation.onTaskbar; }},
    {IDC_ACC_BOTTOM, [](Settings& s) -> bool& { return s.accumulation.onScreenBottom; }},
    {IDC_SKIP_MAXIMIZED, [](Settings& s) -> bool& { return s.accumulation.skipMaximized; }},
    {IDC_DROP_ON_CLOSE, [](Settings& s) -> bool& { return s.accumulation.dropSnowOnClose; }},
    {IDC_SHAKE_TO_CLEAR, [](Settings& s) -> bool& { return s.accumulation.shakeToClear; }},
    {IDC_NATURAL_CAPS, [](Settings& s) -> bool& { return s.accumulation.naturalCaps; }},
    {IDC_ICICLES, [](Settings& s) -> bool& { return s.accumulation.icicles; }},
    {IDC_FALLING_SNOWMEN, [](Settings& s) -> bool& { return s.accumulation.fallingSnowmen; }},
    {IDC_SNOWMEN_HEAVY, [](Settings& s) -> bool& { return s.accumulation.snowmenNeedHeavySnow; }},
    {IDC_SNOWPLOW, [](Settings& s) -> bool& { return s.accumulation.snowplow; }},
    {IDC_SNOWPLOW_ALL, [](Settings& s) -> bool& { return s.accumulation.snowplowAllTaskbars; }},
    {IDC_INTERACTION, [](Settings& s) -> bool& { return s.interaction.enabled; }},
    {IDC_HOLD_KEY, [](Settings& s) -> bool& { return s.interaction.holdKeyEnabled; }},
    {IDC_BLOCK_CLICKS, [](Settings& s) -> bool& { return s.interaction.blockClicksWhileHeld; }},
    {IDC_ALL_MONITORS, [](Settings& s) -> bool& { return s.interaction.objectsOnAllMonitors; }},
    {IDC_UMBRELLA, [](Settings& s) -> bool& { return s.interaction.umbrellaCursor; }},
    {IDC_PAUSE_FULLSCREEN, [](Settings& s) -> bool& { return s.performance.pauseOnFullscreen; }},
    {IDC_REDUCE_BATTERY, [](Settings& s) -> bool& { return s.performance.reduceOnBattery; }},
    {IDC_ANIMATIONS, [](Settings& s) -> bool& { return s.performance.animations; }},
    {IDC_GARLAND, [](Settings& s) -> bool& { return s.festive.garland; }},
    {IDC_FIREWORKS, [](Settings& s) -> bool& { return s.festive.fireworks; }},
    {IDC_STARTUP, [](Settings& s) -> bool& { return s.startupWithWindows; }},
    {IDC_LEFT_CLICK, [](Settings& s) -> bool& { return s.system.trayLeftClickTogglesPause; }},
};

struct SliderField {
    int id;
    int valueId;
    int lo, hi;
    ValueKind kind;
    int& (*ref)(Settings&);
};
const SliderField kSliderFields[] = {
    {IDC_INTENSITY, IDC_INTENSITY_VAL, 0, 100, ValueKind::Percent, [](Settings& s) -> int& { return s.weather.intensity; }},
    {IDC_WIND, IDC_WIND_VAL, -100, 100, ValueKind::Wind, [](Settings& s) -> int& { return s.weather.wind; }},
    {IDC_GUSTINESS, IDC_GUSTINESS_VAL, 0, 100, ValueKind::Percent, [](Settings& s) -> int& { return s.weather.gustiness; }},
    {IDC_FLAKE_SIZE, IDC_FLAKE_SIZE_VAL, 50, 200, ValueKind::Percent, [](Settings& s) -> int& { return s.weather.sizePercent; }},
    {IDC_FALLSPEED, IDC_FALLSPEED_VAL, 25, 200, ValueKind::Percent, [](Settings& s) -> int& { return s.weather.fallSpeedPercent; }},
    {IDC_OPACITY, IDC_OPACITY_VAL, 10, 100, ValueKind::Percent, [](Settings& s) -> int& { return s.weather.opacityPercent; }},
    {IDC_MAXHEIGHT, IDC_MAXHEIGHT_VAL, 0, 100, ValueKind::Px, [](Settings& s) -> int& { return s.accumulation.maxHeightPxAt96Dpi; }},
    {IDC_DRIFT_OPACITY, IDC_DRIFT_OPACITY_VAL, 10, 100, ValueKind::Percent, [](Settings& s) -> int& { return s.accumulation.driftOpacityPercent; }},
    {IDC_SNOWPLOW_DELAY, IDC_SNOWPLOW_DELAY_VAL, 1, 120, ValueKind::Minutes, [](Settings& s) -> int& { return s.accumulation.snowplowDelayMinutes; }},
    {IDC_SNOWMEN_INTERVAL, IDC_SNOWMEN_INTERVAL_VAL, 1, 60, ValueKind::EveryMinutes, [](Settings& s) -> int& { return s.accumulation.snowmenIntervalMinutes; }},
    {IDC_SNOWMEN_MAX, IDC_SNOWMEN_MAX_VAL, 1, 10, ValueKind::Snowmen, [](Settings& s) -> int& { return s.accumulation.snowmenMaxAlive; }},
    {IDC_MITTEN_RADIUS, IDC_MITTEN_RADIUS_VAL, 12, 48, ValueKind::Px, [](Settings& s) -> int& { return s.interaction.brushRadiusPxAt96Dpi; }},
    {IDC_SHOVEL_WIDTH, IDC_SHOVEL_WIDTH_VAL, 48, 160, ValueKind::Px, [](Settings& s) -> int& { return s.interaction.shovelWidthPxAt96Dpi; }},
};

enum class GroupKind { Plain, Language, Theme };
struct RadioGroup {
    int firstId;
    int count;
    GroupKind kind;
    int (*get)(const Settings&);
    void (*set)(Settings&, int);
};
const RadioGroup kRadioGroups[] = {
    {IDC_PRESET_LIGHT, 4, GroupKind::Plain, [](const Settings& s) { return static_cast<int>(s.weather.preset); },
     [](Settings& s, int i) { s.weather.preset = static_cast<WeatherPreset>(i); }},
    {IDC_SHAKE_KEEP, 2, GroupKind::Plain, [](const Settings& s) { return s.accumulation.shakeOffOnMove ? 1 : 0; },
     [](Settings& s, int i) { s.accumulation.shakeOffOnMove = (i == 1); }},
    {IDC_TOOL_MITTEN, 3, GroupKind::Plain, [](const Settings& s) { return static_cast<int>(s.interaction.tool); },
     [](Settings& s, int i) { s.interaction.tool = static_cast<Tool>(i); }},
    {IDC_PROFILE_LOW, 3, GroupKind::Plain, [](const Settings& s) { return static_cast<int>(s.performance.profile); },
     [](Settings& s, int i) { s.performance.profile = static_cast<PerfProfile>(i); }},
    {IDC_FPS_30, 2, GroupKind::Plain, [](const Settings& s) { return s.performance.fpsCap <= 30 ? 0 : 1; },
     [](Settings& s, int i) { s.performance.fpsCap = (i == 0) ? 30 : 60; }},
    {IDC_LANG_AUTO, 3, GroupKind::Language, [](const Settings& s) { return static_cast<int>(s.system.language); },
     [](Settings& s, int i) { s.system.language = static_cast<Language>(i); }},
    {IDC_THEME_NIGHT, 3, GroupKind::Theme, [](const Settings& s) { return static_cast<int>(s.system.uiTheme); },
     [](Settings& s, int i) { s.system.uiTheme = static_cast<UiTheme>(i); }},
    {IDC_SWATCH_NIGHT, 3, GroupKind::Theme, [](const Settings& s) { return static_cast<int>(s.system.uiTheme); },
     [](Settings& s, int i) { s.system.uiTheme = static_cast<UiTheme>(i); }},
    {IDC_HDR_TOOL_SNOWBALL, 3, GroupKind::Plain,
     [](const Settings& s) { return s.interaction.tool == Tool::Snowball ? 0 : s.interaction.tool == Tool::Mitten ? 1 : 2; },
     [](Settings& s, int i) { s.interaction.tool = i == 0 ? Tool::Snowball : i == 1 ? Tool::Mitten : Tool::Shovel; }},
    {IDC_EXCL_BEHIND, 2, GroupKind::Plain, [](const Settings& s) { return static_cast<int>(s.system.excludedSnowMode); },
     [](Settings& s, int i) { s.system.excludedSnowMode = static_cast<ExcludedSnowMode>(i); }},
    {IDC_SKIN_FIRST, 1 + kSnowmanSkinCount, GroupKind::Plain,
     [](const Settings& s) {
         for (int i = 0; i < kSnowmanSkinCount; ++i)
             if (s.interaction.snowmanSkin == kSnowmanSkins[i].id) return i + 1;
         return 0;
     },
     [](Settings& s, int i) { s.interaction.snowmanSkin = i <= 0 ? std::string("random") : std::string(kSnowmanSkins[i - 1].id); }},
};

struct HotkeyField {
    int id;
    int statusId;
    std::string HotkeySettings::* member;
};
const HotkeyField kHotkeyFields[] = {
    {IDC_HOTKEY_PAUSE, IDC_HOTKEY_PAUSE_STATUS, &HotkeySettings::pauseToggle},
    {IDC_HOTKEY_CLEAR, IDC_HOTKEY_CLEAR_STATUS, &HotkeySettings::clearSnow},
    {IDC_HOTKEY_INTERACTION, IDC_HOTKEY_INTERACTION_STATUS, &HotkeySettings::interactionToggle},
};

const StrId kTabTitles[kTabCount] = {StrId::TabSnow,     StrId::TabDrifts, StrId::TabTools, StrId::TabPerformance,
                                     StrId::TabFestive,  StrId::TabSystem, StrId::TabAbout};
const StrId kSceneTitles[4] = {StrId::SceneLight, StrId::SceneNormal, StrId::SceneBlizzard, StrId::SceneThaw};
const StrId kSceneSubs[4] = {StrId::SceneLightSub, StrId::SceneNormalSub, StrId::SceneBlizzardSub, StrId::SceneThawSub};
const StrId kThemeNames[3] = {StrId::ThemeNight, StrId::ThemeGlobe, StrId::ThemePostcard};
// Russian plural forms (1 / 2-4 / 5+); English uses the 1-form only for exactly 1.
StrId pluralRu(int n, StrId one, StrId few, StrId many) {
    if (effectiveLanguage() != Language::Russian) return n == 1 ? one : many;
    const int a = n % 100, b = n % 10;
    if (a >= 11 && a <= 14) return many;
    if (b == 1) return one;
    if (b >= 2 && b <= 4) return few;
    return many;
}
const StrId kHdrToolNames[3] = {StrId::ToolSnowball, StrId::ToolMitten, StrId::ToolShovel};
const Tool kHdrTools[3] = {Tool::Snowball, Tool::Mitten, Tool::Shovel};

}  // namespace

struct SettingsWindow::Impl {
    HINSTANCE instance = nullptr;
    SettingsWindowCallbacks cb;
    Settings s;
    HWND hwnd = nullptr;
    HWND header = nullptr, preview = nullptr, previewReset = nullptr, countdownWnd = nullptr;
    HWND statusLine = nullptr, restoreBtn = nullptr, closeBtn = nullptr;
    HWND pages[kTabCount] = {};
    HWND nav[kTabCount] = {};
    HWND swatches[3] = {};
    HWND tooltip = nullptr;
    std::vector<Ctl> ctls;
    std::vector<Field> fields;
    HFONT font = nullptr, fontHeading = nullptr, fontTitle = nullptr;
    HICON iconBig = nullptr, iconSmall = nullptr, iconAbout = nullptr;
    UINT dpi = 96;
    int curTab = 0;
    int updating = 0;
    bool built = false;
    bool hotkeyWarn[ARRAYSIZE(kHotkeyFields)] = {};
    HWND lastFocus = nullptr;
    Skin skin;
    SceneAnim scenes[4];
    HeaderAnim headerAnim;
    FireworksAnim fireworks;
    std::unique_ptr<SnowPreview> snowPreview;
    bool animTimer = false, diagTimer = false;
    ULONGLONG lastTick = 0;
    unsigned tickCount = 0;
    ProcessPicker picker;
    bool upEditing = false;
    TextRole upMsgRole = TextRole::Secondary;
    D2D1_RECT_F chipRect{};   // theme swatch container in the header, DIPs
    D2D1_RECT_F toolChipRect{};
    HWND hdrTools[3] = {};
    std::wstring interactionKeys;   // from DiagnosticsInfo
    Layer pvBase, pvDrift, countdownLayer, festiveLayer;
    std::uint64_t pvDriftKey = 0;
    ULONGLONG pvDriftTick = 0;
    std::wstring countdownKey;

    // ---- helpers ---------------------------------------------------------------------------------
    const Palette& pal() const { return palette(s.system.uiTheme); }
    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi), 96); }
    float k() const { return 96.0f / static_cast<float>(dpi); }

    HWND ctl(int id) const {
        for (const Ctl& c : ctls)
            if (c.id == id) return c.hwnd;
        return nullptr;
    }

    // The preview only gets the new settings on the next animation tick (<= 20 Hz) and never
    // re-runs its warm-up: applySettings on the existing World.
    bool previewDirty = false;
    double lastChangeMs = 0, maxChangeMs = 0;
    void changed() {
        LARGE_INTEGER q0, q1, qf;
        QueryPerformanceCounter(&q0);
        previewDirty = true;
        if (!animTimer && snowPreview) {   // no timer (hidden): apply now, it is cheap
            snowPreview->apply(s);
            previewDirty = false;
        }
        if (cb.onChanged) cb.onChanged(s);
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&qf);
        lastChangeMs = static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart);
        maxChangeMs = std::max(maxChangeMs, lastChangeMs);
    }

    HWND parentOf(int page) const { return page >= 0 ? pages[page] : page == -2 ? header : hwnd; }

    HWND make(int page, const wchar_t* cls, DWORD style, DWORD exStyle, int id, int x, int y, int w, int h, int strId = -1,
              FontKind fontKind = FontKind::Normal) {
        const wchar_t* text = strId >= 0 ? tr(static_cast<StrId>(strId)) : L"";
        HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parentOf(page),
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        Ctl e;
        e.hwnd = c;
        e.id = id;
        e.page = page;
        e.x = x;
        e.y = y;
        e.w = w;
        e.h = h;
        e.strId = strId;
        e.font = fontKind;
        ctls.push_back(e);
        return c;
    }

    Surface surfaceFor(int page) const { return page >= 0 ? Surface::Page : page == -2 ? Surface::HeaderChip : Surface::Window; }

    HWND label(int page, StrId text, int x, int y, int w, int h = 24, int id = -1, TextRole role = TextRole::Normal) {
        HWND c = make(page, L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, 0, id, x, y, w, h, static_cast<int>(text));
        skin.setTextRole(c, role);
        return c;
    }
    HWND value(int page, int id, int x, int y, int w, int h = 24, TextRole role = TextRole::Value) {
        HWND c = make(page, L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, 0, id, x, y, w, h);
        skin.setTextRole(c, role);
        return c;
    }
    HWND textBlock(int page, StrId text, int x, int y, int w, int h, TextRole role = TextRole::Normal) {
        HWND c = make(page, L"STATIC", SS_LEFT | SS_NOPREFIX | WS_GROUP, 0, -1, x, y, w, h, static_cast<int>(text));
        skin.setTextRole(c, role);
        return c;
    }
    HWND heading(int page, StrId text, int x, int y, int w, int h = 24, int id = -1) {
        HWND c = make(page, L"STATIC", SS_OWNERDRAW | SS_NOPREFIX | WS_GROUP, 0, id, x, y, w, h, static_cast<int>(text),
                      FontKind::Heading);
        Style st;
        st.role = Role::Heading;
        st.surface = surfaceFor(page);
        skin.set(c, st);
        setTextAllowance(c, 4);
        return c;
    }
    HWND toggle(int page, int id, StrId text, int x, int y, int w) {
        HWND c = make(page, L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 0, id, x, y, w, 24, static_cast<int>(text));
        Style st;
        st.role = Role::Toggle;
        st.surface = surfaceFor(page);
        skin.set(c, st);
        setTextAllowance(c, 50);
        return c;
    }
    void segments(int page, int firstId, int count, const StrId* texts, int x, int y, const int* widths, int h = 30) {
        int cx = x;
        for (int i = 0; i < count; ++i) {
            HWND c = make(page, L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), 0, firstId + i, cx, y,
                          widths[i], h, static_cast<int>(texts[i]));
            Style st;
            st.role = Role::Segment;
            st.surface = surfaceFor(page);
            st.segIndex = i;
            st.segCount = count;
            skin.set(c, st);
            setTextAllowance(c, 14);
            cx += widths[i];
        }
    }
    HWND button(int page, int id, StrId text, int x, int y, int w, int h = 30, bool primary = false) {
        HWND c = make(page, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, id, x, y, w, h, static_cast<int>(text));
        Style st;
        st.role = primary ? Role::PrimaryButton : Role::Button;
        st.surface = surfaceFor(page);
        skin.set(c, st);
        setTextAllowance(c, 18);
        return c;
    }
    void sliderRow(int page, int y, StrId text, int id, int labelX = 16, int valueW = 78) {
        const SliderField* f = nullptr;
        for (const SliderField& sf : kSliderFields)
            if (sf.id == id) f = &sf;
        label(page, text, labelX, y, 190 - labelX, 28);
        const int extra = valueW - 78;
        HWND tb = make(page, TRACKBAR_CLASSW, TBS_HORZ | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP | WS_GROUP, 0, id, 190, y, 268 - extra, 28);
        Style st;
        st.role = Role::Slider;
        st.surface = surfaceFor(page);
        skin.set(tb, st);
        if (f) {
            SendMessageW(tb, TBM_SETRANGEMIN, FALSE, f->lo);
            SendMessageW(tb, TBM_SETRANGEMAX, FALSE, f->hi);
            SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
            SendMessageW(tb, TBM_SETPAGESIZE, 0, std::max(1, (f->hi - f->lo) / 10));
            value(page, f->valueId, 466 - extra, y, valueW, 28);
        }
    }
    // Rounded input frame + borderless EDIT inside it.
    HWND fieldEdit(int page, int id, int x, int y, int w, int h, DWORD extraStyle = 0, bool hotkey = false) {
        HWND e = nullptr;
        const int ey = y + (h - kEditH) / 2;
        if (hotkey) {
            e = createHotkeyEdit(instance, pages[page], id);
            Ctl c;
            c.hwnd = e;
            c.id = id;
            c.page = page;
            c.x = x + 9;
            c.y = ey;
            c.w = w - 18;
            c.h = kEditH;
            ctls.push_back(c);
        } else {
            e = make(page, L"EDIT", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP | extraStyle, 0, id, x + 9, ey, w - 18, kEditH);
        }
        fields.push_back({page, x, y, w, h, e});
        return e;
    }
    HWND fieldList(int page, int id, int x, int y, int w, int h, DWORD style) {
        HWND l = make(page, L"LISTBOX", style | WS_TABSTOP | WS_GROUP, 0, id, x + 4, y + 4, w - 8, h - 8);
        fields.push_back({page, x, y, w, h, l});
        return l;
    }
    HWND deco(int page, int id, int x, int y, int w, int h) {
        HWND c = make(page, kDecoClass, WS_CLIPCHILDREN, WS_EX_CONTROLPARENT, id, x, y, w, h);
        SetWindowLongPtrW(c, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        return c;
    }

    // ---- construction --------------------------------------------------------------------------
    void buildSnowPage() {
        const int p = 0;
        for (int i = 0; i < 4; ++i) {
            HWND c = make(p, L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), 0, IDC_PRESET_LIGHT + i,
                          16 + i * 134, 10, 126, 112, static_cast<int>(kSceneTitles[i]));
            Style st;
            st.role = Role::SceneCard;
            st.scene = &scenes[i];
            st.subtitle = tr(kSceneSubs[i]);
            skin.set(c, st);
            setTextAllowance(c, 22);
            scenes[i].ensure(static_cast<WeatherPreset>(i), 1234u + static_cast<unsigned>(i) * 77u);
        }
        heading(p, StrId::GroupUserPresets, 16, 134, 230, 28, IDC_UP_HEADING);
        button(p, IDC_UP_SAVE, StrId::BtnSavePreset, 324, 133, 220, 30);
        fieldEdit(p, IDC_UP_NAME, 246, 133, 214, 30);
        SendMessageW(ctl(IDC_UP_NAME), EM_SETLIMITTEXT, kMaxPresetNameChars, 0);
        SendMessageW(ctl(IDC_UP_NAME), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(StrId::PresetNameCue)));
        button(p, IDC_UP_OK, StrId::BtnOk, 466, 133, 78, 30, true);
        HWND chips = make(p, L"LISTBOX", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_MULTICOLUMN | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                            WS_HSCROLL | WS_TABSTOP | WS_GROUP,
                          0, IDC_UP_LIST, 12, 168, 536, 52);
        Style cs;
        cs.role = Role::Chips;
        cs.glyph = 1;
        skin.set(chips, cs);
        SetWindowSubclass(chips, chipsProc, kChipsSubclassId, reinterpret_cast<DWORD_PTR>(this));
        value(p, IDC_UP_MSG, 16, 220, 528, 20, TextRole::Secondary);
        int y = 242;
        sliderRow(p, y, StrId::LblIntensity, IDC_INTENSITY);
        sliderRow(p, y += 29, StrId::LblWind, IDC_WIND);
        sliderRow(p, y += 29, StrId::LblGustiness, IDC_GUSTINESS);
        sliderRow(p, y += 29, StrId::LblFlakeSize, IDC_FLAKE_SIZE);
        sliderRow(p, y += 29, StrId::LblFallSpeed, IDC_FALLSPEED);
        sliderRow(p, y += 29, StrId::LblOpacity, IDC_OPACITY);
        toggle(p, IDC_SKY_PER_MONITOR, StrId::ChkSkyPerMonitor, 16, 418, 528);
        label(p, StrId::SkyPerMonitorHint, 66, 440, 478, 18, IDC_SKY_PER_MONITOR_HINT, TextRole::Hint);
    }

    void buildDriftsPage() {
        const int p = 1;
        toggle(p, IDC_ACC_ENABLED, StrId::ChkAccumulation, 16, 13, 330);
        button(p, IDC_CLEAR_ALL, StrId::BtnClearAll, 384, 10, 160, 30);
        heading(p, StrId::GroupWhere, 16, 50, 256);
        heading(p, StrId::GroupShake, 290, 50, 254);
        toggle(p, IDC_ACC_WINDOWS, StrId::ChkOnWindows, 16, 76, 262);
        toggle(p, IDC_ACC_TASKBAR, StrId::ChkOnTaskbar, 16, 100, 262);
        toggle(p, IDC_ACC_BOTTOM, StrId::ChkOnScreenBottom, 16, 124, 262);
        const StrId shake[2] = {StrId::RadioShakeKeep, StrId::RadioShakeDrop};
        const int sw[2] = {127, 127};
        segments(p, IDC_SHAKE_KEEP, 2, shake, 290, 75, sw);
        toggle(p, IDC_SHAKE_TO_CLEAR, StrId::ChkShakeToClear, 290, 112, 254);
        sliderRow(p, 154, StrId::LblMaxHeight, IDC_MAXHEIGHT);
        sliderRow(p, 184, StrId::LblDriftOpacity, IDC_DRIFT_OPACITY);
        heading(p, StrId::GroupDriftLook, 16, 216, 528);
        toggle(p, IDC_SKIP_MAXIMIZED, StrId::ChkSkipMaximized, 16, 240, 528);
        toggle(p, IDC_DROP_ON_CLOSE, StrId::ChkDropOnClose, 16, 264, 320);
        toggle(p, IDC_NATURAL_CAPS, StrId::ChkNaturalCaps, 360, 264, 184);
        toggle(p, IDC_SNOWPLOW, StrId::ChkSnowplow, 16, 288, 320);
        toggle(p, IDC_ICICLES, StrId::ChkIcicles, 360, 288, 184);
        sliderRow(p, 312, StrId::LblSnowplowDelay, IDC_SNOWPLOW_DELAY, 40);
        toggle(p, IDC_SNOWPLOW_ALL, StrId::ChkSnowplowAllTaskbars, 40, 346, 226);
        textBlock(p, StrId::SnowplowAllTaskbarsHint, 272, 341, 272, 34, TextRole::Hint);
        ctls.back().id = IDC_SNOWPLOW_ALL_HINT;
        SetWindowLongPtrW(ctls.back().hwnd, GWLP_ID, IDC_SNOWPLOW_ALL_HINT);
        toggle(p, IDC_FALLING_SNOWMEN, StrId::ChkFallingSnowmen, 16, 378, 528);
        sliderRow(p, 402, StrId::LblSnowmenInterval, IDC_SNOWMEN_INTERVAL, 40, 112);
        toggle(p, IDC_SNOWMEN_HEAVY, StrId::ChkSnowmenHeavy, 40, 430, 504);
        sliderRow(p, 454, StrId::LblSnowmenMax, IDC_SNOWMEN_MAX, 40, 112);
    }

    void buildToolsPage() {
        const int p = 2;
        toggle(p, IDC_INTERACTION, StrId::ChkInteraction, 16, 10, 528);
        toggle(p, IDC_HOLD_KEY, StrId::ChkHoldKey, 16, 36, 528);
        toggle(p, IDC_BLOCK_CLICKS, StrId::ChkBlockClicks, 16, 62, 528);
        label(p, StrId::BlockClicksHint, 66, 85, 478, 18, IDC_BLOCK_CLICKS_HINT, TextRole::Hint);
        toggle(p, IDC_ALL_MONITORS, StrId::ChkAllMonitors, 16, 106, 528);
        toggle(p, IDC_UMBRELLA, StrId::ChkUmbrella, 16, 132, 528);
        heading(p, StrId::GroupTool, 16, 164, 528);
        const StrId tools[3] = {StrId::ToolMitten, StrId::ToolShovel, StrId::ToolSnowball};
        const int tw[3] = {130, 130, 130};
        segments(p, IDC_TOOL_MITTEN, 3, tools, 16, 190, tw);
        // Hold hint + how the selected tool works (text set in sync()).
        HWND tip = make(p, L"STATIC", SS_LEFT | SS_NOPREFIX | WS_GROUP, 0, IDC_TOOL_TIP, 16, 226, 528, 54);
        skin.setTextRole(tip, TextRole::Secondary);
        sliderRow(p, 282, StrId::LblMittenRadius, IDC_MITTEN_RADIUS);
        sliderRow(p, 312, StrId::LblShovelWidth, IDC_SHOVEL_WIDTH);
        heading(p, StrId::GroupSkin, 16, 346, 528, 24, IDC_SKIN_HEADING);
        // «Случайный» + every skin, 5 per row.
        const int cols = 5, cw = 528 / cols;
        for (int i = 0; i <= kSnowmanSkinCount; ++i) {
            const int x = 16 + (i % cols) * cw, y = 372 + (i / cols) * 28;
            HWND c = make(p, L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), 0, IDC_SKIN_FIRST + i, x, y, cw, 28,
                          i == 0 ? static_cast<int>(StrId::SkinRandom) : -1);
            if (i > 0) {
                const SnowmanSkin& sk = kSnowmanSkins[i - 1];
                SetWindowTextW(c, effectiveLanguage() == Language::Russian ? sk.nameRu : sk.nameEn);
            }
            Style st;
            st.role = Role::Choice;
            skin.set(c, st);
            setTextAllowance(c, 16);
        }
    }

    std::wstring holdHint(bool shortForm) const {
        if (interactionKeys.empty()) return tr(shortForm ? StrId::HoldHintShortNone : StrId::HoldHintNone);
        return format(tr(shortForm ? StrId::HoldHintShortFmt : StrId::HoldHintFmt), interactionKeys.c_str());
    }

    std::wstring toolTipText() const {
        const StrId tip = s.interaction.tool == Tool::Snowball ? StrId::TipSnowball
                          : s.interaction.tool == Tool::Mitten ? StrId::TipMitten
                                                               : StrId::TipShovel;
        return holdHint(false) + L".\n" + tr(tip);
    }

    void buildPerformancePage() {
        const int p = 3;
        heading(p, StrId::GroupProfile, 16, 10, 528);
        const StrId prof[3] = {StrId::ProfileLow, StrId::ProfileBalanced, StrId::ProfileHigh};
        const int pw[3] = {150, 150, 150};
        segments(p, IDC_PROFILE_LOW, 3, prof, 16, 38, pw);
        heading(p, StrId::GroupFps, 16, 80, 528);
        const StrId fps[2] = {StrId::Fps30, StrId::Fps60};
        const int fw[2] = {130, 130};
        segments(p, IDC_FPS_30, 2, fps, 16, 108, fw);
        toggle(p, IDC_PAUSE_FULLSCREEN, StrId::ChkPauseFullscreen, 16, 152, 528);
        toggle(p, IDC_REDUCE_BATTERY, StrId::ChkReduceBattery, 16, 178, 528);
        toggle(p, IDC_ANIMATIONS, StrId::ChkAnimations, 16, 204, 528);
        heading(p, StrId::GroupDiagnostics, 16, 244, 528);
        struct Row {
            StrId text;
            int id;
        };
        const Row left[] = {{StrId::DiagFps, IDC_DIAG_FPS},           {StrId::DiagFrame, IDC_DIAG_FRAME},
                            {StrId::DiagSim, IDC_DIAG_SIM},           {StrId::DiagParticles, IDC_DIAG_PARTICLES},
                            {StrId::DiagSurfaces, IDC_DIAG_SURFACES}, {StrId::DiagBalls, IDC_DIAG_BALLS}};
        const Row right[] = {{StrId::DiagMonitors, IDC_DIAG_MONITORS},
                             {StrId::DiagWindows, IDC_DIAG_WINDOWS},
                             {StrId::DiagResets, IDC_DIAG_RESETS},
                             {StrId::DiagPower, IDC_DIAG_POWER}};
        const int rowH = 22, top = 272;
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(left)); ++i) {
            label(p, left[i].text, 16, top + i * rowH, 152, rowH, -1, TextRole::Secondary);
            value(p, left[i].id, 170, top + i * rowH, 104, rowH);
        }
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(right)); ++i) {
            label(p, right[i].text, 290, top + i * rowH, 160, rowH, -1, TextRole::Secondary);
            value(p, right[i].id, 452, top + i * rowH, 92, rowH);
        }
        label(p, StrId::DiagStatus, 16, top + 6 * rowH, 152, rowH, -1, TextRole::Secondary);
        value(p, IDC_DIAG_STATUS, 170, top + 6 * rowH, 374, rowH);
    }

    void buildFestivePage() {
        const int p = 4;
        heading(p, StrId::GroupFestive, 16, 10, 528);
        toggle(p, IDC_GARLAND, StrId::ChkGarland, 16, 40, 330);
        toggle(p, IDC_FIREWORKS, StrId::ChkFireworks, 16, 66, 330);
        button(p, IDC_FIREWORKS_NOW, StrId::BtnFireworks, 384, 46, 160, 36, true);
        textBlock(p, StrId::FestiveText, 16, 100, 528, 54, TextRole::Secondary);
        deco(p, IDC_FESTIVE_SCENE, 16, 164, 528, 280);
    }

    void buildSystemPage() {
        const int p = 5;
        toggle(p, IDC_STARTUP, StrId::ChkStartup, 16, 12, 262);
        toggle(p, IDC_LEFT_CLICK, StrId::ChkLeftClick, 290, 12, 254);
        label(p, StrId::LblLanguage, 16, 48, 140, 30);
        const StrId langs[3] = {StrId::LangAuto, StrId::LangAuto, StrId::LangAuto};
        const int lw[3] = {128, 128, 128};
        segments(p, IDC_LANG_AUTO, 3, langs, 160, 48, lw);
        SetWindowTextW(ctl(IDC_LANG_RU), L"Русский");
        SetWindowTextW(ctl(IDC_LANG_EN), L"English");
        for (Ctl& c : ctls)
            if (c.id == IDC_LANG_RU || c.id == IDC_LANG_EN) c.strId = -1;
        label(p, StrId::LblTheme, 16, 86, 140, 30);
        const StrId themes[3] = {StrId::ThemeNight, StrId::ThemeGlobe, StrId::ThemePostcard};
        const int thw[3] = {116, 116, 152};
        segments(p, IDC_THEME_NIGHT, 3, themes, 160, 86, thw);

        heading(p, StrId::GroupHotkeys, 16, 128, 528);
        const StrId names[] = {StrId::LblHotkeyPause, StrId::LblHotkeyClear, StrId::LblHotkeyInteraction};
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(kHotkeyFields)); ++i) {
            const int y = 156 + i * 31;
            label(p, names[i], 16, y, 180, 28);
            fieldEdit(p, kHotkeyFields[i].id, 200, y, 168, 28, 0, true);
            value(p, kHotkeyFields[i].statusId, 380, y, 164, 28, TextRole::Normal);
        }
        textBlock(p, StrId::HotkeyHint, 16, 250, 528, 36, TextRole::Secondary);

        heading(p, StrId::GroupExcluded, 16, 290, 528);
        fieldList(p, IDC_EXCL_LIST, 16, 318, 344, 66, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS | WS_VSCROLL);
        button(p, IDC_EXCL_PICK, StrId::BtnPickRunning, 370, 318, 174, 30);
        button(p, IDC_EXCL_REMOVE, StrId::BtnRemove, 370, 354, 174, 30);
        label(p, StrId::LblExclMode, 16, 390, 196, 28, IDC_EXCL_MODE_LBL);
        const StrId modes[2] = {StrId::ExclBehind, StrId::ExclAround};
        const int mw[2] = {166, 166};
        segments(p, IDC_EXCL_BEHIND, 2, modes, 212, 389, mw);
        label(p, StrId::LblExeName, 16, 426, 96, 30);
        fieldEdit(p, IDC_EXCL_EDIT, 114, 426, 246, 30);
        SendMessageW(ctl(IDC_EXCL_EDIT), EM_SETLIMITTEXT, 120, 0);
        SendMessageW(ctl(IDC_EXCL_EDIT), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(StrId::ExeCue)));
        button(p, IDC_EXCL_ADD, StrId::BtnAdd, 370, 426, 174, 30);
    }

    void buildAboutPage() {
        const int p = 6;
        make(p, L"STATIC", SS_ICON | WS_GROUP, 0, IDC_ABOUT_ICON, 16, 14, 48, 48);
        ctls.back().keepSize = true;
        setTextAllowance(ctls.back().hwnd, -1);
        make(p, L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | WS_GROUP, 0, IDC_ABOUT_TITLE, 78, 10, 176, 34,
             static_cast<int>(StrId::AppName), FontKind::Title);
        label(p, StrId::PoweredBy, 258, 13, 140, 30, IDC_POWERED_BY, TextRole::Secondary);
        // Links open only on an explicit click / Enter (ShellExecute); the app never connects anywhere.
        struct LinkDef {
            int id;
            const wchar_t* text;
            const wchar_t* url;
            int x, w;
        };
        const LinkDef links[] = {{IDC_LINK_GITHUB, L"GitHub", L"https://github.com/DaveBugg", 398, 66},
                                 {IDC_LINK_TELEGRAM, L"Telegram", L"https://t.me/dave_cli", 466, 78}};
        for (const LinkDef& l : links) {
            HWND b = make(p, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, l.id, l.x, 13, l.w, 30);
            SetWindowTextW(b, l.text);
            Style st;
            st.role = Role::Link;
            st.url = l.url;
            skin.set(b, st);
            SetPropW(b, L"SnowUrl", reinterpret_cast<HANDLE>(const_cast<wchar_t*>(l.url)));
            setTextAllowance(b, 10);
        }
        value(p, IDC_ABOUT_VERSION, 79, 44, 465, 20, TextRole::Secondary);
        textBlock(p, StrId::AboutDescription, 16, 78, 528, 40);
        heading(p, StrId::GroupPrivacy, 16, 128, 528);
        textBlock(p, StrId::AboutPrivacy, 16, 156, 528, 40, TextRole::Secondary);
        heading(p, StrId::GroupLog, 16, 206, 528);
        label(p, StrId::LblLogFile, 16, 234, 110, 30);
        fieldEdit(p, IDC_LOG_PATH, 128, 234, 416, 30, ES_READONLY);
        button(p, IDC_OPEN_LOG_FOLDER, StrId::BtnOpenLogFolder, 16, 274, 240, 30);
        textBlock(p, StrId::AboutLicence, 16, 324, 528, 60, TextRole::Secondary);
    }

    void build() {
        skin.setSurface(hwnd, Surface::Window);
        // Header with the theme swatches.
        header = CreateWindowExW(WS_EX_CONTROLPARENT, kDecoClass, tr(StrId::AppName), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HEADER)), instance, nullptr);
        SetWindowLongPtrW(header, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        skin.setSurface(header, Surface::HeaderChip);
        for (int i = 0; i < 3; ++i) {
            hdrTools[i] = make(-2, L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), 0, IDC_HDR_TOOL_SNOWBALL + i,
                               0, 0, 0, 0, static_cast<int>(kHdrToolNames[i]));
            Style st;
            st.role = Role::ToolIcon;
            st.surface = Surface::HeaderChip;
            st.glyph = static_cast<int>(kHdrTools[i]);
            skin.set(hdrTools[i], st);
            setTextAllowance(hdrTools[i], -1);
        }
        for (int i = 0; i < 3; ++i) {
            swatches[i] = make(-2, L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), 0, IDC_SWATCH_NIGHT + i, 0,
                               0, 0, 0, static_cast<int>(kThemeNames[i]));
            Style st;
            st.role = Role::Swatch;
            st.surface = Surface::HeaderChip;
            st.swatch = static_cast<UiTheme>(i);
            skin.set(swatches[i], st);
            setTextAllowance(swatches[i], -1);
        }
        // Navigation.
        for (int i = 0; i < kTabCount; ++i) {
            nav[i] = make(-1, L"BUTTON", BS_AUTORADIOBUTTON | (i == 0 ? (WS_TABSTOP | WS_GROUP) : 0), 0, IDC_NAV_FIRST + i, 0, 0, 0,
                          0, static_cast<int>(kTabTitles[i]));
            Style st;
            st.role = Role::Nav;
            st.surface = Surface::Nav;
            st.glyph = i;
            skin.set(nav[i], st);
        }
        // Pages.
        for (int i = 0; i < kTabCount; ++i) {
            pages[i] = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClass, L"", WS_CHILD | WS_GROUP | WS_CLIPCHILDREN | (i == 0 ? WS_VISIBLE : 0), 0,
                                       0, 10, 10, hwnd, nullptr, instance, this);
            skin.setSurface(pages[i], Surface::Page);
        }
        buildSnowPage();
        buildDriftsPage();
        buildToolsPage();
        buildPerformancePage();
        buildFestivePage();
        buildSystemPage();
        buildAboutPage();

        // Right column + bottom bar (main window children).
        preview = make(-1, kDecoClass, 0, 0, IDC_PREVIEW, 0, 0, 0, 0, static_cast<int>(StrId::PreviewTitle));
        SetWindowLongPtrW(preview, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        previewReset = button(-1, IDC_PREVIEW_RESET, StrId::BtnResetPreview, 0, 0, 0, 0);
        countdownWnd = make(-1, kDecoClass, 0, 0, IDC_COUNTDOWN, 0, 0, 0, 0);
        SetWindowLongPtrW(countdownWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        statusLine = make(-1, L"STATIC", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS | WS_GROUP, 0, IDC_STATUS_LINE, 0, 0, 0, 0);
        skin.setTextRole(statusLine, TextRole::Secondary);
        setTextAllowance(statusLine, -1);
        restoreBtn = button(-1, IDC_RESTORE, StrId::BtnRestoreDefaults, 0, 0, 0, 0);
        closeBtn = button(-1, IDC_CLOSE, StrId::BtnClose, 0, 0, 0, 0, true);
        for (HWND b : {previewReset, restoreBtn, closeBtn}) {
            Style st = *skin.find(b);
            st.surface = Surface::Window;
            skin.set(b, st);
        }

        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd,
                                  nullptr, instance, nullptr);
        for (int i = 0; i < 6; ++i) {
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = header;
            ti.uId = reinterpret_cast<UINT_PTR>(i < 3 ? swatches[i] : hdrTools[i - 3]);
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        built = true;
    }

    // ---- fonts, icons, layout ----------------------------------------------------------------------
    void destroyGdi() {
        if (font) DeleteObject(font);
        if (fontHeading) DeleteObject(fontHeading);
        if (fontTitle) DeleteObject(fontTitle);
        font = fontHeading = fontTitle = nullptr;
        if (iconBig) DestroyIcon(iconBig);
        if (iconSmall) DestroyIcon(iconSmall);
        if (iconAbout) DestroyIcon(iconAbout);
        iconBig = iconSmall = iconAbout = nullptr;
    }

    void createGdi() {
        HFONT oldFont = font, oldHeading = fontHeading, oldTitle = fontTitle;
        HICON oldBig = iconBig, oldSmall = iconSmall, oldAbout = iconAbout;
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
            ncm.lfMessageFont = {};
            ncm.lfMessageFont.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
            wcscpy_s(ncm.lfMessageFont.lfFaceName, L"Segoe UI");
        }
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        const float bodyDip = static_cast<float>(std::abs(ncm.lfMessageFont.lfHeight)) * 96.0f / static_cast<float>(dpi);
        Painter::instance().setBodyFont(ncm.lfMessageFont.lfFaceName, bodyDip);
        LOGFONTW head = ncm.lfMessageFont;
        head.lfHeight = -static_cast<LONG>(static_cast<float>(std::abs(ncm.lfMessageFont.lfHeight)) * 1.1f + 0.5f);
        head.lfWeight = FW_SEMIBOLD;
        wcsncpy_s(head.lfFaceName, Painter::instance().displayFamily().c_str(), _TRUNCATE);
        fontHeading = CreateFontIndirectW(&head);
        LOGFONTW title = head;
        title.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 19, 10);
        fontTitle = CreateFontIndirectW(&title);

        const UINT realDpi = GetDpiForWindow(hwnd);
        iconBig = TrayIcon::createAppIcon(GetSystemMetricsForDpi(SM_CXICON, realDpi));
        iconSmall = TrayIcon::createAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, realDpi));
        iconAbout = TrayIcon::createAppIcon(px(48));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
        for (const Ctl& c : ctls) {
            if (c.id == IDC_ABOUT_ICON) {
                SendMessageW(c.hwnd, STM_SETICON, reinterpret_cast<WPARAM>(iconAbout), 0);
                continue;
            }
            HFONT f = c.font == FontKind::Title ? fontTitle : c.font == FontKind::Heading ? fontHeading : font;
            SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        }
        if (tooltip) SendMessageW(tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        if (oldFont) DeleteObject(oldFont);
        if (oldHeading) DeleteObject(oldHeading);
        if (oldTitle) DeleteObject(oldTitle);
        if (oldBig) DestroyIcon(oldBig);
        if (oldSmall) DestroyIcon(oldSmall);
        if (oldAbout) DestroyIcon(oldAbout);
    }

    void place(HWND h, int x, int y, int w, int hh) { SetWindowPos(h, nullptr, px(x), px(y), px(w), px(hh), SWP_NOZORDER | SWP_NOACTIVATE); }

    // Positions everything for the current theme/dpi; keeps the window's top-left unless `centre`.
    void layout(const POINT* anchor, bool centre) {
        const Palette& P = pal();
        int pageX = 0, pageY = 0;
        int clientW = 0;
        if (!P.pillTabs) {
            pageX = kNavW + 16;
            pageY = kHeaderH + 14;
            for (int i = 0; i < kTabCount; ++i) place(nav[i], 10, kHeaderH + 14 + i * 44, kNavW - 20, 40);
        } else {
            pageX = 12;
            const int pillsY = kHeaderH + 6;
            pageY = pillsY + kPillH + 6;
            TextStyle ts{Painter::instance().bodySize(), DWRITE_FONT_WEIGHT_SEMI_BOLD};
            int x = 12;
            for (int i = 0; i < kTabCount; ++i) {
                const int w = static_cast<int>(Painter::instance().measure(tr(kTabTitles[i]), ts) + 0.5f) + 30;
                place(nav[i], x, pillsY, w, kPillH);
                x += w + 4;
            }
        }
        for (int i = 0; i < kTabCount; ++i) {
            Style st = *skin.find(nav[i]);
            st.pill = P.pillTabs;
            st.surface = P.pillTabs ? Surface::Window : Surface::Nav;
            skin.set(nav[i], st);
            setTextAllowance(nav[i], P.pillTabs ? 8 : 50);
        }
        const int rightX = pageX + kPageW + 16;
        clientW = rightX + kRightW + 14;
        place(header, 0, 0, clientW, kHeaderH);
        const int chipW = 3 * 34 + 14, chipX = clientW - 16 - chipW;
        chipRect = D2D1::RectF(static_cast<float>(chipX), 11, static_cast<float>(chipX + chipW), 53);
        for (int i = 0; i < 3; ++i) place(swatches[i], chipX + 7 + i * 34, 15, 34, 34);
        {
            TextStyle cs{Painter::instance().bodySize() * 0.92f};
            const int capTheme = static_cast<int>(Painter::instance().measure(tr(StrId::CaptionTheme), cs) + 0.5f);
            const int toolX = chipX - 8 - capTheme - 22 - chipW;
            toolChipRect = D2D1::RectF(static_cast<float>(toolX), 11, static_cast<float>(toolX + chipW), 53);
            for (int i = 0; i < 3; ++i) place(hdrTools[i], toolX + 7 + i * 34, 15, 34, 34);
        }
        for (HWND page : pages) place(page, pageX, pageY, kPageW, kPageH);
        for (const Ctl& c : ctls) {
            if (c.page < 0) continue;
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            if (c.keepSize) flags |= SWP_NOSIZE;
            SetWindowPos(c.hwnd, nullptr, px(c.x), px(c.y), px(c.w), px(c.h), flags);
        }
        place(preview, rightX, pageY, kRightW, 214);
        place(previewReset, rightX, pageY + 222, kRightW, 32);
        place(countdownWnd, rightX, pageY + 268, kRightW, kPageH - 268);
        const int bottomY = pageY + kPageH + 8;
        place(closeBtn, clientW - 14 - 120, bottomY, 120, kBottomH);
        place(restoreBtn, clientW - 14 - 120 - 10 - 290, bottomY, 290, kBottomH);
        place(statusLine, pageX + 2, bottomY, clientW - 14 - 120 - 10 - 290 - 12 - pageX, kBottomH);
        HWND chips = ctl(IDC_UP_LIST);
        SendMessageW(chips, LB_SETITEMHEIGHT, 0, px(34));
        SendMessageW(chips, LB_SETCOLUMNWIDTH, px(152), 0);

        RECT wr{0, 0, px(clientW), px(bottomY + kBottomH + 10)};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, GetDpiForWindow(hwnd));
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        RECT cur{};
        GetWindowRect(hwnd, &cur);
        int x = anchor ? anchor->x : cur.left;
        int y = anchor ? anchor->y : cur.top;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (centre) {
                x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
                y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
            }
            x = std::max<int>(std::min<int>(x, mi.rcWork.right - w), mi.rcWork.left);
            y = std::max<int>(std::min<int>(y, mi.rcWork.bottom - h), mi.rcWork.top);
        }
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    UINT layoutDpi() const { return g_dpiOverride ? g_dpiOverride : GetDpiForWindow(hwnd); }

    void applyThemeVisuals() {
        const Palette& P = pal();
        skin.pal = &P;
        skin.dpi = dpi;
        BOOL dark = P.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const COLORREF caption = P.dark ? P.headerTop : P.headerTop;
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        const COLORREF captionText = P.headerText;
        DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &captionText, sizeof(captionText));
        for (int id : {IDC_EXCL_LIST, IDC_UP_LIST}) {
            HWND l = ctl(id);
            if (l) SetWindowTheme(l, P.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
    }

    // ---- model -> controls ---------------------------------------------------------------------------
    std::wstring valueText(ValueKind kind, int v) const {
        switch (kind) {
            case ValueKind::Percent: return format(L"%d %%", v);
            case ValueKind::Px: return format(L"%d px", v);
            case ValueKind::Minutes: return format(tr(StrId::UnitMinFmt), v);
            case ValueKind::EveryMinutes:
                if (v == 1) return tr(StrId::SnowmenEveryMinute);
                return format(tr(pluralRu(v, StrId::SnowmenEvery1Fmt, StrId::SnowmenEvery2Fmt, StrId::SnowmenEvery5Fmt)), v);
            case ValueKind::Snowmen:
                return format(tr(pluralRu(v, StrId::Snowmen1Fmt, StrId::Snowmen2Fmt, StrId::Snowmen5Fmt)), v);
            case ValueKind::Wind:
                if (v < 0) return format(L"← %d", -v);
                if (v > 0) return format(L"%d →", v);
                return tr(StrId::WindCalm);
        }
        return {};
    }

    void refreshHotkeyStatus(int index) {
        const HotkeyField& f = kHotkeyFields[index];
        const std::string& text = s.hotkeys.*(f.member);
        std::wstring status;
        bool warn = false;
        if (text.empty()) {
            status = tr(StrId::HotkeyDisabled);
        } else if (cb.validateHotkey) {
            status = cb.validateHotkey(text);
            warn = !status.empty();
            if (!warn) status = tr(StrId::HotkeyOk);
        } else {
            status = tr(StrId::HotkeyOk);
        }
        HWND statusCtl = ctl(f.statusId);
        skin.setTextRole(statusCtl, text.empty() ? TextRole::Secondary : warn ? TextRole::Error : TextRole::Ok);
        hotkeyWarn[index] = warn;
        setTextIfChanged(statusCtl, status);
        if (statusCtl) InvalidateRect(statusCtl, nullptr, TRUE);
    }

    void fillExclusions() {
        HWND list = ctl(IDC_EXCL_LIST);
        if (!list) return;
        const int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const std::string& name : s.system.excludedProcesses) {
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(name).c_str()));
        }
        const int count = static_cast<int>(s.system.excludedProcesses.size());
        if (sel >= 0 && count > 0) SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(std::min(sel, count - 1)), 0);
    }

    void fillChips() {
        HWND list = ctl(IDC_UP_LIST);
        if (!list) return;
        const int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const UserPreset& p : s.userPresets) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(p.name).c_str()));
        const int count = static_cast<int>(s.userPresets.size());
        if (sel >= 0 && count > 0) SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(std::min(sel, count - 1)), 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }

    void setUpMessage(const std::wstring& text, TextRole role) {
        HWND m = ctl(IDC_UP_MSG);
        upMsgRole = role;
        skin.setTextRole(m, role);
        setTextIfChanged(m, text);
        InvalidateRect(m, nullptr, TRUE);
    }

    void showUpEditor(bool on) {
        upEditing = on;
        ShowWindow(ctl(IDC_UP_SAVE), on ? SW_HIDE : SW_SHOW);
        ShowWindow(ctl(IDC_UP_NAME), on ? SW_SHOW : SW_HIDE);
        ShowWindow(ctl(IDC_UP_OK), on ? SW_SHOW : SW_HIDE);
        InvalidateRect(pages[0], nullptr, TRUE);
        if (on) {
            SetWindowTextW(ctl(IDC_UP_NAME), L"");
            SetFocus(ctl(IDC_UP_NAME));
        }
    }

    void enable(int id, bool on) {
        HWND c = ctl(id);
        if (!c || (IsWindowEnabled(c) != FALSE) == on) return;
        if (!on && GetFocus() == c) SetFocus(nav[curTab]);
        EnableWindow(c, on);
    }

    void updateEnableStates() {
        const bool acc = s.accumulation.enabled;
        for (int id : {IDC_ACC_WINDOWS, IDC_ACC_TASKBAR, IDC_ACC_BOTTOM, IDC_MAXHEIGHT, IDC_MAXHEIGHT_VAL, IDC_SHAKE_KEEP, IDC_SHAKE_DROP,
                       IDC_SKIP_MAXIMIZED, IDC_DROP_ON_CLOSE, IDC_DRIFT_OPACITY, IDC_DRIFT_OPACITY_VAL, IDC_SHAKE_TO_CLEAR,
                       IDC_NATURAL_CAPS, IDC_ICICLES, IDC_FALLING_SNOWMEN, IDC_SNOWPLOW})
            enable(id, acc);
        const bool plow = acc && s.accumulation.snowplow;
        for (int id : {IDC_SNOWPLOW_DELAY, IDC_SNOWPLOW_DELAY_VAL, IDC_SNOWPLOW_ALL, IDC_SNOWPLOW_ALL_HINT}) enable(id, plow);
        const bool snowmen = acc && s.accumulation.fallingSnowmen;
        for (int id : {IDC_SNOWMEN_INTERVAL, IDC_SNOWMEN_INTERVAL_VAL, IDC_SNOWMEN_HEAVY, IDC_SNOWMEN_MAX, IDC_SNOWMEN_MAX_VAL}) enable(id, snowmen);
        const bool inter = s.interaction.enabled;
        for (int id : {IDC_HOLD_KEY, IDC_BLOCK_CLICKS, IDC_BLOCK_CLICKS_HINT, IDC_ALL_MONITORS, IDC_TOOL_MITTEN, IDC_TOOL_SHOVEL,
                       IDC_TOOL_SNOWBALL, IDC_MITTEN_RADIUS, IDC_MITTEN_RADIUS_VAL, IDC_SHOVEL_WIDTH, IDC_SHOVEL_WIDTH_VAL, IDC_TOOL_TIP})
            enable(id, inter);
        for (int i = 0; i <= kSnowmanSkinCount; ++i) enable(IDC_SKIN_FIRST + i, inter);
        HWND list = ctl(IDC_EXCL_LIST);
        enable(IDC_EXCL_REMOVE, list && SendMessageW(list, LB_GETCURSEL, 0, 0) != LB_ERR);
    }

    void sync() {
        if (!built) return;
        ++updating;
        for (const BoolField& f : kBoolFields) {
            HWND c = ctl(f.id);
            const WPARAM want = f.ref(s) ? BST_CHECKED : BST_UNCHECKED;
            if (static_cast<WPARAM>(SendMessageW(c, BM_GETCHECK, 0, 0)) != want) SendMessageW(c, BM_SETCHECK, want, 0);
        }
        for (const RadioGroup& g : kRadioGroups) {
            const int sel = std::clamp(g.get(s), 0, g.count - 1);
            for (int i = 0; i < g.count; ++i) {
                HWND c = ctl(g.firstId + i);
                const WPARAM want = i == sel ? BST_CHECKED : BST_UNCHECKED;
                if (static_cast<WPARAM>(SendMessageW(c, BM_GETCHECK, 0, 0)) != want) SendMessageW(c, BM_SETCHECK, want, 0);
            }
        }
        for (int i = 0; i < kTabCount; ++i) SendMessageW(nav[i], BM_SETCHECK, i == curTab ? BST_CHECKED : BST_UNCHECKED, 0);
        for (const SliderField& f : kSliderFields) {
            const int v = std::clamp(f.ref(s), f.lo, f.hi);
            HWND tb = ctl(f.id);
            if (static_cast<int>(SendMessageW(tb, TBM_GETPOS, 0, 0)) != v) SendMessageW(tb, TBM_SETPOS, TRUE, v);
            setTextIfChanged(ctl(f.valueId), valueText(f.kind, v));
        }
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(kHotkeyFields)); ++i) {
            HWND edit = ctl(kHotkeyFields[i].id);
            const std::string& text = s.hotkeys.*(kHotkeyFields[i].member);
            if (getHotkeyEditText(edit) != text) setHotkeyEditText(edit, text);
            refreshHotkeyStatus(i);
        }
        fillExclusions();
        fillChips();
        setTextIfChanged(ctl(IDC_TOOL_TIP), toolTipText());
        updateEnableStates();
        --updating;
    }

    void relabel() {
        if (!built) return;
        ++updating;
        SetWindowTextW(hwnd, tr(StrId::SettingsTitle));
        for (const Ctl& c : ctls) {
            if (c.strId >= 0) SetWindowTextW(c.hwnd, tr(static_cast<StrId>(c.strId)));
        }
        for (int i = 0; i < 4; ++i) {
            HWND card = ctl(IDC_PRESET_LIGHT + i);
            Style st = *skin.find(card);
            st.subtitle = tr(kSceneSubs[i]);
            skin.set(card, st);
        }
        for (int i = 0; i < kSnowmanSkinCount; ++i) {
            SetWindowTextW(ctl(IDC_SKIN_FIRST + 1 + i), effectiveLanguage() == Language::Russian ? kSnowmanSkins[i].nameRu : kSnowmanSkins[i].nameEn);
        }
        SendMessageW(ctl(IDC_UP_NAME), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(StrId::PresetNameCue)));
        SendMessageW(ctl(IDC_EXCL_EDIT), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(StrId::ExeCue)));
        --updating;
        layout(nullptr, false);
        sync();
        setUpMessage(s.userPresets.empty() ? L"" : tr(StrId::PresetsHint), TextRole::Secondary);
        refreshDiagnostics();
    }

    void applyLanguage() {
        const Language before = effectiveLanguage();
        setLanguage(s.system.language);
        if (built && effectiveLanguage() != before) relabel();
    }

    UiTheme shownTheme = UiTheme::Night;
    void applyTheme(bool force = false) {
        if (!built) return;
        if (!force && shownTheme == s.system.uiTheme) return;
        shownTheme = s.system.uiTheme;
        applyThemeVisuals();
        layout(nullptr, false);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    void refreshDiagnostics() {
        if (!built) return;
        const bool have = static_cast<bool>(cb.getDiagnostics);
        const DiagnosticsInfo d = have ? cb.getDiagnostics() : DiagnosticsInfo{};
        std::wstring status = d.statusText;
        if (status.empty()) status = have ? tr(d.paused || d.autoPaused ? StrId::StatusPaused : StrId::StatusRunning) : L"—";
        setTextIfChanged(statusLine, format(tr(StrId::StatusLabelFmt), status.c_str()));
        setTextIfChanged(ctl(IDC_DIAG_FPS), format(L"%.1f", static_cast<double>(d.fps)));
        setTextIfChanged(ctl(IDC_DIAG_FRAME), format(L"%.2f %ls", static_cast<double>(d.frameMs), tr(StrId::UnitMs)));
        setTextIfChanged(ctl(IDC_DIAG_SIM), format(L"%.2f %ls", static_cast<double>(d.simStepMs), tr(StrId::UnitMs)));
        setTextIfChanged(ctl(IDC_DIAG_PARTICLES), format(L"%u", d.particles));
        setTextIfChanged(ctl(IDC_DIAG_SURFACES), format(L"%u", d.surfaces));
        setTextIfChanged(ctl(IDC_DIAG_BALLS), format(L"%u / %u", d.snowballs, d.snowmen));
        setTextIfChanged(ctl(IDC_DIAG_MONITORS), format(L"%d", d.monitors));
        setTextIfChanged(ctl(IDC_DIAG_WINDOWS), format(L"%d", d.trackedWindows));
        setTextIfChanged(ctl(IDC_DIAG_RESETS), format(L"%d", d.deviceResets));
        setTextIfChanged(ctl(IDC_DIAG_POWER), tr(d.onBattery ? StrId::PowerBattery : StrId::PowerMains));
        setTextIfChanged(ctl(IDC_DIAG_STATUS), status);
        if (d.interactionKeys != interactionKeys) {
            interactionKeys = d.interactionKeys;
            setTextIfChanged(ctl(IDC_TOOL_TIP), toolTipText());
            paintDecoNow(header);
        }
        setTextIfChanged(header, holdHint(false));   // accessible text of the header
        setTextIfChanged(ctl(IDC_ABOUT_VERSION), format(tr(StrId::AboutVersionFmt), widen(SNOW_VERSION).c_str()));
        setTextIfChanged(ctl(IDC_LOG_PATH), d.logPath.empty() ? std::wstring(L"—") : d.logPath);
        HWND open = ctl(IDC_OPEN_LOG_FOLDER);
        if (open && (IsWindowEnabled(open) != FALSE) != !d.logPath.empty()) EnableWindow(open, !d.logPath.empty());
        // Countdown (accessible text = what is drawn).
        const Countdown c = computeCountdown(countdownNow());
        const std::wstring key = c.big + L" " + c.caption;
        if (key != countdownKey) {
            countdownKey = key;
            SetWindowTextW(countdownWnd, key.c_str());
            InvalidateRect(countdownWnd, nullptr, FALSE);
        }
    }

    // ---- painting ------------------------------------------------------------------------------------
    void paintPage(HWND page, HDC dc) {
        int index = -1;
        for (int i = 0; i < kTabCount; ++i)
            if (pages[i] == page) index = i;
        RECT rc{};
        GetClientRect(page, &rc);
        // Only the invalid part: themed child controls ask their parent to repaint the area behind
        // them (DrawThemeParentBackground) on every animation frame.
        RECT clip{};
        if (GetClipBox(dc, &clip) == NULLREGION || !IntersectRect(&clip, &clip, &rc)) return;
        // Skinned controls paint their whole rectangle opaquely: nothing to do behind them.
        for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!skin.find(child) || !IsWindowVisible(child)) continue;
            RECT cr{};
            GetWindowRect(child, &cr);
            MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&cr), 2);
            if (clip.left >= cr.left && clip.top >= cr.top && clip.right <= cr.right && clip.bottom <= cr.bottom) return;
        }
        const Palette& P = pal();
        HWND focus = GetFocus();
        Painter& pt = Painter::instance();
        const D2D1_SIZE_F full = D2D1::SizeF(static_cast<float>(rc.right) * k(), static_cast<float>(rc.bottom) * k());
        pt.paint(dc, clip, dpi, [&](ID2D1RenderTarget* rt, D2D1_SIZE_F) {
            rt->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(clip.left) * k(), -static_cast<float>(clip.top) * k()));
            const D2D1_SIZE_F size = full;
            if (P.pageIsCard) {
                pt.fill(D2D1::RectF(0, 0, size.width, size.height), col(P.bg));
                const D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
                pt.fillRound(card, 12, col(P.page));
                pt.strokeRound(card, 12, col(P.pageBorder), 1.0f);
            } else {
                pt.fill(D2D1::RectF(0, 0, size.width, size.height), col(P.page));
            }
            for (const Field& f : fields) {
                if (f.page != index || !IsWindowVisible(f.inner)) continue;
                const D2D1_RECT_F r = D2D1::RectF(static_cast<float>(f.x), static_cast<float>(f.y), static_cast<float>(f.x + f.w),
                                                  static_cast<float>(f.y + f.h));
                const bool on = focus == f.inner;
                const bool enabled = IsWindowEnabled(f.inner) != FALSE;
                pt.fillRound(r, 7, col(P.field, enabled ? 1.0f : 0.6f));
                pt.strokeRound(r, 7, col(on ? P.accent : P.fieldBorder), on ? 2.0f : 1.0f);
            }
        });
    }

    void paintDeco(HWND w, HDC dc) {
        RECT rc{};
        GetClientRect(w, &rc);
        const int W = rc.right, H = rc.bottom;
        if (W <= 0 || H <= 0) return;
        const Palette& P = pal();
        Painter& pt = Painter::instance();
        Compositor& comp = Compositor::instance();
        const float sc = static_cast<float>(dpi) / 96.0f;
        const int id = GetDlgCtrlID(w);
        wchar_t key[256];
        if (id == IDC_HEADER) {
            const std::wstring sub = holdHint(false);
            headerAnim.ensure(0xC0FFEEu, 46);
            swprintf_s(key, L"hdr|%d|%ls|%.1f|%.1f|%ls", static_cast<int>(s.system.uiTheme), sub.c_str(), chipRect.left, toolChipRect.left,
                       tr(StrId::CaptionTheme));
            HeaderChip chips[2];
            chips[0].rect = toolChipRect;
            chips[0].caption = tr(StrId::GroupTool);
            chips[1].rect = chipRect;
            chips[1].caption = tr(StrId::CaptionTheme);
            headerAnim.layer.render(key, W, H, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F size) {
                drawHeaderStatic(pt, size, P, tr(StrId::AppName), sub, chips, 2);
            });
            if (comp.begin(headerAnim.layer, W, H)) {
                drawHeaderDynamic(comp, sc, P, headerAnim, static_cast<float>(H) / sc);
                comp.end(dc, 0, 0);
            }
        } else if (id == IDC_PREVIEW) {
            if (!snowPreview) return;
            const float wd = static_cast<float>(W) / sc;
            const float aw = wd - 20;
            const D2D1_RECT_F area = D2D1::RectF(10, 36, 10 + aw, 36 + aw * SnowPreview::kDesktopH / SnowPreview::kDesktopW);
            swprintf_s(key, L"pv|%d|%d", static_cast<int>(s.system.uiTheme), static_cast<int>(effectiveLanguage()));
            pvBase.render(key, W, H, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F size) {
                pt.fill(D2D1::RectF(0, 0, size.width, size.height), col(P.bg));
                const D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
                pt.fillRound(card, 10, col(P.card));
                pt.strokeRound(card, 10, col(P.cardBorder), 1.0f);
                TextStyle ts{pt.bodySize(), DWRITE_FONT_WEIGHT_SEMI_BOLD};
                ts.display = true;
                pt.text(tr(StrId::PreviewTitle), D2D1::RectF(14, 6, size.width - 90, 32), ts, col(P.heading));
                TextStyle hs{pt.bodySize() * 0.88f};
                hs.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
                pt.text(tr(StrId::PreviewHint), D2D1::RectF(size.width - 120, 6, size.width - 14, 32), hs, col(P.textSecondary));
                snowPreview->drawBackground(pt, area, P);
            });
            // Drifts and desktop objects change slowly: re-rendered at most ~7 times per second.
            const std::uint64_t dk = snowPreview->driftKey();
            const ULONGLONG now = GetTickCount64();
            const std::wstring driftLayerKey = std::wstring(L"pvd|") + key;
            if (!pvDrift.valid(driftLayerKey, W, H, dpi) || (dk != pvDriftKey && now - pvDriftTick >= 140)) {
                pvDrift.reset();
                pvDrift.renderFrom(pvBase, driftLayerKey, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F) { snowPreview->drawDrifts(pt, area); });
                pvDriftKey = dk;
                pvDriftTick = now;
            }
            if (comp.begin(pvDrift, W, H)) {
                comp.setClip(static_cast<int>(area.left * sc), static_cast<int>(area.top * sc), static_cast<int>(area.right * sc),
                             static_cast<int>(area.bottom * sc));
                snowPreview->drawParticles(comp, area.left * sc, area.top * sc, aw * sc / static_cast<float>(SnowPreview::kDesktopW));
                comp.end(dc, 0, 0);
            }
        } else if (id == IDC_COUNTDOWN) {
            const Countdown c = computeCountdown(countdownNow());
            swprintf_s(key, L"cd|%d|%ls|%ls", static_cast<int>(s.system.uiTheme), c.big.c_str(), c.caption.c_str());
            countdownLayer.render(key, W, H, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F size) {
                drawCountdownStatic(pt, size, P, tr(StrId::CountdownTitle), c.big, c.caption, c.festive);
            });
            if (comp.begin(countdownLayer, W, H)) {
                drawCountdownDynamic(comp, sc, static_cast<float>(W) / sc, static_cast<float>(H) / sc, headerAnim.time);
                comp.end(dc, 0, 0);
            }
        } else if (id == IDC_FESTIVE_SCENE) {
            swprintf_s(key, L"fest|%d", static_cast<int>(s.system.uiTheme));
            festiveLayer.render(key, W, H, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F size) { drawFestiveStatic(pt, size, P); });
            if (comp.begin(festiveLayer, W, H)) {
                drawFestiveDynamic(comp, sc, static_cast<float>(W) / sc, static_cast<float>(H) / sc, fireworks);
                comp.end(dc, 0, 0);
            }
        }
    }

    // ---- timers --------------------------------------------------------------------------------------
    void updateTimers(bool visible) {
        const bool want = hwnd && visible && !IsIconic(hwnd);
        if (want && !animTimer) {
            lastTick = GetTickCount64();
            SetTimer(hwnd, kAnimTimerId, kAnimTimerMs, nullptr);
            animTimer = true;
        } else if (!want && animTimer) {
            KillTimer(hwnd, kAnimTimerId);
            animTimer = false;
        }
        if (want && !diagTimer) {
            refreshDiagnostics();
            SetTimer(hwnd, kDiagTimerId, kDiagTimerMs, nullptr);
            diagTimer = true;
        } else if (!want && diagTimer) {
            KillTimer(hwnd, kDiagTimerId);
            diagTimer = false;
        }
    }

    void paintDecoNow(HWND w) {
        if (!w || !IsWindowVisible(w)) return;
        HDC dc = GetDC(w);
        if (!dc) return;
        paintDeco(w, dc);
        ReleaseDC(w, dc);
    }

    void onAnimTick() {
        const ULONGLONG now = GetTickCount64();
        const float dt = std::min(0.1f, static_cast<float>(now - lastTick) / 1000.0f);
        lastTick = now;
        ++tickCount;
        headerAnim.step(dt);
        paintDecoNow(header);
        if (curTab == 0 && (tickCount % 2) == 0) {   // scene cards at half rate
            for (int i = 0; i < 4; ++i) {
                scenes[i].step(dt * 2);
                skin.repaintNow(ctl(IDC_PRESET_LIGHT + i));
            }
        }
        if (curTab == 4) {
            fireworks.step(dt);
            paintDecoNow(ctl(IDC_FESTIVE_SCENE));
        }
        if (snowPreview) {
            if (previewDirty) {
                snowPreview->apply(s);
                previewDirty = false;
            }
            snowPreview->tick(dt);
            paintDecoNow(preview);
        }
        if (tickCount % 3 == 0) paintDecoNow(countdownWnd);
    }

    // ---- behaviour -------------------------------------------------------------------------------------
    void showTab(int index) {
        index = std::clamp(index, 0, kTabCount - 1);
        curTab = index;
        if (index == 4 && fireworks.time == 0) {
            for (int i = 0; i < 40; ++i) fireworks.step(0.05f);   // start with a few bursts in the sky
        }
        for (int i = 0; i < kTabCount; ++i) {
            const WPARAM want = i == index ? BST_CHECKED : BST_UNCHECKED;
            if (static_cast<WPARAM>(SendMessageW(nav[i], BM_GETCHECK, 0, 0)) != want) SendMessageW(nav[i], BM_SETCHECK, want, 0);
            // No cross-fade between pages: the old page's item must not look selected, even briefly.
            BufferedPaintStopAllAnimations(nav[i]);
            InvalidateRect(nav[i], nullptr, FALSE);
        }
        HWND focus = GetFocus();
        for (int i = 0; i < kTabCount; ++i) {
            if (i != index && focus && IsChild(pages[i], focus)) SetFocus(nav[index]);
            ShowWindow(pages[i], i == index ? SW_SHOW : SW_HIDE);
        }
    }

    void restoreDefaults() {
        std::vector<UserPreset> keep = s.userPresets;   // user data, not a setting: kept
        s = Settings{};
        s.userPresets = std::move(keep);
        applyLanguage();
        applyTheme();
        sync();
        changed();
    }

    void addExclusionName(std::wstring name) {
        name.erase(std::remove(name.begin(), name.end(), L'"'), name.end());
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name.erase(0, slash + 1);
        name = trim(name);
        if (name.empty()) return;
        if (name.find(L'.') == std::wstring::npos) name += L".exe";
        for (const std::string& existing : s.system.excludedProcesses)
            if (equalsNoCase(widen(existing), name)) return;
        s.system.excludedProcesses.push_back(narrow(name));
    }

    void addExclusion() {
        HWND edit = ctl(IDC_EXCL_EDIT);
        const size_t before = s.system.excludedProcesses.size();
        addExclusionName(windowText(edit));
        SetWindowTextW(edit, L"");
        if (s.system.excludedProcesses.size() == before) return;
        ++updating;
        fillExclusions();
        updateEnableStates();
        --updating;
        changed();
    }

    void removeExclusion() {
        HWND list = ctl(IDC_EXCL_LIST);
        const LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
        if (sel == LB_ERR || static_cast<size_t>(sel) >= s.system.excludedProcesses.size()) return;
        s.system.excludedProcesses.erase(s.system.excludedProcesses.begin() + sel);
        ++updating;
        fillExclusions();
        updateEnableStates();
        --updating;
        changed();
    }

    void openPicker() {
        picker.open(instance, hwnd, s.system.uiTheme, [this](const std::vector<std::wstring>& names) {
            const size_t before = s.system.excludedProcesses.size();
            for (const std::wstring& n : names) addExclusionName(n);
            if (s.system.excludedProcesses.size() == before) return;
            ++updating;
            fillExclusions();
            updateEnableStates();
            --updating;
            changed();
        });
    }

    void savePreset() {
        const std::wstring name = windowText(ctl(IDC_UP_NAME));
        switch (validatePresetName(name, s)) {
            case PresetNameError::Length: setUpMessage(tr(StrId::ErrPresetLength), TextRole::Error); return;
            case PresetNameError::Duplicate: setUpMessage(tr(StrId::ErrPresetDuplicate), TextRole::Error); return;
            case PresetNameError::TooMany: setUpMessage(tr(StrId::ErrPresetTooMany), TextRole::Error); return;
            case PresetNameError::None: break;
        }
        s.userPresets.push_back(makeUserPreset(s, name));
        ++updating;
        fillChips();
        --updating;
        showUpEditor(false);
        SendMessageW(ctl(IDC_UP_LIST), LB_SETCURSEL, s.userPresets.size() - 1, 0);
        setUpMessage(tr(StrId::PresetSaved), TextRole::Ok);
        SetFocus(ctl(IDC_UP_LIST));
        changed();
    }

    void applyPreset(int index) {
        if (index < 0 || static_cast<size_t>(index) >= s.userPresets.size()) return;
        const Settings before = s;
        applyUserPreset(s, s.userPresets[static_cast<size_t>(index)]);
        sync();
        if (!(s == before)) changed();
    }

    void deletePreset(int index) {
        if (index < 0 || static_cast<size_t>(index) >= s.userPresets.size()) return;
        s.userPresets.erase(s.userPresets.begin() + index);
        ++updating;
        fillChips();
        --updating;
        setUpMessage(s.userPresets.empty() ? L"" : tr(StrId::PresetsHint), TextRole::Secondary);
        changed();
    }

    static LRESULT CALLBACK chipsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        Impl* self = reinterpret_cast<Impl*>(ref);
        switch (msg) {
            case WM_LBUTTONUP: {
                const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, lp);
                const int index = LOWORD(hit);
                if (HIWORD(hit) == 0) {
                    RECT ir{};
                    SendMessageW(h, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&ir));
                    const float x = static_cast<float>(GET_X_LPARAM_(lp) - ir.left) * self->k();
                    const float w = static_cast<float>(ir.right - ir.left) * self->k();
                    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
                    if (x >= Skin::chipDeleteZone(w)) self->deletePreset(index);
                    else self->applyPreset(index);
                    return r;
                }
                break;
            }
            case WM_KEYDOWN:
                if (wp == VK_DELETE) {
                    self->deletePreset(static_cast<int>(SendMessageW(h, LB_GETCURSEL, 0, 0)));
                    return 0;
                }
                if (wp == VK_SPACE) {
                    self->applyPreset(static_cast<int>(SendMessageW(h, LB_GETCURSEL, 0, 0)));
                    return 0;
                }
                break;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                InvalidateRect(h, nullptr, FALSE);
                break;
            case WM_PAINT:
                if (SendMessageW(h, LB_GETCOUNT, 0, 0) == 0) {
                    PAINTSTRUCT ps;
                    HDC dc = BeginPaint(h, &ps);
                    RECT rc{};
                    GetClientRect(h, &rc);
                    const Palette& P = self->pal();
                    Painter::instance().paint(dc, rc, self->dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s2) {
                        Painter& pt = Painter::instance();
                        pt.fill(D2D1::RectF(0, 0, s2.width, s2.height), col(P.page));
                        const D2D1_RECT_F box = D2D1::RectF(4.5f, 3.5f, s2.width - 4.5f, 37.5f);
                        pt.fillRound(box, 8, col(P.control, 0.5f));
                        pt.strokeRound(box, 8, col(P.controlBorder, 0.8f), 1.0f);
                        TextStyle ts{pt.bodySize()};
                        ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                        pt.text(tr(StrId::PresetsEmpty), box, ts, col(P.textSecondary));
                        if (GetFocus() == h) pt.strokeRound(box, 8, col(P.focus), 1.5f);
                    });
                    EndPaint(h, &ps);
                    return 0;
                }
                break;
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(h, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, surfaceBrush(self->pal(), Surface::Page));
                return 1;
            }
            case WM_NCDESTROY:
                RemoveWindowSubclass(h, chipsProc, kChipsSubclassId);
                break;
            default:
                break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }
    static int GET_X_LPARAM_(LPARAM lp) { return static_cast<int>(static_cast<short>(LOWORD(lp))); }

    void onCommand(int id, int code, HWND source) {
        if (updating > 0) return;
        if (code == HKN_CHANGED) {
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(kHotkeyFields)); ++i) {
                if (kHotkeyFields[i].id != id) continue;
                std::string& field = s.hotkeys.*(kHotkeyFields[i].member);
                const std::string text = getHotkeyEditText(source);
                if (field == text) return;
                field = text;
                refreshHotkeyStatus(i);
                changed();
                return;
            }
            return;
        }
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            for (const Field& f : fields) {
                if (f.inner != source) continue;
                RECT r{px(f.x) - 2, px(f.y) - 2, px(f.x + f.w) + 2, px(f.y + f.h) + 2};
                InvalidateRect(pages[f.page], &r, TRUE);
            }
            return;
        }
        if (id == IDC_EXCL_LIST) {
            if (code == LBN_SELCHANGE) updateEnableStates();
            return;
        }
        if (id == IDC_UP_LIST) return;
        if (code != BN_CLICKED) return;

        if (id >= IDC_NAV_FIRST && id <= IDC_NAV_LAST) {
            showTab(id - IDC_NAV_FIRST);
            return;
        }
        const Settings before = s;
        for (const BoolField& f : kBoolFields) {
            if (f.id != id) continue;
            f.ref(s) = SendMessageW(source, BM_GETCHECK, 0, 0) == BST_CHECKED;
            updateEnableStates();
            if (!(s == before)) changed();
            return;
        }
        for (const RadioGroup& g : kRadioGroups) {
            if (id < g.firstId || id >= g.firstId + g.count) continue;
            if (SendMessageW(source, BM_GETCHECK, 0, 0) != BST_CHECKED) return;
            g.set(s, id - g.firstId);
            if (s == before) return;
            if (g.kind == GroupKind::Plain) {
                ++updating;
                sync();
                --updating;
            }
            if (g.kind == GroupKind::Language) applyLanguage();
            if (g.kind == GroupKind::Theme) {
                ++updating;
                sync();   // keep swatches and «Система» segments in step
                --updating;
                applyTheme();
            }
            changed();
            return;
        }
        switch (id) {
            case IDC_CLEAR_ALL:
                if (cb.onClearSnow) cb.onClearSnow();
                break;
            case IDC_FIREWORKS_NOW:
                if (cb.onFireworks) cb.onFireworks();
                break;
            case IDC_PREVIEW_RESET:
                if (snowPreview) {
                    snowPreview->reset();
                    InvalidateRect(preview, nullptr, FALSE);
                }
                break;
            case IDC_RESTORE:
                if (MessageBoxW(hwnd, tr(StrId::RestoreConfirmText), tr(StrId::AppName),
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST) == IDYES) {
                    restoreDefaults();
                }
                break;
            case IDC_CLOSE:
            case IDCANCEL:
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                break;
            case IDOK: {   // Enter (sent by IsDialogMessage)
                HWND focus = GetFocus();
                if (focus == ctl(IDC_EXCL_EDIT)) addExclusion();
                else if (focus == ctl(IDC_UP_NAME)) savePreset();
                else if (focus == ctl(IDC_UP_LIST)) applyPreset(static_cast<int>(SendMessageW(focus, LB_GETCURSEL, 0, 0)));
                else if (const Style* st = skin.find(focus)) {
                    if (st->role == Role::SceneCard || st->role == Role::Toggle || st->role == Role::Segment || st->role == Role::Swatch)
                        SendMessageW(focus, BM_CLICK, 0, 0);
                }
                break;
            }
            case IDC_UP_SAVE:
                showUpEditor(true);
                setUpMessage(L"", TextRole::Secondary);
                break;
            case IDC_UP_OK:
                savePreset();
                break;
            case IDC_EXCL_ADD:
                addExclusion();
                break;
            case IDC_EXCL_REMOVE:
                removeExclusion();
                break;
            case IDC_EXCL_PICK:
                openPicker();
                break;
            case IDC_LINK_GITHUB:
            case IDC_LINK_TELEGRAM:
                if (const Style* st = skin.find(source); st && st->url) {
                    ShellExecuteW(nullptr, L"open", st->url, nullptr, nullptr, SW_SHOWNORMAL);
                }
                break;
            case IDC_OPEN_LOG_FOLDER: {
                if (!cb.getDiagnostics) break;
                std::wstring path = cb.getDiagnostics().logPath;
                const size_t slash = path.find_last_of(L"\\/");
                if (slash == std::wstring::npos) break;
                path.erase(slash);
                ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            default:
                break;
        }
    }

    void onScroll(HWND source) {
        if (updating > 0 || !source) return;
        for (const SliderField& f : kSliderFields) {
            if (ctl(f.id) != source) continue;
            const int v = std::clamp(static_cast<int>(SendMessageW(source, TBM_GETPOS, 0, 0)), f.lo, f.hi);
            setTextIfChanged(ctl(f.valueId), valueText(f.kind, v));
            if (f.ref(s) != v) {
                f.ref(s) = v;
                changed();
            }
            return;
        }
    }

    void onDpiChanged(UINT newDpi, const RECT* suggested) {
        if (!built) return;
        dpi = g_dpiOverride ? g_dpiOverride : newDpi;
        skin.dpi = dpi;
        createGdi();
        POINT anchor{suggested->left, suggested->top};
        layout(&anchor, false);
    }

    // Forwarded child-container messages (pages, header, deco) and the main window share this.
    bool commonMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* r) {
        switch (msg) {
            case WM_COMMAND:
                onCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
                *r = 0;
                return true;
            case WM_HSCROLL:
                onScroll(reinterpret_cast<HWND>(lp));
                *r = 0;
                return true;
            case WM_NOTIFY: {
                const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
                if (hdr && hdr->code == TTN_GETDISPINFOW) {
                    auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
                    for (int i = 0; i < 3; ++i) {
                        if (reinterpret_cast<HWND>(hdr->idFrom) == swatches[i]) di->lpszText = const_cast<wchar_t*>(tr(kThemeNames[i]));
                        if (reinterpret_cast<HWND>(hdr->idFrom) == hdrTools[i]) di->lpszText = const_cast<wchar_t*>(tr(kHdrToolNames[i]));
                    }
                    *r = 0;
                    return true;
                }
                return skin.onNotify(lp, r);
            }
            case WM_DRAWITEM:
                if (skin.onDrawItem(lp)) {
                    *r = TRUE;
                    return true;
                }
                return false;
            case WM_MEASUREITEM: {
                auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
                mi->itemHeight = static_cast<UINT>(px(34));
                *r = TRUE;
                return true;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
                return skin.onCtlColor(msg, wp, lp, r);
            default:
                return false;
        }
    }

    // ---- window procedures ---------------------------------------------------------------------------
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        LRESULT r = 0;
        if (self->built && self->commonMessage(msg, wp, lp, &r)) return r;
        switch (msg) {
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                HDC dc = reinterpret_cast<HDC>(wp);
                const Palette& P = self->pal();
                FillRect(dc, &rc, surfaceBrush(P, Surface::Window));
                if (!P.pillTabs) {
                    RECT navRc{0, 0, self->px(kNavW), rc.bottom};
                    FillRect(dc, &navRc, surfaceBrush(P, Surface::Nav));
                }
                return 1;
            }
            case WM_TIMER:
                if (wp == kDiagTimerId) {
                    if (!IsIconic(hwnd)) self->refreshDiagnostics();
                    return 0;
                }
                if (wp == kAnimTimerId) {
                    self->onAnimTick();
                    return 0;
                }
                break;
            case WM_SHOWWINDOW:
                self->updateTimers(wp != FALSE);
                break;
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) self->updateTimers(false);
                else if (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) self->updateTimers(IsWindowVisible(hwnd) != FALSE);
                break;
            case WM_DPICHANGED:
                self->onDpiChanged(HIWORD(wp), reinterpret_cast<const RECT*>(lp));
                return 0;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                if (self->built && (msg == WM_THEMECHANGED || wp == SPI_SETNONCLIENTMETRICS)) {
                    self->createGdi();
                    self->layout(nullptr, false);
                }
                break;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) {
                    HWND focus = GetFocus();
                    if (focus && IsChild(hwnd, focus)) self->lastFocus = focus;
                }
                break;
            case WM_SETFOCUS: {
                HWND target = self->lastFocus;
                if (!target || !IsWindow(target) || !IsWindowVisible(target) || !IsWindowEnabled(target)) target = self->nav[self->curTab];
                if (target) SetFocus(target);
                return 0;
            }
            case uitest::kMsgRestoreDefaultsNoConfirm:
                self->restoreDefaults();
                return 0;
            case uitest::kMsgQueryTimers:
                return (self->animTimer ? 1 : 0) | (self->diagTimer ? 2 : 0);
            case uitest::kMsgPreviewAdvance:
                if (self->snowPreview) {
                    self->snowPreview->advance(static_cast<float>(wp));
                    RedrawWindow(self->preview, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                    return static_cast<LRESULT>(self->snowPreview->snowMass());
                }
                return 0;
            case WM_CLOSE:
                DestroyWindow(hwnd);   // closing never quits the application
                return 0;
            case WM_DESTROY:
                self->picker.close();
                KillTimer(hwnd, kDiagTimerId);
                KillTimer(hwnd, kAnimTimerId);
                self->animTimer = self->diagTimer = false;
                if (self->tooltip) DestroyWindow(self->tooltip);
                self->tooltip = nullptr;
                return 0;
            case WM_NCDESTROY: {
                const LRESULT res = DefWindowProcW(hwnd, msg, wp, lp);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                for (HWND& t : self->hdrTools) t = nullptr;
                self->hwnd = self->header = self->preview = self->previewReset = self->countdownWnd = nullptr;
                self->statusLine = self->restoreBtn = self->closeBtn = nullptr;
                for (HWND& page : self->pages) page = nullptr;
                for (HWND& n : self->nav) n = nullptr;
                for (HWND& w : self->swatches) w = nullptr;
                self->ctls.clear();
                self->fields.clear();
                self->skin.clear();
                self->built = false;
                self->lastFocus = nullptr;
                self->upEditing = false;
                self->snowPreview.reset();
                self->countdownKey.clear();
                self->destroyGdi();
                return res;
            }
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // Pages and deco windows: plain containers that forward to the top-level window.
    static LRESULT CALLBACK containerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            if (cs->lpCreateParams) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(h, msg, wp, lp);
        }
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self || !self->hwnd) return DefWindowProcW(h, msg, wp, lp);
        LRESULT r = 0;
        switch (msg) {
            case WM_COMMAND:
            case WM_HSCROLL:
            case WM_NOTIFY:
            case WM_DRAWITEM:
            case WM_MEASUREITEM:
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
                if (self->commonMessage(msg, wp, lp, &r)) return r;
                break;
            case WM_ERASEBKGND: {
                wchar_t cls[48] = {};
                GetClassNameW(h, cls, ARRAYSIZE(cls));
                if (lstrcmpW(cls, kPageClass) == 0) self->paintPage(h, reinterpret_cast<HDC>(wp));
                return 1;
            }
            case WM_SETCURSOR:
                if (const Style* st = self->skin.find(reinterpret_cast<HWND>(wp)); st && st->role == Role::Link) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            case WM_PAINT: {
                wchar_t cls[48] = {};
                GetClassNameW(h, cls, ARRAYSIZE(cls));
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                if (lstrcmpW(cls, kDecoClass) == 0) self->paintDeco(h, dc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_PRINTCLIENT: {
                wchar_t cls[48] = {};
                GetClassNameW(h, cls, ARRAYSIZE(cls));
                if (lstrcmpW(cls, kDecoClass) == 0) self->paintDeco(h, reinterpret_cast<HDC>(wp));
                else self->paintPage(h, reinterpret_cast<HDC>(wp));
                return 0;
            }
            case WM_NCDESTROY:
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                break;
            default:
                break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void registerClasses() {
        auto reg = [this](const wchar_t* name, WNDPROC proc, HBRUSH bg) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            if (GetClassInfoExW(instance, name, &wc)) return;
            wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = bg;
            wc.lpszClassName = name;
            RegisterClassExW(&wc);
        };
        reg(kWindowClass, wndProc, nullptr);
        reg(kPageClass, containerProc, nullptr);
        reg(kDecoClass, containerProc, nullptr);
    }

    bool createWindow() {
        if (!instance) instance = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        registerClasses();
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi)) {
            x = (mi.rcWork.left + mi.rcWork.right) / 2 - 100;
            y = (mi.rcWork.top + mi.rcWork.bottom) / 2 - 100;
        }
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        HWND created = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, tr(StrId::SettingsTitle), style, x, y, 200, 200, nullptr,
                                       nullptr, instance, this);
        if (!created) return false;
        hwnd = created;
        dpi = layoutDpi();
        curTab = 0;
        shownTheme = s.system.uiTheme;
        snowPreview = std::make_unique<SnowPreview>();
        snowPreview->apply(s);
        snowPreview->reset();
        build();
        applyThemeVisuals();
        createGdi();
        layout(nullptr, true);
        sync();
        setUpMessage(s.userPresets.empty() ? L"" : tr(StrId::PresetsHint), TextRole::Secondary);
        showUpEditor(false);
        refreshDiagnostics();
        showTab(0);
        return true;
    }
};

SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}

SettingsWindow::~SettingsWindow() { close(); }

void SettingsWindow::initialize(HINSTANCE instance, SettingsWindowCallbacks callbacks) {
    impl_->instance = instance;
    impl_->cb = std::move(callbacks);
}

void SettingsWindow::show(const Settings& current) {
    Impl& d = *impl_;
    d.s = current;
    d.applyLanguage();
    if (!d.hwnd) {
        if (!d.createWindow()) return;
    } else {
        d.applyTheme();
        d.sync();
        if (d.snowPreview) d.snowPreview->apply(d.s);
    }
    if (IsIconic(d.hwnd)) ShowWindow(d.hwnd, SW_RESTORE);
    else ShowWindow(d.hwnd, SW_SHOWNORMAL);
    d.updateTimers(true);
    SetForegroundWindow(d.hwnd);
    if (!GetFocus() || !IsChild(d.hwnd, GetFocus())) SetFocus(d.nav[d.curTab]);
}

void SettingsWindow::close() {
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
}

bool SettingsWindow::isOpen() const { return impl_->hwnd != nullptr; }

HWND SettingsWindow::hwnd() const { return impl_->hwnd; }

void SettingsWindow::update(const Settings& current) {
    Impl& d = *impl_;
    d.s = current;
    if (!d.hwnd) return;
    d.applyLanguage();
    d.applyTheme();
    d.sync();
    if (d.snowPreview) d.snowPreview->apply(d.s);
}

bool SettingsWindow::preTranslateMessage(MSG* msg) {
    Impl& d = *impl_;
    if (!msg) return false;
    if (d.picker.preTranslateMessage(msg)) return true;
    if (!d.hwnd || !d.built) return false;
    if (msg->hwnd != d.hwnd && !IsChild(d.hwnd, msg->hwnd)) return false;

    const bool keyMsg = msg->message >= WM_KEYFIRST && msg->message <= WM_KEYLAST;
    if (keyMsg) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool alt = GetKeyState(VK_MENU) < 0;
        const bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;

        if (isHotkeyEdit(msg->hwnd)) {
            // The capture field must see its keys: only plain Tab / Shift+Tab / Esc keep their
            // dialog meaning. No TranslateMessage: the field does not want WM_CHAR.
            const bool plain = !ctrl && !alt && !win;
            const bool navigation = msg->message == WM_KEYDOWN && plain &&
                                    (msg->wParam == VK_TAB || (msg->wParam == VK_ESCAPE && !shift));
            const bool altF4 = msg->wParam == VK_F4 && alt;
            if (altF4) return false;
            if (!navigation) {
                DispatchMessageW(msg);
                return true;
            }
        } else if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE && d.upEditing && msg->hwnd == d.ctl(IDC_UP_NAME)) {
            d.showUpEditor(false);
            d.setUpMessage(d.s.userPresets.empty() ? L"" : tr(StrId::PresetsHint), TextRole::Secondary);
            SetFocus(d.ctl(IDC_UP_SAVE));
            return true;
        } else if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB && ctrl && !alt) {
            const int next = (d.curTab + (shift ? kTabCount - 1 : 1)) % kTabCount;
            d.showTab(next);
            if (GetFocus() && !IsChild(d.pages[next], GetFocus())) SetFocus(d.nav[next]);
            return true;
        }
    }
    return IsDialogMessageW(d.hwnd, msg) != FALSE;
}

}  // namespace snow

// ---- test seams -----------------------------------------------------------------------------------
namespace snow::uitest {

void setDpiOverride(UINT dpi) { snow::g_dpiOverride = dpi; }

HWND findControl(HWND settingsWindow, int id) {
    struct Query {
        int id;
        HWND found;
    } q{id, nullptr};
    EnumChildWindows(
        settingsWindow,
        [](HWND child, LPARAM lp) -> BOOL {
            auto* query = reinterpret_cast<Query*>(lp);
            if (GetDlgCtrlID(child) == query->id) {
                query->found = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&q));
    return q.found;
}

void setToday(const SYSTEMTIME* localTime) { snow::ui::setCountdownOverride(localTime); }

const wchar_t* linkUrl(HWND link) { return link ? static_cast<const wchar_t*>(GetPropW(link, L"SnowUrl")) : nullptr; }

}  // namespace snow::uitest
