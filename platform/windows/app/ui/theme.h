// Colour palettes of the three settings-window themes. Every theme offers the same
// features; only colours and decoration differ.
#pragma once
#include <windows.h>

#include "core/settings.h"

namespace snow::ui {

// What lies behind a control (used to fill its background when custom drawn).
enum class Surface : unsigned char { Window, Nav, Page, Card, HeaderChip };

struct Palette {
    UiTheme theme = UiTheme::Night;
    bool dark = true;        // dark scroll bars etc.
    bool pillTabs = false;   // false: vertical navigation column (A); true: pill tabs on top (B, C)
    bool garland = false;    // header garland (C)
    bool pageIsCard = false; // the content page is drawn as a bordered card on the window background

    COLORREF bg, nav, navSel, navText, navTextSel;
    COLORREF page, pageBorder;
    COLORREF card, cardBorder;
    COLORREF text, textSecondary, heading;
    COLORREF accent, accentText;
    COLORREF control, controlBorder, controlText;
    COLORREF field, fieldBorder, fieldText;
    COLORREF toggleOn, toggleOff, knobOn, knobOff;
    COLORREF focus, error, ok;
    COLORREF headerTop, headerBottom, headerText, headerTextSecondary, headerChip;
    COLORREF snow;
    COLORREF skyTop, skyBottom, ground;
    COLORREF desktopTop, desktopBottom, fakeWindow, fakeTitle, fakeTaskbar;

    COLORREF surface(Surface s) const;
};

const Palette& palette(UiTheme theme);

// Brushes for WM_CTLCOLOR* (owned by this module, never delete them).
HBRUSH surfaceBrush(const Palette& p, Surface s);
HBRUSH fieldBrush(const Palette& p);

}  // namespace snow::ui
