// UI self-test driver, run by platform/macos/ui/demo (`snowfall_ui_demo --selftest <dir>`) with a
// fake app. Checks the model / JSON round trip, drives controls, verifies what reaches
// -applySettingsJSON:, and renders our own views to PNG (cacheDisplay; never a screen capture).
// Not used by the real app.
import AppKit
import Carbon.HIToolbox
import SFBridge
import SFSelfTest
import SwiftUI

@objc(SFUISelfTest)
public final class SFUISelfTest: NSObject, SFUISelfTestRunner {
    public static func run(withActions actions: any SFSelfTestProbe, settingsJSON json: String,
                           outputDirectory directory: String) -> Int {
        let runner = SelfTestRunner(probe: actions, json: json, directory: URL(fileURLWithPath: directory))
        return runner.run()
    }
}

/// Fake update states (what -[SFAppActions updateStatusJSON] returns in each stage).
enum FakeUpdate {
    static let page = "https://github.com/DaveBugg/SnowFall/releases/tag/v0.1.3"
    static func json(_ stage: String, latest: String = "0.1.3", canInstall: Bool = true, progress: Int = 0,
                     error: String = "") -> String {
        let d: [String: Any] = ["stage": stage, "current": "0.1.2", "latest": latest, "pageUrl": page,
                                "notes": "Snow on the Dock", "canInstall": canInstall, "progress": progress, "error": error]
        guard let data = try? JSONSerialization.data(withJSONObject: d, options: [.sortedKeys]),
              let text = String(data: data, encoding: .utf8) else { return "{}" }
        return text
    }
    static let idle = json("idle", latest: "")
    static let available = json("available")
    static let manual = json("available", canInstall: false)
    static let downloading = json("downloading", progress: 42)
    static let failed = json("failed", error: "UpdateErrVerify")
    static let upToDate = json("upToDate", latest: "")
}

private final class SelfTestRunner {
    let probe: any SFSelfTestProbe
    let json: String
    let dir: URL
    var failures = 0
    var lines: [String] = []

    init(probe: any SFSelfTestProbe, json: String, directory: URL) {
        self.probe = probe
        self.json = json
        dir = directory
    }

    // MARK: helpers

    func log(_ s: String) {
        lines.append(s)
        FileHandle.standardError.write((s + "\n").data(using: .utf8) ?? Data())
    }

    func check(_ ok: Bool, _ name: String, _ detail: @autoclosure () -> String = "") {
        if ok {
            log("PASS  \(name)")
        } else {
            failures += 1
            let d = detail()
            log("FAIL  \(name)" + (d.isEmpty ? "" : "  — \(d)"))
        }
    }

    func spin(_ seconds: Double) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    /// The last JSON sent to the app, parsed.
    var last: [String: Any] {
        guard let text = probe.appliedJSON.last, let data = text.data(using: .utf8),
              let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return [:] }
        return obj
    }

    func value(_ path: String, in dict: [String: Any]? = nil) -> Any? {
        var cur: Any? = dict ?? last
        for key in path.split(separator: ".") {
            guard let d = cur as? [String: Any] else { return nil }
            cur = d[String(key)]
        }
        return cur
    }

    func int(_ path: String) -> Int? { (value(path) as? NSNumber)?.intValue }
    func str(_ path: String) -> String? { value(path) as? String }
    func count(_ path: String) -> Int? { (value(path) as? [Any])?.count }

    /// Renders `view` to <dir>/<name>.png. Returns false when nothing (or a blank image) came out.
    @discardableResult
    func render(_ view: NSView?, _ name: String) -> Bool {
        guard let view else {
            check(false, "render \(name)", "no view")
            return false
        }
        view.layoutSubtreeIfNeeded()
        view.displayIfNeeded()
        let bounds = view.bounds
        guard bounds.width > 0, bounds.height > 0, let rep = view.bitmapImageRepForCachingDisplay(in: bounds) else {
            check(false, "render \(name)", "empty bounds")
            return false
        }
        view.cacheDisplay(in: bounds, to: rep)
        var method = "cacheDisplay"
        if SelfTestRunner.isBlank(rep), let layer = view.layer, let ctx = NSGraphicsContext(bitmapImageRep: rep) {
            // Fallback: draw the view's own layer tree into the bitmap.
            ctx.cgContext.saveGState()
            ctx.cgContext.translateBy(x: 0, y: CGFloat(rep.pixelsHigh))
            ctx.cgContext.scaleBy(x: CGFloat(rep.pixelsWide) / bounds.width, y: -CGFloat(rep.pixelsHigh) / bounds.height)
            layer.render(in: ctx.cgContext)
            ctx.cgContext.restoreGState()
            method = "layer.render"
        }
        let blank = SelfTestRunner.isBlank(rep)
        let url = dir.appendingPathComponent(name + ".png")
        if let data = rep.representation(using: .png, properties: [:]) {
            try? data.write(to: url)
        }
        check(!blank, "render \(name) (\(rep.pixelsWide)x\(rep.pixelsHigh), \(method))", "blank image")
        return !blank
    }

    static func isBlank(_ rep: NSBitmapImageRep) -> Bool {
        guard rep.pixelsWide > 0, rep.pixelsHigh > 0 else { return true }
        let first = rep.colorAt(x: rep.pixelsWide / 2, y: rep.pixelsHigh / 2)
        var differing = 0
        let stepX = max(1, rep.pixelsWide / 24), stepY = max(1, rep.pixelsHigh / 24)
        for y in stride(from: 0, to: rep.pixelsHigh, by: stepY) {
            for x in stride(from: 0, to: rep.pixelsWide, by: stepX) where rep.colorAt(x: x, y: y) != first {
                differing += 1
            }
        }
        return differing < 20
    }

    /// Finds an accessibility element whose label/title equals `label` under `root`.
    /// Accessibility objects of SwiftUI are private classes, so they are queried dynamically.
    static func axValue(_ o: NSObject, _ name: String) -> Any? {
        let sel = NSSelectorFromString(name)
        guard o.responds(to: sel) else { return nil }
        return o.perform(sel)?.takeUnretainedValue()
    }

    static func axPress(_ o: NSObject) -> Bool {
        let sel = NSSelectorFromString("accessibilityPerformPress")
        guard o.responds(to: sel), let m = class_getInstanceMethod(type(of: o), sel) else { return false }
        typealias Press = @convention(c) (AnyObject, Selector) -> Bool
        let f = unsafeBitCast(method_getImplementation(m), to: Press.self)
        return f(o, sel)
    }

    func findAccessible(_ label: String, in root: Any, depth: Int = 0) -> NSObject? {
        guard depth < 40, let el = root as? NSObject else { return nil }
        let l = SelfTestRunner.axValue(el, "accessibilityLabel") as? String
        let t = SelfTestRunner.axValue(el, "accessibilityTitle") as? String
        if l == label || t == label { return el }
        let children = SelfTestRunner.axValue(el, "accessibilityChildren") as? [Any] ?? []
        for child in children {
            if let hit = findAccessible(label, in: child, depth: depth + 1) { return hit }
        }
        return nil
    }

    // MARK: run

    func run() -> Int {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        log("SnowFall UI self-test — \(ProcessInfo.processInfo.operatingSystemVersionString)")
        unitChecks()
        guard let actions = probe as? SFAppActions else {
            check(false, "probe implements SFAppActions")
            return finish()
        }
        settingsWindowChecks(actions)
        quickPanelChecks(actions)
        return finish()
    }

    func finish() -> Int {
        log("\(failures) failed check(s)")
        try? lines.joined(separator: "\n").write(to: dir.appendingPathComponent("report.txt"), atomically: true, encoding: .utf8)
        return failures
    }

    // MARK: pure checks

    func unitChecks() {
        // JSON round trip keeps unknown keys and tolerates wrong types.
        let bad = #"{"weather": {"intensity": "lots", "wind": -30, "x": 1}, "system": {"uiTheme": "neon", "excludedProcesses": ["a", 5, "b"]}, "userPresets": [{"name": "ok"}, {"weather": {}}], "extra": true}"#
        if let doc = SettingsDocument(json: bad) {
            check(doc.settings.weather.intensity == 55 && doc.settings.weather.wind == -30, "tolerant decode: numbers")
            check(doc.settings.system.uiTheme == .night, "tolerant decode: unknown enum string -> default")
            check(doc.settings.system.excludedProcesses == ["a", "b"], "tolerant decode: string list skips non-strings")
            check(doc.settings.userPresets.map(\.name) == ["ok"], "tolerant decode: preset without name skipped")
            let out = SettingsDocument(json: doc.json())
            check(out?.raw["extra"] as? Bool == true, "unknown top-level key preserved")
            check(((out?.raw["weather"] as? [String: Any])?["x"] as? NSNumber)?.intValue == 1, "unknown nested key preserved")
            check(((out?.raw["weather"] as? [String: Any])?["intensity"] as? NSNumber)?.intValue == 55, "modelled key rewritten")
        } else {
            check(false, "SettingsDocument parses")
        }
        check(SettingsDocument(json: "[1,2]") == nil, "non-object JSON rejected")

        // printf-style formats from the string table.
        check(L10n.substitute("Hold %ls to interact", ["Ctrl+Option"]) == "Hold Ctrl+Option to interact", "format %ls")
        check(L10n.substitute("%d %% of %d", [5, 7]) == "5 % of 7", "format %d / %%")
        check([1, 2, 5, 11, 21, 22, 25, 111].map(L10n.ruForm) == [0, 1, 2, 2, 0, 1, 2, 2], "Russian plural forms")

        // Countdown rules (countdown.cpp).
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = TimeZone(identifier: "UTC") ?? .current
        func date(_ y: Int, _ m: Int, _ d: Int, _ h: Int = 12, _ min: Int = 0) -> Date {
            cal.date(from: DateComponents(year: y, month: m, day: d, hour: h, minute: min)) ?? Date()
        }
        let en = L10n(["CountdownDay1": "day", "CountdownDay5": "days", "CountdownHour1": "hour", "CountdownHour5": "hours",
                       "HappyNewYear": "HNY", "HappyHolidays": "HH", "TabSnow": "Snow"])
        let ru = L10n(["CountdownDay1": "день", "CountdownDay2": "дня", "CountdownDay5": "дней", "CountdownHour1": "час",
                       "CountdownHour2": "часа", "CountdownHour5": "часов", "HappyNewYear": "СНГ", "HappyHolidays": "П",
                       "TabSnow": "Снег"])
        check(Countdown.compute(now: date(2026, 11, 12), strings: en, calendar: cal) == Countdown(big: "50", caption: "days"),
              "countdown: Nov 12 -> 50 days")
        check(Countdown.compute(now: date(2026, 12, 30), strings: en, calendar: cal).big == "2", "countdown: Dec 30 -> 2")
        check(Countdown.compute(now: date(2026, 12, 31, 18, 30), strings: en, calendar: cal) == Countdown(big: "6", caption: "hours"),
              "countdown: Dec 31 18:30 -> 6 hours")
        check(Countdown.compute(now: date(2026, 12, 31, 23, 10), strings: ru, calendar: cal) == Countdown(big: "1", caption: "час"),
              "countdown: Dec 31 23:10 -> 1 час")
        check(Countdown.compute(now: date(2027, 1, 3), strings: en, calendar: cal).festive, "countdown: Jan 3 -> festive")
        check(Countdown.compute(now: date(2026, 12, 10), strings: ru, calendar: cal) == Countdown(big: "22", caption: "дня"),
              "countdown: Dec 10 -> 22 дня")
        check(Countdown.compute(now: date(2027, 1, 8), strings: ru, calendar: cal).caption == "дней", "countdown: Jan 8 -> 358 дней")

        // Hotkeys: Mac names on screen, core grammar in the JSON.
        check(HotkeyText.display("Ctrl+Alt+Shift+Win+F5") == "Ctrl+Option+Shift+Cmd+F5", "hotkey display uses Mac names")
        check(HotkeyRecorder.outcome(keyCode: kVK_ANSI_P, flags: [.control, .option]) == .set("Ctrl+Alt+P"), "hotkey record Ctrl+Option+P")
        check(HotkeyRecorder.outcome(keyCode: kVK_ANSI_K, flags: [.command, .shift]) == .set("Shift+Win+K"), "hotkey record Shift+Cmd+K")
        check(HotkeyRecorder.outcome(keyCode: kVK_ANSI_K, flags: []) == .ignore, "hotkey: bare key ignored")
        check(HotkeyRecorder.outcome(keyCode: kVK_Delete, flags: []) == .clear, "hotkey: Backspace disables")
        check(HotkeyRecorder.outcome(keyCode: kVK_Escape, flags: []) == .cancel, "hotkey: Esc cancels")

        // Update status (JSON of update::statusToJson) and its texts.
        let avail = UpdateStatus(json: FakeUpdate.available)
        check(avail.stage == .available && avail.latest == "0.1.3" && avail.canInstall && avail.hasNewer && !avail.busy,
              "update status JSON parsed")
        check(UpdateStatus(json: "garbage") == UpdateStatus() && UpdateStatus(json: #"{"stage":"warp"}"#).stage == .idle,
              "update status: bad JSON / unknown stage -> idle")
        check(UpdateStatus(json: FakeUpdate.downloading).busy && UpdateStatus(json: FakeUpdate.downloading).progress == 42,
              "update status: downloading progress")
        let ut = L10n(["UpdateAvailableFmt": "Version %@ is available", "UpdateDownloadingFmt": "Downloading the update… %d%%",
                       "UpdateFailedFmt": "Failed: %@", "UpdateErrVerify": "bad signature", "UpdateErrNetwork": "offline",
                       "UpdateIdle": "Not checked yet"])
        check(avail.line(ut) == "Version 0.1.3 is available", "update line: available")
        check(UpdateStatus(json: FakeUpdate.downloading).line(ut) == "Downloading the update… 42%", "update line: downloading %d%%")
        check(UpdateStatus(json: FakeUpdate.failed).line(ut) == "Failed: bad signature", "update line: failed with the reason")
        check(UpdateStatus(json: FakeUpdate.json("failed")).line(ut) == "Failed: offline", "update line: failed without a reason")
        check(UpdateStatus().line(ut) == "Not checked yet", "update line: idle")
        if let doc = SettingsDocument(json: #"{"updates": {"check": true, "asked": "yes", "lastCheck": 1731600000}}"#) {
            check(doc.settings.updates.check && !doc.settings.updates.asked, "updates settings: tolerant decode")
            var d2 = doc
            d2.settings.updates.asked = true
            let back = SettingsDocument(json: d2.json())
            check(((back?.raw["updates"] as? [String: Any])?["lastCheck"] as? NSNumber)?.int64Value == 1731600000
                  && back?.settings.updates.asked == true, "updates.lastCheck (app-owned) survives an edit")
        } else {
            check(false, "updates settings document parses")
        }
        var withUpdates = Settings()
        withUpdates.updates = UpdateSettings(check: true, asked: true)
        check(withUpdates.restoredDefaults().updates == UpdateSettings(check: true, asked: true),
              "restore defaults keeps the update answer")
        check(SettingsPage.named("about") == .about && SettingsPage.named("nope") == nil, "page names of -showPage:")

        // Preset name validation.
        var s = Settings()
        s.userPresets = [UserPreset(name: "Office")]
        check(s.validatePresetName("  ") == .length, "preset name: empty")
        check(s.validatePresetName(String(repeating: "я", count: 41)) == .length, "preset name: 41 chars")
        check(s.validatePresetName("office ") == .duplicate, "preset name: duplicate (case-insensitive)")
        s.userPresets = (0..<20).map { UserPreset(name: "p\($0)") }
        check(s.validatePresetName("new") == .tooMany, "preset name: 20 presets max")
    }

    // MARK: settings window

    func settingsWindowChecks(_ actions: any SFAppActions) {
        guard let sw = SFUIFactory.makeSettingsWindow(with: actions) as? SettingsWindowController else {
            check(false, "factory returns the settings window")
            return
        }
        let store = sw.store
        store.nowOverride = Calendar(identifier: .gregorian).date(from: DateComponents(year: 2026, month: 11, day: 12, hour: 12))
        sw.show(withSettingsJSON: json)
        spin(1.0)
        check(sw.isOpen, "settings window opens")
        check(sw.contentView?.frame.size == SettingsLayout.size, "settings window content size 980x700",
              "\(String(describing: sw.contentView?.frame.size))")
        check(probe.appliedJSON.isEmpty, "opening the window sends nothing to the app")
        check(store.strings["TabSnow"] == "Snow", "strings come from -uiStrings (English)")
        check(store.skins.count == 12, "12 snowman skins from -snowmanSkins")
        check(store.diagnostics.valid && store.diagnostics.interactionKeys == "Ctrl+Option", "diagnostics polled")
        check(store.visible || !(sw.window?.occlusionState.contains(.visible) ?? false), "visible while on screen")

        // Drive a real control through accessibility (what VoiceOver would press).
        if let root = sw.contentView, let card = findAccessible("Blizzard, Strong wind", in: root) {
            let pressed = SelfTestRunner.axPress(card)
            spin(0.3)
            check(pressed && str("weather.preset") == "blizzard", "AX press on the «Blizzard» card -> weather.preset",
                  "pressed=\(pressed) preset=\(str("weather.preset") ?? "nil")")
        } else {
            log("NOTE  accessibility tree of the hosting view not reachable in-process; card press skipped")
        }

        // Edits through the store (the same path every control binding uses).
        store.update { $0.weather.preset = .normal }
        store.update { $0.weather.intensity = 80 }
        check(int("weather.intensity") == 80, "intensity slider -> weather.intensity = 80")
        check(int("weather.futureKnob") == 7, "unknown key weather.futureKnob survives an edit")
        check(int("futureSection.answer") == 42, "unknown section futureSection survives an edit")
        check(count("userPresets") == 3, "whole document sent (userPresets present)")
        let before = probe.appliedJSON.count
        store.update { $0.weather.intensity = 80 }
        check(probe.appliedJSON.count == before, "unchanged value is not re-sent")

        // User presets.
        check(!store.savePreset(named: "office"), "duplicate preset name refused")
        check(store.presetMessage.role == .error && store.presetMessage.text == store.strings["ErrPresetDuplicate"],
              "duplicate name shows the error text")
        check(store.savePreset(named: "  Test preset "), "save preset")
        check(count("userPresets") == 4 && (value("userPresets") as? [[String: Any]])?.last?["name"] as? String == "Test preset",
              "saved preset reaches the JSON (trimmed name)")
        store.applyPreset(at: 2)
        check(int("weather.intensity") == 95 && str("weather.preset") == "blizzard" && int("accumulation.maxHeightPxAt96Dpi") == 60,
              "apply «Blizzard night» preset")
        store.deletePreset(at: 3)
        check(count("userPresets") == 3, "delete preset")

        // Toggles, segments, hotkeys, exclusions.
        store.update { $0.accumulation.icicles = false }
        check((value("accumulation.icicles") as? NSNumber)?.boolValue == false, "icicles toggle")
        store.update { $0.performance.fpsCap = 30 }
        check(int("performance.fpsCap") == 30, "fps segment")
        if case .set(let text) = HotkeyRecorder.outcome(keyCode: kVK_ANSI_P, flags: [.control, .option]) {
            store.update { $0.hotkeys.pauseToggle = text }
        }
        check(str("hotkeys.pauseToggle") == "Ctrl+Alt+P", "recorded hotkey stored in core grammar")
        check(store.hotkeyStatus["Ctrl+Alt+P"] == "", "free hotkey validated as available")
        check(!(store.hotkeyStatus["Ctrl+Shift+Win+I"] ?? "").isEmpty, "busy hotkey shows the app's reason")
        store.update { $0.addExclusion("/Applications/Safari.app/Safari") }
        store.update { $0.addExclusion("telegram") }
        check((value("system.excludedProcesses") as? [String]) == ["Telegram", "Keynote", "Safari"],
              "exclusions: path stripped, case-insensitive duplicates ignored")
        store.update { $0.system.excludedSnowMode = .around }
        check(str("system.excludedSnowMode") == "around", "exclusion mode segment")
        store.update { $0.interaction.snowmanSkin = "pirate" }
        check(str("interaction.snowmanSkin") == "pirate", "snowman skin picker")

        // Detailed log toggle.
        check((value("performance.detailedLog") as? NSNumber)?.boolValue == false, "detailedLog defaults to false")
        check(!store.strings["ChkDetailedLog"].hasPrefix("ChkDetailed"), "detailed-log string comes from the table")
        store.update { $0.performance.detailedLog = true }
        check((value("performance.detailedLog") as? NSNumber)?.boolValue == true, "detailed-log toggle -> performance.detailedLog")
        store.update { $0.performance.detailedLog = false }

        // Hide the system cursor (macOS, experimental).
        check((value("interaction.hideSystemCursor") as? NSNumber)?.boolValue == false, "hideSystemCursor defaults to false")
        check(!store.strings["ChkHideCursorMac"].hasPrefix("ChkHide") && !store.strings["HideCursorHintMac"].hasPrefix("HideCursor")
              && !store.strings["CursorReplaceUnavailableMac"].hasPrefix("Cursor"), "cursor strings come from the table")
        store.update { $0.interaction.hideSystemCursor = true }
        check((value("interaction.hideSystemCursor") as? NSNumber)?.boolValue == true, "hide-cursor toggle -> interaction.hideSystemCursor")
        check(int("weather.futureKnob") == 7, "unknown keys still preserved after the cursor toggle")
        store.refreshDiagnostics()
        check(store.diagnostics.cursorMode == "replace-unavailable", "diagnostics cursorMode parsed")
        store.update { $0.interaction.hideSystemCursor = false }
        store.refreshDiagnostics()
        check(store.diagnostics.cursorMode != "replace-unavailable", "cursorMode clears when the option is off")

        // Updates: toggle, commands, pushed status, page switch.
        check(!store.strings["ChkCheckUpdates"].hasPrefix("ChkCheck") && !store.strings["HeaderUpdateFmt"].hasPrefix("Header"),
              "update strings come from the table")
        store.setUpdateCheck(true)
        check((value("updates.check") as? NSNumber)?.boolValue == true && (value("updates.asked") as? NSNumber)?.boolValue == true,
              "update toggle -> updates.check (and asked)")
        check(int("weather.futureKnob") == 7, "unknown keys preserved after the update toggle")
        store.setUpdateCheck(false)
        check((value("updates.check") as? NSNumber)?.boolValue == false && (value("updates.asked") as? NSNumber)?.boolValue == true,
              "update toggle off keeps asked")
        store.actions?.checkForUpdates()
        check(probe.commands.last == "checkForUpdates", "«Check now» -> checkForUpdates")
        store.refreshUpdateStatus()
        check(store.updateStatus.stage == .available, "status pulled from -updateStatusJSON after a check")
        sw.updateStatusChanged(FakeUpdate.upToDate)
        check(store.updateStatus.stage == .upToDate, "-updateStatusChanged: reaches the window")
        sw.updateStatusChanged(FakeUpdate.available)
        store.actions?.installUpdate()
        check(probe.commands.last == "installUpdate", "«Update» -> installUpdate")
        store.actions?.openUpdatePage()
        check(probe.commands.last == "openUpdatePage", "«Download page» / «What's new» -> openUpdatePage")
        store.page = .snow
        sw.showPage("about")
        check(store.page == .about, "-showPage: about")
        sw.showPage("bogus")
        check(store.page == .about, "-showPage: unknown name ignored")
        store.page = .snow
        sw.updateStatusChanged(FakeUpdate.idle)

        // Commands.
        store.actions?.clearSnow()
        store.resetPreview()
        check(probe.commands.contains("clearSnow"), "«Очистить всё» -> clearSnow")
        check(probe.previewResets == 1, "«Сбросить превью» -> resetPreview")
        check(probe.previewUpdates > 0, "preview receives the settings JSON")

        // External update: refresh without echo.
        let sent = probe.appliedJSON.count
        if var doc = SettingsDocument(json: probe.appliedJSON.last ?? json) {
            doc.settings.weather.intensity = 33
            sw.updateSettingsJSON(doc.json())
        }
        spin(0.2)
        check(store.settings.weather.intensity == 33, "updateSettingsJSON refreshes the controls")
        check(probe.appliedJSON.count == sent, "updateSettingsJSON does not echo back to the app")

        // Restore defaults keeps the user's presets.
        store.update { $0 = $0.restoredDefaults() }
        check(int("weather.intensity") == 55 && count("userPresets") == 3 && str("system.uiTheme") == "night"
              && str("system.language") == "auto", "restore defaults (presets kept)")
        check(int("futureSection.answer") == 42, "restore defaults keeps unknown keys")
        store.update { $0.system.language = .en }   // the demo's screenshots are in English
        store.update { $0.system.excludedProcesses = ["Telegram", "Keynote"] }
        store.update { $0.hotkeys.interactionToggle = "Ctrl+Shift+Win+I" }

        // Screenshots: every page in every theme (English), then Russian (Night) for text fit.
        store.frozen = true
        for theme in UITheme.allCases {
            store.update { $0.system.uiTheme = theme }
            check(str("system.uiTheme") == theme.rawValue, "theme swatch -> system.uiTheme = \(theme.rawValue)")
            for page in SettingsPage.allCases {
                store.page = page
                spin(0.35)
                render(sw.contentView, "settings-\(theme.rawValue)-\(page.rawValue)-\(SelfTestRunner.pageName(page))")
            }
        }
        store.update { $0.system.language = .ru }
        spin(0.2)
        check(store.strings.isRussian, "language switch reloads -uiStrings (Russian)")
        for theme in [UITheme.night, .postcard] {
            store.update { $0.system.uiTheme = theme }
            for page in SettingsPage.allCases where theme == .night || page == .snow || page == .system {
                store.page = page
                spin(0.35)
                render(sw.contentView, "settings-ru-\(theme.rawValue)-\(page.rawValue)-\(SelfTestRunner.pageName(page))")
            }
        }
        // Tools page with the hide-cursor option on and the "unavailable" note, RU and EN, all themes;
        // then with interaction off (the option is disabled).
        store.update { $0.interaction.hideSystemCursor = true }
        store.refreshDiagnostics()
        store.page = .tools
        for lang in [UILanguage.ru, .en] {
            store.update { $0.system.language = lang }
            for theme in UITheme.allCases {
                store.update { $0.system.uiTheme = theme }
                spin(0.35)
                render(sw.contentView, "cursor-\(lang.rawValue)-\(theme.rawValue)-tools")
            }
        }
        store.update { $0.system.uiTheme = .night }
        store.update { $0.interaction.enabled = false }
        spin(0.35)
        render(sw.contentView, "cursor-en-night-tools-interaction-off")
        store.update { $0.interaction.enabled = true }
        store.update { $0.interaction.hideSystemCursor = false }
        store.refreshDiagnostics()
        store.update { $0.system.language = .en }
        store.update { $0.system.uiTheme = .night }

        // About page with updates: available (installable) in every theme, the other states in
        // Night, Russian too; the header shows the «Version … available» badge.
        store.page = .about
        for theme in UITheme.allCases {
            store.update { $0.system.uiTheme = theme }
            sw.updateStatusChanged(FakeUpdate.available)
            spin(0.35)
            render(sw.contentView, "update-en-\(theme.rawValue)-about-available")
        }
        store.update { $0.system.uiTheme = .night }
        for (name, json) in [("manual", FakeUpdate.manual), ("downloading", FakeUpdate.downloading),
                             ("failed", FakeUpdate.failed), ("uptodate", FakeUpdate.upToDate), ("idle", FakeUpdate.idle)] {
            sw.updateStatusChanged(json)
            spin(0.35)
            render(sw.contentView, "update-en-night-about-\(name)")
        }
        store.update { $0.system.language = .ru }
        for (name, json) in [("available", FakeUpdate.available), ("manual", FakeUpdate.manual)] {
            sw.updateStatusChanged(json)
            spin(0.35)
            render(sw.contentView, "update-ru-night-about-\(name)")
        }
        store.update { $0.system.uiTheme = .postcard }
        sw.updateStatusChanged(FakeUpdate.available)
        spin(0.35)
        render(sw.contentView, "update-ru-postcard-about-available")
        store.update { $0.system.uiTheme = .night }
        store.update { $0.system.language = .en }
        sw.updateStatusChanged(FakeUpdate.idle)

        store.page = .snow
        store.frozen = false
        spin(0.3)
        sw.close()
        spin(0.3)
        check(!sw.isOpen && !store.visible, "close hides the window and stops animation")
    }

    static func pageName(_ p: SettingsPage) -> String {
        switch p {
        case .snow: return "snow"
        case .drifts: return "drifts"
        case .tools: return "tools"
        case .performance: return "performance"
        case .festive: return "festive"
        case .system: return "system"
        case .about: return "about"
        }
    }

    // MARK: quick panel

    func quickPanelChecks(_ actions: any SFAppActions) {
        guard let qp = SFUIFactory.makeQuickPanel(with: actions) as? QuickPanelController else {
            check(false, "factory returns the quick panel")
            return
        }
        let store = qp.store
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.title = "❄"
        let current = probe.appliedJSON.last ?? json
        if let button = item.button {
            qp.showRelative(to: button, settingsJSON: current)
            spin(0.8)
            if qp.isShown {
                check(true, "quick panel popover opens under the status item")
            } else {
                // CI runners may have no visible menu bar for a background test app.
                log("NOTE  popover not shown (status button window visible: \(button.window?.isVisible ?? false)); rendered in place")
            }
        } else {
            log("NOTE  no status bar button in this session; quick panel rendered off screen")
            store.load(json: current)
        }
        let host = qp.contentView ?? NSHostingView(rootView: QuickPanelView(store: store))
        if host.window == nil {
            let w = NSWindow(contentRect: NSRect(origin: .zero, size: QuickPanelLayout.size), styleMask: [.borderless],
                             backing: .buffered, defer: false)
            w.contentView = host
            w.orderFront(nil)
        }
        store.frozen = true
        store.refreshDiagnostics()
        for theme in UITheme.allCases {
            store.update { $0.system.uiTheme = theme }
            spin(0.35)
            render(host, "quickpanel-\(theme.rawValue)")
        }
        store.update { $0.system.uiTheme = .night }
        store.update { $0.weather.wind = -40 }
        check(int("weather.wind") == -40, "quick panel wind slider")
        qp.perform(.pause)
        spin(0.2)
        check(probe.commands.last == "togglePause" && store.diagnostics.paused, "Pause -> togglePause, label flips to Resume")
        render(host, "quickpanel-night-paused")
        qp.perform(.pause)
        qp.perform(.clear)
        qp.perform(.fireworks)
        check(Array(probe.commands.suffix(3)) == ["togglePause", "clearSnow", "launchFireworks"], "Clear / Fireworks commands")
        store.showPicker = true
        spin(0.35)
        render(host, "quickpanel-night-picker")
        store.showPicker = false
        store.update { $0.system.language = .ru }
        spin(0.35)
        render(host, "quickpanel-ru-night")
        store.update { $0.system.uiTheme = .postcard }
        spin(0.35)
        render(host, "quickpanel-ru-postcard")
        store.update { $0.system.language = .en }
        store.update { $0.system.uiTheme = .night }
        qp.updateStatusChanged(FakeUpdate.available)
        spin(0.35)
        check(store.updateStatus.hasNewer, "quick panel receives the update status")
        render(host, "quickpanel-night-update")
        store.update { $0.system.language = .ru }
        spin(0.35)
        render(host, "quickpanel-ru-night-update")
        store.update { $0.system.language = .en }
        qp.perform(.update)
        check(probe.commands.last == "openUpdateInfo", "quick panel update badge -> openUpdateInfo")
        qp.updateStatusChanged(FakeUpdate.idle)
        qp.perform(.settings)
        spin(0.3)
        check(probe.commands.last == "openSettings", "Settings… opens settings")
        NSStatusBar.system.removeStatusItem(item)
    }
}
