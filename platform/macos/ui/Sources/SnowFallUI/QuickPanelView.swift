// The quick panel in the status-bar popover (Windows: platform/windows/app/ui/quick_panel.cpp).
import AppKit
import SwiftUI

enum QuickPanelLayout {
    static let size = CGSize(width: 344, height: 520)
}

struct QuickPanelView: View {
    @ObservedObject var store: UIStore
    var onCommand: (QuickCommand) -> Void = { _ in }
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    enum QuickCommand { case pause, clear, fireworks, settings, update }

    var body: some View {
        let p = store.palette
        VStack(spacing: 0) {
            header
            if store.showPicker {
                ProcessPicker(store: store, compact: true, onDone: { store.showPicker = false })
            } else {
                content
            }
            Spacer(minLength: 0)
        }
        .frame(width: QuickPanelLayout.size.width, height: QuickPanelLayout.size.height)
        .background(p.bg.c)
        .environment(\.palette, p)
        .preferredColorScheme(p.dark ? .dark : .light)
    }

    private var animate: Bool { store.animating && !reduceMotion }

    private var header: some View {
        let p = store.palette
        return ZStack(alignment: .leading) {
            HeaderBackground(palette: p, animate: animate)
            HStack(spacing: 10) {
                Canvas { ctx, s in
                    Glyphs.logo(ctx, palette: p, center: CGPoint(x: s.width / 2, y: s.height / 2), size: 26)
                }
                .frame(width: 34, height: 34)
                .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 1) {
                    Text(store.strings["AppName"]).font(.system(size: 16, weight: .semibold, design: .rounded))
                        .foregroundColor(p.headerText.c)
                    Text(store.statusText).font(.system(size: 12)).foregroundColor(p.headerTextSecondary.c).lineLimit(1)
                }
                .accessibilityElement(children: .combine)
                Spacer(minLength: 6)
                if store.updateStatus.hasNewer {
                    UpdateBadge(store: store, palette: p) { onCommand(.update) }
                }
            }
            .padding(.horizontal, 14)
            .padding(.bottom, p.garland ? 8 : 0)
        }
        .frame(height: 56)
        .accessibilityElement(children: .contain)
    }

    private var content: some View {
        let p = store.palette, t = store.strings, s = store.settings
        return VStack(alignment: .leading, spacing: 10) {
            Text(store.holdHint(short: true)).font(.system(size: 12.5, weight: .medium)).foregroundColor(p.text.c)
                .lineLimit(1).minimumScaleFactor(0.8)
            HStack(spacing: 6) {
                ForEach(WeatherPreset.allCases) { preset in
                    SceneCard(preset: preset, title: t[preset.shortTitleId], selected: s.weather.preset == preset,
                              compact: true, animate: animate) {
                        store.update { $0.weather.preset = preset }
                    }
                }
            }
            .frame(height: 76)
            .accessibilityElement(children: .contain)
            .accessibilityLabel(t["GroupWeather"])
            VStack(spacing: 2) {
                SliderRow(label: t["QuickIntensity"], value: store.binding(\.weather.intensity), range: 0...100,
                          valueText: ValueText.percent(s.weather.intensity), labelWidth: 84, valueWidth: 52)
                SliderRow(label: t["QuickWind"], value: store.binding(\.weather.wind), range: -100...100,
                          valueText: ValueText.wind(s.weather.wind, t), labelWidth: 84, valueWidth: 52)
            }
            HStack(spacing: 12) {
                Text(t["GroupTool"]).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c)
                    .frame(width: 84, alignment: .leading)
                ToolChip(store: store, palette: p, onWindowSurface: true)
                Spacer(minLength: 0)
            }
            SectionHeading(text: t["MenuUserPresets"])
            if s.userPresets.isEmpty {
                HintText(text: t["QuickNoPresets"])
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(Array(s.userPresets.enumerated()), id: \.offset) { i, preset in
                            PresetChip(name: preset.name, deletable: false, apply: { store.applyPreset(at: i) })
                        }
                    }
                    .padding(.vertical, 1)
                }
                .frame(height: 32)
            }
            HStack(spacing: 10) {
                Text(t.format("QuickExclusionsFmt", s.system.excludedProcesses.count))
                    .font(.system(size: UIMetrics.font)).foregroundColor(p.text.c).lineLimit(1).minimumScaleFactor(0.8)
                Spacer(minLength: 4)
                Button(t["BtnExcludeApp"]) { store.showPicker = true }
                    .buttonStyle(SnowButtonStyle())
                    .frame(width: 186)
            }
            SnowSegmented(options: [(ExcludedSnowMode.behind, t["ExclBehind"]), (.around, t["ExclAround"])],
                          selection: store.binding(\.system.excludedSnowMode), height: 30, label: t["LblExclMode"])
            Spacer(minLength: 0)
            VStack(spacing: 8) {
                HStack(spacing: 8) {
                    Button(t[store.diagnostics.paused ? "MenuResume" : "MenuPause"]) { onCommand(.pause) }
                        .buttonStyle(SnowButtonStyle(primary: true, height: 32))
                    Button(t["MenuClearSnow"]) { onCommand(.clear) }
                        .buttonStyle(SnowButtonStyle(height: 32))
                }
                HStack(spacing: 8) {
                    Button(t["BtnFireworks"]) { onCommand(.fireworks) }
                        .buttonStyle(SnowButtonStyle(height: 32))
                    Button(t["MenuSettings"]) { onCommand(.settings) }
                        .buttonStyle(SnowButtonStyle(height: 32))
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.top, 8)
        .padding(.bottom, 12)
        .frame(maxHeight: .infinity, alignment: .top)
    }
}
