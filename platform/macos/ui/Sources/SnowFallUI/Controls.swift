// Themed building blocks: switch, slider, segmented control, buttons, chips, headings, fields.
// Each control keeps native accessibility (VoiceOver sees a real switch / slider / button) and
// keyboard operation (Tab focus with Full Keyboard Access, Space to press, arrows on sliders).
import AppKit
import SwiftUI

enum UIMetrics {
    static let font: CGFloat = 13
    static let small: CGFloat = 11.5
    static let corner: CGFloat = 8
}

extension View {
    /// Dims and blocks a control group like a disabled Win32 control.
    func enabled(_ on: Bool) -> some View {
        disabled(!on).opacity(on ? 1 : 0.45)
    }
}

// MARK: - switch

struct SnowToggle: View {
    @Environment(\.palette) private var p
    let title: String
    @Binding var isOn: Bool
    var hint: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Toggle(isOn: $isOn) {
                Text(title).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .toggleStyle(SnowSwitchStyle())
            if let hint, !hint.isEmpty {
                Text(hint).font(.system(size: UIMetrics.small)).foregroundColor(p.textSecondary.c)
                    .padding(.leading, 46)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

struct SnowSwitchStyle: ToggleStyle {
    func makeBody(configuration: Configuration) -> some View {
        SnowSwitchBody(configuration: configuration)
    }
}

private struct SnowSwitchBody: View {
    @Environment(\.palette) private var p
    let configuration: ToggleStyleConfiguration

    var body: some View {
        let on = configuration.isOn
        Button {
            configuration.isOn.toggle()
        } label: {
            HStack(alignment: .center, spacing: 10) {
                ZStack(alignment: on ? .trailing : .leading) {
                    Capsule().fill(on ? p.toggleOn.c : p.toggleOff.c)
                    Capsule().strokeBorder(on ? Color.clear : p.controlBorder.c, lineWidth: 1)
                    Circle().fill(on ? p.knobOn.c : p.knobOff.c)
                        .frame(width: 14, height: 14)
                        .padding(.horizontal, 3)
                }
                .frame(width: 36, height: 20)
                .animation(.easeOut(duration: 0.12), value: on)
                configuration.label
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityRepresentation {
            Toggle(isOn: configuration.$isOn) { configuration.label }
        }
    }
}

// MARK: - slider

struct SnowSlider: View {
    @Environment(\.palette) private var p
    @Environment(\.isEnabled) private var isEnabled
    @Binding var value: Int
    let range: ClosedRange<Int>
    var step = 1
    let label: String
    var valueText = ""
    @FocusState private var focused: Bool

    private var fraction: CGFloat {
        CGFloat(value - range.lowerBound) / CGFloat(max(1, range.upperBound - range.lowerBound))
    }

    var body: some View {
        GeometryReader { geo in
            let knob: CGFloat = 16
            let usable = max(1, geo.size.width - knob)
            let x = knob / 2 + usable * fraction
            ZStack(alignment: .leading) {
                Capsule().fill(p.toggleOff.c).frame(height: 4)
                    .padding(.horizontal, knob / 2)
                Capsule().fill(p.accent.c).frame(width: max(0, x - knob / 2), height: 4)
                    .offset(x: knob / 2)
                Circle().fill(p.accent.c).frame(width: knob, height: knob)
                    .overlay(Circle().fill(p.pageFill.c).frame(width: 7, height: 7))
                    .overlay(Circle().stroke(p.focus.c.opacity(focused ? 0.9 : 0), lineWidth: 1.5).frame(width: knob + 5, height: knob + 5))
                    .offset(x: x - knob / 2)
            }
            .frame(height: geo.size.height)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                let f = min(1, max(0, (g.location.x - knob / 2) / usable))
                set(range.lowerBound + Int((CGFloat(range.upperBound - range.lowerBound) * f).rounded()))
            })
        }
        .frame(height: 22)
        .focusable(isEnabled)
        .focused($focused)
        .onMoveCommand { dir in
            switch dir {
            case .left, .down: set(value - step)
            case .right, .up: set(value + step)
            default: break
            }
        }
        .accessibilityRepresentation {
            Slider(value: Binding(get: { Double(value) }, set: { set(Int($0.rounded())) }),
                   in: Double(range.lowerBound)...Double(range.upperBound), step: Double(step)) {
                Text(label)
            }
            .accessibilityValue(valueText)
        }
    }

    private func set(_ v: Int) {
        let snapped = range.lowerBound + ((v - range.lowerBound) / step) * step
        let clamped = min(range.upperBound, max(range.lowerBound, snapped))
        if clamped != value { value = clamped }
    }
}

/// «Интенсивность:  ———●———  55 %»
struct SliderRow: View {
    @Environment(\.palette) private var p
    let label: String
    @Binding var value: Int
    let range: ClosedRange<Int>
    let valueText: String
    var labelWidth: CGFloat = 170
    var valueWidth: CGFloat = 92

    var body: some View {
        HStack(spacing: 12) {
            Text(label).font(.system(size: UIMetrics.font)).foregroundColor(p.text.c)
                .frame(width: labelWidth, alignment: .leading)
                .lineLimit(1).minimumScaleFactor(0.8)
            SnowSlider(value: $value, range: range, label: label, valueText: valueText)
            Text(valueText).font(.system(size: UIMetrics.font).monospacedDigit()).foregroundColor(p.textSecondary.c)
                .frame(width: valueWidth, alignment: .leading)
                .lineLimit(1).minimumScaleFactor(0.8)
                .accessibilityHidden(true)
        }
        .frame(minHeight: 26)
    }
}

// MARK: - segmented control

struct SnowSegmented<T: Hashable>: View {
    @Environment(\.palette) private var p
    let options: [(value: T, title: String)]
    @Binding var selection: T
    var height: CGFloat = 28
    var label = ""

    var body: some View {
        HStack(spacing: 2) {
            ForEach(options.indices, id: \.self) { i in
                let selected = options[i].value == selection
                Button {
                    selection = options[i].value
                } label: {
                    Text(options[i].title)
                        .font(.system(size: UIMetrics.font, weight: selected ? .semibold : .regular))
                        .lineLimit(1).minimumScaleFactor(0.75)
                        .foregroundColor(selected ? p.accentText.c : p.controlText.c)
                        .padding(.horizontal, 10)
                        .frame(maxWidth: .infinity, minHeight: height - 4)
                        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? p.accent.c : Color.clear))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? [.isSelected] : [])
            }
        }
        .padding(2)
        .background(RoundedRectangle(cornerRadius: UIMetrics.corner).fill(p.control.c))
        .overlay(RoundedRectangle(cornerRadius: UIMetrics.corner).strokeBorder(p.controlBorder.c, lineWidth: 1))
        .accessibilityElement(children: .contain)
        .accessibilityLabel(label)
    }
}

// MARK: - buttons

struct SnowButtonStyle: ButtonStyle {
    var primary = false
    var height: CGFloat = 30

    func makeBody(configuration: Configuration) -> some View {
        SnowButtonBody(configuration: configuration, primary: primary, height: height)
    }
}

private struct SnowButtonBody: View {
    @Environment(\.palette) private var p
    @Environment(\.isEnabled) private var isEnabled
    let configuration: ButtonStyleConfiguration
    let primary: Bool
    let height: CGFloat

    var body: some View {
        configuration.label
            .font(.system(size: UIMetrics.font, weight: primary ? .semibold : .regular))
            .lineLimit(1).minimumScaleFactor(0.75)
            .foregroundColor(primary ? p.accentText.c : p.controlText.c)
            .padding(.horizontal, 14)
            .frame(minHeight: height)
            .frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: UIMetrics.corner).fill(primary ? p.accent.c : p.control.c))
            .overlay(RoundedRectangle(cornerRadius: UIMetrics.corner)
                .strokeBorder(primary ? Color.clear : p.controlBorder.c, lineWidth: 1))
            .opacity(configuration.isPressed ? 0.75 : (isEnabled ? 1 : 0.45))
            .contentShape(Rectangle())
    }
}

// MARK: - texts

struct SectionHeading: View {
    @Environment(\.palette) private var p
    let text: String
    var rule = true

    var body: some View {
        HStack(spacing: 10) {
            Text(text).font(.system(size: UIMetrics.font, weight: .semibold)).foregroundColor(p.heading.c)
                .lineLimit(1).fixedSize()
                .accessibilityAddTraits(.isHeader)
            if rule {
                Rectangle().fill(p.cardBorder.c).frame(height: 1)
            }
        }
        .frame(minHeight: 22)
    }
}

struct HintText: View {
    @Environment(\.palette) private var p
    let text: String
    var role: MessageRole = .secondary

    var body: some View {
        Text(text)
            .font(.system(size: UIMetrics.small))
            .foregroundColor(role == .error ? p.error.c : role == .ok ? p.ok.c : p.textSecondary.c)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct BodyText: View {
    @Environment(\.palette) private var p
    let text: String
    var secondary = false

    var body: some View {
        Text(text)
            .font(.system(size: UIMetrics.font))
            .foregroundColor(secondary ? p.textSecondary.c : p.text.c)
            .lineSpacing(2)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Single-line text field in the theme's colours.
struct SnowField: View {
    @Environment(\.palette) private var p
    let placeholder: String
    @Binding var text: String
    var onSubmit: () -> Void = {}

    var body: some View {
        ZStack(alignment: .leading) {
            if text.isEmpty {
                Text(placeholder).font(.system(size: UIMetrics.font)).foregroundColor(p.textSecondary.c.opacity(0.8))
                    .padding(.horizontal, 9)
                    .accessibilityHidden(true)
            }
            TextField("", text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: UIMetrics.font))
                .foregroundColor(p.fieldText.c)
                .padding(.horizontal, 9)
                .onSubmit(onSubmit)
                .accessibilityLabel(placeholder)
        }
        .frame(height: 30)
        .background(RoundedRectangle(cornerRadius: 6).fill(p.field.c))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(p.fieldBorder.c, lineWidth: 1))
    }
}

/// A rounded card (preview, countdown, scene cards...).
struct CardBackground: View {
    @Environment(\.palette) private var p
    var radius: CGFloat = 12

    var body: some View {
        RoundedRectangle(cornerRadius: radius).fill(p.card.c)
            .overlay(RoundedRectangle(cornerRadius: radius).strokeBorder(p.cardBorder.c, lineWidth: 1))
    }
}

// MARK: - user preset chips

struct PresetChip: View {
    @Environment(\.palette) private var p
    let name: String
    var deletable = true
    let apply: () -> Void
    var delete: () -> Void = {}

    var body: some View {
        HStack(spacing: 6) {
            Button(action: apply) {
                HStack(spacing: 7) {
                    Circle().fill(p.accent.c).frame(width: 6, height: 6)
                    Text(name).font(.system(size: UIMetrics.font)).foregroundColor(p.controlText.c).lineLimit(1)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(name)
            if deletable {
                Button(action: delete) {
                    Text("×").font(.system(size: 15)).foregroundColor(p.textSecondary.c)
                        .frame(width: 16, height: 16)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(name + " ×")
            }
        }
        .padding(.leading, 11)
        .padding(.trailing, deletable ? 7 : 12)
        .frame(height: 28)
        .background(Capsule().fill(p.control.c))
        .overlay(Capsule().strokeBorder(p.controlBorder.c, lineWidth: 1))
    }
}

/// Wrapping row layout for chips.
struct FlowLayout: Layout {
    var spacing: CGFloat = 8
    var lineSpacing: CGFloat = 8

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? 400
        var x: CGFloat = 0, y: CGFloat = 0, lineH: CGFloat = 0, maxX: CGFloat = 0
        for s in subviews {
            let size = s.sizeThatFits(.unspecified)
            if x > 0 && x + size.width > width {
                x = 0
                y += lineH + lineSpacing
                lineH = 0
            }
            x += size.width + spacing
            maxX = max(maxX, x - spacing)
            lineH = max(lineH, size.height)
        }
        return CGSize(width: min(width, maxX), height: y + lineH)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, lineH: CGFloat = 0
        for s in subviews {
            let size = s.sizeThatFits(.unspecified)
            if x > bounds.minX && x + size.width > bounds.maxX {
                x = bounds.minX
                y += lineH + lineSpacing
                lineH = 0
            }
            s.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
            x += size.width + spacing
            lineH = max(lineH, size.height)
        }
    }
}
