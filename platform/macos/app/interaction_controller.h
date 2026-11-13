// InteractionController (macOS): the only place that touches user input.
//
// Normal mode: nothing is hooked, the overlays ignore the mouse.
// Interaction mode (hold Ctrl+Option, the hotkey or the menu): a CGEventTap
// (kCGEventTapOptionDefault, mouse events only) is installed. It swallows a left press only
// when it lands on visible snow / a grabbable object (snapshot hitRegions) — or, with
// blockClicksWhileHeld, every left/right press while the chord is physically held — and
// forwards pointer moves to the simulation. Leaving the mode removes the tap. The keyboard
// is never tapped: the chord is polled with +[NSEvent modifierFlags].
//
// The tap needs the Accessibility permission. Without it the mode is "view only" (the tool
// follows the pointer, clicks go to the apps) and a one-time explanation offers to open
// System Settings. The system cursor cannot be hidden from a background app, so the tool
// sprite is drawn around the arrow (CursorDraw::Hold) unless interaction.hideSystemCursor can
// hide it (CursorDraw::Replace, see system/cursor_hider.h and AppHost::updateCursorMode).
//
// Main thread only.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "core/snapshot.h"

namespace snow {
class SimRunner;
}

namespace snow::mac {

class InteractionController {
public:
    InteractionController(SimRunner& runner, const SnapshotExchange& snapshots);
    ~InteractionController();

    void start();      // begins polling the hold chord
    void shutdown();

    void setFeatureEnabled(bool enabled);
    void setHoldKeyEnabled(bool enabled);
    void setBlockClicksWhileHeld(bool block) { blockWhileHeld_ = block; }
    void setSuspended(bool suspended);
    void setToggled(bool on);
    bool toggled() const { return toggled_; }
    bool active() const { return active_; }
    bool tapInstalled() const;          // false while active = view-only (no permission)

    static std::string chordName();     // "Ctrl+Option"

    std::function<void(bool active)> onActiveChanged;
    std::function<void()> onCycleTool;                 // right click on snow
    // Interaction mode entered without the Accessibility permission (called at most once
    // per run, only when the user clearly wanted the mode: toggle or a long hold).
    std::function<void()> onNeedsAccessibility;

private:
    struct Impl;
    friend struct Impl;
    void evaluate();
    void poll();

    SimRunner& runner_;
    const SnapshotExchange& snapshots_;
    std::unique_ptr<Impl> impl_;

    bool feature_ = true;
    bool holdEnabled_ = true;
    bool blockWhileHeld_ = true;
    bool suspended_ = false;
    bool toggled_ = false;
    bool held_ = false;
    double chordSince_ = 0;
    bool active_ = false;
    bool capturing_ = false;
    bool swallowRightUp_ = false;
    bool askedForPermission_ = false;
};

}  // namespace snow::mac
