// UI texts. Every text comes from -[SFAppActions uiStrings] (keyed by the StrId names of
// core/i18n/strings_table.inc); nothing user-visible is hard-coded here.
import Foundation

struct L10n: Equatable {
    let table: [String: String]

    init(_ table: [String: String] = [:]) { self.table = table }

    /// Text for a StrId name. A missing id shows up as the id itself (visible in screenshots).
    subscript(_ id: String) -> String { table[id] ?? id }

    /// The table is Russian when its words are Cyrillic (the app picks the language).
    var isRussian: Bool {
        guard let sample = table["TabSnow"] ?? table["AppName"] else { return false }
        return sample.unicodeScalars.contains { (0x0400...0x04FF).contains($0.value) }
    }

    /// printf-style formatting of the table's format strings: %ls / %s / %@ take the next
    /// string argument, %d the next integer, %% is a percent sign.
    func format(_ id: String, _ args: CustomStringConvertible...) -> String {
        L10n.substitute(self[id], args)
    }

    static func substitute(_ pattern: String, _ args: [CustomStringConvertible]) -> String {
        var out = ""
        var next = 0
        var it = pattern.makeIterator()
        while let ch = it.next() {
            guard ch == "%" else {
                out.append(ch)
                continue
            }
            guard let spec = it.next() else {
                out.append("%")
                break
            }
            switch spec {
            case "%":
                out.append("%")
            case "l":
                _ = it.next()   // "%ls"
                fallthrough
            case "s", "@", "d", "i", "u":
                if next < args.count { out += args[next].description }
                next += 1
            default:
                out.append("%")
                out.append(spec)
            }
        }
        return out
    }

    // MARK: Russian plural forms (1 день / 2 дня / 5 дней), same rule as the Windows UI.

    /// 0: one, 1: few, 2: many.
    static func ruForm(_ n: Int) -> Int {
        let a = abs(n) % 100, b = abs(n) % 10
        if (11...14).contains(a) { return 2 }
        if b == 1 { return 0 }
        if (2...4).contains(b) { return 1 }
        return 2
    }

    /// Picks one of three ids by the Russian plural rule (English tables: "one" for 1, else "many").
    func plural(_ n: Int, _ one: String, _ few: String, _ many: String) -> String {
        if isRussian {
            return [one, few, many][L10n.ruForm(n)]
        }
        return n == 1 ? one : many
    }
}

/// Snowman skin from -snowmanSkins.
struct SnowmanSkinInfo: Equatable, Identifiable {
    let id: String
    let name: String
}
