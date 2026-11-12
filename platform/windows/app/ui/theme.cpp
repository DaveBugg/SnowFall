#include "theme.h"

#include <map>

namespace snow::ui {
namespace {

constexpr COLORREF hex(unsigned v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

Palette makeNight() {
    Palette p{};
    p.theme = UiTheme::Night;
    p.dark = true;
    p.bg = hex(0x0C1A2E);
    p.nav = hex(0x081222);
    p.navSel = hex(0x163055);
    p.navText = hex(0x85B7EB);
    p.navTextSel = hex(0xE6F1FB);
    p.page = hex(0x0C1A2E);
    p.pageBorder = hex(0x0C1A2E);
    p.card = hex(0x11243F);
    p.cardBorder = hex(0x23436C);
    p.text = hex(0xE6F1FB);
    p.textSecondary = hex(0x85B7EB);
    p.heading = hex(0xB9DCFF);
    p.accent = hex(0xD4EBFF);
    p.accentText = hex(0x0C1A2E);
    p.control = hex(0x163055);
    p.controlBorder = hex(0x2C5182);
    p.controlText = hex(0xE6F1FB);
    p.field = hex(0x081222);
    p.fieldBorder = hex(0x2C5182);
    p.fieldText = hex(0xE6F1FB);
    p.toggleOn = hex(0xD4EBFF);
    p.toggleOff = hex(0x1F3A60);
    p.knobOn = hex(0x0C1A2E);
    p.knobOff = hex(0x85B7EB);
    p.focus = hex(0xFFFFFF);
    p.error = hex(0xFF9C8F);
    p.ok = hex(0x86E3B0);
    p.headerTop = hex(0x060E1C);
    p.headerBottom = hex(0x183460);
    p.headerText = hex(0xE6F1FB);
    p.headerTextSecondary = hex(0x85B7EB);
    p.headerChip = hex(0x10223D);
    p.snow = hex(0xFFFFFF);
    p.skyTop = hex(0x040B17);
    p.skyBottom = hex(0x1C3C6A);
    p.ground = hex(0xDDEBF8);
    p.desktopTop = hex(0x1D3D6B);
    p.desktopBottom = hex(0x0B1A33);
    p.fakeWindow = hex(0x2B4870);
    p.fakeTitle = hex(0x3B5D8C);
    p.fakeTaskbar = hex(0x050C18);
    return p;
}

Palette makeGlobe() {
    Palette p{};
    p.theme = UiTheme::Globe;
    p.dark = false;
    p.pillTabs = true;
    p.pageIsCard = true;
    p.bg = hex(0xE6F1FB);
    p.nav = hex(0xE6F1FB);
    p.navSel = hex(0x185FA5);
    p.navText = hex(0x0C447C);
    p.navTextSel = hex(0xFFFFFF);
    p.page = hex(0xFFFFFF);
    p.pageBorder = hex(0x85B7EB);
    p.card = hex(0xFFFFFF);
    p.cardBorder = hex(0x85B7EB);
    p.text = hex(0x0C447C);
    p.textSecondary = hex(0x3B6996);
    p.heading = hex(0x185FA5);
    p.accent = hex(0x185FA5);
    p.accentText = hex(0xFFFFFF);
    p.control = hex(0xEAF3FC);
    p.controlBorder = hex(0x85B7EB);
    p.controlText = hex(0x0C447C);
    p.field = hex(0xF7FBFE);
    p.fieldBorder = hex(0x85B7EB);
    p.fieldText = hex(0x0C447C);
    p.toggleOn = hex(0x185FA5);
    p.toggleOff = hex(0xC6DBEF);
    p.knobOn = hex(0xFFFFFF);
    p.knobOff = hex(0xFFFFFF);
    p.focus = hex(0x0C447C);
    p.error = hex(0xB3261E);
    p.ok = hex(0x1D7743);
    p.headerTop = hex(0x14558F);
    p.headerBottom = hex(0x5296D6);
    p.headerText = hex(0xFFFFFF);
    p.headerTextSecondary = hex(0xDCEBFA);
    p.headerChip = hex(0x2A6DAE);
    p.snow = hex(0xFFFFFF);
    p.skyTop = hex(0x2F6AA8);
    p.skyBottom = hex(0x9CC4EA);
    p.ground = hex(0xFFFFFF);
    p.desktopTop = hex(0x4F86BF);
    p.desktopBottom = hex(0x9DC3E8);
    p.fakeWindow = hex(0x8DB3DA);
    p.fakeTitle = hex(0x6E9BCB);
    p.fakeTaskbar = hex(0x0C447C);
    return p;
}

Palette makePostcard() {
    Palette p{};
    p.theme = UiTheme::Postcard;
    p.dark = false;
    p.pillTabs = true;
    p.garland = true;
    p.pageIsCard = true;
    p.bg = hex(0xFAECE7);
    p.nav = hex(0xFAECE7);
    p.navSel = hex(0x993C1D);
    p.navText = hex(0x712B13);
    p.navTextSel = hex(0xFFF4EE);
    p.page = hex(0xFFF8F5);
    p.pageBorder = hex(0xF0B9A5);
    p.card = hex(0xF5C4B3);
    p.cardBorder = hex(0xE3A48D);
    p.text = hex(0x712B13);
    p.textSecondary = hex(0x8C4428);
    p.heading = hex(0x993C1D);
    p.accent = hex(0x993C1D);
    p.accentText = hex(0xFFF4EE);
    p.control = hex(0xFBE3DA);
    p.controlBorder = hex(0xE3A48D);
    p.controlText = hex(0x712B13);
    p.field = hex(0xFFFFFF);
    p.fieldBorder = hex(0xE3A48D);
    p.fieldText = hex(0x712B13);
    p.toggleOn = hex(0x993C1D);
    p.toggleOff = hex(0xEDCBBE);
    p.knobOn = hex(0xFFF4EE);
    p.knobOff = hex(0xFFFFFF);
    p.focus = hex(0x4A1B0C);
    p.error = hex(0xB3261E);
    p.ok = hex(0x2E7D32);
    p.headerTop = hex(0x3A1408);
    p.headerBottom = hex(0x5E2512);
    p.headerText = hex(0xFAECE7);
    p.headerTextSecondary = hex(0xF5C4B3);
    p.headerChip = hex(0x4A1B0C);
    p.snow = hex(0xFFFFFF);
    p.skyTop = hex(0x4A1B0C);
    p.skyBottom = hex(0xB65A3C);
    p.ground = hex(0xFFF4EE);
    p.desktopTop = hex(0xB65A3C);
    p.desktopBottom = hex(0x6B2A15);
    p.fakeWindow = hex(0xD98E73);
    p.fakeTitle = hex(0xC2674A);
    p.fakeTaskbar = hex(0x3A1408);
    return p;
}

}  // namespace

COLORREF Palette::surface(Surface s) const {
    switch (s) {
        case Surface::Window: return bg;
        case Surface::Nav: return nav;
        case Surface::Page: return page;
        case Surface::Card: return card;
        case Surface::HeaderChip: return headerChip;
    }
    return bg;
}

const Palette& palette(UiTheme theme) {
    static const Palette night = makeNight();
    static const Palette globe = makeGlobe();
    static const Palette postcard = makePostcard();
    switch (theme) {
        case UiTheme::Globe: return globe;
        case UiTheme::Postcard: return postcard;
        default: return night;
    }
}

namespace {
HBRUSH cachedBrush(COLORREF c) {
    static std::map<COLORREF, HBRUSH> brushes;   // a dozen colours at most; live for the process
    auto it = brushes.find(c);
    if (it != brushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    brushes[c] = b;
    return b;
}
}  // namespace

HBRUSH surfaceBrush(const Palette& p, Surface s) { return cachedBrush(p.surface(s)); }
HBRUSH fieldBrush(const Palette& p) { return cachedBrush(p.field); }

}  // namespace snow::ui
