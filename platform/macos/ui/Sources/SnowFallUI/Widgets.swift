// Hotkey recorder fields and the «Выбрать из запущенных…» picker.
import AppKit
import SwiftUI

// MARK: - hotkey recorder

enum HotkeyRecorder {
    enum Outcome: Equatable {
        case ignore              // modifier-less key or a key the core grammar cannot express
        case cancel              // Esc
        case clear               // Backspace / Delete: hotkey disabled
        case set(String)         // core text, e.g. "Ctrl+Alt+S"
    }

    /// What a key press means while a field is recording.
    static func outcome(keyCode: Int, flags: NSEvent.ModifierFlags) -> Outcome {
        let mods = HotkeyText.modifiers(flags)
        if mods.isEmpty {
            switch keyCode {
            case 53: return .cancel                 // kVK_Escape
            case 51, 117: return .clear             // kVK_Delete (Backspace), kVK_ForwardDelete
            default: return .ignore                 // bare keys are never registered globally
            }
        }
        guard let key = HotkeyText.keyName(keyCode: keyCode) else { return .ignore }
        return .set(HotkeyText.coreText(mods, key: key))
    }
}

struct HotkeyRow: View {
    @ObservedObject var store: UIStore
    let label: String
    let keyPath: WritableKeyPath<Settings, String>
    @State private var recording = false
    @State private var liveMods: HotkeyText.Modifiers = []
    @State private var monitor: Any?

    var body: some View {
        let p = store.palette, t = store.strings
        let text = store.settings[keyPath: keyPath]
        HStack(spacing: 10) {
            Text(label).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c)
                .lineLimit(1).minimumScaleFactor(0.8)
                .frame(width: 180, alignment: .leading)
                .accessibilityHidden(true)
            Button {
                recording ? stop() : start()
            } label: {
                Text(fieldText(text, t))
                    .font(.system(size: UIMetrics.font, weight: text.isEmpty || recording ? .regular : .medium))
                    .foregroundColor(text.isEmpty && !recording ? p.textSecondary.c : p.fieldText.c)
                    .lineLimit(1).minimumScaleFactor(0.8)
                    .padding(.horizontal, 10)
                    .frame(width: 190, height: 30, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 6).fill(p.field.c))
                    .overlay(RoundedRectangle(cornerRadius: 6)
                        .strokeBorder(recording ? p.accent.c : p.fieldBorder.c, lineWidth: recording ? 2 : 1))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(label)
            .accessibilityValue(text.isEmpty ? t["HotkeyDisabled"] : HotkeyText.display(text))
            let st = status(text, t)
            Text(st.text).font(.system(size: 12.5))
                .foregroundColor(st.role == .error ? p.error.c : st.role == .ok ? p.ok.c : p.textSecondary.c)
                .lineLimit(2).minimumScaleFactor(0.85)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onDisappear { stop() }
    }

    private func fieldText(_ text: String, _ t: L10n) -> String {
        if recording {
            return liveMods.isEmpty ? "…" : HotkeyText.displayText(liveMods, key: nil) + "+…"
        }
        return text.isEmpty ? t["HotkeyDisabled"] : HotkeyText.display(text)
    }

    private func status(_ text: String, _ t: L10n) -> (text: String, role: MessageRole) {
        if text.isEmpty { return (t["HotkeyDisabled"], .secondary) }
        let reason = store.hotkeyStatus[text] ?? ""
        return reason.isEmpty ? (t["HotkeyOk"], .ok) : (reason, .error)
    }

    private func start() {
        stop()
        recording = true
        liveMods = []
        let path = keyPath
        let store = self.store
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged, .leftMouseDown]) { event in
            switch event.type {
            case .flagsChanged:
                liveMods = HotkeyText.modifiers(event.modifierFlags)
                return event
            case .leftMouseDown:
                stop()   // a click anywhere ends recording (a click on the field restarts it)
                return event
            default:
                switch HotkeyRecorder.outcome(keyCode: Int(event.keyCode), flags: event.modifierFlags) {
                case .ignore:
                    NSSound.beep()
                case .cancel:
                    stop()
                case .clear:
                    stop()
                    store.update { $0[keyPath: path] = "" }
                case .set(let text):
                    stop()
                    store.update { $0[keyPath: path] = text }
                }
                return nil   // swallowed: the key never reaches other controls
            }
        }
    }

    private func stop() {
        if let m = monitor { NSEvent.removeMonitor(m) }
        monitor = nil
        recording = false
        liveMods = []
    }
}

// MARK: - running apps picker

struct ProcessPicker: View {
    @ObservedObject var store: UIStore
    var compact = false
    let onDone: () -> Void
    @State private var names: [String] = []
    @State private var selected: Set<String> = []

    var body: some View {
        let p = store.palette, t = store.strings
        let excluded = Set(store.settings.system.excludedProcesses.map { $0.lowercased() })
        VStack(alignment: .leading, spacing: 10) {
            Text(t["PickerTitle"]).font(.system(size: 15, weight: .semibold)).foregroundColor(p.heading.c)
                .accessibilityAddTraits(.isHeader)
            HintText(text: t["PickerHint"])
            Group {
                if names.isEmpty {
                    HintText(text: t["PickerEmpty"])
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
                } else {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(names, id: \.self) { name in
                                let already = excluded.contains(name.lowercased())
                                let on = already || selected.contains(name)
                                Button {
                                    if selected.contains(name) { selected.remove(name) } else { selected.insert(name) }
                                } label: {
                                    HStack(spacing: 8) {
                                        RoundedRectangle(cornerRadius: 4)
                                            .fill(on ? p.accent.c : Color.clear)
                                            .overlay(RoundedRectangle(cornerRadius: 4)
                                                .strokeBorder(on ? Color.clear : p.controlBorder.c, lineWidth: 1.2))
                                            .overlay(Text(on ? "✓" : "").font(.system(size: 11, weight: .bold))
                                                .foregroundColor(p.accentText.c))
                                            .frame(width: 16, height: 16)
                                        Text(name).font(.system(size: UIMetrics.font)).foregroundColor(p.fieldText.c)
                                            .lineLimit(1)
                                        Spacer(minLength: 0)
                                    }
                                    .padding(.horizontal, 8)
                                    .frame(height: 26)
                                    .background(RoundedRectangle(cornerRadius: 5)
                                        .fill(selected.contains(name) ? p.control.c : Color.clear))
                                    .contentShape(Rectangle())
                                }
                                .buttonStyle(.plain)
                                .disabled(already)
                                .opacity(already ? 0.55 : 1)
                                .accessibilityLabel(name)
                                .accessibilityAddTraits(on ? [.isSelected] : [])
                            }
                        }
                        .padding(4)
                    }
                }
            }
            .frame(height: compact ? 210 : 240)
            .background(RoundedRectangle(cornerRadius: 6).fill(p.field.c))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(p.fieldBorder.c, lineWidth: 1))
            HStack(spacing: 10) {
                Spacer(minLength: 0)
                Button(t["BtnCancel"], action: onDone)
                    .buttonStyle(SnowButtonStyle())
                    .frame(width: compact ? 120 : 130)
                    .keyboardShortcut(.cancelAction)
                Button(t["BtnAdd"]) {
                    let chosen = names.filter { selected.contains($0) }
                    store.update { s in
                        for n in chosen { s.addExclusion(n) }
                    }
                    onDone()
                }
                .buttonStyle(SnowButtonStyle(primary: true))
                .frame(width: compact ? 120 : 130)
                .disabled(selected.isEmpty)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(compact ? 14 : 20)
        .frame(width: compact ? nil : 440)
        .background(p.bg.c)
        .onAppear {
            names = (store.actions?.runningAppNames() ?? []).sorted { $0.localizedCaseInsensitiveCompare($1) == .orderedAscending }
        }
    }
}
