// Update state from -[SFAppActions updateStatusJSON] (core/update/update.h statusToJson) and the
// texts the UI shows for it. The app does the networking; the UI only displays and asks.
import Foundation

enum UpdateStage: String, Equatable {
    case idle, checking, upToDate, available, downloading, installing, failed
}

struct UpdateStatus: Equatable {
    var stage: UpdateStage = .idle
    var current = ""
    var latest = ""
    var pageUrl = ""
    var notes = ""
    var canInstall = false
    var progress = 0
    /// StrId name of the reason when failed ("UpdateErrNetwork", …).
    var error = ""

    init() {}

    init(json: String) {
        guard let data = json.data(using: .utf8),
              let d = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return }
        stage = UpdateStage(rawValue: d["stage"] as? String ?? "") ?? .idle
        current = d["current"] as? String ?? ""
        latest = d["latest"] as? String ?? ""
        pageUrl = d["pageUrl"] as? String ?? ""
        notes = d["notes"] as? String ?? ""
        canInstall = (d["canInstall"] as? NSNumber)?.boolValue ?? false
        progress = max(0, min(100, (d["progress"] as? NSNumber)?.intValue ?? 0))
        error = d["error"] as? String ?? ""
    }

    /// A newer version is known (also while it downloads / installs).
    var hasNewer: Bool { !latest.isEmpty && [.available, .downloading, .installing].contains(stage) }
    var busy: Bool { [.checking, .downloading, .installing].contains(stage) }

    /// The status line of the «Обновления» group.
    func line(_ t: L10n) -> String {
        switch stage {
        case .idle: return t["UpdateIdle"]
        case .checking: return t["UpdateChecking"]
        case .upToDate: return t["UpdateUpToDate"]
        case .available: return t.format("UpdateAvailableFmt", latest)
        case .downloading: return t.format("UpdateDownloadingFmt", progress)
        case .installing: return t["UpdateInstalling"]
        case .failed: return t.format("UpdateFailedFmt", t[error.isEmpty ? "UpdateErrNetwork" : error])
        }
    }

    var lineRole: MessageRole {
        switch stage {
        case .failed: return .error
        case .available, .upToDate: return .ok
        default: return .secondary
        }
    }
}
