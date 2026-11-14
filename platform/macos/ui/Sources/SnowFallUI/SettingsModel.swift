// Settings model mirroring core/core/settings.h and the JSON of core/settings/settings_json.cpp.
//
// Decoding is tolerant like the core (a missing or mistyped key keeps the default). Encoding
// writes every modelled key; SettingsDocument merges it into the JSON received from the app so
// keys this UI does not know about (newer core, other platforms) survive an edit.
import Foundation

enum WeatherPreset: String, Codable, CaseIterable, Identifiable {
    case light, normal, blizzard, thaw
    var id: String { rawValue }
}

enum PerfProfile: String, Codable, CaseIterable { case low, balanced, high }
enum ToolKind: String, Codable, CaseIterable { case mitten, shovel, snowball }
enum UILanguage: String, Codable, CaseIterable { case auto, ru, en }
enum ExcludedSnowMode: String, Codable, CaseIterable { case behind, around }
enum UITheme: String, Codable, CaseIterable { case night, globe, postcard }

struct WeatherSettings: Codable, Equatable {
    var preset: WeatherPreset = .normal
    var intensity = 55            // 0..100
    var wind = 10                 // -100..100
    var gustiness = 20            // 0..100
    var sizePercent = 100         // 50..200
    var fallSpeedPercent = 100    // 25..200
    var opacityPercent = 100      // 10..100
    var skyPerMonitor = true
}

struct AccumulationSettings: Codable, Equatable {
    var enabled = true
    var onWindows = true
    var onTaskbar = true
    var onScreenBottom = true
    var maxHeightPxAt96Dpi = 36   // 0..100
    var shakeOffOnMove = true
    var skipMaximized = true
    var dropSnowOnClose = true
    var driftOpacityPercent = 90  // 10..100
    var shakeToClear = false
    var naturalCaps = true
    var fallingSnowmen = true
    var snowmenIntervalMinutes = 2   // 1..60
    var snowmenNeedHeavySnow = true
    var snowmenMaxAlive = 3          // 1..10
    var snowplow = true
    var snowplowDelayMinutes = 10    // 1..120
    var snowplowAllTaskbars = true
    var icicles = true
}

struct InteractionSettings: Codable, Equatable {
    var enabled = true
    var holdKeyEnabled = true
    var tool: ToolKind = .snowball
    var snowmanSkin = "random"
    var brushRadiusPxAt96Dpi = 28    // 12..48
    var shovelWidthPxAt96Dpi = 96    // 48..160
    var blockClicksWhileHeld = true
    var objectsOnAllMonitors = false
    var umbrellaCursor = true
    var hideSystemCursor = false     // macOS: the tool replaces the system cursor (experimental)
}

struct PerformanceSettings: Codable, Equatable {
    var profile: PerfProfile = .balanced
    var fpsCap = 60
    var pauseOnFullscreen = true
    var reduceOnBattery = true
    var animations = true
    var detailedLog = false          // debug records + 2 MB log instead of 256 KB
}

struct HotkeySettings: Codable, Equatable {
    var pauseToggle = "Ctrl+Alt+S"
    var clearSnow = "Ctrl+Alt+X"
    var interactionToggle = "Ctrl+Alt+I"
}

struct SystemSettings: Codable, Equatable {
    var language: UILanguage = .auto
    var uiTheme: UITheme = .night
    var trayLeftClickTogglesPause = false
    var excludedProcesses: [String] = []
    var excludedSnowMode: ExcludedSnowMode = .behind
}

struct FestiveSettings: Codable, Equatable {
    var garland = false
    var fireworks = true
}

struct UserPreset: Codable, Equatable {
    var name = ""
    var weather = WeatherSettings()
    var maxHeightPxAt96Dpi = 36
    var driftOpacityPercent = 90
}

struct PrivacySettings: Codable, Equatable {
    var telemetry = false
}

/// json "updates". "lastCheck" belongs to the app: it is not modelled, so the document keeps the
/// received value (and the app ignores what the UI sends for it).
struct UpdateSettings: Codable, Equatable {
    var check = false   // automatic check once a day
    var asked = false   // the one-time question was answered
}

struct Settings: Codable, Equatable {
    var schemaVersion = 1
    var enabled = true
    var startupWithWindows = false
    var weather = WeatherSettings()
    var accumulation = AccumulationSettings()
    var interaction = InteractionSettings()
    var performance = PerformanceSettings()
    var hotkeys = HotkeySettings()
    var system = SystemSettings()
    var festive = FestiveSettings()
    var userPresets: [UserPreset] = []
    var privacy = PrivacySettings()
    var updates = UpdateSettings()

    static let maxUserPresets = 20
    static let maxPresetNameChars = 40
}

// MARK: - tolerant decoding

private extension KeyedDecodingContainer {
    /// The value for `key`, or `fallback` when it is missing or has the wrong type.
    func value<T: Decodable>(_ key: Key, _ fallback: T) -> T {
        guard let v = try? decodeIfPresent(T.self, forKey: key) else { return fallback }
        return v
    }
}

extension WeatherSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        preset = c.value(.preset, preset)
        intensity = c.value(.intensity, intensity)
        wind = c.value(.wind, wind)
        gustiness = c.value(.gustiness, gustiness)
        sizePercent = c.value(.sizePercent, sizePercent)
        fallSpeedPercent = c.value(.fallSpeedPercent, fallSpeedPercent)
        opacityPercent = c.value(.opacityPercent, opacityPercent)
        skyPerMonitor = c.value(.skyPerMonitor, skyPerMonitor)
    }
}

extension AccumulationSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = c.value(.enabled, enabled)
        onWindows = c.value(.onWindows, onWindows)
        onTaskbar = c.value(.onTaskbar, onTaskbar)
        onScreenBottom = c.value(.onScreenBottom, onScreenBottom)
        maxHeightPxAt96Dpi = c.value(.maxHeightPxAt96Dpi, maxHeightPxAt96Dpi)
        shakeOffOnMove = c.value(.shakeOffOnMove, shakeOffOnMove)
        skipMaximized = c.value(.skipMaximized, skipMaximized)
        dropSnowOnClose = c.value(.dropSnowOnClose, dropSnowOnClose)
        driftOpacityPercent = c.value(.driftOpacityPercent, driftOpacityPercent)
        shakeToClear = c.value(.shakeToClear, shakeToClear)
        naturalCaps = c.value(.naturalCaps, naturalCaps)
        fallingSnowmen = c.value(.fallingSnowmen, fallingSnowmen)
        snowmenIntervalMinutes = c.value(.snowmenIntervalMinutes, snowmenIntervalMinutes)
        snowmenNeedHeavySnow = c.value(.snowmenNeedHeavySnow, snowmenNeedHeavySnow)
        snowmenMaxAlive = c.value(.snowmenMaxAlive, snowmenMaxAlive)
        snowplow = c.value(.snowplow, snowplow)
        snowplowDelayMinutes = c.value(.snowplowDelayMinutes, snowplowDelayMinutes)
        snowplowAllTaskbars = c.value(.snowplowAllTaskbars, snowplowAllTaskbars)
        icicles = c.value(.icicles, icicles)
    }
}

extension InteractionSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = c.value(.enabled, enabled)
        holdKeyEnabled = c.value(.holdKeyEnabled, holdKeyEnabled)
        tool = c.value(.tool, tool)
        snowmanSkin = c.value(.snowmanSkin, snowmanSkin)
        brushRadiusPxAt96Dpi = c.value(.brushRadiusPxAt96Dpi, brushRadiusPxAt96Dpi)
        shovelWidthPxAt96Dpi = c.value(.shovelWidthPxAt96Dpi, shovelWidthPxAt96Dpi)
        blockClicksWhileHeld = c.value(.blockClicksWhileHeld, blockClicksWhileHeld)
        objectsOnAllMonitors = c.value(.objectsOnAllMonitors, objectsOnAllMonitors)
        umbrellaCursor = c.value(.umbrellaCursor, umbrellaCursor)
        hideSystemCursor = c.value(.hideSystemCursor, hideSystemCursor)
    }
}

extension PerformanceSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        profile = c.value(.profile, profile)
        fpsCap = c.value(.fpsCap, fpsCap)
        pauseOnFullscreen = c.value(.pauseOnFullscreen, pauseOnFullscreen)
        reduceOnBattery = c.value(.reduceOnBattery, reduceOnBattery)
        animations = c.value(.animations, animations)
        detailedLog = c.value(.detailedLog, detailedLog)
    }
}

extension HotkeySettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        pauseToggle = c.value(.pauseToggle, pauseToggle)
        clearSnow = c.value(.clearSnow, clearSnow)
        interactionToggle = c.value(.interactionToggle, interactionToggle)
    }
}

extension SystemSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        language = c.value(.language, language)
        uiTheme = c.value(.uiTheme, uiTheme)
        trayLeftClickTogglesPause = c.value(.trayLeftClickTogglesPause, trayLeftClickTogglesPause)
        // Like the core: a list with a non-string entry keeps only the strings.
        if let list = try? c.decodeIfPresent([LossyString].self, forKey: .excludedProcesses) {
            excludedProcesses = list.compactMap { $0.value }
        }
        excludedSnowMode = c.value(.excludedSnowMode, excludedSnowMode)
    }
}

extension FestiveSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        garland = c.value(.garland, garland)
        fireworks = c.value(.fireworks, fireworks)
    }
}

extension UserPreset {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)   // entries without a name are skipped
        weather = c.value(.weather, weather)
        maxHeightPxAt96Dpi = c.value(.maxHeightPxAt96Dpi, maxHeightPxAt96Dpi)
        driftOpacityPercent = c.value(.driftOpacityPercent, driftOpacityPercent)
    }
}

extension UpdateSettings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        check = c.value(.check, check)
        asked = c.value(.asked, asked)
    }
}

extension PrivacySettings {
    init(from decoder: Decoder) throws {
        self.init()   // telemetry is never read: always false
    }
}

extension Settings {
    init(from decoder: Decoder) throws {
        self.init()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        schemaVersion = c.value(.schemaVersion, schemaVersion)
        enabled = c.value(.enabled, enabled)
        startupWithWindows = c.value(.startupWithWindows, startupWithWindows)
        weather = c.value(.weather, weather)
        accumulation = c.value(.accumulation, accumulation)
        interaction = c.value(.interaction, interaction)
        performance = c.value(.performance, performance)
        hotkeys = c.value(.hotkeys, hotkeys)
        system = c.value(.system, system)
        festive = c.value(.festive, festive)
        if let list = try? c.decodeIfPresent([Lossy<UserPreset>].self, forKey: .userPresets) {
            userPresets = list.compactMap { $0.value }
        }
        privacy = PrivacySettings()
        updates = c.value(.updates, updates)
    }
}

/// Decodes any JSON value; `value` is nil when it is not a string.
private struct LossyString: Decodable {
    let value: String?
    init(from decoder: Decoder) throws {
        value = try? decoder.singleValueContainer().decode(String.self)
    }
}

/// Decodes any JSON value; `value` is nil when it does not decode as T.
private struct Lossy<T: Decodable>: Decodable {
    let value: T?
    init(from decoder: Decoder) throws {
        value = try? T(from: decoder)
    }
}

// MARK: - document (JSON in, JSON out, unknown keys preserved)

struct SettingsDocument {
    private(set) var raw: [String: Any]
    var settings: Settings

    init() {
        raw = [:]
        settings = Settings()
    }

    /// nil when `json` is not a JSON object.
    init?(json: String) {
        guard let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data),
              let dict = obj as? [String: Any] else { return nil }
        raw = dict
        settings = (try? JSONDecoder().decode(Settings.self, from: data)) ?? Settings()
    }

    /// The whole settings JSON: the received document with every modelled key replaced.
    func json() -> String {
        var merged = raw
        if let data = try? JSONEncoder().encode(settings),
           let obj = try? JSONSerialization.jsonObject(with: data),
           let known = obj as? [String: Any] {
            merged = SettingsDocument.merge(known, into: raw)
        }
        guard let out = try? JSONSerialization.data(withJSONObject: merged, options: [.sortedKeys]),
              let text = String(data: out, encoding: .utf8) else { return "{}" }
        return text
    }

    /// Objects are merged key by key (recursively); anything else in `known` replaces the old value.
    static func merge(_ known: [String: Any], into base: [String: Any]) -> [String: Any] {
        var out = base
        for (key, value) in known {
            if let sub = value as? [String: Any], let old = base[key] as? [String: Any] {
                out[key] = merge(sub, into: old)
            } else {
                out[key] = value
            }
        }
        return out
    }
}

// MARK: - helpers shared by the views

enum PresetNameError: Equatable { case none, length, duplicate, tooMany }

extension Settings {
    func validatePresetName(_ raw: String) -> PresetNameError {
        let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty || name.count > Settings.maxPresetNameChars { return .length }
        if userPresets.contains(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame }) { return .duplicate }
        if userPresets.count >= Settings.maxUserPresets { return .tooMany }
        return .none
    }

    func makeUserPreset(named name: String) -> UserPreset {
        UserPreset(name: name.trimmingCharacters(in: .whitespacesAndNewlines), weather: weather,
                   maxHeightPxAt96Dpi: accumulation.maxHeightPxAt96Dpi,
                   driftOpacityPercent: accumulation.driftOpacityPercent)
    }

    mutating func apply(_ preset: UserPreset) {
        weather = preset.weather
        accumulation.maxHeightPxAt96Dpi = preset.maxHeightPxAt96Dpi
        accumulation.driftOpacityPercent = preset.driftOpacityPercent
    }

    /// «Восстановить значения по умолчанию»: everything back to defaults except the user's own
    /// presets (user data, not a setting) — same rule as the Windows UI — and the answer about
    /// update checks (a consent, not a look).
    func restoredDefaults() -> Settings {
        var s = Settings()
        s.userPresets = userPresets
        s.updates = updates
        return s
    }

    /// Adds an app name to the exclusions (trimmed, path stripped, case-insensitive unique).
    /// Returns false when nothing was added.
    @discardableResult
    mutating func addExclusion(_ raw: String) -> Bool {
        var name = raw.replacingOccurrences(of: "\"", with: "")
        if let slash = name.lastIndex(where: { $0 == "/" || $0 == "\\" }) {
            name = String(name[name.index(after: slash)...])
        }
        name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return false }
        if system.excludedProcesses.contains(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) { return false }
        system.excludedProcesses.append(name)
        return true
    }
}
