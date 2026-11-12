// Unit tests of the pure parts of the macOS shell (ctest: mac_unit).
#define SNOW_TEST_MAIN
#include "snow_test.h"

#include <Carbon/Carbon.h>
#include <stdlib.h>
#include <unistd.h>

#include <regex>

#include "app/icon_render.h"
#include "render/msl_translate.h"
#include "settings/settings_store.h"
#include "system/hotkey_map.h"
#include "system/text_util.h"
#include "tracker/desktop_model.h"

using namespace snow;
using namespace snow::mac;

// ---- coordinates ------------------------------------------------------------------------
TEST(coords_primary_screen) {
    CHECK((cocoaToGlobal(CocoaRect{0, 0, 1440, 900}, 900) == RectI{0, 0, 1440, 900}));
    // Visible frame below a 25 pt menu bar and above a 70 pt Dock.
    CHECK((cocoaToGlobal(CocoaRect{0, 70, 1440, 805}, 900) == RectI{0, 25, 1440, 830}));
}

TEST(coords_secondary_screens) {
    // A screen above the primary has negative global y; one to the left negative x.
    CHECK((cocoaToGlobal(CocoaRect{0, 900, 1920, 1080}, 900) == RectI{0, -1080, 1920, 0}));
    CHECK((cocoaToGlobal(CocoaRect{-1280, -124, 1280, 1024}, 900) == RectI{-1280, 0, 0, 1024}));
    const RectI g{100, -500, 900, 100};
    const CocoaRect c = globalToCocoa(g, 900);
    CHECK((cocoaToGlobal(c, 900) == g));
    CHECK_NEAR(c.y, 800, 1e-9);
}

TEST(coords_rounding) { CHECK((roundRect(10.4, 20.6, 99.2, 50.0) == RectI{10, 21, 110, 71})); }

// ---- Dock ---------------------------------------------------------------------------------
ScreenInfo screen(RectI frame, RectI visible, std::uint32_t id = 1, bool primary = true) {
    ScreenInfo s;
    s.displayId = id;
    s.frame = frame;
    s.visible = visible;
    s.primary = primary;
    s.backingScale = 2;
    return s;
}

TEST(dock_bottom) {
    const ScreenInfo s = screen({0, 0, 1440, 900}, {0, 25, 1440, 830});
    CHECK(dockEdge(s) == DockEdge::Bottom);
    CHECK((dockTaskbarRect(s, 600) == RectI{420, 830, 1020, 899}));       // centred, 1 pt above the bottom
    CHECK((dockTaskbarRect(s, 0) == RectI{0, 830, 1440, 899}));           // unknown width: full width
    CHECK((dockTaskbarRect(s, 600, 400) == RectI{100, 830, 700, 899}));   // AX-provided centre
    CHECK((dockTaskbarRect(s, 600, 50) == RectI{0, 830, 600, 899}));      // clamped to the screen
    CHECK((dockTaskbarRect(s, 5000) == RectI{0, 830, 1440, 899}));
}

TEST(dock_side_or_hidden) {
    CHECK(dockEdge(screen({0, 0, 1440, 900}, {80, 25, 1440, 900})) == DockEdge::Left);
    CHECK(dockEdge(screen({0, 0, 1440, 900}, {0, 25, 1360, 900})) == DockEdge::Right);
    CHECK(dockEdge(screen({0, 0, 1440, 900}, {0, 25, 1440, 896})) == DockEdge::None);   // auto-hide slack
    CHECK(dockTaskbarRect(screen({0, 0, 1440, 900}, {80, 25, 1440, 900}), 600).empty());
    CHECK(dockTaskbarRect(screen({0, 0, 1440, 900}, {0, 25, 1440, 900}), 600).empty());
}

// ---- classification -------------------------------------------------------------------------
RawWindow win(std::uint32_t n, int pid, int layer, RectI b, const char* owner = "App", float alpha = 1) {
    RawWindow w;
    w.number = n;
    w.pid = pid;
    w.layer = layer;
    w.bounds = b;
    w.alpha = alpha;
    w.owner = owner;
    return w;
}

TEST(classify_windows) {
    ClassifyContext ctx;
    ctx.ownPid = 42;
    ctx.overlayLevel = 19;
    ctx.screens = {screen({0, 0, 1440, 900}, {0, 25, 1440, 830})};
    ctx.ignored = {77};
    CHECK(classify(win(1, 10, 0, {100, 100, 600, 500}), ctx) == Verdict::App);
    CHECK(classify(win(2, 10, 0, {100, 100, 600, 500}, "App", 0.0f), ctx) == Verdict::Skip);   // invisible
    CHECK(classify(win(3, 10, 0, {100, 100, 102, 500}), ctx) == Verdict::Skip);                 // too thin
    CHECK(classify(win(4, 11, 20, {0, 0, 1440, 900}, "Dock"), ctx) == Verdict::Skip);
    CHECK(classify(win(5, 12, 0, {0, 0, 1440, 25}, "Window Server"), ctx) == Verdict::Skip);
    CHECK(classify(win(6, 13, 25, {1300, 0, 1330, 24}, "Control Center"), ctx) == Verdict::Skip);
    CHECK(classify(win(7, 10, 3, {200, 200, 400, 300}), ctx) == Verdict::Occluder);             // floating palette
    CHECK(classify(win(8, 10, 8, {0, 0, 1440, 900}), ctx) == Verdict::Skip);                     // full-screen veil
    CHECK(classify(win(9, 10, 101, {200, 200, 400, 300}), ctx) == Verdict::Skip);                // menu: above us
    CHECK(classify(win(10, 42, 19, {0, 0, 1440, 900}), ctx) == Verdict::Skip);                   // our overlay
    CHECK(classify(win(11, 42, 0, {300, 300, 700, 600}), ctx) == Verdict::App);                  // our settings window
    CHECK(classify(win(12, 10, -2147483624, {0, 0, 1440, 900}), ctx) == Verdict::Skip);          // desktop
    CHECK(classify(win(77, 10, 0, {100, 100, 600, 500}), ctx) == Verdict::Skip);                 // ignored
}

// ---- layout builder ---------------------------------------------------------------------------
struct Fixture {
    LayoutBuilder b;
    TrackerFrame f;
    std::set<std::uint32_t> alive;
    std::map<int, std::vector<std::string>> names;
    Fixture() {
        b.context.ownPid = 42;
        f.screens = {screen({0, 0, 1440, 900}, {0, 25, 1440, 830})};
        f.dockWidth = 600;
    }
    bool build(DesktopLayout& out, bool verify = true) {
        return b.build(
            f, [&](const std::vector<std::uint32_t>& q) {
                std::set<std::uint32_t> r;
                for (auto n : q)
                    if (alive.count(n)) r.insert(n);
                return r;
            },
            [&](int pid) { return names[pid]; }, out, verify);
    }
};

TEST(builder_basic_layout) {
    Fixture fx;
    fx.f.windows = {win(1, 10, 0, {100, 100, 600, 500}), win(2, 11, 0, {300, 200, 900, 700})};
    DesktopLayout L;
    CHECK(fx.build(L));
    CHECK(L.monitors.size() == 1 && L.monitors[0].primary && L.monitors[0].dpiScale == 1.0f);
    CHECK((L.monitors[0].work == RectI{0, 25, 1440, 830}));
    CHECK(L.windows.size() == 3);
    CHECK(L.windows[0].kind == WindowKind::Taskbar);   // Dock first (front-most)
    CHECK(L.windows[1].kind == WindowKind::App && L.windows[2].kind == WindowKind::App);
    CHECK(L.windows[1].id != L.windows[2].id);
    CHECK(L.windows[1].cornerRadiusPx > 0 && !L.windows[1].maximized);
    CHECK(L.revision == 1);
    DesktopLayout L2;
    CHECK(!fx.build(L2));   // unchanged: nothing published
}

TEST(builder_move_keeps_id) {
    Fixture fx;
    fx.f.windows = {win(1, 10, 0, {100, 100, 600, 500})};
    DesktopLayout L;
    fx.build(L);
    const WindowId id = L.windows[1].id;
    fx.f.windows[0].bounds = RectI{150, 120, 650, 520};
    DesktopLayout M;
    CHECK(fx.build(M));
    CHECK(M.windows[1].id == id && M.revision == L.revision + 1);
}

TEST(builder_minimize_and_close) {
    Fixture fx;
    fx.f.windows = {win(1, 10, 0, {100, 100, 600, 500})};
    DesktopLayout L;
    fx.build(L);
    const WindowId id = L.windows[1].id;
    // Minimized: off the on-screen list, but it still exists.
    fx.alive = {1};
    fx.f.windows.clear();
    DesktopLayout M;
    CHECK(fx.build(M));
    CHECK(M.windows.size() == 1 && M.hiddenWindows.size() == 1 && M.hiddenWindows[0] == id);
    // Restored: same id.
    fx.f.windows = {win(1, 10, 0, {100, 100, 600, 500})};
    DesktopLayout R;
    CHECK(fx.build(R));
    CHECK(R.windows[1].id == id && R.hiddenWindows.empty());
    // Closed: gone from both lists, the id is never reused.
    fx.alive.clear();
    fx.f.windows.clear();
    DesktopLayout C;
    CHECK(fx.build(C));
    CHECK(C.windows.size() == 1 && C.hiddenWindows.empty());
    fx.f.windows = {win(1, 10, 0, {100, 100, 600, 500})};
    DesktopLayout N;
    fx.build(N);
    CHECK(N.windows[1].id != id);
}

TEST(builder_exclusions_fullscreen_maximized) {
    Fixture fx;
    fx.names[10] = {"safari"};
    fx.names[11] = {"game", "my game"};
    fx.b.excluded = {"safari.app", "my game"};
    fx.f.frontPid = 12;
    fx.f.windows = {win(1, 10, 0, {100, 100, 600, 500}), win(2, 11, 0, {200, 200, 600, 500}),
                    win(3, 12, 0, {0, 0, 1440, 900}), win(4, 13, 0, {0, 25, 1440, 830})};
    DesktopLayout L;
    fx.build(L);
    CHECK(L.windows.size() == 5);
    CHECK(!L.windows[1].snowAllowed && !L.windows[2].snowAllowed);
    CHECK(L.windows[3].maximized && L.windows[3].cornerRadiusPx == 0);   // fullscreen window
    CHECK(L.windows[4].maximized);                                        // zoomed under the menu bar
    CHECK(L.fullscreenAppActive);
    fx.f.frontPid = 13;
    DesktopLayout M;
    CHECK(fx.build(M));
    CHECK(!M.fullscreenAppActive);
}

// ---- hotkeys ------------------------------------------------------------------------------------
TEST(hotkey_keycodes_match_carbon) {
    const unsigned letters[26] = {kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
                                  kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
                                  kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
                                  kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z};
    for (unsigned i = 0; i < 26; ++i) {
        unsigned k = 999;
        CHECK(vkToMacKeyCode(0x41 + i, k) && k == letters[i]);
    }
    const unsigned digits[10] = {kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
                                 kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
    for (unsigned i = 0; i < 10; ++i) {
        unsigned k = 999;
        CHECK(vkToMacKeyCode(0x30 + i, k) && k == digits[i]);
    }
    const unsigned fkeys[20] = {kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,  kVK_F8,  kVK_F9,  kVK_F10,
                                kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
    for (unsigned i = 0; i < 20; ++i) {
        unsigned k = 999;
        CHECK(vkToMacKeyCode(0x70 + i, k) && k == fkeys[i]);
    }
    unsigned k = 0;
    CHECK(vkToMacKeyCode(0x20, k) && k == kVK_Space);
    CHECK(vkToMacKeyCode(0x0D, k) && k == kVK_Return);
    CHECK(vkToMacKeyCode(0x2E, k) && k == kVK_ForwardDelete);
    CHECK(vkToMacKeyCode(0x25, k) && k == kVK_LeftArrow);
    CHECK(vkToMacKeyCode(0x22, k) && k == kVK_PageDown);
    CHECK(!vkToMacKeyCode(0x13, k));   // Pause has no Mac key
    CHECK(!vkToMacKeyCode(0x84, k));   // F21
    CHECK(kCarbonCmd == cmdKey && kCarbonShift == shiftKey && kCarbonOption == optionKey && kCarbonControl == controlKey);
    CHECK(modifiersToCarbon(kModCtrl | kModAlt) == (controlKey | optionKey));
    CHECK(modifiersToCarbon(kModWin | kModShift) == (cmdKey | shiftKey));
}

TEST(hotkey_mac_spelling) {
    CHECK(canonicalHotkeyText("Ctrl+Option+S") == "Ctrl+Alt+S");
    CHECK(canonicalHotkeyText("cmd+shift+f1") == "Shift+Win+F1");
    CHECK(canonicalHotkeyText("\xE2\x8C\x83+\xE2\x8C\xA5+X") == "Ctrl+Alt+X");   // ⌃+⌥+X
    CHECK(canonicalHotkeyText("Ctrl+Alt+I") == "Ctrl+Alt+I");
    CHECK(canonicalHotkeyText("S").empty());   // no modifier
    CHECK(canonicalHotkeyText("").empty());
    CHECK(macHotkeyDisplay("Ctrl+Alt+S") == "Ctrl+Option+S");
    CHECK(macHotkeyDisplay("Shift+Win+F1") == "Shift+Cmd+F1");
    CHECK(macHotkeyDisplay("").empty());
}

// ---- shader translation ---------------------------------------------------------------------------
TEST(msl_translation) {
    for (ShaderStage st : {ShaderStage::SpriteVertex, ShaderStage::SpriteFragment, ShaderStage::DriftVertex,
                           ShaderStage::DriftFragment}) {
        const std::string s = buildMslLibrary(st);
        CHECK(s.find(mslEntryPoint(st)) != std::string::npos);
        CHECK(s.find("cbuffer") == std::string::npos);
        CHECK(s.find("SV_") == std::string::npos);
        CHECK(s.find("TEXCOORD") == std::string::npos);
        CHECK(s.find("[unroll]") == std::string::npos && s.find("[branch]") == std::string::npos &&
              s.find("[forcecase]") == std::string::npos);
        CHECK(s.find("static const") == std::string::npos);
        CHECK(s.find("nointerpolation") == std::string::npos);
        CHECK(!std::regex_search(s, std::regex(R"((^|[^_\w])(lerp|frac|ddx|ddy)\s*\()")));
        CHECK(!std::regex_search(s, std::regex(R"(\bmain\s*\()")));
        CHECK(!std::regex_search(s, std::regex(R"((^|[^.\w])gOpacity\b)")));   // always F_.gOpacity
    }
    CHECK(translateHlsl("float patch = 1.0; float x = lerp(a, b, t);", "m") == "float patch_ = 1.0; float x = sf_lerp(a, b, t);");
    CHECK(translateHlsl("struct V { float4 p : SV_Position; float c = x ? a : B; };", "m").find(": B") == std::string::npos);
    CHECK(translateHlsl("float y = c ? a : B;", "m") == "float y = c ? a : B;");   // ternaries outside structs untouched
}

// ---- settings store -------------------------------------------------------------------------------
TEST(settings_store_round_trip) {
    char tmpl[] = "/tmp/snowfall-unit-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    SettingsStore::setDataDirectoryOverride(fromUtf8(std::string(dir) + "/nested/data"));
    CHECK(toUtf8(SettingsStore::dataDirectory()) == std::string(dir) + "/nested/data");
    SettingsStore store(SettingsStore::defaultPath());
    Settings missing;
    missing.weather.intensity = 3;
    CHECK(store.load(missing) && missing == Settings{});   // first run: defaults
    Settings s;
    s.weather.preset = WeatherPreset::Blizzard;
    s.system.excludedProcesses = {"Safari", "Игра"};
    s.userPresets.push_back(UserPreset{"Вечер", s.weather, 40, 80});
    CHECK(store.save(s));
    CHECK(access((toUtf8(store.path()) + ".tmp").c_str(), F_OK) != 0);   // temp file renamed away
    Settings back;
    CHECK(store.load(back) && back == s);
}

// ---- text + icon ------------------------------------------------------------------------------------
TEST(text_utf8_round_trip) {
    const std::string s = "Снег ❄ snow \xF0\x9F\x8E\x84";   // incl. a 4-byte sequence
    CHECK(toUtf8(fromUtf8(s)) == s);
    CHECK(fromUtf8(s).size() == 13);
    CHECK(toLowerAscii("Google Chrome.APP") == "google chrome.app");
}

TEST(icon_render) {
    const auto icon = renderAppIconRGBA(64);
    CHECK(icon.size() == 64u * 64u * 4u);
    CHECK(icon[(32 * 64 + 32) * 4 + 3] == 255);   // centre opaque
    CHECK(icon[3] == 0);                          // corner transparent
    const auto glyph = renderMenuBarGlyphRGBA(36);
    CHECK(glyph[(18 * 36 + 18) * 4 + 3] > 200);    // flake centre
    CHECK(glyph[3] == 0);
}
