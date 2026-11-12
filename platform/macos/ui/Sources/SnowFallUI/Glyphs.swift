// Vector glyphs drawn like the Windows UI (decor.cpp): navigation icons, tools, logo, theme swatches.
import SwiftUI

enum NavGlyph: Int {
    case snowflake, drift, shovel, gauge, star, gear, info

    static func forPage(_ page: SettingsPage) -> NavGlyph {
        switch page {
        case .snow: return .snowflake
        case .drifts: return .drift
        case .tools: return .shovel
        case .performance: return .gauge
        case .festive: return .star
        case .system: return .gear
        case .about: return .info
        }
    }
}

enum Glyphs {
    static func line(_ ctx: GraphicsContext, _ a: CGPoint, _ b: CGPoint, _ c: Color, _ w: CGFloat) {
        var p = Path()
        p.move(to: a)
        p.addLine(to: b)
        ctx.stroke(p, with: .color(c), style: StrokeStyle(lineWidth: w, lineCap: .round))
    }

    static func nav(_ ctx: GraphicsContext, _ glyph: NavGlyph, center: CGPoint, size: CGFloat, color c: Color) {
        let r = size / 2, w = max(1.2, size / 12), cx = center.x, cy = center.y
        let stroke = StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round)
        switch glyph {
        case .snowflake:
            for k in 0..<3 {
                let a = Double.pi / 2 + Double(k) * Double.pi / 3
                let dx = cos(a) * r, dy = sin(a) * r
                line(ctx, CGPoint(x: cx - dx, y: cy - dy), CGPoint(x: cx + dx, y: cy + dy), c, w)
            }
            for k in 0..<6 {
                let a = -Double.pi / 2 + Double(k) * Double.pi / 3
                let bx = cx + cos(a) * r * 0.62, by = cy + sin(a) * r * 0.62
                for s in [-1.0, 1.0] {
                    let ba = a + s * Double.pi * 0.78
                    line(ctx, CGPoint(x: bx, y: by), CGPoint(x: bx + cos(ba) * r * 0.3, y: by + sin(ba) * r * 0.3), c, w)
                }
            }
        case .drift:
            var p = Path()
            p.move(to: CGPoint(x: cx - r, y: cy + r * 0.45))
            p.addCurve(to: CGPoint(x: cx + r * 0.25, y: cy - r * 0.15),
                       control1: CGPoint(x: cx - r * 0.5, y: cy - r * 0.6), control2: CGPoint(x: cx, y: cy + r * 0.2))
            p.addCurve(to: CGPoint(x: cx + r, y: cy + r * 0.45),
                       control1: CGPoint(x: cx + r * 0.5, y: cy - r * 0.45), control2: CGPoint(x: cx + r * 0.85, y: cy))
            ctx.stroke(p, with: .color(c), style: stroke)
            line(ctx, CGPoint(x: cx - r, y: cy + r * 0.75), CGPoint(x: cx + r, y: cy + r * 0.75), c, w)
        case .shovel:
            line(ctx, CGPoint(x: cx - r * 0.75, y: cy - r * 0.75), CGPoint(x: cx + r * 0.2, y: cy + r * 0.2), c, w)
            line(ctx, CGPoint(x: cx - r * 0.95, y: cy - r * 0.55), CGPoint(x: cx - r * 0.55, y: cy - r * 0.95), c, w)
            var p = Path()
            p.move(to: CGPoint(x: cx + r * 0.05, y: cy + r * 0.45))
            p.addLine(to: CGPoint(x: cx + r * 0.45, y: cy + r * 0.05))
            p.addLine(to: CGPoint(x: cx + r, y: cy + r * 0.6))
            p.addLine(to: CGPoint(x: cx + r * 0.6, y: cy + r))
            p.closeSubpath()
            ctx.stroke(p, with: .color(c), style: stroke)
        case .gauge:
            var p = Path()
            for i in 0...24 {   // upper half circle, left to right
                let a = Double.pi + Double.pi * Double(i) / 24
                let pt = CGPoint(x: cx + cos(a) * r, y: cy + r * 0.35 + sin(a) * r)
                if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
            }
            ctx.stroke(p, with: .color(c), style: stroke)
            line(ctx, CGPoint(x: cx, y: cy + r * 0.35), CGPoint(x: cx + r * 0.5, y: cy - r * 0.25), c, w)
            ctx.fill(Path(ellipseIn: CGRect(x: cx - w * 1.2, y: cy + r * 0.35 - w * 1.2, width: w * 2.4, height: w * 2.4)),
                     with: .color(c))
        case .star:
            ctx.stroke(starPath(center: center, r: r), with: .color(c), style: stroke)
        case .gear:
            for k in 0..<8 {
                let a = Double(k) * Double.pi / 4
                line(ctx, CGPoint(x: cx + cos(a) * r * 0.62, y: cy + sin(a) * r * 0.62),
                     CGPoint(x: cx + cos(a) * r, y: cy + sin(a) * r), c, w * 1.4)
            }
            ctx.stroke(Path(ellipseIn: CGRect(x: cx - r * 0.62, y: cy - r * 0.62, width: r * 1.24, height: r * 1.24)),
                       with: .color(c), lineWidth: w)
            ctx.stroke(Path(ellipseIn: CGRect(x: cx - r * 0.22, y: cy - r * 0.22, width: r * 0.44, height: r * 0.44)),
                       with: .color(c), lineWidth: w)
        case .info:
            ctx.stroke(Path(ellipseIn: CGRect(x: cx - r * 0.92, y: cy - r * 0.92, width: r * 1.84, height: r * 1.84)),
                       with: .color(c), lineWidth: w)
            ctx.fill(Path(ellipseIn: CGRect(x: cx - w * 0.8, y: cy - r * 0.42 - w * 0.8, width: w * 1.6, height: w * 1.6)),
                     with: .color(c))
            line(ctx, CGPoint(x: cx, y: cy - r * 0.08), CGPoint(x: cx, y: cy + r * 0.5), c, w)
        }
    }

    static func starPath(center: CGPoint, r: CGFloat) -> Path {
        var p = Path()
        for i in 0..<10 {
            let a = -Double.pi / 2 + Double(i) * Double.pi / 5
            let rr = i % 2 == 1 ? r * 0.42 : r
            let pt = CGPoint(x: center.x + cos(a) * rr, y: center.y + sin(a) * rr + r * 0.08)
            if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
        }
        p.closeSubpath()
        return p
    }

    static func tool(_ ctx: GraphicsContext, _ tool: ToolKind, center: CGPoint, size: CGFloat, color c: Color) {
        let r = size / 2, w = max(1.3, size / 11), cx = center.x, cy = center.y
        switch tool {
        case .snowball:
            ctx.stroke(Path(ellipseIn: CGRect(x: cx - r * 0.8, y: cy - r * 0.8, width: r * 1.6, height: r * 1.6)),
                       with: .color(c), lineWidth: w)
            ctx.fill(Path(ellipseIn: CGRect(x: cx + r * 0.18 - r * 0.45, y: cy + r * 0.22 - r * 0.38, width: r * 0.9, height: r * 0.76)),
                     with: .color(c.opacity(0.35)))
            ctx.fill(Path(ellipseIn: CGRect(x: cx - r * 0.3 - w * 0.9, y: cy - r * 0.3 - w * 0.9, width: w * 1.8, height: w * 1.8)),
                     with: .color(c))
        case .mitten:
            let cuff = CGRect(x: cx - r * 0.42, y: cy + r * 0.52, width: r * 0.92, height: r * 0.38)
            ctx.stroke(Path(roundedRect: cuff, cornerRadius: w), with: .color(c), lineWidth: w)
            var p = Path()
            p.move(to: CGPoint(x: cx - r * 0.38, y: cy + r * 0.5))
            p.addLine(to: CGPoint(x: cx - r * 0.42, y: cy - r * 0.35))
            p.addCurve(to: CGPoint(x: cx + r * 0.45, y: cy - r * 0.35),
                       control1: CGPoint(x: cx - r * 0.42, y: cy - r * 0.95), control2: CGPoint(x: cx + r * 0.45, y: cy - r * 0.95))
            p.addLine(to: CGPoint(x: cx + r * 0.45, y: cy - r * 0.05))
            p.addCurve(to: CGPoint(x: cx + r * 0.75, y: cy + r * 0.15),
                       control1: CGPoint(x: cx + r * 0.65, y: cy - r * 0.3), control2: CGPoint(x: cx + r * 0.95, y: cy - r * 0.1))
            p.addLine(to: CGPoint(x: cx + r * 0.46, y: cy + r * 0.5))
            ctx.stroke(p, with: .color(c), style: StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round))
        case .shovel:
            nav(ctx, .shovel, center: center, size: size * 0.9, color: c)
        }
    }

    /// App logo: a glowing snowflake (Globe: a little snow globe).
    static func logo(_ ctx: GraphicsContext, palette p: Palette, center: CGPoint, size: CGFloat) {
        let r = size / 2, cx = center.x, cy = center.y
        if p.theme == .globe {
            ctx.fill(Path(ellipseIn: CGRect(x: cx - r * 0.86, y: cy - r * 0.98, width: r * 1.72, height: r * 1.72)),
                     with: .color(.white.opacity(0.22)))
            ctx.stroke(Path(ellipseIn: CGRect(x: cx - r * 0.86, y: cy - r * 0.98, width: r * 1.72, height: r * 1.72)),
                       with: .color(.white.opacity(0.9)), lineWidth: 1.4)
            var base = Path()
            base.move(to: CGPoint(x: cx - r * 0.7, y: cy + r * 0.62))
            base.addLine(to: CGPoint(x: cx + r * 0.7, y: cy + r * 0.62))
            base.addLine(to: CGPoint(x: cx + r * 0.9, y: cy + r))
            base.addLine(to: CGPoint(x: cx - r * 0.9, y: cy + r))
            base.closeSubpath()
            ctx.fill(base, with: .color(p.headerChip.c))
            nav(ctx, .snowflake, center: CGPoint(x: cx, y: cy - r * 0.12), size: size * 0.55, color: .white)
            return
        }
        glow(ctx, center: center, radius: r * 1.4, color: p.headerText.c(0.18))
        nav(ctx, .snowflake, center: center, size: size, color: p.headerText.c)
    }

    static func glow(_ ctx: GraphicsContext, center: CGPoint, radius: CGFloat, color: Color) {
        let rect = CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2)
        ctx.fill(Path(ellipseIn: rect),
                 with: .radialGradient(Gradient(colors: [color, color.opacity(0)]), center: center, startRadius: 0,
                                       endRadius: radius))
    }
}

/// A glyph as a view.
struct GlyphView: View {
    enum Kind { case nav(NavGlyph), tool(ToolKind) }
    let kind: Kind
    let color: Color
    var size: CGFloat = 18

    var body: some View {
        Canvas { ctx, s in
            let c = CGPoint(x: s.width / 2, y: s.height / 2)
            switch kind {
            case .nav(let g): Glyphs.nav(ctx, g, center: c, size: size, color: color)
            case .tool(let t): Glyphs.tool(ctx, t, center: c, size: size, color: color)
            }
        }
        .frame(width: size + 6, height: size + 6)
        .accessibilityHidden(true)
    }
}

/// Theme swatch for the header: a little circle in the theme's colours.
struct ThemeSwatch: View {
    let theme: UITheme

    var body: some View {
        Canvas { ctx, s in
            let p = Palette.of(theme)
            let rect = CGRect(origin: .zero, size: s).insetBy(dx: 1, dy: 1)
            ctx.fill(Path(ellipseIn: rect), with: .linearGradient(Gradient(colors: [p.headerTop.c, p.headerBottom.c]),
                                                                  startPoint: CGPoint(x: 0, y: 0),
                                                                  endPoint: CGPoint(x: 0, y: s.height)))
            let dot = CGRect(x: s.width * 0.42, y: s.height * 0.42, width: s.width * 0.42, height: s.height * 0.42)
            ctx.fill(Path(ellipseIn: dot), with: .color(theme == .night ? p.accent.c : p.page.c))
            if theme == .postcard {
                ctx.fill(Path(ellipseIn: CGRect(x: s.width * 0.25, y: s.height * 0.28, width: 4, height: 4)),
                         with: .color(Color(.sRGB, red: 1, green: 0.8, blue: 0.3, opacity: 1)))
            }
            ctx.stroke(Path(ellipseIn: rect), with: .color(.white.opacity(0.5)), lineWidth: 1)
        }
        .accessibilityHidden(true)
    }
}
