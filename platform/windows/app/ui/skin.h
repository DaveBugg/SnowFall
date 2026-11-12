// Skin: theme-aware rendering of real Win32 controls (hybrid approach). The controls stay
// ordinary BUTTON / TRACKBAR / STATIC / EDIT / LISTBOX windows (keyboard navigation, accessible
// names, test seams keep working); the Skin paints them through NM_CUSTOMDRAW, WM_DRAWITEM and
// WM_CTLCOLOR*. One Skin per top-level window. UI thread only.
#pragma once
#include <windows.h>

#include <map>

#include "decor.h"
#include "theme.h"

namespace snow::ui {

enum class Role : unsigned char {
    Toggle,          // BS_AUTOCHECKBOX drawn as a switch + label
    Segment,         // BS_AUTORADIOBUTTON drawn as one segment of a pill control
    Button,          // push button (secondary)
    PrimaryButton,   // push button (accent)
    Nav,             // BS_AUTORADIOBUTTON navigation item (vertical list or pill tab)
    Swatch,          // BS_AUTORADIOBUTTON theme swatch (graphic only)
    SceneCard,       // BS_AUTORADIOBUTTON preset card with an animated scene
    Slider,          // trackbar
    Heading,         // SS_OWNERDRAW section heading
    Chips,           // owner-draw listbox of user-preset chips
    ToolIcon,        // BS_AUTORADIOBUTTON drawn as a tool glyph button (glyph = Tool value)
    Link,            // push button drawn as a text link (url in Style::url); hand cursor
    Choice,          // BS_AUTORADIOBUTTON drawn as a standalone pill (grids of choices)
};

enum class TextRole : unsigned char { Normal, Secondary, Value, Ok, Error, Hint };

struct Style {
    Role role = Role::Button;
    Surface surface = Surface::Page;
    int segIndex = 0, segCount = 1;
    int glyph = 0;                       // Nav glyph; Chips: 1 = items show a delete "×"
    bool pill = false;                   // Nav: horizontal pill tab
    UiTheme swatch = UiTheme::Night;
    SceneAnim* scene = nullptr;
    const wchar_t* subtitle = nullptr;   // SceneCard subtitle (static string)
    bool compact = false;                // SceneCard in the quick panel
    const wchar_t* url = nullptr;        // Link target (static string)
};

class Skin {
public:
    const Palette* pal = &palette(UiTheme::Night);
    UINT dpi = 96;

    void set(HWND hwnd, const Style& style);
    const Style* find(HWND hwnd) const;
    void setSurface(HWND container, Surface s) { surfaces_[container] = s; }
    void setTextRole(HWND hwnd, TextRole r) { textRoles_[hwnd] = r; }
    void forget(HWND hwnd);
    void clear();

    // Handlers; return true when the message was handled (result in *result).
    bool onNotify(LPARAM lp, LRESULT* result);
    bool onDrawItem(LPARAM lp);
    bool onCtlColor(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
    // Optional per-control text colour override (hotkey status...). Return true to use `out`.
    bool (*textColourHook)(void* ctx, HWND hwnd, COLORREF* out) = nullptr;
    void* hookCtx = nullptr;

    Surface surfaceOf(HWND container) const;
    // Redraws a skinned button immediately through GetDC (animation frames; avoids the button's
    // own buffered WM_PAINT path).
    void repaintNow(HWND hwnd);
    // X coordinate (DIPs) of the "×" hot zone start inside a chip item rect of width w.
    static float chipDeleteZone(float w) { return w - 26.0f; }

private:
    void drawButtonLike(HWND hwnd, const Style& st, HDC dc, const RECT& rc, UINT itemState);
    void drawSlider(HWND hwnd, const Style& st, HDC dc, const RECT& rc);
    static LRESULT CALLBACK sliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
    std::map<HWND, Style> styles_;
    std::map<HWND, Surface> surfaces_;
    std::map<HWND, TextRole> textRoles_;
};

}  // namespace snow::ui
