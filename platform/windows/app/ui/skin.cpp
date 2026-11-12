#include "skin.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ui_util.h"

namespace snow::ui {
namespace {

bool focusCuesHidden(HWND hwnd) {
    return (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
}

D2D1_RECT_F rf(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

}  // namespace

namespace {
constexpr UINT_PTR kSliderSubclassId = 0x534C4452;   // 'SLDR'
}

// Trackbars paint some states (thumb tracking, focus, keyboard moves) straight to the screen,
// bypassing WM_PAINT/custom draw: that left ghost thumbs. Skinned trackbars are therefore painted
// completely by us, in one blit, after every message that can change what they show.
LRESULT CALLBACK Skin::sliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    Skin* self = reinterpret_cast<Skin*>(ref);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        case WM_PRINTCLIENT: {
            const Style* st = self->find(h);
            if (!st) break;
            RECT rc{};
            GetClientRect(h, &rc);
            if (msg == WM_PRINTCLIENT) {
                self->drawSlider(h, *st, reinterpret_cast<HDC>(wp), rc);
                return 0;
            }
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            self->drawSlider(h, *st, dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, sliderProc, kSliderSubclassId);
            break;
        default:
            break;
    }
    const LRESULT r = DefSubclassProc(h, msg, wp, lp);
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case WM_CAPTURECHANGED:
        case WM_UPDATEUISTATE:
        case TBM_SETPOS:
        case TBM_SETPOSNOTIFY:
        case TBM_SETRANGEMIN:
        case TBM_SETRANGEMAX:
            if (msg != WM_MOUSEMOVE || GetCapture() == h) {
                if (const Style* st = self->find(h); st && IsWindowVisible(h)) {
                    RECT rc{};
                    GetClientRect(h, &rc);
                    if (HDC dc = GetDC(h)) {
                        self->drawSlider(h, *st, dc, rc);
                        ReleaseDC(h, dc);
                    }
                    ValidateRect(h, nullptr);   // nothing left for a later partial repaint
                }
            }
            break;
        default:
            break;
    }
    return r;
}

void Skin::set(HWND hwnd, const Style& style) {
    if (!hwnd) return;
    const bool fresh = styles_.find(hwnd) == styles_.end();
    styles_[hwnd] = style;
    if (fresh && style.role == Role::Slider) SetWindowSubclass(hwnd, sliderProc, kSliderSubclassId, reinterpret_cast<DWORD_PTR>(this));
}

const Style* Skin::find(HWND hwnd) const {
    auto it = styles_.find(hwnd);
    return it == styles_.end() ? nullptr : &it->second;
}

void Skin::forget(HWND hwnd) {
    styles_.erase(hwnd);
    surfaces_.erase(hwnd);
    textRoles_.erase(hwnd);
}

void Skin::clear() {
    styles_.clear();
    surfaces_.clear();
    textRoles_.clear();
}

Surface Skin::surfaceOf(HWND container) const {
    auto it = surfaces_.find(container);
    return it == surfaces_.end() ? Surface::Window : it->second;
}

bool Skin::onNotify(LPARAM lp, LRESULT* result) {
    const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
    if (!hdr || hdr->code != static_cast<UINT>(NM_CUSTOMDRAW)) return false;
    const Style* st = find(hdr->hwndFrom);
    if (!st) return false;
    const NMCUSTOMDRAW* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
    if (cd->dwDrawStage != CDDS_PREPAINT) {
        // Never let the control draw a default part (thumb/channel) on its own.
        *result = st->role == Role::Slider ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
        return true;
    }
    RECT rc{};
    GetClientRect(hdr->hwndFrom, &rc);
    if (st->role == Role::Slider) drawSlider(hdr->hwndFrom, *st, cd->hdc, rc);
    else drawButtonLike(hdr->hwndFrom, *st, cd->hdc, rc, cd->uItemState);
    *result = CDRF_SKIPDEFAULT;
    return true;
}

void Skin::repaintNow(HWND hwnd) {
    const Style* st = find(hwnd);
    if (!st || !IsWindowVisible(hwnd)) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    UINT state = 0;
    if (PtInRect(&rc, pt) && WindowFromPoint([&] { POINT s = pt; ClientToScreen(hwnd, &s); return s; }()) == hwnd) state |= CDIS_HOT;
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    drawButtonLike(hwnd, *st, dc, rc, state);
    ReleaseDC(hwnd, dc);
}

void Skin::drawButtonLike(HWND hwnd, const Style& st, HDC dc, const RECT& rc, UINT itemState) {
    const Palette& p = *pal;
    const std::wstring text = windowText(hwnd);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool focused = (GetFocus() == hwnd) && !focusCuesHidden(hwnd);
    const bool hot = (itemState & CDIS_HOT) != 0 && enabled;
    const bool pressed = (itemState & CDIS_SELECTED) != 0;
    Painter& pt = Painter::instance();
    const D2D1_COLOR_F bgc = col(p.surface(st.surface));

    if (st.role == Role::SceneCard) {
        if (!st.scene) return;
        SceneCardState cs;
        cs.selected = checked;
        cs.focused = focused;
        cs.enabled = enabled;
        cs.hot = hot;
        cs.compact = st.compact;
        const std::wstring subtitle = st.subtitle ? st.subtitle : L"";
        const std::wstring key = sceneKey(p, st.surface, *st.scene, text, subtitle, cs);
        st.scene->layer.render(key, rc.right, rc.bottom, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s) {
            drawSceneStatic(pt, s, p, st.surface, *st.scene, text, subtitle, cs);
        });
        Compositor& c = Compositor::instance();
        if (c.begin(st.scene->layer, rc.right, rc.bottom)) {
            drawSceneDynamic(c, static_cast<float>(dpi) / 96.0f, p, *st.scene, cs);
            c.end(dc, rc.left, rc.top);
        }
        return;
    }

    pt.paint(dc, rc, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s) {
        pt.fill(rf(0, 0, s.width, s.height), bgc);
        const float dis = enabled ? 1.0f : 0.45f;
        TextStyle ts{pt.bodySize()};
        switch (st.role) {
            case Role::Toggle: {
                const float tw = 36, th = 20, ty = (s.height - th) / 2, tx = 3;
                const D2D1_RECT_F track = rf(tx, ty, tx + tw, ty + th);
                if (checked) pt.fillRound(track, th / 2, col(p.toggleOn, dis));
                else {
                    pt.fillRound(track, th / 2, col(p.toggleOff, dis));
                    pt.strokeRound(track, th / 2, col(p.controlBorder, dis), 1.0f);
                }
                const float kx = checked ? tx + tw - th / 2 : tx + th / 2;
                pt.fillEllipse(kx, ty + th / 2, th / 2 - 3.5f + (hot ? 0.8f : 0), th / 2 - 3.5f + (hot ? 0.8f : 0),
                               col(checked ? p.knobOn : p.knobOff, enabled ? 1.0f : 0.6f));
                if (focused) pt.strokeRound(rf(tx - 2.5f, ty - 2.5f, tx + tw + 2.5f, ty + th + 2.5f), th / 2 + 2.5f, col(p.focus), 1.5f);
                pt.text(text, rf(tx + tw + 9, 0, s.width, s.height), ts, col(p.text, enabled ? 1.0f : 0.5f));
                break;
            }
            case Role::Segment: {
                const float r = 7.0f, top = 1.5f, bottom = s.height - 1.5f;
                float l = 0.5f, rr = s.width - 0.5f;
                if (st.segIndex > 0) l -= r + 1;
                if (st.segIndex < st.segCount - 1) rr += r + 1;
                const D2D1_RECT_F shape = rf(l, top, rr, bottom);
                pt.rt()->PushAxisAlignedClip(rf(0, 0, s.width, s.height), D2D1_ANTIALIAS_MODE_ALIASED);
                pt.fillRound(shape, r, col(checked ? p.accent : (hot ? mixColor(p.control, p.accent, 0.12f) : p.control), dis));
                pt.strokeRound(shape, r, col(checked ? p.accent : p.controlBorder, dis), 1.0f);
                pt.rt()->PopAxisAlignedClip();
                if (st.segIndex < st.segCount - 1 && !checked) pt.line(s.width - 0.5f, top + 5, s.width - 0.5f, bottom - 5, col(p.controlBorder, dis), 1.0f);
                ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                if (checked) ts.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
                pt.text(text, rf(4, 0, s.width - 4, s.height), ts, col(checked ? p.accentText : p.controlText, enabled ? 1.0f : 0.5f));
                if (focused) pt.strokeRound(rf(3, top + 2.5f, s.width - 3, bottom - 2.5f), r - 2, col(checked ? p.accentText : p.focus), 1.5f);
                break;
            }
            case Role::Button:
            case Role::PrimaryButton: {
                const bool primary = st.role == Role::PrimaryButton;
                COLORREF fillC = primary ? p.accent : p.control;
                if (hot) fillC = mixColor(fillC, primary ? p.text : p.accent, primary ? 0.10f : 0.14f);
                if (pressed) fillC = mixColor(fillC, RGB(0, 0, 0), 0.12f);
                const D2D1_RECT_F shape = rf(1, 1, s.width - 1, s.height - 1);
                pt.fillRound(shape, 7, col(fillC, dis));
                if (!primary) pt.strokeRound(shape, 7, col(p.controlBorder, dis), 1.0f);
                ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                ts.weight = primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
                pt.text(text, rf(6, 0, s.width - 6, s.height), ts, col(primary ? p.accentText : p.controlText, enabled ? 1.0f : 0.5f));
                if (focused) pt.strokeRound(rf(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), 8, col(p.focus), 2.0f);
                break;
            }
            case Role::Nav: {
                if (st.pill) {
                    const D2D1_RECT_F shape = rf(1, 2, s.width - 1, s.height - 2);
                    const float r = (shape.bottom - shape.top) / 2;
                    if (checked) pt.fillRound(shape, r, col(p.navSel));
                    else if (hot) pt.fillRound(shape, r, col(mixColor(p.bg, p.navSel, 0.12f)));
                    ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                    ts.weight = checked ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
                    pt.text(text, rf(4, 0, s.width - 4, s.height), ts, col(checked ? p.navTextSel : p.navText));
                    if (focused) pt.strokeRound(rf(0.5f, 1, s.width - 0.5f, s.height - 1), r + 1, col(p.focus), 1.5f);
                } else {
                    const D2D1_RECT_F shape = rf(2, 1, s.width - 2, s.height - 1);
                    if (checked) {
                        pt.fillRound(shape, 8, col(p.navSel));
                        pt.fillRound(rf(2, 9, 5, s.height - 9), 1.5f, col(p.accent));
                    } else if (hot) {
                        pt.fillRound(shape, 8, col(mixColor(p.nav, p.navSel, 0.5f)));
                    }
                    const D2D1_COLOR_F tc = col(checked ? p.navTextSel : p.navText);
                    drawNavGlyph(pt, st.glyph, 24, s.height / 2, 16, tc);
                    ts.weight = checked ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
                    pt.text(text, rf(44, 0, s.width - 6, s.height), ts, tc);
                    if (focused) pt.strokeRound(rf(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), 9, col(p.focus), 1.5f);
                }
                break;
            }
            case Role::ToolIcon: {
                const bool header = st.surface == Surface::HeaderChip;
                const float cx = s.width / 2, cy = s.height / 2;
                D2D1_COLOR_F glyphC;
                if (header) {
                    if (checked) {
                        pt.fillEllipse(cx, cy, s.height / 2 - 3, s.height / 2 - 3, col(p.headerText, 0.22f));
                        pt.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s.height / 2 - 3, s.height / 2 - 3), pt.brush(col(p.headerText)), 2.0f);
                    } else if (hot) {
                        pt.fillEllipse(cx, cy, s.height / 2 - 3, s.height / 2 - 3, col(p.headerText, 0.12f));
                    }
                    glyphC = col(p.headerText, checked ? 1.0f : 0.8f);
                    if (focused) pt.strokeRound(D2D1::RectF(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), s.height / 2, col(p.headerText), 1.0f);
                } else {
                    const D2D1_RECT_F shape = D2D1::RectF(1.5f, 1.5f, s.width - 1.5f, s.height - 1.5f);
                    pt.fillRound(shape, 8, col(checked ? p.accent : hot ? mixColor(p.control, p.accent, 0.12f) : p.control, dis));
                    if (!checked) pt.strokeRound(shape, 8, col(p.controlBorder, dis), 1.0f);
                    glyphC = col(checked ? p.accentText : p.controlText, enabled ? 1.0f : 0.5f);
                    if (focused) pt.strokeRound(D2D1::RectF(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), 9, col(checked ? p.text : p.focus), 1.5f);
                }
                drawToolGlyph(pt, static_cast<Tool>(st.glyph), cx, cy, std::min(s.width, s.height) * 0.56f, glyphC);
                break;
            }
            case Role::Link: {
                const COLORREF lc = p.dark ? p.heading : p.accent;
                TextStyle ls{pt.bodySize(), DWRITE_FONT_WEIGHT_SEMI_BOLD};
                ls.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                const float tw = pt.measure(text, ls);
                pt.text(text, D2D1::RectF(2, 0, s.width - 2, s.height), ls, col(lc, enabled ? 1.0f : 0.5f));
                if (hot || pressed) {
                    const float y = s.height / 2 + pt.bodySize() * 0.62f;
                    pt.line((s.width - tw) / 2, y, (s.width + tw) / 2, y, col(lc), 1.0f);
                }
                if (focused) pt.strokeRound(D2D1::RectF(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), 6, col(p.focus), 1.5f);
                break;
            }
            case Role::Choice: {
                const D2D1_RECT_F shape = D2D1::RectF(2, 2, s.width - 2, s.height - 2);
                const float r = (shape.bottom - shape.top) / 2;
                pt.fillRound(shape, r, col(checked ? p.accent : hot ? mixColor(p.control, p.accent, 0.12f) : p.control, dis));
                if (!checked) pt.strokeRound(shape, r, col(p.controlBorder, dis), 1.0f);
                ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                if (checked) ts.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
                pt.text(text, D2D1::RectF(6, 0, s.width - 6, s.height), ts, col(checked ? p.accentText : p.controlText, enabled ? 1.0f : 0.5f));
                if (focused) pt.strokeRound(D2D1::RectF(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), r + 1.5f, col(p.focus), 1.5f);
                break;
            }
            case Role::Swatch: {
                const Palette& sp = palette(st.swatch);
                const float cx = s.width / 2, cy = s.height / 2, r = std::min(s.width, s.height) / 2 - 5;
                pt.fillEllipse(cx, cy, r, r, col(sp.bg));
                pt.fillEllipse(cx + r * 0.28f, cy + r * 0.28f, r * 0.5f, r * 0.5f, col(sp.accent));
                if (sp.garland) pt.fillEllipse(cx - r * 0.35f, cy - r * 0.3f, r * 0.22f, r * 0.22f, col(RGB(255, 205, 80)));
                pt.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), pt.brush(col(p.headerText, 0.45f)), 1.0f);
                if (checked) pt.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.5f, r + 2.5f), pt.brush(col(p.headerText)), 2.0f);
                else if (hot) pt.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.5f, r + 2.5f), pt.brush(col(p.headerText, 0.5f)), 1.5f);
                if (focused) pt.strokeRound(rf(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), s.height / 2, col(p.headerText), 1.0f);
                break;
            }
            default:
                break;
        }
    });
}

void Skin::drawSlider(HWND hwnd, const Style& st, HDC dc, const RECT& rc) {
    const Palette& p = *pal;
    RECT channel{}, thumb{};
    SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool focused = GetFocus() == hwnd && !focusCuesHidden(hwnd);
    const float k = 96.0f / static_cast<float>(dpi);
    Painter& pt = Painter::instance();
    pt.paint(dc, rc, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s) {
        pt.fill(rf(0, 0, s.width, s.height), col(p.surface(st.surface)));
        const float dis = enabled ? 1.0f : 0.4f;
        const float cy = s.height / 2;
        const float x0 = static_cast<float>(channel.left) * k + 2, x1 = static_cast<float>(channel.right) * k - 2;
        const float tx = static_cast<float>(thumb.left + thumb.right) * 0.5f * k;
        pt.fillRound(rf(x0, cy - 2, x1, cy + 2), 2, col(p.toggleOff, dis));
        pt.fillRound(rf(x0, cy - 2, std::max(x0 + 4, tx), cy + 2), 2, col(p.accent, dis));
        if (focused) pt.fillEllipse(tx, cy, 11, 11, col(p.accent, 0.22f));
        pt.fillEllipse(tx, cy, 8, 8, col(enabled ? p.accent : p.toggleOff));
        pt.fillEllipse(tx, cy, 3.2f, 3.2f, col(p.accentText, enabled ? 1.0f : 0.5f));
        if (focused) pt.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 10.5f, 10.5f), pt.brush(col(p.focus)), 1.5f);
    });
}

bool Skin::onDrawItem(LPARAM lp) {
    const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
    if (!di) return false;
    const Style* st = find(di->hwndItem);
    if (!st) return false;
    const Palette& p = *pal;
    Painter& pt = Painter::instance();
    if (st->role == Role::Heading && di->CtlType == ODT_STATIC) {
        const std::wstring text = windowText(di->hwndItem);
        pt.paint(di->hDC, di->rcItem, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s) {
            pt.fill(rf(0, 0, s.width, s.height), col(p.surface(st->surface)));
            TextStyle ts{pt.bodySize() * 1.1f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
            ts.display = true;
            const float w = pt.measure(text, ts);
            pt.text(text, rf(0, 0, s.width, s.height), ts, col(p.heading));
            if (w + 12 < s.width) pt.line(w + 10, s.height / 2 + 1, s.width, s.height / 2 + 1, col(p.cardBorder, 0.7f), 1.0f);
        });
        return true;
    }
    if (st->role == Role::Chips && di->CtlType == ODT_LISTBOX) {
        if (di->itemID == static_cast<UINT>(-1)) return true;
        const int len = static_cast<int>(SendMessageW(di->hwndItem, LB_GETTEXTLEN, di->itemID, 0));
        std::wstring text(static_cast<size_t>(std::max(len, 0)) + 1, L'\0');
        SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(std::max(len, 0)));
        const bool listFocused = GetFocus() == di->hwndItem;
        const bool sel = (di->itemState & ODS_SELECTED) != 0 && listFocused;
        const bool focus = (di->itemState & ODS_FOCUS) != 0 && listFocused;
        const bool deletable = st->glyph == 1;
        pt.paint(di->hDC, di->rcItem, dpi, [&](ID2D1RenderTarget*, D2D1_SIZE_F s) {
            pt.fill(rf(0, 0, s.width, s.height), col(p.surface(st->surface)));
            const D2D1_RECT_F chip = rf(2, 3, s.width - 4, s.height - 3);
            const float r = (chip.bottom - chip.top) / 2;
            pt.fillRound(chip, r, col(sel ? mixColor(p.control, p.accent, 0.25f) : p.control));
            pt.strokeRound(chip, r, col(sel ? p.accent : p.controlBorder), sel ? 1.5f : 1.0f);
            if (deletable) {
                pt.fillEllipse(chip.left + 13, s.height / 2, 3.5f, 3.5f, col(p.accent));
                TextStyle ts{pt.bodySize()};
                pt.text(text, rf(chip.left + 22, 0, chipDeleteZone(s.width), s.height), ts, col(p.controlText));
            } else {   // compact chip (quick panel): centred name, no marker
                TextStyle ts{pt.bodySize() * 0.94f};
                ts.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                pt.text(text, rf(chip.left + 6, 0, chip.right - 6, s.height), ts, col(p.controlText));
            }
            if (deletable) {
                const float cx = chip.right - 13, cy = s.height / 2;
                pt.line(cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, col(p.textSecondary), 1.4f);
                pt.line(cx - 3.5f, cy + 3.5f, cx + 3.5f, cy - 3.5f, col(p.textSecondary), 1.4f);
            }
            if (focus) pt.strokeRound(rf(0.5f, 1.5f, s.width - 2.5f, s.height - 1.5f), r + 1, col(p.focus), 1.5f);
        });
        return true;
    }
    return false;
}

bool Skin::onCtlColor(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    HDC dc = reinterpret_cast<HDC>(wp);
    HWND child = reinterpret_cast<HWND>(lp);
    const Palette& p = *pal;
    wchar_t cls[16] = {};
    GetClassNameW(child, cls, ARRAYSIZE(cls));
    const bool isEdit = lstrcmpiW(cls, L"Edit") == 0;
    const bool isList = lstrcmpiW(cls, L"ListBox") == 0;
    const Style* st = find(child);
    if (msg == WM_CTLCOLOREDIT || (isEdit && msg == WM_CTLCOLORSTATIC) || (isList && !(st && st->role == Role::Chips))) {
        SetTextColor(dc, p.fieldText);
        SetBkColor(dc, p.field);
        SetBkMode(dc, OPAQUE);
        *result = reinterpret_cast<LRESULT>(fieldBrush(p));
        return true;
    }
    const Surface surface = st ? st->surface : surfaceOf(GetParent(child));
    COLORREF text = p.text;
    auto it = textRoles_.find(child);
    const TextRole role = it == textRoles_.end() ? TextRole::Normal : it->second;
    switch (role) {
        case TextRole::Secondary:
        case TextRole::Hint: text = p.textSecondary; break;
        case TextRole::Value: text = p.heading; break;
        case TextRole::Ok: text = p.ok; break;
        case TextRole::Error: text = p.error; break;
        default: break;
    }
    if (textColourHook) {
        COLORREF c{};
        if (textColourHook(hookCtx, child, &c)) text = c;
    }
    if (!IsWindowEnabled(child)) text = mixColor(text, p.surface(surface), 0.5f);
    SetTextColor(dc, text);
    SetBkColor(dc, p.surface(surface));
    SetBkMode(dc, TRANSPARENT);
    *result = reinterpret_cast<LRESULT>(surfaceBrush(p, surface));
    return true;
}

}  // namespace snow::ui
