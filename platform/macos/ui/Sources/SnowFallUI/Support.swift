// Small pure helpers: live diagnostics, the New Year countdown and hotkey text conversion.
import AppKit
import Carbon.HIToolbox

// MARK: - diagnostics (-diagnosticsJSON)

struct Diagnostics: Equatable {
    var fps = 0.0
    var frameMs = 0.0
    var simStepMs = 0.0
    var particles = 0
    var surfaces = 0
    var snowballs = 0
    var snowmen = 0
    var monitors = 0
    var trackedWindows = 0
    var deviceResets = 0
    var paused = false
    var autoPaused = false
    var onBattery = false
    var statusText = ""
    var interactionKeys = ""
    var logPath = ""
    var version = ""
    /// "replace-unavailable" when the app could not hide the system cursor (interaction.hideSystemCursor).
    var cursorMode = ""
    var valid = false

    init() {}

    init(json: String) {
        guard let data = json.data(using: .utf8),
              let d = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return }
        func num(_ k: String) -> Double? { (d[k] as? NSNumber)?.doubleValue }
        func int(_ k: String) -> Int { Int(num(k) ?? 0) }
        func bool(_ k: String) -> Bool { (d[k] as? NSNumber)?.boolValue ?? false }
        func str(_ k: String) -> String { d[k] as? String ?? "" }
        fps = num("fps") ?? 0
        frameMs = num("frameMs") ?? 0
        simStepMs = num("simStepMs") ?? 0
        particles = int("particles")
        surfaces = int("surfaces")
        snowballs = int("snowballs")
        snowmen = int("snowmen")
        monitors = int("monitors")
        trackedWindows = int("trackedWindows")
        deviceResets = int("deviceResets")
        paused = bool("paused")
        autoPaused = bool("autoPaused")
        onBattery = bool("onBattery")
        statusText = str("statusText")
        interactionKeys = str("interactionKeys")
        logPath = str("logPath")
        version = str("version")
        cursorMode = str("cursorMode")
        valid = true
    }
}

// MARK: - New Year countdown (rules of platform/windows/app/ui/countdown.cpp)

struct Countdown: Equatable {
    var big = ""
    var caption = ""
    var festive = false

    static func compute(now: Date, strings t: L10n, calendar: Calendar = .current) -> Countdown {
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute], from: now)
        let year = c.year ?? 2000, month = c.month ?? 1, day = c.day ?? 1
        if month == 1 && day <= 7 {
            return Countdown(big: t["HappyNewYear"], caption: t["HappyHolidays"], festive: true)
        }
        if month == 12 && day == 31 {
            let minutesLeft = 24 * 60 - ((c.hour ?? 0) * 60 + (c.minute ?? 0))
            let hours = (minutesLeft + 59) / 60
            return Countdown(big: "\(hours)",
                             caption: t[t.plural(hours, "CountdownHour1", "CountdownHour2", "CountdownHour5")])
        }
        // Whole days between today and January 1 (calendar days, not 24-hour periods).
        var utc = Calendar(identifier: .gregorian)
        utc.timeZone = TimeZone(identifier: "UTC") ?? .current
        let today = utc.date(from: DateComponents(year: year, month: month, day: day)) ?? now
        let newYear = utc.date(from: DateComponents(year: year + 1, month: 1, day: 1)) ?? now
        let days = utc.dateComponents([.day], from: today, to: newYear).day ?? 0
        return Countdown(big: "\(days)", caption: t[t.plural(days, "CountdownDay1", "CountdownDay2", "CountdownDay5")])
    }
}

// MARK: - hotkeys

/// Hotkey texts are stored in the core grammar (core/settings/hotkey.h: "Ctrl+Alt+Shift+Win+<Key>",
/// which the core's sanitize() keeps) and shown with Mac names: Ctrl, Option, Shift, Cmd.
enum HotkeyText {
    struct Modifiers: OptionSet, Hashable {
        let rawValue: Int
        static let ctrl = Modifiers(rawValue: 1)
        static let option = Modifiers(rawValue: 2)
        static let shift = Modifiers(rawValue: 4)
        static let command = Modifiers(rawValue: 8)
    }

    /// Core text for modifiers + key name ("Ctrl+Alt+S"); the order is the core's canonical one.
    static func coreText(_ mods: Modifiers, key: String) -> String {
        var parts: [String] = []
        if mods.contains(.ctrl) { parts.append("Ctrl") }
        if mods.contains(.option) { parts.append("Alt") }
        if mods.contains(.shift) { parts.append("Shift") }
        if mods.contains(.command) { parts.append("Win") }
        parts.append(key)
        return parts.joined(separator: "+")
    }

    /// Parses core text (also accepts the Mac spellings) into modifiers + key; nil if malformed.
    static func parse(_ text: String) -> (Modifiers, String)? {
        let tokens = text.split(separator: "+").map { $0.trimmingCharacters(in: .whitespaces) }
        guard !tokens.isEmpty else { return nil }
        var mods: Modifiers = []
        var key: String?
        for t in tokens {
            switch t.lowercased() {
            case "ctrl", "control", "⌃": mods.insert(.ctrl)
            case "alt", "option", "opt", "⌥": mods.insert(.option)
            case "shift", "⇧": mods.insert(.shift)
            case "win", "cmd", "command", "⌘": mods.insert(.command)
            default:
                if key != nil || t.isEmpty { return nil }
                key = t
            }
        }
        guard let k = key else { return nil }
        return (mods, k)
    }

    /// "Ctrl+Option+Shift+Cmd+S" — the Mac order of modifier names (⌃⌥⇧⌘).
    static func display(_ text: String) -> String {
        guard let (mods, key) = parse(text) else { return text }
        return displayText(mods, key: key)
    }

    static func displayText(_ mods: Modifiers, key: String?) -> String {
        var parts: [String] = []
        if mods.contains(.ctrl) { parts.append("Ctrl") }
        if mods.contains(.option) { parts.append("Option") }
        if mods.contains(.shift) { parts.append("Shift") }
        if mods.contains(.command) { parts.append("Cmd") }
        if let key { parts.append(key) }
        return parts.joined(separator: "+")
    }

    static func modifiers(_ flags: NSEvent.ModifierFlags) -> Modifiers {
        var m: Modifiers = []
        if flags.contains(.control) { m.insert(.ctrl) }
        if flags.contains(.option) { m.insert(.option) }
        if flags.contains(.shift) { m.insert(.shift) }
        if flags.contains(.command) { m.insert(.command) }
        return m
    }

    /// Core key name for a hardware key code (layout-independent for letters and digits), or nil
    /// for keys the core grammar cannot express.
    static func keyName(keyCode: Int) -> String? {
        if let name = namedKeys[keyCode] { return name }
        if let fn = functionKeys.firstIndex(of: keyCode) { return "F\(fn + 1)" }
        return nil
    }

    private static let namedKeys: [Int: String] = [
        kVK_ANSI_A: "A", kVK_ANSI_B: "B", kVK_ANSI_C: "C", kVK_ANSI_D: "D", kVK_ANSI_E: "E", kVK_ANSI_F: "F",
        kVK_ANSI_G: "G", kVK_ANSI_H: "H", kVK_ANSI_I: "I", kVK_ANSI_J: "J", kVK_ANSI_K: "K", kVK_ANSI_L: "L",
        kVK_ANSI_M: "M", kVK_ANSI_N: "N", kVK_ANSI_O: "O", kVK_ANSI_P: "P", kVK_ANSI_Q: "Q", kVK_ANSI_R: "R",
        kVK_ANSI_S: "S", kVK_ANSI_T: "T", kVK_ANSI_U: "U", kVK_ANSI_V: "V", kVK_ANSI_W: "W", kVK_ANSI_X: "X",
        kVK_ANSI_Y: "Y", kVK_ANSI_Z: "Z",
        kVK_ANSI_0: "0", kVK_ANSI_1: "1", kVK_ANSI_2: "2", kVK_ANSI_3: "3", kVK_ANSI_4: "4",
        kVK_ANSI_5: "5", kVK_ANSI_6: "6", kVK_ANSI_7: "7", kVK_ANSI_8: "8", kVK_ANSI_9: "9",
        kVK_Space: "Space", kVK_Tab: "Tab", kVK_Escape: "Esc", kVK_Return: "Enter", kVK_ANSI_KeypadEnter: "Enter",
        kVK_Help: "Insert", kVK_ForwardDelete: "Delete", kVK_Home: "Home", kVK_End: "End",
        kVK_PageUp: "PageUp", kVK_PageDown: "PageDown",
        kVK_UpArrow: "Up", kVK_DownArrow: "Down", kVK_LeftArrow: "Left", kVK_RightArrow: "Right",
    ]

    private static let functionKeys: [Int] = [
        kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10,
        kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20,
    ]
}
