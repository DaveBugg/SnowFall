// The settings window: header, navigation (sidebar in «Зимняя ночь», pills in the other two
// themes), the page, the right column (live preview, countdown) and the bottom bar.
import AppKit
import SFBridge
import SwiftUI

enum SettingsLayout {
    /// Whole window content, title bar area included (the header runs under the traffic lights).
    static let size = CGSize(width: 980, height: 700)
    static let headerContent: CGFloat = 64
    static let sidebar: CGFloat = 186
    static let rightColumn: CGFloat = 246
}

struct SettingsRootView: View {
    @ObservedObject var store: UIStore
    var onClose: () -> Void = {}
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var confirmRestore = false

    private var p: Palette { store.palette }
    private var animate: Bool { store.animating && !reduceMotion }

    var body: some View {
        VStack(spacing: 0) {
            SettingsHeader(store: store, animate: animate)
            HStack(spacing: 0) {
                if !p.pillTabs {
                    Sidebar(store: store)
                }
                VStack(spacing: 0) {
                    if p.pillTabs {
                        PillTabs(store: store)
                            .padding(.horizontal, 16)
                            .padding(.top, 12)
                    }
                    HStack(alignment: .top, spacing: 16) {
                        pageArea
                        RightColumn(store: store, animate: animate)
                            .frame(width: SettingsLayout.rightColumn)
                    }
                    .padding(.leading, p.pillTabs ? 16 : 20)
                    .padding(.trailing, 16)
                    .padding(.top, p.pillTabs ? 10 : 16)
                    bottomBar
                }
            }
        }
        .frame(minWidth: SettingsLayout.size.width, maxWidth: .infinity, minHeight: SettingsLayout.size.height,
               maxHeight: .infinity)
        .background(p.bg.c)
        .ignoresSafeArea()   // the header runs under the transparent title bar
        .environment(\.palette, p)
        .preferredColorScheme(p.dark ? .dark : .light)
        .alert(store.strings["AppName"], isPresented: $confirmRestore) {
            Button(store.strings["BtnRestoreDefaults"], role: .destructive) {
                store.update { $0 = $0.restoredDefaults() }
            }
            Button(store.strings["BtnCancel"], role: .cancel) {}
        } message: {
            Text(store.strings["RestoreConfirmText"])
        }
    }

    @ViewBuilder private var pageArea: some View {
        let content = ScrollView(.vertical, showsIndicators: true) {
            PageContent(store: store, animate: animate)
                .padding(p.pageIsCard ? EdgeInsets(top: 14, leading: 16, bottom: 16, trailing: 16)
                                      : EdgeInsets(top: 0, leading: 0, bottom: 12, trailing: 6))
                .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .id(store.page)   // every page starts at the top
        if p.pageIsCard {
            content
                .background(RoundedRectangle(cornerRadius: 12).fill(p.page.c))
                .clipShape(RoundedRectangle(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(p.pageBorder.c, lineWidth: 1))
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            content.frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var bottomBar: some View {
        HStack(spacing: 12) {
            Text(store.strings.format("StatusLabelFmt", store.statusText))
                .font(.system(size: UIMetrics.font)).foregroundColor(p.textSecondary.c)
                .lineLimit(1)
            Spacer(minLength: 8)
            Button(store.strings["BtnRestoreDefaults"]) { confirmRestore = true }
                .buttonStyle(SnowButtonStyle(height: 32))
                .frame(width: 260)
            Button(store.strings["BtnClose"], action: onClose)
                .buttonStyle(SnowButtonStyle(primary: true, height: 32))
                .frame(width: 120)
                .keyboardShortcut(.cancelAction)
        }
        .padding(.leading, p.pillTabs ? 18 : 20)
        .padding(.trailing, 16)
        .frame(height: 58)
    }
}

// MARK: - header

struct SettingsHeader: View {
    @ObservedObject var store: UIStore
    let animate: Bool

    var body: some View {
        let p = store.palette
        ZStack(alignment: .bottomLeading) {
            HeaderBackground(palette: p, animate: animate)
            HStack(spacing: 12) {
                Canvas { ctx, s in
                    Glyphs.logo(ctx, palette: p, center: CGPoint(x: s.width / 2, y: s.height / 2), size: 30)
                }
                .frame(width: 40, height: 40)
                .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 1) {
                    HStack(alignment: .center, spacing: 10) {
                        Text(store.strings["AppName"]).font(.system(size: 22, weight: .semibold, design: .rounded))
                            .foregroundColor(p.headerText.c)
                            .accessibilityAddTraits(.isHeader)
                        if store.updateStatus.hasNewer {
                            UpdateBadge(store: store, palette: p) { store.page = .about }
                        }
                    }
                    Text(store.holdHint(short: false)).font(.system(size: 12.5)).foregroundColor(p.headerTextSecondary.c)
                        .lineLimit(1).minimumScaleFactor(0.8)
                }
                Spacer(minLength: 12)
                HeaderCaption(text: store.strings["GroupTool"], palette: p)
                ToolChip(store: store, palette: p)
                HeaderCaption(text: store.strings["CaptionTheme"], palette: p)
                    .padding(.leading, 6)
                ThemeChip(store: store, palette: p)
            }
            .padding(.horizontal, 18)
            .padding(.bottom, p.garland ? 14 : 10)
            .frame(height: SettingsLayout.headerContent + (p.garland ? 6 : 0), alignment: .center)
        }
        .frame(height: store.titlebarHeight + SettingsLayout.headerContent)
    }
}

/// «Доступна версия 0.1.3» on the header: opens the About page (settings) / the settings (quick panel).
struct UpdateBadge: View {
    @ObservedObject var store: UIStore
    let palette: Palette
    let action: () -> Void

    var body: some View {
        let text = store.strings.format("HeaderUpdateFmt", store.updateStatus.latest)
        Button(action: action) {
            HStack(spacing: 5) {
                Circle().fill(palette.accent.c).frame(width: 7, height: 7)
                Text(text).font(.system(size: 11.5, weight: .semibold)).foregroundColor(palette.headerText.c)
                    .lineLimit(1).fixedSize()
            }
            .padding(.horizontal, 9)
            .frame(height: 22)
            .background(Capsule().fill(palette.headerChip.c))
            .overlay(Capsule().strokeBorder(palette.headerText.c(0.35), lineWidth: 1))
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(text)
        .help(text)
    }
}

private struct HeaderCaption: View {
    let text: String
    let palette: Palette

    var body: some View {
        Text(text).font(.system(size: 12)).foregroundColor(palette.headerTextSecondary.c).lineLimit(1).fixedSize()
            .accessibilityHidden(true)
    }
}

/// Tool selector in the header: snowball / mitten / shovel.
struct ToolChip: View {
    @ObservedObject var store: UIStore
    let palette: Palette
    var onWindowSurface = false   // quick panel: drawn on the window, not on the header

    private static let order: [ToolKind] = [.snowball, .mitten, .shovel]

    var body: some View {
        let p = palette
        HStack(spacing: 4) {
            ForEach(ToolChip.order, id: \.self) { tool in
                let selected = store.settings.interaction.tool == tool
                let fg = onWindowSurface ? (selected ? p.accentText.c : p.controlText.c) : p.headerText.c
                Button {
                    store.update { $0.interaction.tool = tool }
                } label: {
                    GlyphView(kind: .tool(tool), color: fg, size: 18)
                        .frame(width: 34, height: 30)
                        .background(
                            RoundedRectangle(cornerRadius: onWindowSurface ? 8 : 15)
                                .fill(selected ? (onWindowSurface ? p.accent.c : p.headerText.c(0.16)) :
                                    (onWindowSurface ? p.control.c : Color.clear)))
                        .overlay(
                            RoundedRectangle(cornerRadius: onWindowSurface ? 8 : 15)
                                .strokeBorder(onWindowSurface ? (selected ? Color.clear : p.controlBorder.c) :
                                    (selected ? p.headerText.c(0.85) : Color.clear), lineWidth: selected ? 1.5 : 1))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(store.strings[ToolChip.titleId(tool)])
                .accessibilityAddTraits(selected ? [.isSelected] : [])
                .help(store.strings[ToolChip.titleId(tool)])
            }
        }
        .padding(onWindowSurface ? 0 : 4)
        .background(Capsule().fill(onWindowSurface ? Color.clear : p.headerChip.c))
        .overlay(Capsule().strokeBorder(onWindowSurface ? Color.clear : p.headerText.c(0.25), lineWidth: 1))
        .accessibilityElement(children: .contain)
        .accessibilityLabel(store.strings["GroupTool"])
    }

    static func titleId(_ t: ToolKind) -> String {
        switch t {
        case .snowball: return "ToolSnowball"
        case .mitten: return "ToolMitten"
        case .shovel: return "ToolShovel"
        }
    }
}

struct ThemeChip: View {
    @ObservedObject var store: UIStore
    let palette: Palette

    var body: some View {
        HStack(spacing: 6) {
            ForEach(UITheme.allCases, id: \.self) { theme in
                let selected = store.settings.system.uiTheme == theme
                Button {
                    store.update { $0.system.uiTheme = theme }
                } label: {
                    ThemeSwatch(theme: theme)
                        .frame(width: 24, height: 24)
                        .padding(3)
                        .overlay(Circle().strokeBorder(selected ? palette.headerText.c : Color.clear, lineWidth: 1.5))
                        .contentShape(Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(store.strings[ThemeChip.titleId(theme)])
                .accessibilityAddTraits(selected ? [.isSelected] : [])
                .help(store.strings[ThemeChip.titleId(theme)])
            }
        }
        .padding(4)
        .background(Capsule().fill(palette.headerChip.c))
        .overlay(Capsule().strokeBorder(palette.headerText.c(0.25), lineWidth: 1))
        .accessibilityElement(children: .contain)
        .accessibilityLabel(store.strings["CaptionTheme"])
    }

    static func titleId(_ t: UITheme) -> String {
        switch t {
        case .night: return "ThemeNight"
        case .globe: return "ThemeGlobe"
        case .postcard: return "ThemePostcard"
        }
    }
}

// MARK: - navigation

struct Sidebar: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let p = store.palette
        VStack(alignment: .leading, spacing: 4) {
            ForEach(SettingsPage.allCases) { page in
                let selected = store.page == page
                Button {
                    store.page = page
                } label: {
                    HStack(spacing: 10) {
                        GlyphView(kind: .nav(NavGlyph.forPage(page)), color: selected ? p.navTextSel.c : p.navText.c, size: 17)
                        Text(store.strings[page.titleId])
                            .font(.system(size: UIMetrics.font, weight: selected ? .semibold : .regular))
                            .foregroundColor(selected ? p.navTextSel.c : p.navText.c)
                            .lineLimit(1).minimumScaleFactor(0.7)
                        Spacer(minLength: 0)
                    }
                    .padding(.leading, 12)
                    .frame(height: 38)
                    .background(
                        RoundedRectangle(cornerRadius: 8).fill(selected ? p.navSel.c : Color.clear)
                            .overlay(alignment: .leading) {
                                if selected {
                                    Capsule().fill(p.accent.c).frame(width: 3, height: 18).padding(.leading, 2)
                                }
                            })
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? [.isSelected] : [])
            }
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.top, 16)
        .frame(width: SettingsLayout.sidebar)
        .frame(maxHeight: .infinity)
        .background(p.nav.c)
        .accessibilityElement(children: .contain)
    }
}

struct PillTabs: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let p = store.palette
        HStack(spacing: 4) {
            ForEach(SettingsPage.allCases) { page in
                let selected = store.page == page
                Button {
                    store.page = page
                } label: {
                    Text(store.strings[page.titleId])
                        .font(.system(size: UIMetrics.font, weight: selected ? .semibold : .regular))
                        .foregroundColor(selected ? p.navTextSel.c : p.navText.c)
                        .lineLimit(1)
                        .padding(.horizontal, 14)
                        .frame(height: 30)
                        .background(Capsule().fill(selected ? p.navSel.c : Color.clear))
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? [.isSelected] : [])
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .contain)
    }
}

// MARK: - right column

struct RightColumn: View {
    @ObservedObject var store: UIStore
    let animate: Bool

    var body: some View {
        let p = store.palette
        VStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text(store.strings["PreviewTitle"]).font(.system(size: 12.5, weight: .semibold)).foregroundColor(p.heading.c)
                    Spacer()
                    Text(store.strings["PreviewHint"]).font(.system(size: UIMetrics.small)).foregroundColor(p.textSecondary.c)
                }
                PreviewHost(view: store.preview)
                    .frame(height: 140)
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                    .accessibilityLabel(store.strings["PreviewTitle"])
            }
            .padding(12)
            .background(CardBackground())
            Button(store.strings["BtnResetPreview"]) { store.resetPreview() }
                .buttonStyle(SnowButtonStyle(height: 30))
            CountdownCard(title: store.strings["CountdownTitle"],
                          countdown: Countdown.compute(now: store.now, strings: store.strings), animate: animate)
                .frame(maxHeight: .infinity)
        }
        .padding(.bottom, 2)
    }
}

/// Embeds the app's live preview (-makePreviewView). The preview NSView is shared; each
/// SwiftUI container re-parents it, so theme/layout switches never lose it.
struct PreviewHost: NSViewRepresentable {
    let view: NSView?

    func makeNSView(context: Context) -> NSView {
        let container = NSView()
        container.wantsLayer = true
        attach(to: container)
        return container
    }

    func updateNSView(_ container: NSView, context: Context) {
        attach(to: container)
    }

    private func attach(to container: NSView) {
        guard let v = view, v.superview !== container else { return }
        v.removeFromSuperview()
        v.frame = container.bounds
        v.autoresizingMask = [.width, .height]
        container.addSubview(v)
    }
}

// MARK: - page switch

struct PageContent: View {
    @ObservedObject var store: UIStore
    let animate: Bool

    var body: some View {
        switch store.page {
        case .snow: SnowPage(store: store, animate: animate)
        case .drifts: DriftsPage(store: store)
        case .tools: ToolsPage(store: store)
        case .performance: PerformancePage(store: store)
        case .festive: FestivePage(store: store, animate: animate)
        case .system: SystemPage(store: store)
        case .about: AboutPage(store: store)
        }
    }
}
