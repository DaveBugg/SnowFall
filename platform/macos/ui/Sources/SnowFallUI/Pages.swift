// The seven settings pages. Same contents and dependencies as the Windows window
// (platform/windows/app/ui/settings_window.cpp: build*Page / updateEnableStates).
import AppKit
import SwiftUI

// MARK: - value texts

enum ValueText {
    static func percent(_ v: Int) -> String { "\(v) %" }
    static func px(_ v: Int) -> String { "\(v) px" }
    static func minutes(_ v: Int, _ t: L10n) -> String { t.format("UnitMinFmt", v) }

    static func everyMinutes(_ v: Int, _ t: L10n) -> String {
        if v == 1 { return t["SnowmenEveryMinute"] }
        return t.format(t.plural(v, "SnowmenEvery1Fmt", "SnowmenEvery2Fmt", "SnowmenEvery5Fmt"), v)
    }

    static func snowmen(_ v: Int, _ t: L10n) -> String {
        t.format(t.plural(v, "Snowmen1Fmt", "Snowmen2Fmt", "Snowmen5Fmt"), v)
    }

    static func wind(_ v: Int, _ t: L10n) -> String {
        if v < 0 { return "← \(-v)" }
        if v > 0 { return "\(v) →" }
        return t["WindCalm"]
    }
}

extension WeatherPreset {
    var sceneTitleId: String {
        switch self {
        case .light: return "SceneLight"
        case .normal: return "SceneNormal"
        case .blizzard: return "SceneBlizzard"
        case .thaw: return "SceneThaw"
        }
    }

    var sceneSubtitleId: String { sceneTitleId + "Sub" }

    var shortTitleId: String {
        switch self {
        case .light: return "PresetLight"
        case .normal: return "PresetNormal"
        case .blizzard: return "PresetBlizzard"
        case .thaw: return "PresetThaw"
        }
    }
}

// MARK: - «Снег»

struct SnowPage: View {
    @ObservedObject var store: UIStore
    let animate: Bool

    var body: some View {
        let t = store.strings
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 8) {
                ForEach(WeatherPreset.allCases) { preset in
                    SceneCard(preset: preset, title: t[preset.sceneTitleId], subtitle: t[preset.sceneSubtitleId],
                              selected: store.settings.weather.preset == preset, animate: animate) {
                        store.update { $0.weather.preset = preset }
                    }
                }
            }
            .frame(height: 116)
            .accessibilityElement(children: .contain)
            .accessibilityLabel(t["GroupWeather"])

            UserPresetsSection(store: store)

            VStack(spacing: 3) {
                SliderRow(label: t["LblIntensity"], value: store.binding(\.weather.intensity), range: 0...100,
                          valueText: ValueText.percent(store.settings.weather.intensity))
                SliderRow(label: t["LblWind"], value: store.binding(\.weather.wind), range: -100...100,
                          valueText: ValueText.wind(store.settings.weather.wind, t))
                SliderRow(label: t["LblGustiness"], value: store.binding(\.weather.gustiness), range: 0...100,
                          valueText: ValueText.percent(store.settings.weather.gustiness))
                SliderRow(label: t["LblFlakeSize"], value: store.binding(\.weather.sizePercent), range: 50...200,
                          valueText: ValueText.percent(store.settings.weather.sizePercent))
                SliderRow(label: t["LblFallSpeed"], value: store.binding(\.weather.fallSpeedPercent), range: 25...200,
                          valueText: ValueText.percent(store.settings.weather.fallSpeedPercent))
                SliderRow(label: t["LblOpacity"], value: store.binding(\.weather.opacityPercent), range: 10...100,
                          valueText: ValueText.percent(store.settings.weather.opacityPercent))
            }
            SnowToggle(title: t["ChkSkyPerMonitor"], isOn: store.binding(\.weather.skyPerMonitor), hint: t["SkyPerMonitorHint"])
        }
    }
}

struct UserPresetsSection: View {
    @ObservedObject var store: UIStore
    @State private var editing = false
    @State private var name = ""

    var body: some View {
        let t = store.strings
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                SectionHeading(text: t["GroupUserPresets"])
                if editing {
                    SnowField(placeholder: t["PresetNameCue"], text: $name, onSubmit: save)
                        .frame(width: 190)
                        .onExitCommand { editing = false }
                    Button(t["BtnOk"], action: save)
                        .buttonStyle(SnowButtonStyle(primary: true))
                        .frame(width: 56)
                    Button(t["BtnCancel"]) {
                        editing = false
                        store.presetMessage = ("", .secondary)
                    }
                    .buttonStyle(SnowButtonStyle())
                    .fixedSize()
                } else {
                    Button(t["BtnSavePreset"]) {
                        name = ""
                        editing = true
                        store.presetMessage = ("", .secondary)
                    }
                    .buttonStyle(SnowButtonStyle())
                    .frame(width: 210)
                }
            }
            if store.settings.userPresets.isEmpty {
                HintText(text: t["PresetsEmpty"])
            } else {
                FlowLayout(spacing: 8, lineSpacing: 8) {
                    ForEach(Array(store.settings.userPresets.enumerated()), id: \.offset) { i, preset in
                        PresetChip(name: preset.name, apply: { store.applyPreset(at: i) }, delete: { store.deletePreset(at: i) })
                            .onDeleteCommand { store.deletePreset(at: i) }
                    }
                }
            }
            let hint = store.presetHint
            if !hint.text.isEmpty {
                HintText(text: hint.text, role: hint.role)
            }
        }
    }

    private func save() {
        if store.savePreset(named: name) {
            editing = false
            name = ""
        }
    }
}

// MARK: - «Сугробы»

struct DriftsPage: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let t = store.strings, a = store.settings.accumulation
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                SnowToggle(title: t["ChkAccumulation"], isOn: store.binding(\.accumulation.enabled))
                Spacer(minLength: 12)
                Button(t["BtnClearAll"]) { store.actions?.clearSnow() }
                    .buttonStyle(SnowButtonStyle())
                    .frame(width: 170)
            }
            Group {
                HStack(alignment: .top, spacing: 18) {
                    VStack(alignment: .leading, spacing: 7) {
                        SectionHeading(text: t["GroupWhere"])
                        SnowToggle(title: t["ChkOnWindows"], isOn: store.binding(\.accumulation.onWindows))
                        SnowToggle(title: t["ChkOnTaskbar"], isOn: store.binding(\.accumulation.onTaskbar))
                        SnowToggle(title: t["ChkOnScreenBottom"], isOn: store.binding(\.accumulation.onScreenBottom))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    VStack(alignment: .leading, spacing: 7) {
                        SectionHeading(text: t["GroupShake"])
                        SnowSegmented(options: [(false, t["RadioShakeKeep"]), (true, t["RadioShakeDrop"])],
                                      selection: store.binding(\.accumulation.shakeOffOnMove), label: t["GroupShake"])
                        SnowToggle(title: t["ChkShakeToClear"], isOn: store.binding(\.accumulation.shakeToClear))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                VStack(spacing: 3) {
                    SliderRow(label: t["LblMaxHeight"], value: store.binding(\.accumulation.maxHeightPxAt96Dpi), range: 0...100,
                              valueText: ValueText.px(a.maxHeightPxAt96Dpi))
                    SliderRow(label: t["LblDriftOpacity"], value: store.binding(\.accumulation.driftOpacityPercent),
                              range: 10...100, valueText: ValueText.percent(a.driftOpacityPercent))
                }
                SectionHeading(text: t["GroupDriftLook"])
                VStack(alignment: .leading, spacing: 7) {
                    SnowToggle(title: t["ChkSkipMaximized"], isOn: store.binding(\.accumulation.skipMaximized))
                    HStack(alignment: .top) {
                        SnowToggle(title: t["ChkDropOnClose"], isOn: store.binding(\.accumulation.dropSnowOnClose))
                            .frame(maxWidth: .infinity, alignment: .leading)
                        SnowToggle(title: t["ChkNaturalCaps"], isOn: store.binding(\.accumulation.naturalCaps))
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    HStack(alignment: .top) {
                        SnowToggle(title: t["ChkSnowplow"], isOn: store.binding(\.accumulation.snowplow))
                            .frame(maxWidth: .infinity, alignment: .leading)
                        SnowToggle(title: t["ChkIcicles"], isOn: store.binding(\.accumulation.icicles))
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    VStack(alignment: .leading, spacing: 5) {
                        SliderRow(label: t["LblSnowplowDelay"], value: store.binding(\.accumulation.snowplowDelayMinutes),
                                  range: 1...120, valueText: ValueText.minutes(a.snowplowDelayMinutes, t), labelWidth: 146)
                        SnowToggle(title: t["ChkSnowplowAllTaskbars"], isOn: store.binding(\.accumulation.snowplowAllTaskbars),
                                   hint: t["SnowplowAllTaskbarsHint"])
                    }
                    .padding(.leading, 24)
                    .enabled(a.snowplow)
                    SnowToggle(title: t["ChkFallingSnowmen"], isOn: store.binding(\.accumulation.fallingSnowmen))
                    VStack(alignment: .leading, spacing: 5) {
                        SliderRow(label: t["LblSnowmenInterval"], value: store.binding(\.accumulation.snowmenIntervalMinutes),
                                  range: 1...60, valueText: ValueText.everyMinutes(a.snowmenIntervalMinutes, t),
                                  labelWidth: 146, valueWidth: 120)
                        SnowToggle(title: t["ChkSnowmenHeavy"], isOn: store.binding(\.accumulation.snowmenNeedHeavySnow))
                        SliderRow(label: t["LblSnowmenMax"], value: store.binding(\.accumulation.snowmenMaxAlive),
                                  range: 1...10, valueText: ValueText.snowmen(a.snowmenMaxAlive, t),
                                  labelWidth: 146, valueWidth: 120)
                    }
                    .padding(.leading, 24)
                    .enabled(a.fallingSnowmen)
                }
            }
            .enabled(a.enabled)
        }
    }
}

// MARK: - «Инструменты»

struct ToolsPage: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let t = store.strings, i = store.settings.interaction
        VStack(alignment: .leading, spacing: 10) {
            SnowToggle(title: t["ChkInteraction"], isOn: store.binding(\.interaction.enabled))
            Group {
                SnowToggle(title: t["ChkHoldKey"], isOn: store.binding(\.interaction.holdKeyEnabled))
                VStack(alignment: .leading, spacing: 2) {
                    SnowToggle(title: t["ChkHideCursorMac"], isOn: store.binding(\.interaction.hideSystemCursor),
                               hint: t["HideCursorHintMac"])
                    if i.hideSystemCursor && store.diagnostics.cursorMode == "replace-unavailable" {
                        HintText(text: t["CursorReplaceUnavailableMac"])
                            .padding(.leading, 46)
                    }
                }
                SnowToggle(title: t["ChkBlockClicks"], isOn: store.binding(\.interaction.blockClicksWhileHeld),
                           hint: t["BlockClicksHint"])
                SnowToggle(title: t["ChkAllMonitors"], isOn: store.binding(\.interaction.objectsOnAllMonitors))
            }
            .enabled(i.enabled)
            SnowToggle(title: t["ChkUmbrella"], isOn: store.binding(\.interaction.umbrellaCursor))
            Group {
                SectionHeading(text: t["GroupTool"])
                    .padding(.top, 4)
                ToolSegmented(store: store)
                BodyText(text: toolTip, secondary: true)
                VStack(spacing: 3) {
                    SliderRow(label: t["LblMittenRadius"], value: store.binding(\.interaction.brushRadiusPxAt96Dpi),
                              range: 12...48, valueText: ValueText.px(i.brushRadiusPxAt96Dpi))
                    SliderRow(label: t["LblShovelWidth"], value: store.binding(\.interaction.shovelWidthPxAt96Dpi),
                              range: 48...160, valueText: ValueText.px(i.shovelWidthPxAt96Dpi))
                }
                SectionHeading(text: t["GroupSkin"])
                    .padding(.top, 4)
                SkinPicker(store: store)
            }
            .enabled(i.enabled)
        }
    }

    private var toolTip: String {
        let tip: String
        switch store.settings.interaction.tool {
        case .snowball: tip = "TipSnowball"
        case .mitten: tip = "TipMitten"
        case .shovel: tip = "TipShovel"
        }
        return store.holdHint(short: false) + ".\n" + store.strings[tip]
    }
}

/// Mitten / Shovel / Snowball as big segments with the tool glyph.
struct ToolSegmented: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let p = store.palette, t = store.strings
        HStack(spacing: 8) {
            ForEach([ToolKind.mitten, .shovel, .snowball], id: \.self) { tool in
                let selected = store.settings.interaction.tool == tool
                Button {
                    store.update { $0.interaction.tool = tool }
                } label: {
                    HStack(spacing: 8) {
                        GlyphView(kind: .tool(tool), color: selected ? p.accentText.c : p.controlText.c, size: 18)
                        Text(t[ToolChip.titleId(tool)]).font(.system(size: UIMetrics.font, weight: selected ? .semibold : .regular))
                            .foregroundColor(selected ? p.accentText.c : p.controlText.c)
                            .lineLimit(1).minimumScaleFactor(0.8)
                    }
                    .frame(maxWidth: .infinity, minHeight: 36)
                    .background(RoundedRectangle(cornerRadius: UIMetrics.corner).fill(selected ? p.accent.c : p.control.c))
                    .overlay(RoundedRectangle(cornerRadius: UIMetrics.corner)
                        .strokeBorder(selected ? Color.clear : p.controlBorder.c, lineWidth: 1))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? [.isSelected] : [])
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel(t["GroupTool"])
    }
}

/// «Случайный» + every skin from -snowmanSkins, as a grid of choice chips.
struct SkinPicker: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let p = store.palette
        let items = [SnowmanSkinInfo(id: "random", name: store.strings["SkinRandom"])] + store.skins
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 4), spacing: 6) {
            ForEach(items) { skin in
                let selected = store.settings.interaction.snowmanSkin == skin.id
                Button {
                    store.update { $0.interaction.snowmanSkin = skin.id }
                } label: {
                    HStack(spacing: 6) {
                        Circle()
                            .strokeBorder(selected ? p.accent.c : p.controlBorder.c, lineWidth: selected ? 4.5 : 1.2)
                            .frame(width: 13, height: 13)
                        Text(skin.name).font(.system(size: 12.5)).foregroundColor(p.text.c)
                            .lineLimit(1).minimumScaleFactor(0.75)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 8)
                    .frame(height: 28)
                    .background(RoundedRectangle(cornerRadius: 7).fill(selected ? p.control.c : Color.clear))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(skin.name)
                .accessibilityAddTraits(selected ? [.isSelected] : [])
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel(store.strings["GroupSkin"])
    }
}

// MARK: - «Производительность»

struct PerformancePage: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let t = store.strings, d = store.diagnostics
        VStack(alignment: .leading, spacing: 10) {
            SectionHeading(text: t["GroupProfile"])
            SnowSegmented(options: [(PerfProfile.low, t["ProfileLow"]), (.balanced, t["ProfileBalanced"]), (.high, t["ProfileHigh"])],
                          selection: store.binding(\.performance.profile), height: 30, label: t["GroupProfile"])
            SectionHeading(text: t["GroupFps"]).padding(.top, 4)
            SnowSegmented(options: [(30, t["Fps30"]), (60, t["Fps60"])],
                          selection: Binding(get: { store.settings.performance.fpsCap <= 30 ? 30 : 60 },
                                             set: { v in store.update { $0.performance.fpsCap = v } }),
                          height: 30, label: t["GroupFps"])
                .frame(maxWidth: 300)
            VStack(alignment: .leading, spacing: 8) {
                SnowToggle(title: t["ChkPauseFullscreen"], isOn: store.binding(\.performance.pauseOnFullscreen))
                SnowToggle(title: t["ChkReduceBattery"], isOn: store.binding(\.performance.reduceOnBattery))
                SnowToggle(title: t["ChkAnimations"], isOn: store.binding(\.performance.animations))
                SnowToggle(title: t["ChkDetailedLog"], isOn: store.binding(\.performance.detailedLog))
            }
            .padding(.top, 4)
            SectionHeading(text: t["GroupDiagnostics"]).padding(.top, 6)
            let ms = t["UnitMs"]
            let left: [(String, String)] = [
                (t["DiagFps"], d.valid ? String(format: "%.0f", d.fps) : "—"),
                (t["DiagFrame"], d.valid ? String(format: "%.2f %@", d.frameMs, ms) : "—"),
                (t["DiagSim"], d.valid ? String(format: "%.2f %@", d.simStepMs, ms) : "—"),
                (t["DiagParticles"], d.valid ? "\(d.particles)" : "—"),
                (t["DiagSurfaces"], d.valid ? "\(d.surfaces)" : "—"),
                (t["DiagBalls"], d.valid ? "\(d.snowballs) / \(d.snowmen)" : "—"),
            ]
            let right: [(String, String)] = [
                (t["DiagMonitors"], d.valid ? "\(d.monitors)" : "—"),
                (t["DiagWindows"], d.valid ? "\(d.trackedWindows)" : "—"),
                (t["DiagResets"], d.valid ? "\(d.deviceResets)" : "—"),
                (t["DiagPower"], d.valid ? t[d.onBattery ? "PowerBattery" : "PowerMains"] : "—"),
            ]
            HStack(alignment: .top, spacing: 18) {
                DiagColumn(rows: left)
                DiagColumn(rows: right)
            }
            DiagColumn(rows: [(t["DiagStatus"], store.statusText)])
        }
    }
}

private struct DiagColumn: View {
    @Environment(\.palette) private var p
    let rows: [(String, String)]

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(rows.indices, id: \.self) { i in
                HStack(spacing: 8) {
                    Text(rows[i].0).font(.system(size: 12.5)).foregroundColor(p.textSecondary.c).lineLimit(1)
                        .minimumScaleFactor(0.8)
                    Spacer(minLength: 4)
                    Text(rows[i].1).font(.system(size: 12.5).monospacedDigit()).foregroundColor(p.text.c).lineLimit(1)
                }
                .accessibilityElement(children: .combine)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// MARK: - «Праздник»

struct FestivePage: View {
    @ObservedObject var store: UIStore
    let animate: Bool

    var body: some View {
        let t = store.strings
        VStack(alignment: .leading, spacing: 12) {
            SectionHeading(text: t["GroupFestive"])
            HStack(alignment: .center, spacing: 16) {
                VStack(alignment: .leading, spacing: 8) {
                    SnowToggle(title: t["ChkGarland"], isOn: store.binding(\.festive.garland))
                    SnowToggle(title: t["ChkFireworks"], isOn: store.binding(\.festive.fireworks))
                }
                Spacer(minLength: 8)
                Button(t["BtnFireworks"]) { store.actions?.launchFireworks() }
                    .buttonStyle(SnowButtonStyle(primary: true, height: 36))
                    .frame(width: 160)
            }
            BodyText(text: t["FestiveText"], secondary: true)
            FestiveScene(animate: animate)
                .frame(height: 250)
        }
    }
}

// MARK: - «Система»

struct SystemPage: View {
    @ObservedObject var store: UIStore
    @State private var selectedExclusion: String?
    @State private var newName = ""
    @State private var picking = false

    var body: some View {
        let t = store.strings
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 16) {
                SnowToggle(title: t["ChkStartup"], isOn: store.binding(\.startupWithWindows))
                    .frame(maxWidth: .infinity, alignment: .leading)
                SnowToggle(title: t["ChkLeftClick"], isOn: store.binding(\.system.trayLeftClickTogglesPause))
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            LabeledRow(label: t["LblLanguage"], width: 136) {
                // Language names are written in their own language, as in every language menu.
                SnowSegmented(options: [(UILanguage.auto, t["LangAuto"]), (.ru, "Русский"), (.en, "English")],
                              selection: store.binding(\.system.language), label: t["LblLanguage"])
            }
            LabeledRow(label: t["LblTheme"], width: 136) {
                SnowSegmented(options: [(UITheme.night, t["ThemeNight"]), (.globe, t["ThemeGlobe"]), (.postcard, t["ThemePostcard"])],
                              selection: store.binding(\.system.uiTheme), equalWidths: false, label: t["LblTheme"])
            }

            Group {
                SectionHeading(text: t["GroupHotkeys"]).padding(.top, 4)
                HotkeyRow(store: store, label: t["LblHotkeyPause"], keyPath: \.hotkeys.pauseToggle)
                HotkeyRow(store: store, label: t["LblHotkeyClear"], keyPath: \.hotkeys.clearSnow)
                HotkeyRow(store: store, label: t["LblHotkeyInteraction"], keyPath: \.hotkeys.interactionToggle)
                HintText(text: t["HotkeyHint"])
            }
            exclusions
        }
        .sheet(isPresented: $picking) {
            ProcessPicker(store: store, onDone: { picking = false })
                .environment(\.palette, store.palette)
        }
    }

    @ViewBuilder private var exclusions: some View {
        let t = store.strings, s = store.settings
        Group {
            SectionHeading(text: t["GroupExcluded"]).padding(.top, 4)
            HStack(alignment: .top, spacing: 10) {
                ExclusionList(names: s.system.excludedProcesses, selection: $selectedExclusion)
                    .frame(height: 96)
                VStack(spacing: 8) {
                    Button(t["BtnPickRunning"]) { picking = true }
                        .buttonStyle(SnowButtonStyle())
                    Button(t["BtnRemove"]) {
                        if let sel = selectedExclusion {
                            store.update { $0.system.excludedProcesses.removeAll { $0 == sel } }
                            selectedExclusion = nil
                        }
                    }
                    .buttonStyle(SnowButtonStyle())
                    .disabled(selectedExclusion == nil)
                }
                .frame(width: 200)
            }
            LabeledRow(label: t["LblExclMode"], width: 200) {
                SnowSegmented(options: [(ExcludedSnowMode.behind, t["ExclBehind"]), (.around, t["ExclAround"])],
                              selection: store.binding(\.system.excludedSnowMode), label: t["LblExclMode"])
            }
            HStack(spacing: 10) {
                Text(t["LblExeName"]).font(.system(size: UIMetrics.font)).foregroundColor(store.palette.text.c)
                    .lineLimit(1).fixedSize()
                SnowField(placeholder: t["ExeCue"], text: $newName, onSubmit: addTyped)
                Button(t["BtnAdd"], action: addTyped)
                    .buttonStyle(SnowButtonStyle())
                    .frame(width: 200)
                    .disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
    }

    private func addTyped() {
        let name = newName
        newName = ""
        store.update { $0.addExclusion(name) }
    }
}

struct LabeledRow<Content: View>: View {
    @Environment(\.palette) private var p
    let label: String
    var width: CGFloat = 160
    @ViewBuilder let content: () -> Content

    var body: some View {
        HStack(spacing: 10) {
            Text(label).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c)
                .lineLimit(2).minimumScaleFactor(0.85)
                .frame(width: width, alignment: .leading)
                .accessibilityHidden(true)
            content()
        }
    }
}

struct ExclusionList: View {
    @Environment(\.palette) private var p
    let names: [String]
    @Binding var selection: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 1) {
                ForEach(names, id: \.self) { name in
                    let selected = selection == name
                    Button {
                        selection = selected ? nil : name
                    } label: {
                        Text(name).font(.system(size: UIMetrics.font))
                            .foregroundColor(selected ? p.accentText.c : p.fieldText.c)
                            .lineLimit(1)
                            .padding(.horizontal, 8)
                            .frame(maxWidth: .infinity, minHeight: 22, alignment: .leading)
                            .background(RoundedRectangle(cornerRadius: 4).fill(selected ? p.accent.c : Color.clear))
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(selected ? [.isSelected] : [])
                }
            }
            .padding(4)
        }
        .background(RoundedRectangle(cornerRadius: 6).fill(p.field.c))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(p.fieldBorder.c, lineWidth: 1))
    }
}

// MARK: - «О программе»

struct AboutPage: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let t = store.strings, p = store.palette
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .center, spacing: 14) {
                Image(nsImage: NSImage(named: NSImage.applicationIconName) ?? NSImage())
                    .resizable()
                    .frame(width: 52, height: 52)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(alignment: .firstTextBaseline, spacing: 12) {
                        Text(t["AppName"]).font(.system(size: 24, weight: .semibold, design: .rounded)).foregroundColor(p.heading.c)
                            .lineLimit(1).fixedSize()   // never wraps when the scroll bar narrows the page
                        Text(t["PoweredBy"]).font(.system(size: UIMetrics.font)).foregroundColor(p.textSecondary.c)
                            .fixedSize()
                    }
                    Text(t.format("AboutVersionFmt", store.diagnostics.version.isEmpty ? "—" : store.diagnostics.version))
                        .font(.system(size: UIMetrics.font)).foregroundColor(p.textSecondary.c)
                }
                Spacer(minLength: 8)
                LinkButton(title: "GitHub", url: "https://github.com/DaveBugg")
                LinkButton(title: "Telegram", url: "https://t.me/dave_cli")
            }
            BodyText(text: t["AboutDescription"])
            UpdatesSection(store: store)
            SectionHeading(text: t["GroupPrivacy"]).padding(.top, 4)
            BodyText(text: t["AboutPrivacy"], secondary: true)
            SectionHeading(text: t["GroupLog"]).padding(.top, 4)
            HStack(spacing: 10) {
                Text(t["LblLogFile"]).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c).fixedSize()
                Text(store.diagnostics.logPath.isEmpty ? "—" : store.diagnostics.logPath)
                    .font(.system(size: 12).monospaced()).foregroundColor(p.fieldText.c)
                    .lineLimit(1).truncationMode(.middle)
                    .textSelection(.enabled)
                    .padding(.horizontal, 9)
                    .frame(maxWidth: .infinity, minHeight: 30, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 6).fill(p.field.c))
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(p.fieldBorder.c, lineWidth: 1))
            }
            Button(t["BtnOpenLogFolder"]) {
                let path = store.diagnostics.logPath
                guard !path.isEmpty else { return }
                NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
            }
            .buttonStyle(SnowButtonStyle())
            .frame(width: 250)
            .disabled(store.diagnostics.logPath.isEmpty)
            BodyText(text: t["AboutLicence"], secondary: true)
                .padding(.top, 4)
        }
    }
}

/// «Обновления»: automatic check, «Check now», the state, and for a newer version «Update»
/// (signed release, replaceable copy) or the manual hint with the download page.
struct UpdatesSection: View {
    @ObservedObject var store: UIStore

    var body: some View {
        let t = store.strings, u = store.updateStatus
        VStack(alignment: .leading, spacing: 8) {
            SectionHeading(text: t["GroupUpdates"]).padding(.top, 4)
            SnowToggle(title: t["ChkCheckUpdates"],
                       isOn: Binding(get: { store.settings.updates.check }, set: { store.setUpdateCheck($0) }))
            HStack(spacing: 12) {
                Button(t["BtnCheckUpdates"]) { store.actions?.checkForUpdates() }
                    .buttonStyle(SnowButtonStyle())
                    .frame(width: 190)
                    .disabled(u.busy)
                HintText(text: u.line(t), role: u.lineRole)
                    .accessibilityAddTraits(.updatesFrequently)
            }
            if u.stage == .available || (u.stage == .failed && !u.latest.isEmpty) {
                if u.canInstall && u.stage == .available {
                    HStack(spacing: 10) {
                        Button(t["BtnUpdateInstall"]) { store.actions?.installUpdate() }
                            .buttonStyle(SnowButtonStyle(primary: true))
                            .frame(width: 190)
                        Button(t["BtnWhatsNew"]) { store.actions?.openUpdatePage() }
                            .buttonStyle(SnowButtonStyle())
                            .frame(width: 190)
                        Spacer(minLength: 0)
                    }
                } else {
                    HintText(text: t["UpdateManualHint"])
                    HStack(spacing: 10) {
                        Button(t["BtnOpenReleasePage"]) { store.actions?.openUpdatePage() }
                            .buttonStyle(SnowButtonStyle(primary: true))
                            .frame(width: 190)
                        Button(t["BtnWhatsNew"]) { store.actions?.openUpdatePage() }
                            .buttonStyle(SnowButtonStyle())
                            .frame(width: 190)
                        Spacer(minLength: 0)
                    }
                }
            } else if u.stage == .downloading {
                ProgressView(value: Double(u.progress), total: 100)
                    .progressViewStyle(.linear)
                    .tint(store.palette.accent.c)
                    .frame(maxWidth: 400)
                    .accessibilityLabel(u.line(t))
            }
        }
    }
}

/// A link that opens only on an explicit click (the app itself never connects anywhere).
struct LinkButton: View {
    @Environment(\.palette) private var p
    let title: String
    let url: String

    var body: some View {
        Button {
            if let u = URL(string: url) { NSWorkspace.shared.open(u) }
        } label: {
            Text(title).font(.system(size: UIMetrics.font, weight: .medium)).foregroundColor(p.heading.c).underline()
                .padding(.horizontal, 6)
                .frame(height: 28)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(url)
        .accessibilityAddTraits(.isLink)
        .accessibilityHint(url)
    }
}
