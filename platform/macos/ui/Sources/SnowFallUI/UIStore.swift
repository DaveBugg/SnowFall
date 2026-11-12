// State shared by the views of one window (settings window or quick panel).
//
// Every user edit goes through update(_:): the model changes, the WHOLE settings JSON (with
// unknown keys preserved) is sent to -applySettingsJSON:, and the preview gets it too.
// load(json:) is the way back from the app (-updateSettingsJSON:): it never calls the app.
import AppKit
import Combine
import SFBridge
import SwiftUI

enum SettingsPage: Int, CaseIterable, Identifiable {
    case snow, drifts, tools, performance, festive, system, about
    var id: Int { rawValue }

    var titleId: String {
        switch self {
        case .snow: return "TabSnow"
        case .drifts: return "TabDrifts"
        case .tools: return "TabTools"
        case .performance: return "TabPerformance"
        case .festive: return "TabFestive"
        case .system: return "TabSystem"
        case .about: return "TabAbout"
        }
    }
}

enum MessageRole { case secondary, error, ok }

final class UIStore: ObservableObject {
    weak var actions: (any SFAppActions)?

    @Published private(set) var settings = Settings()
    @Published private(set) var strings = L10n()
    @Published private(set) var skins: [SnowmanSkinInfo] = []
    @Published private(set) var diagnostics = Diagnostics()
    @Published private(set) var hotkeyStatus: [String: String] = [:]   // core text -> "" (ok) or reason
    /// Window/popover on screen and not occluded: decorative animation and polling run only then.
    @Published var visible = false
    /// Self-test: decorative animation stops at a fixed frame so screenshots are reproducible.
    @Published var frozen = false
    @Published var page: SettingsPage = .snow
    /// Quick panel: the running-apps picker replaces the panel content.
    @Published var showPicker = false
    @Published var presetMessage: (text: String, role: MessageRole) = ("", .secondary)
    /// Fixed "now" for the countdown (self-test screenshots); nil = the real clock.
    @Published var nowOverride: Date?

    private var document = SettingsDocument()
    private var stringsLanguage: UILanguage?
    private(set) var loaded = false

    /// The preview view from -makePreviewView (settings window only), fed with every change.
    weak var preview: NSView?

    init(actions: any SFAppActions) {
        self.actions = actions
    }

    var palette: Palette { Palette.of(settings.system.uiTheme) }
    /// Theme changes (for the hosting window / popover appearance).
    var themePublisher: AnyPublisher<UITheme, Never> {
        $settings.map(\.system.uiTheme).removeDuplicates().eraseToAnyPublisher()
    }
    /// String table changes (window title).
    var stringsPublisher: AnyPublisher<L10n, Never> { $strings.eraseToAnyPublisher() }
    /// Decorations animate only while on screen (and never in the frozen self-test mode).
    var animating: Bool { visible && !frozen }
    var json: String { document.json() }

    // MARK: app -> UI

    /// Replaces the model with the app's JSON; no echo to the app.
    func load(json: String) {
        guard let doc = SettingsDocument(json: json) else { return }
        document = doc
        if settings != doc.settings { settings = doc.settings }
        loaded = true
        refreshStringsIfNeeded()
        refreshHotkeyStatus()
        pushToPreview()
    }

    func refreshStringsIfNeeded(force: Bool = false) {
        guard let actions else { return }
        if !force, stringsLanguage == settings.system.language, !strings.table.isEmpty { return }
        stringsLanguage = settings.system.language
        let table = L10n(actions.uiStrings())
        if table != strings { strings = table }
        let list = actions.snowmanSkins().compactMap { d -> SnowmanSkinInfo? in
            guard let id = d["id"], !id.isEmpty else { return nil }
            return SnowmanSkinInfo(id: id, name: d["name"] ?? id)
        }
        if list != skins { skins = list }
        refreshHotkeyStatus()
    }

    func refreshDiagnostics() {
        guard let actions else { return }
        let d = Diagnostics(json: actions.diagnosticsJSON())
        if d != diagnostics { diagnostics = d }
    }

    // MARK: UI -> app

    /// Applies a user edit and sends the whole settings JSON to the app (only if something changed).
    func update(_ change: (inout Settings) -> Void) {
        var s = settings
        change(&s)
        guard s != settings else { return }
        let languageChanged = s.system.language != settings.system.language
        let hotkeysChanged = s.hotkeys != settings.hotkeys
        settings = s
        document.settings = s
        let text = document.json()
        actions?.applySettingsJSON(text)
        pushToPreview(text)
        if languageChanged { refreshStringsIfNeeded(force: true) }
        if hotkeysChanged { refreshHotkeyStatus() }
    }

    func binding<T: Equatable>(_ keyPath: WritableKeyPath<Settings, T>) -> Binding<T> {
        Binding(get: { self.settings[keyPath: keyPath] },
                set: { value in self.update { $0[keyPath: keyPath] = value } })
    }

    private func pushToPreview(_ text: String? = nil) {
        guard let p = preview as? SFPreviewView else { return }
        p.setSettingsJSON(text ?? document.json())
    }

    func resetPreview() {
        (preview as? SFPreviewView)?.resetPreview()
    }

    // MARK: hotkeys

    private func refreshHotkeyStatus() {
        guard let actions else { return }
        var status: [String: String] = [:]
        for text in [settings.hotkeys.pauseToggle, settings.hotkeys.clearSnow, settings.hotkeys.interactionToggle]
        where !text.isEmpty {
            status[text] = actions.validateHotkey(text)
        }
        if status != hotkeyStatus { hotkeyStatus = status }
    }

    // MARK: user presets

    /// Saves the current look under `name`; returns false (and sets the message) when invalid.
    @discardableResult
    func savePreset(named name: String) -> Bool {
        switch settings.validatePresetName(name) {
        case .length:
            presetMessage = (strings["ErrPresetLength"], .error)
            return false
        case .duplicate:
            presetMessage = (strings["ErrPresetDuplicate"], .error)
            return false
        case .tooMany:
            presetMessage = (strings["ErrPresetTooMany"], .error)
            return false
        case .none:
            break
        }
        update { $0.userPresets.append($0.makeUserPreset(named: name)) }
        presetMessage = (strings["PresetSaved"], .ok)
        return true
    }

    func applyPreset(at index: Int) {
        guard settings.userPresets.indices.contains(index) else { return }
        update { $0.apply($0.userPresets[index]) }
    }

    func deletePreset(at index: Int) {
        guard settings.userPresets.indices.contains(index) else { return }
        update { $0.userPresets.remove(at: index) }
        presetMessage = ("", .secondary)
    }

    /// The hint under the chips («Щелчок — применить…») when there is no fresher message.
    var presetHint: (text: String, role: MessageRole) {
        if !presetMessage.text.isEmpty { return presetMessage }
        return (settings.userPresets.isEmpty ? "" : strings["PresetsHint"], .secondary)
    }

    // MARK: texts used in several places

    /// «Удерживайте Ctrl+Option, чтобы…» (long) or «Удерживайте Ctrl+Option — и трогайте снег» (short).
    func holdHint(short: Bool) -> String {
        let keys = diagnostics.interactionKeys
        if keys.isEmpty || !settings.interaction.enabled {
            return strings[short ? "HoldHintShortNone" : "HoldHintNone"]
        }
        return strings.format(short ? "HoldHintShortFmt" : "HoldHintFmt", keys)
    }

    var statusText: String {
        if !diagnostics.statusText.isEmpty { return diagnostics.statusText }
        guard diagnostics.valid else { return "—" }
        return strings[diagnostics.paused || diagnostics.autoPaused ? "StatusPaused" : "StatusRunning"]
    }

    var now: Date { nowOverride ?? Date() }
}
