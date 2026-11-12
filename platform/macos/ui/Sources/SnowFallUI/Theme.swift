// The three looks of the settings window and the quick panel — the same palettes as the Windows
// UI (platform/windows/app/ui/theme.cpp). Themes define every colour themselves, independent of
// the system light/dark appearance.
import AppKit
import SwiftUI

struct RGB: Equatable {
    var r: Double, g: Double, b: Double

    init(_ hex: UInt32) {
        r = Double((hex >> 16) & 0xFF) / 255
        g = Double((hex >> 8) & 0xFF) / 255
        b = Double(hex & 0xFF) / 255
    }

    init(r: Double, g: Double, b: Double) {
        self.r = r
        self.g = g
        self.b = b
    }

    var c: Color { Color(.sRGB, red: r, green: g, blue: b, opacity: 1) }
    func c(_ alpha: Double) -> Color { Color(.sRGB, red: r, green: g, blue: b, opacity: alpha) }
    var ns: NSColor { NSColor(srgbRed: r, green: g, blue: b, alpha: 1) }

    /// Linear mix: t = 0 -> self, t = 1 -> other.
    func mix(_ o: RGB, _ t: Double) -> RGB {
        RGB(r: r + (o.r - r) * t, g: g + (o.g - g) * t, b: b + (o.b - b) * t)
    }
}

struct Palette: Equatable {
    var theme: UITheme
    var dark: Bool
    var pillTabs = false     // Globe / Postcard: pills on top instead of a sidebar
    var garland = false      // Postcard: garland in the header
    var pageIsCard = false
    var bg, nav, navSel, navText, navTextSel, page, pageBorder, card, cardBorder: RGB
    var text, textSecondary, heading, accent, accentText: RGB
    var control, controlBorder, controlText, field, fieldBorder, fieldText: RGB
    var toggleOn, toggleOff, knobOn, knobOff, focus, error, ok: RGB
    var headerTop, headerBottom, headerText, headerTextSecondary, headerChip: RGB
    var snow, skyTop, skyBottom, ground: RGB

    static func of(_ theme: UITheme) -> Palette {
        switch theme {
        case .night: return night
        case .globe: return globe
        case .postcard: return postcard
        }
    }

    static let night = Palette(
        theme: .night, dark: true,
        bg: RGB(0x0C1A2E), nav: RGB(0x081222), navSel: RGB(0x163055), navText: RGB(0x85B7EB), navTextSel: RGB(0xE6F1FB),
        page: RGB(0x0C1A2E), pageBorder: RGB(0x0C1A2E), card: RGB(0x11243F), cardBorder: RGB(0x23436C),
        text: RGB(0xE6F1FB), textSecondary: RGB(0x85B7EB), heading: RGB(0xB9DCFF), accent: RGB(0xD4EBFF),
        accentText: RGB(0x0C1A2E),
        control: RGB(0x163055), controlBorder: RGB(0x2C5182), controlText: RGB(0xE6F1FB),
        field: RGB(0x081222), fieldBorder: RGB(0x2C5182), fieldText: RGB(0xE6F1FB),
        toggleOn: RGB(0xD4EBFF), toggleOff: RGB(0x1F3A60), knobOn: RGB(0x0C1A2E), knobOff: RGB(0x85B7EB),
        focus: RGB(0xFFFFFF), error: RGB(0xFF9C8F), ok: RGB(0x86E3B0),
        headerTop: RGB(0x060E1C), headerBottom: RGB(0x183460), headerText: RGB(0xE6F1FB),
        headerTextSecondary: RGB(0x85B7EB), headerChip: RGB(0x10223D),
        snow: RGB(0xFFFFFF), skyTop: RGB(0x040B17), skyBottom: RGB(0x1C3C6A), ground: RGB(0xDDEBF8))

    static let globe = Palette(
        theme: .globe, dark: false, pillTabs: true, pageIsCard: true,
        bg: RGB(0xE6F1FB), nav: RGB(0xE6F1FB), navSel: RGB(0x185FA5), navText: RGB(0x0C447C), navTextSel: RGB(0xFFFFFF),
        page: RGB(0xFFFFFF), pageBorder: RGB(0x85B7EB), card: RGB(0xFFFFFF), cardBorder: RGB(0x85B7EB),
        text: RGB(0x0C447C), textSecondary: RGB(0x3B6996), heading: RGB(0x185FA5), accent: RGB(0x185FA5),
        accentText: RGB(0xFFFFFF),
        control: RGB(0xEAF3FC), controlBorder: RGB(0x85B7EB), controlText: RGB(0x0C447C),
        field: RGB(0xF7FBFE), fieldBorder: RGB(0x85B7EB), fieldText: RGB(0x0C447C),
        toggleOn: RGB(0x185FA5), toggleOff: RGB(0xC6DBEF), knobOn: RGB(0xFFFFFF), knobOff: RGB(0xFFFFFF),
        focus: RGB(0x0C447C), error: RGB(0xB3261E), ok: RGB(0x1D7743),
        headerTop: RGB(0x14558F), headerBottom: RGB(0x5296D6), headerText: RGB(0xFFFFFF),
        headerTextSecondary: RGB(0xDCEBFA), headerChip: RGB(0x2A6DAE),
        snow: RGB(0xFFFFFF), skyTop: RGB(0x2F6AA8), skyBottom: RGB(0x9CC4EA), ground: RGB(0xFFFFFF))

    static let postcard = Palette(
        theme: .postcard, dark: false, pillTabs: true, garland: true, pageIsCard: true,
        bg: RGB(0xFAECE7), nav: RGB(0xFAECE7), navSel: RGB(0x993C1D), navText: RGB(0x712B13), navTextSel: RGB(0xFFF4EE),
        page: RGB(0xFFF8F5), pageBorder: RGB(0xF0B9A5), card: RGB(0xF5C4B3), cardBorder: RGB(0xE3A48D),
        text: RGB(0x712B13), textSecondary: RGB(0x8C4428), heading: RGB(0x993C1D), accent: RGB(0x993C1D),
        accentText: RGB(0xFFF4EE),
        control: RGB(0xFBE3DA), controlBorder: RGB(0xE3A48D), controlText: RGB(0x712B13),
        field: RGB(0xFFFFFF), fieldBorder: RGB(0xE3A48D), fieldText: RGB(0x712B13),
        toggleOn: RGB(0x993C1D), toggleOff: RGB(0xEDCBBE), knobOn: RGB(0xFFF4EE), knobOff: RGB(0xFFFFFF),
        focus: RGB(0x4A1B0C), error: RGB(0xB3261E), ok: RGB(0x2E7D32),
        headerTop: RGB(0x3A1408), headerBottom: RGB(0x5E2512), headerText: RGB(0xFAECE7),
        headerTextSecondary: RGB(0xF5C4B3), headerChip: RGB(0x4A1B0C),
        snow: RGB(0xFFFFFF), skyTop: RGB(0x4A1B0C), skyBottom: RGB(0xB65A3C), ground: RGB(0xFFF4EE))

    /// System appearance for native pieces (text cursor, alerts, scrollers) inside the window.
    var appearance: NSAppearance? { NSAppearance(named: dark ? .darkAqua : .aqua) }

    /// Inner background of the page area (Night: none, the page sits on the window).
    var pageFill: RGB { pageIsCard ? page : bg }
}

private struct PaletteKey: EnvironmentKey {
    static let defaultValue = Palette.night
}

extension EnvironmentValues {
    var palette: Palette {
        get { self[PaletteKey.self] }
        set { self[PaletteKey.self] = newValue }
    }
}
