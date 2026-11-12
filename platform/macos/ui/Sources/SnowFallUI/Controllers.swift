// AppKit side of the bridge: the settings NSWindow and the status-bar popover hosting the
// SwiftUI views, plus the @objc(SFUIFactory) entry point the app looks up by name.
import AppKit
import Combine
import SFBridge
import SwiftUI

// MARK: - factory

/// Entry point of the UI module. The app shell finds it with NSClassFromString(@"SFUIFactory").
@objc(SFUIFactory)
public final class SFUIFactory: NSObject, SFUIFactoryProtocol {
    public static func makeSettingsWindow(with actions: any SFAppActions) -> any SFSettingsWindow {
        SettingsWindowController(actions: actions)
    }

    public static func makeQuickPanel(with actions: any SFAppActions) -> any SFQuickPanel {
        QuickPanelController(actions: actions)
    }
}

/// Polls -diagnosticsJSON every 0.5 s while its owner is visible.
final class DiagnosticsPoller: NSObject {
    private var timer: Timer?
    private weak var store: UIStore?

    init(store: UIStore) {
        self.store = store
    }

    var running: Bool { timer != nil }

    func start() {
        guard timer == nil else { return }
        store?.refreshDiagnostics()
        let t = Timer(timeInterval: 0.5, target: self, selector: #selector(tick), userInfo: nil, repeats: true)
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    @objc private func tick() {
        store?.refreshDiagnostics()
    }
}

// MARK: - settings window

final class SettingsWindowController: NSObject, SFSettingsWindow, NSWindowDelegate {
    let store: UIStore
    private(set) var window: NSWindow?
    private var hosting: NSHostingView<SettingsRootView>?
    private lazy var poller = DiagnosticsPoller(store: store)
    private var subscriptions: Set<AnyCancellable> = []

    init(actions: any SFAppActions) {
        store = UIStore(actions: actions)
        super.init()
        store.themePublisher
            .sink { [weak self] theme in self?.applyTheme(theme) }
            .store(in: &subscriptions)
        store.stringsPublisher
            .sink { [weak self] strings in self?.window?.title = strings["SettingsTitle"] }
            .store(in: &subscriptions)
    }

    var isOpen: Bool { window?.isVisible ?? false }

    func show(withSettingsJSON json: String) {
        store.load(json: json)
        let w = window ?? makeWindow()
        applyTheme(store.settings.system.uiTheme)
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        updateVisibility()
    }

    func updateSettingsJSON(_ json: String) {
        store.load(json: json)
    }

    func close() {
        window?.close()
    }

    /// The SwiftUI root, for the self-test renderer.
    var contentView: NSView? { hosting }

    private func makeWindow() -> NSWindow {
        if store.preview == nil, let actions = store.actions {
            store.preview = actions.makePreviewView()
        }
        let w = NSWindow(contentRect: NSRect(origin: .zero, size: SettingsLayout.size),
                         styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView],
                         backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.title = store.strings["SettingsTitle"]
        w.isReleasedWhenClosed = false
        w.isMovableByWindowBackground = false
        w.tabbingMode = .disallowed
        w.delegate = self
        let root = SettingsRootView(store: store, onClose: { [weak self] in self?.close() })
        let host = NSHostingView(rootView: root)
        host.frame = NSRect(origin: .zero, size: SettingsLayout.size)
        w.contentView = host
        // With a full-size content view the frame is what we want the content to be.
        w.setFrame(NSRect(origin: w.frame.origin, size: SettingsLayout.size), display: false)
        store.titlebarHeight = max(0, w.frame.height - w.contentLayoutRect.height)
        w.center()
        w.setFrameAutosaveName("SnowFallSettings")
        hosting = host
        window = w
        return w
    }

    private func applyTheme(_ theme: UITheme) {
        guard let w = window else { return }
        let p = Palette.of(theme)
        w.appearance = p.appearance
        w.backgroundColor = p.headerTop.ns
    }

    private func updateVisibility() {
        guard let w = window else { return }
        let visible = w.isVisible && !w.isMiniaturized && w.occlusionState.contains(.visible)
        if store.visible != visible { store.visible = visible }
        if visible || w.isVisible { poller.start() } else { poller.stop() }
    }

    // MARK: NSWindowDelegate

    func windowDidChangeOcclusionState(_ notification: Notification) { updateVisibility() }
    func windowDidMiniaturize(_ notification: Notification) { updateVisibility() }
    func windowDidDeminiaturize(_ notification: Notification) { updateVisibility() }
    func windowDidBecomeKey(_ notification: Notification) { updateVisibility() }

    func windowWillClose(_ notification: Notification) {
        store.visible = false
        poller.stop()
    }
}

// MARK: - quick panel

final class QuickPanelController: NSObject, SFQuickPanel, NSPopoverDelegate {
    let store: UIStore
    private var popover: NSPopover?
    private var hosting: NSHostingView<QuickPanelView>?
    private lazy var poller = DiagnosticsPoller(store: store)
    private var subscriptions: Set<AnyCancellable> = []
    private var backdrop: PopoverBackdrop?

    init(actions: any SFAppActions) {
        store = UIStore(actions: actions)
        super.init()
        store.themePublisher
            .sink { [weak self] theme in self?.applyTheme(theme) }
            .store(in: &subscriptions)
    }

    var isShown: Bool { popover?.isShown ?? false }

    func showRelative(to button: NSStatusBarButton, settingsJSON json: String) {
        store.load(json: json)
        store.showPicker = false
        let pop = popover ?? makePopover()
        applyTheme(store.settings.system.uiTheme)
        pop.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        pop.contentViewController?.view.window?.makeKey()
        installBackdrop()
        store.visible = true
        poller.start()
    }

    func updateSettingsJSON(_ json: String) {
        store.load(json: json)
    }

    func close() {
        popover?.performClose(nil)
    }

    /// The SwiftUI root, for the self-test renderer.
    var contentView: NSView? { hosting }

    func perform(_ command: QuickPanelView.QuickCommand) {
        guard let actions = store.actions else { return }
        switch command {
        case .pause:
            actions.togglePause()
            store.refreshDiagnostics()
        case .clear:
            actions.clearSnow()
        case .fireworks:
            actions.launchFireworks()
        case .settings:
            close()
            actions.openSettings()
        }
    }

    private func makePopover() -> NSPopover {
        let root = QuickPanelView(store: store, onCommand: { [weak self] in self?.perform($0) })
        let host = NSHostingView(rootView: root)
        host.frame = NSRect(origin: .zero, size: QuickPanelLayout.size)
        let vc = NSViewController()
        vc.view = host
        let pop = NSPopover()
        pop.contentViewController = vc
        pop.contentSize = QuickPanelLayout.size
        pop.behavior = .transient
        pop.animates = true
        pop.delegate = self
        hosting = host
        popover = pop
        return pop
    }

    private func applyTheme(_ theme: UITheme) {
        let p = Palette.of(theme)
        popover?.appearance = p.appearance
        backdrop?.color = p.bg.ns
    }

    /// Paints the popover frame (arrow included) in the theme's window colour instead of the
    /// system material, so the panel looks the same as on Windows.
    private func installBackdrop() {
        guard let frameView = popover?.contentViewController?.view.window?.contentView?.superview else { return }
        if backdrop?.superview !== frameView {
            backdrop?.removeFromSuperview()
            let b = PopoverBackdrop(frame: frameView.bounds)
            b.autoresizingMask = [.width, .height]
            frameView.addSubview(b, positioned: .below, relativeTo: frameView.subviews.first)
            backdrop = b
        }
        backdrop?.color = store.palette.bg.ns
    }

    // MARK: NSPopoverDelegate

    func popoverDidClose(_ notification: Notification) {
        store.visible = false
        store.showPicker = false
        poller.stop()
    }
}

final class PopoverBackdrop: NSView {
    var color: NSColor = .windowBackgroundColor {
        didSet { needsDisplay = true }
    }

    override func draw(_ dirtyRect: NSRect) {
        color.setFill()
        dirtyRect.fill()
    }
}
