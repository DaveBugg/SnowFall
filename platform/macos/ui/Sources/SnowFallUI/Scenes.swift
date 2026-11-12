// Animated decorations (Canvas + TimelineView at ~20 fps, paused while the window is hidden or
// when Reduce Motion is on): weather scene cards, header snow, countdown tree, festive scene.
// Drawing follows platform/windows/app/ui/decor.cpp.
import SwiftUI

// MARK: - flakes (stateless: positions are a function of time)

struct DecoFlake {
    var x, y, r, speed, phase, sway, alpha: Double
}

struct FlakeField {
    let flakes: [DecoFlake]

    init(seed: UInt32, count: Int, minR: Double, maxR: Double, minSpeed: Double, maxSpeed: Double, minAlpha: Double = 0.55) {
        var s = seed == 0 ? 1 : seed
        func rnd() -> UInt32 {
            s ^= s << 13
            s ^= s >> 17
            s ^= s << 5
            return s
        }
        func frand(_ a: Double, _ b: Double) -> Double { a + (b - a) * Double(rnd() % 10000) / 10000 }
        var list: [DecoFlake] = []
        for _ in 0..<count {
            let x = frand(0, 1), y = frand(0, 1), r = frand(minR, maxR)
            let k = (r - minR) / max(0.01, maxR - minR)
            let speed = minSpeed + (maxSpeed - minSpeed) * (0.35 * frand(0, 1) + 0.65 * k)
            list.append(DecoFlake(x: x, y: y, r: r, speed: speed, phase: frand(0, 2 * Double.pi), sway: frand(0.004, 0.02),
                                  alpha: frand(minAlpha, 1)))
        }
        flakes = list
    }

    static func wrap(_ v: Double, _ lo: Double, _ hi: Double) -> Double {
        let span = hi - lo
        var r = (v - lo).truncatingRemainder(dividingBy: span)
        if r < 0 { r += span }
        return lo + r
    }

    /// Draws into `rect`; `streak` > 0 draws wind-blown streaks instead of dots.
    func draw(_ ctx: GraphicsContext, in rect: CGRect, time t: Double, wind: Double, color: Color, scale: Double = 1,
              streak: Double = 0) {
        for f in flakes {
            let fy = FlakeField.wrap(f.y + f.speed * t, -0.05, 1.05)
            let fx = FlakeField.wrap(f.x + wind * t * (0.6 + f.speed), -0.05, 1.05)
            let x = rect.minX + (fx + f.sway * sin(t * 1.3 + f.phase)) * rect.width
            let y = rect.minY + fy * rect.height
            if streak > 0 {
                let len = streak * f.speed * rect.height * 0.12
                var p = Path()
                p.move(to: CGPoint(x: x - wind * len * 2, y: y - len))
                p.addLine(to: CGPoint(x: x, y: y))
                ctx.stroke(p, with: .color(color.opacity(f.alpha * 0.8)),
                           style: StrokeStyle(lineWidth: f.r * 0.9 * scale, lineCap: .round))
            } else {
                let r = f.r * scale
                if f.r > 1.6 {
                    ctx.fill(Path(ellipseIn: CGRect(x: x - r * 1.8, y: y - r * 1.8, width: r * 3.6, height: r * 3.6)),
                             with: .color(color.opacity(f.alpha * 0.12)))
                }
                ctx.fill(Path(ellipseIn: CGRect(x: x - r, y: y - r, width: r * 2, height: r * 2)),
                         with: .color(color.opacity(f.alpha)))
            }
        }
    }

    static func forPreset(_ p: WeatherPreset) -> FlakeField {
        switch p {
        case .light: return light
        case .normal: return normal
        case .blizzard: return blizzard
        case .thaw: return thaw
        }
    }

    static let light = FlakeField(seed: 1234, count: 16, minR: 0.7, maxR: 1.7, minSpeed: 0.08, maxSpeed: 0.16)
    static let normal = FlakeField(seed: 1311, count: 42, minR: 0.7, maxR: 2.0, minSpeed: 0.16, maxSpeed: 0.32)
    static let blizzard = FlakeField(seed: 1388, count: 90, minR: 0.5, maxR: 1.4, minSpeed: 0.5, maxSpeed: 0.9)
    static let thaw = FlakeField(seed: 1465, count: 12, minR: 1.3, maxR: 2.4, minSpeed: 0.32, maxSpeed: 0.48, minAlpha: 0.5)
    static let header = FlakeField(seed: 777, count: 60, minR: 0.6, maxR: 1.8, minSpeed: 0.10, maxSpeed: 0.30, minAlpha: 0.35)
}

// MARK: - shapes shared by the scenes

enum SceneDraw {
    static func hill(_ ctx: GraphicsContext, rect: CGRect, top: CGFloat, amp: CGFloat, phase: Double, color: Color) {
        var p = Path()
        let w = rect.width
        p.move(to: CGPoint(x: rect.minX, y: rect.maxY))
        p.addLine(to: CGPoint(x: rect.minX, y: top + amp * sin(phase)))
        p.addCurve(to: CGPoint(x: rect.maxX, y: top + amp * cos(phase)),
                   control1: CGPoint(x: rect.minX + w * 0.3, y: top - amp),
                   control2: CGPoint(x: rect.minX + w * 0.55, y: top + amp * 1.4))
        p.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        p.closeSubpath()
        ctx.fill(p, with: .color(color))
    }

    static func triangle(_ ctx: GraphicsContext, _ a: CGPoint, _ b: CGPoint, _ c: CGPoint, _ color: Color) {
        var p = Path()
        p.move(to: a)
        p.addLine(to: b)
        p.addLine(to: c)
        p.closeSubpath()
        ctx.fill(p, with: .color(color))
    }

    static func firTree(_ ctx: GraphicsContext, cx: CGFloat, baseY: CGFloat, h: CGFloat, body: Color, cap: Color) {
        for i in 0..<3 {
            let t = CGFloat(i)
            let top = baseY - h + t * h * 0.25
            let bottom = baseY - h * 0.45 + t * h * 0.22
            let half = h * (0.22 + 0.1 * t)
            triangle(ctx, CGPoint(x: cx, y: top), CGPoint(x: cx + half, y: bottom), CGPoint(x: cx - half, y: bottom), body)
            triangle(ctx, CGPoint(x: cx, y: top), CGPoint(x: cx + half * 0.35, y: top + (bottom - top) * 0.35),
                     CGPoint(x: cx - half * 0.35, y: top + (bottom - top) * 0.35), cap)
        }
        ctx.fill(Path(CGRect(x: cx - h * 0.04, y: baseY - h * 0.1, width: h * 0.08, height: h * 0.1)), with: .color(body))
    }

    static func ellipse(_ ctx: GraphicsContext, _ cx: CGFloat, _ cy: CGFloat, _ rx: CGFloat, _ ry: CGFloat, _ c: Color) {
        ctx.fill(Path(ellipseIn: CGRect(x: cx - rx, y: cy - ry, width: rx * 2, height: ry * 2)), with: .color(c))
    }

    static let lightColours: [RGB] = [RGB(0xFFCD50), RGB(0xFF5A50), RGB(0x5ADC78), RGB(0x5AA0FF),
                                      RGB(0xBE78FF), RGB(0xFFFFFF), RGB(0xFF963C), RGB(0x5AE6E6)]

    /// Garland wire with bulbs along y; lit bulbs blink with `time` (nil = all lit, static).
    static func garland(_ ctx: GraphicsContext, x0: CGFloat, x1: CGFloat, y: CGFloat, sag: CGFloat, time: Double?) {
        let span: CGFloat = 96
        let spans = max(1, Int(((x1 - x0) / span).rounded()))
        let sw = (x1 - x0) / CGFloat(spans)
        var bulb = 0
        for s in 0..<spans {
            let a = x0 + sw * CGFloat(s)
            var wire = Path()
            wire.move(to: CGPoint(x: a, y: y))
            wire.addQuadCurve(to: CGPoint(x: a + sw, y: y), control: CGPoint(x: a + sw / 2, y: y + 2 * sag))
            ctx.stroke(wire, with: .color(RGB(0x1E3C24).c), lineWidth: 1.5)
            for k in 1...3 {
                let u = CGFloat(k) / 4
                let bx = a + sw * u, by = y + 2 * sag * 2 * u * (1 - u) + 2
                let col = lightColours[bulb % 5 == 4 ? 3 : bulb % 4]
                ctx.fill(Path(CGRect(x: bx - 1.6, y: by - 1, width: 3.2, height: 2.5)), with: .color(RGB(0x46504A).c))
                let on = time.map { sin($0 * 2.2 + Double(bulb) * 1.7) > -0.35 } ?? true
                if on {
                    Glyphs.glow(ctx, center: CGPoint(x: bx, y: by + 4), radius: 9, color: col.c(0.55))
                    ellipse(ctx, bx, by + 4.5, 2.8, 3.8, col.c)
                    ellipse(ctx, bx - 0.8, by + 3.4, 0.9, 1.2, Color.white.opacity(0.7))
                } else {
                    ellipse(ctx, bx, by + 4.5, 2.8, 3.8, col.mix(RGB(0x281E1E), 0.6).c)
                }
                bulb += 1
            }
        }
    }
}

/// TimelineView that ticks at ~20 fps only while `animate` is true.
struct Animated<Content: View>: View {
    let animate: Bool
    @ViewBuilder let content: (Double) -> Content

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 20.0, paused: !animate)) { tl in
            content(animate ? tl.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 3600) : 2.0)
        }
    }
}

// MARK: - weather scene card

struct SceneCard: View {
    @Environment(\.palette) private var p
    let preset: WeatherPreset
    let title: String
    var subtitle = ""
    let selected: Bool
    var compact = false
    let animate: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            let textH: CGFloat = compact ? 24 : 40
            VStack(spacing: 0) {
                Animated(animate: animate) { t in
                    Canvas { ctx, size in
                        SceneCard.drawScene(ctx, size: size, preset: preset, palette: p, time: t)
                    }
                }
                .clipShape(TopRounded(radius: 9))
                VStack(alignment: compact ? .center : .leading, spacing: 1) {
                    Text(title).font(.system(size: compact ? 12 : UIMetrics.font, weight: .semibold))
                        .foregroundColor(p.text.c).lineLimit(1).minimumScaleFactor(0.7)
                    if !compact {
                        Text(subtitle).font(.system(size: UIMetrics.small)).foregroundColor(p.textSecondary.c)
                            .lineLimit(1).minimumScaleFactor(0.7)
                    }
                }
                .frame(maxWidth: .infinity, alignment: compact ? .center : .leading)
                .padding(.horizontal, compact ? 3 : 10)
                .frame(height: textH)
            }
            .background(RoundedRectangle(cornerRadius: 10).fill(p.card.c))
            .overlay(RoundedRectangle(cornerRadius: 10)
                .strokeBorder(selected ? p.accent.c : p.cardBorder.c, lineWidth: selected ? 2 : 1))
            .overlay(alignment: .topTrailing) {
                if selected { CheckBadge().padding(6) }
            }
            .contentShape(RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(compact ? title : "\(title), \(subtitle)")
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }

    static func drawScene(_ ctx: GraphicsContext, size: CGSize, preset: WeatherPreset, palette p: Palette, time t: Double) {
        let scene = CGRect(origin: .zero, size: size)
        let sw = size.width, sh = size.height
        var skyTop = p.skyTop, skyBottom = p.skyBottom
        if preset == .blizzard {
            skyTop = skyTop.mix(RGB(r: 150 / 255, g: 165 / 255, b: 185 / 255), 0.25)
            skyBottom = skyBottom.mix(RGB(r: 210 / 255, g: 220 / 255, b: 232 / 255), 0.35)
        } else if preset == .thaw {
            skyTop = skyTop.mix(RGB(r: 120 / 255, g: 135 / 255, b: 150 / 255), 0.35)
            skyBottom = skyBottom.mix(RGB(r: 170 / 255, g: 180 / 255, b: 190 / 255), 0.35)
        }
        ctx.fill(Path(scene), with: .linearGradient(Gradient(colors: [skyTop.c, skyBottom.c]),
                                                    startPoint: .zero, endPoint: CGPoint(x: 0, y: sh)))
        let ground = p.ground.c
        let dark = p.skyTop.mix(RGB(r: 8 / 255, g: 30 / 255, b: 28 / 255), 0.45).c
        let flakes = FlakeField.forPreset(preset)
        switch preset {
        case .light:
            let mx = sw - sw * 0.2, my = sh * 0.26, mr = sh * 0.12
            Glyphs.glow(ctx, center: CGPoint(x: mx, y: my), radius: mr * 3.2, color: RGB(0xFFF6D6).c(0.35))
            SceneDraw.ellipse(ctx, mx, my, mr, mr, RGB(0xFFF8E1).c)
            SceneDraw.ellipse(ctx, mx + mr * 0.45, my - mr * 0.25, mr * 0.85, mr * 0.85, skyTop.c)
            for i in 0..<5 {
                SceneDraw.ellipse(ctx, sw * (0.1 + 0.15 * CGFloat(i)), sh * (0.12 + 0.09 * CGFloat(i % 3)), 0.8, 0.8,
                                  Color.white.opacity(0.5 + 0.1 * Double(i % 3)))
            }
            SceneDraw.hill(ctx, rect: scene, top: sh - sh * 0.24, amp: sh * 0.05, phase: 0.6, color: ground)
            let hx = sw * 0.28, hy = sh - sh * 0.24
            ctx.fill(Path(CGRect(x: hx - sh * 0.12, y: hy - sh * 0.14, width: sh * 0.24, height: sh * 0.14 + 2)), with: .color(dark))
            SceneDraw.triangle(ctx, CGPoint(x: hx - sh * 0.16, y: hy - sh * 0.13), CGPoint(x: hx, y: hy - sh * 0.3),
                               CGPoint(x: hx + sh * 0.16, y: hy - sh * 0.13), ground)
            Glyphs.glow(ctx, center: CGPoint(x: hx, y: hy - sh * 0.06), radius: sh * 0.12, color: RGB(0xFFC85A).c(0.5))
            ctx.fill(Path(CGRect(x: hx - sh * 0.035, y: hy - sh * 0.09, width: sh * 0.07, height: sh * 0.065)),
                     with: .color(RGB(0xFFD26E).c))
            flakes.draw(ctx, in: scene, time: t, wind: 0, color: p.snow.c)
        case .normal:
            SceneDraw.hill(ctx, rect: scene, top: sh - sh * 0.26, amp: sh * 0.06, phase: 1.3, color: ground)
            SceneDraw.firTree(ctx, cx: sw * 0.22, baseY: sh - sh * 0.2, h: sh * 0.5, body: dark, cap: ground)
            SceneDraw.firTree(ctx, cx: sw * 0.4, baseY: sh - sh * 0.17, h: sh * 0.36, body: dark, cap: ground)
            SceneDraw.firTree(ctx, cx: sw * 0.8, baseY: sh - sh * 0.22, h: sh * 0.44, body: dark, cap: ground)
            flakes.draw(ctx, in: scene, time: t, wind: 0.04, color: p.snow.c)
        case .blizzard:
            SceneDraw.hill(ctx, rect: scene, top: sh - sh * 0.3, amp: sh * 0.09, phase: 2.4, color: ground)
            SceneDraw.firTree(ctx, cx: sw * 0.7, baseY: sh - sh * 0.24, h: sh * 0.42,
                              body: p.skyTop.mix(RGB(0x283C46), 0.5).c(0.75), cap: ground)
            ctx.fill(Path(scene), with: .color(Color.white.opacity(0.10)))
            flakes.draw(ctx, in: scene, time: t, wind: 0.55, color: p.snow.c, streak: 1)
        case .thaw:
            let eave = sh * 0.14
            ctx.fill(Path(CGRect(x: 0, y: 0, width: sw, height: eave)), with: .color(dark))
            ctx.fill(Path(CGRect(x: 0, y: eave - 3, width: sw, height: 3)), with: .color(ground))
            for i in 0..<5 {
                let ix = sw * (0.12 + 0.19 * CGFloat(i))
                let len = sh * (0.12 + 0.06 * CGFloat((i * 7) % 3))
                SceneDraw.triangle(ctx, CGPoint(x: ix - 3, y: eave), CGPoint(x: ix + 3, y: eave), CGPoint(x: ix, y: eave + len),
                                   RGB(0xDCF0FF).c(0.85))
                // drops falling from the icicle tips
                let period = 1.4 + 0.3 * Double(i % 3)
                let ph = (t + Double(i) * 0.53).truncatingRemainder(dividingBy: period) / period
                let dy = eave + len + CGFloat(ph * ph) * (sh - eave - len)
                SceneDraw.ellipse(ctx, ix, dy, 1.4, 2.2, RGB(0xC8E6FF).c(0.9 * (1 - ph * 0.4)))
            }
            SceneDraw.hill(ctx, rect: scene, top: sh - sh * 0.18, amp: sh * 0.03, phase: 0.2,
                           color: p.ground.mix(RGB(0xA0B4C3), 0.35).c)
            SceneDraw.ellipse(ctx, sw * 0.55, sh - sh * 0.08, sw * 0.22, sh * 0.04, RGB(0x8CB9E1).c(0.7))
            flakes.draw(ctx, in: scene, time: t, wind: 0, color: p.snow.mix(RGB(0xC8E1F5), 0.3).c)
        }
    }
}

struct CheckBadge: View {
    @Environment(\.palette) private var p

    var body: some View {
        Canvas { ctx, s in
            ctx.fill(Path(ellipseIn: CGRect(origin: .zero, size: s)), with: .color(p.accent.c))
            var m = Path()
            m.move(to: CGPoint(x: s.width * 0.28, y: s.height * 0.52))
            m.addLine(to: CGPoint(x: s.width * 0.44, y: s.height * 0.68))
            m.addLine(to: CGPoint(x: s.width * 0.74, y: s.height * 0.34))
            ctx.stroke(m, with: .color(p.accentText.c), style: StrokeStyle(lineWidth: 1.8, lineCap: .round, lineJoin: .round))
        }
        .frame(width: 17, height: 17)
        .accessibilityHidden(true)
    }
}

/// Rectangle with only the top corners rounded.
struct TopRounded: Shape {
    var radius: CGFloat

    func path(in rect: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: rect.minX, y: rect.maxY))
        p.addLine(to: CGPoint(x: rect.minX, y: rect.minY + radius))
        p.addQuadCurve(to: CGPoint(x: rect.minX + radius, y: rect.minY), control: CGPoint(x: rect.minX, y: rect.minY))
        p.addLine(to: CGPoint(x: rect.maxX - radius, y: rect.minY))
        p.addQuadCurve(to: CGPoint(x: rect.maxX, y: rect.minY + radius), control: CGPoint(x: rect.maxX, y: rect.minY))
        p.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        p.closeSubpath()
        return p
    }
}

// MARK: - header background

struct HeaderBackground: View {
    let palette: Palette
    let animate: Bool
    var flakeCount = 60

    var body: some View {
        Animated(animate: animate) { t in
            Canvas { ctx, size in
                HeaderBackground.draw(ctx, size: size, palette: palette, time: t)
            }
        }
    }

    static func draw(_ ctx: GraphicsContext, size: CGSize, palette p: Palette, time t: Double) {
        let all = CGRect(origin: .zero, size: size)
        ctx.fill(Path(all), with: .linearGradient(Gradient(colors: [p.headerTop.c, p.headerBottom.c]),
                                                  startPoint: .zero, endPoint: CGPoint(x: 0, y: size.height)))
        switch p.theme {
        case .night:
            for i in 0..<36 {
                let sx = (Double(i) * 97.3).truncatingRemainder(dividingBy: size.width)
                let sy = 4 + (Double(i) * 37.1).truncatingRemainder(dividingBy: size.height * 0.6)
                SceneDraw.ellipse(ctx, sx, sy, 0.7, 0.7, Color.white.opacity(0.3 + 0.1 * Double(i % 4)))
            }
            let mx = size.width * 0.62, my = size.height * 0.42
            Glyphs.glow(ctx, center: CGPoint(x: mx, y: my), radius: 40, color: RGB(0xD2E6FF).c(0.18))
            SceneDraw.ellipse(ctx, mx, my, 9, 9, RGB(0xF0F6FF).c(0.92))
            SceneDraw.ellipse(ctx, mx + 4, my - 2.5, 8, 8, p.headerTop.mix(p.headerBottom, 0.4).c)
        case .globe:
            Glyphs.glow(ctx, center: CGPoint(x: size.width * 0.5, y: size.height * 1.4), radius: size.height * 2.2,
                        color: Color.white.opacity(0.22))
            SceneDraw.hill(ctx, rect: all, top: size.height - 8, amp: 3, phase: 0.8, color: Color.white.opacity(0.35))
        case .postcard:
            break
        }
        FlakeField.header.draw(ctx, in: all, time: t, wind: 0.015, color: p.snow.c)
        if p.garland {
            SceneDraw.garland(ctx, x0: -20, x1: size.width + 20, y: size.height - 17, sag: 5, time: t)
        }
    }
}

// MARK: - countdown card

struct CountdownCard: View {
    @Environment(\.palette) private var p
    let title: String
    let countdown: Countdown
    let animate: Bool

    var body: some View {
        ZStack(alignment: .topLeading) {
            RoundedRectangle(cornerRadius: 12)
                .fill(LinearGradient(colors: [p.card.c, p.card.mix(p.accent, p.dark ? 0.10 : 0.06).c],
                                     startPoint: .top, endPoint: .bottom))
            RoundedRectangle(cornerRadius: 12).strokeBorder(p.cardBorder.c, lineWidth: 1)
            Animated(animate: animate) { t in
                Canvas { ctx, size in CountdownCard.drawTree(ctx, size: size, palette: p, time: t) }
            }
            .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 0) {
                Text(title).font(.system(size: 12.5, weight: .semibold)).foregroundColor(p.textSecondary.c)
                Spacer(minLength: 4)
                Text(countdown.big)
                    .font(.system(size: countdown.festive ? 24 : 46, weight: .semibold, design: .rounded))
                    .foregroundColor(p.heading.c)
                    .lineLimit(2).minimumScaleFactor(0.6)
                Spacer(minLength: 4)
                Text(countdown.caption).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(EdgeInsets(top: 14, leading: 16, bottom: 14, trailing: 96))
        }
        .accessibilityElement(children: .combine)
    }

    static func drawTree(_ ctx: GraphicsContext, size: CGSize, palette p: Palette, time t: Double) {
        let tx = size.width - 52, base = size.height - 16, th = min(size.height - 34, 96)
        let treeC: RGB = p.theme == .postcard ? RGB(0x2E5E3C) : p.dark ? RGB(0x286E5A) : RGB(0x2E7860)
        SceneDraw.firTree(ctx, cx: tx, baseY: base, h: th, body: treeC.c, cap: Color.white.opacity(0.85))
        Glyphs.glow(ctx, center: CGPoint(x: tx, y: base - th - 3), radius: 9, color: RGB(0xFFDC78).c(0.35 + 0.2 * sin(t * 3)))
        ctx.fill(Glyphs.starPath(center: CGPoint(x: tx, y: base - th - 3), r: 5), with: .color(RGB(0xFFD666).c))
        for i in 0..<9 {
            let u = 0.25 + 0.08 * CGFloat(i)
            let lx = tx + (i % 2 == 1 ? 1 : -1) * th * 0.22 * (u - 0.15) * 1.4
            let ly = base - th + th * u
            let on = sin(t * 2.4 + Double(i) * 1.3) > -0.2
            let col = SceneDraw.lightColours[i % 5]
            if on { Glyphs.glow(ctx, center: CGPoint(x: lx, y: ly), radius: 5, color: col.c(0.6)) }
            SceneDraw.ellipse(ctx, lx, ly, 1.6, 1.6, on ? col.c : col.mix(RGB(0x000000), 0.5).c)
        }
    }
}

// MARK: - festive scene (night city, garland and fireworks)

struct FestiveScene: View {
    @Environment(\.palette) private var p
    let animate: Bool

    var body: some View {
        Animated(animate: animate) { t in
            Canvas { ctx, size in FestiveScene.draw(ctx, size: size, palette: p, time: t) }
        }
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(p.cardBorder.c, lineWidth: 1))
        .accessibilityHidden(true)
    }

    static func draw(_ ctx: GraphicsContext, size: CGSize, palette p: Palette, time t: Double) {
        let w = size.width, h = size.height
        let top = p.dark ? p.skyTop : p.skyTop.mix(RGB(0x0A0E28), 0.45)
        ctx.fill(Path(CGRect(origin: .zero, size: size)),
                 with: .linearGradient(Gradient(colors: [top.c, p.skyBottom.mix(RGB(0x141432), 0.4).c]),
                                       startPoint: .zero, endPoint: CGPoint(x: 0, y: h)))
        for i in 0..<40 {
            let sx = 8 + (Double(i) * 131.7).truncatingRemainder(dividingBy: max(1, w - 16))
            let sy = 8 + (Double(i) * 53.3).truncatingRemainder(dividingBy: max(1, h * 0.55))
            SceneDraw.ellipse(ctx, sx, sy, 0.8, 0.8, Color.white.opacity(0.25 + 0.12 * Double(i % 4)))
        }
        let mx = w - 60, my: CGFloat = 46
        Glyphs.glow(ctx, center: CGPoint(x: mx, y: my), radius: 44, color: RGB(0xFFF0D2).c(0.16))
        SceneDraw.ellipse(ctx, mx, my, 14, 14, RGB(0xFFF6E1).c)
        SceneDraw.ellipse(ctx, mx + 6, my - 4, 12.5, 12.5, top.c)
        // fireworks: a burst every ~1.1 s at a pseudo-random place, each lives 2.6 s
        let period = 1.1
        let newest = Int(floor(t / period))
        for k in max(0, newest - 3)...newest {
            var seed = UInt32(truncatingIfNeeded: k &* 2_654_435_761 &+ 12345)
            func next() -> Double {
                seed = seed &* 1_103_515_245 &+ 12345
                return Double((seed >> 8) % 1000) / 1000
            }
            let a = next(), b = next(), colour = Int(next() * 8) % 8
            let start = Double(k) * period
            let age = t - start
            guard age >= 0, age <= 2.6 else { continue }
            let cx = CGFloat(0.15 + 0.7 * a) * w, cy = CGFloat(0.18 + 0.3 * b) * h
            let col = SceneDraw.lightColours[colour]
            if age < 0.35 {
                let u = age / 0.35
                let y = h - (h - cy) * u
                Glyphs.line(ctx, CGPoint(x: cx, y: y + 10), CGPoint(x: cx, y: y), RGB(0xFFE6B4).c(0.8), 1.4)
                continue
            }
            let e = age - 0.35
            let alpha = max(0, 1 - e / 2.2)
            if e < 0.25 { Glyphs.glow(ctx, center: CGPoint(x: cx, y: cy), radius: 30, color: col.c(0.35 * (1 - e / 0.25))) }
            for j in 0..<28 {
                let an = Double(j) * 2 * Double.pi / 28
                let v = 46.0 * (0.75 + 0.25 * Double((j * 7) % 4) / 3)
                func pos(_ e: Double) -> CGPoint {
                    CGPoint(x: cx + cos(an) * v * (1 - exp(-2.2 * e)), y: cy + sin(an) * v * (1 - exp(-2.2 * e)) + 14 * e * e)
                }
                let head = pos(e), tail = pos(max(0, e - 0.12))
                Glyphs.line(ctx, tail, head, col.c(alpha * 0.55), 1.2)
                SceneDraw.ellipse(ctx, head.x, head.y, 1.4, 1.4, col.mix(RGB(0xFFFFFF), 0.4).c(alpha))
            }
        }
        // city silhouette with lit windows
        let city = top.mix(RGB(0x000000), 0.45).c
        var x: CGFloat = 1
        var i = 0
        while x < w - 2 {
            let bw = 18 + CGFloat((i * 37) % 23), bh = 20 + CGFloat((i * 53) % 34)
            ctx.fill(Path(CGRect(x: x, y: h - bh, width: min(bw - 2, w - 1.5 - x), height: bh - 1)), with: .color(city))
            for wy in 0..<3 where ((i + wy) % 3) == 0 && x + 9 < w - 2 {
                ctx.fill(Path(CGRect(x: x + 5, y: h - bh + 6 + CGFloat(wy) * 9, width: 4, height: 4)),
                         with: .color(RGB(0xFFD278).c(0.7)))
            }
            x += bw
            i += 1
        }
        ctx.fill(Path(roundedRect: CGRect(x: 1, y: h - 9, width: w - 2, height: 8), cornerRadius: 6),
                 with: .color(p.ground.c(0.95)))
        SceneDraw.garland(ctx, x0: 4, x1: w - 4, y: 6, sag: 7, time: t)
    }
}
