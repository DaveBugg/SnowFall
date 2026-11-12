// InteractionController: the ONLY place where the app touches user input.
//
// Normal mode: nothing is hooked; the overlay is fully click-through.
// Interaction mode (explicitly entered by holding the modifier chord, a hotkey or the tray):
// a low-level mouse hook is installed. It swallows a left-button press ONLY when the press
// lands on visible snow / a snowball (hit regions published by the simulation); every other
// click, drag and wheel event passes to the applications untouched, so normal window
// dragging keeps working. Exception (setting blockClicksWhileHeld): while the user is
// physically holding the mode chord, every left/right press goes to the tool, so pressing
// next to the snow never starts a desktop rubber-band selection. Leaving the mode removes the hook. Keyboard is never hooked:
// the hold chord is polled with GetAsyncKeyState. Focus is never taken.
//
// UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <string>

#include "core/snapshot.h"

namespace snow {

class SimRunner;

// Modifier chord that temporarily enables interaction mode while held.
enum class HoldChord { AltShift, CtrlShift, CtrlAlt };

class InteractionController {
public:
    InteractionController(SimRunner& runner, const SnapshotExchange& snapshots);
    ~InteractionController();

    // `host` receives WM_TIMER (timer id kTimerId) -> call onTimer(); and gets
    // PostMessage(host, cycleToolMsg, 0, 0) on a right click over snow.
    void initialize(HWND host, UINT cycleToolMsg);
    void shutdown();

    void setFeatureEnabled(bool enabled);      // settings.interaction.enabled
    void setHoldKeyEnabled(bool enabled);      // settings.interaction.holdKeyEnabled
    void setBlockClicksWhileHeld(bool block) { blockWhileHeld_ = block; }
    void setSuspended(bool suspended);         // overlays hidden / app frozen: force off
    void setToggled(bool on);                  // hotkey / tray toggle
    bool toggled() const { return toggled_; }
    bool active() const { return active_; }

    void onTimer();                            // polls the hold chord

    HoldChord holdChord() const { return chord_; }
    static std::wstring chordName(HoldChord c);

    std::function<void(bool active)> onActiveChanged;

    static constexpr UINT_PTR kTimerId = 0x534E;

private:
    void evaluate();
    void installHook();
    void removeHook();
    bool hit(POINT pt) const;
    bool captureAll() const;
    bool chordDown() const;
    LRESULT onMouse(WPARAM msg, const MSLLHOOKSTRUCT& info, bool& swallow);
    static LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp);
    static HoldChord pickChord();

    SimRunner& runner_;
    const SnapshotExchange& snapshots_;
    HWND host_ = nullptr;
    UINT cycleMsg_ = 0;
    HHOOK hook_ = nullptr;
    HoldChord chord_ = HoldChord::AltShift;

    bool feature_ = true;
    bool holdEnabled_ = true;
    bool blockWhileHeld_ = true;
    bool suspended_ = false;
    bool toggled_ = false;
    bool held_ = false;
    ULONGLONG chordSince_ = 0;
    bool active_ = false;
    bool capturing_ = false;       // a left press was swallowed; swallow its release too
    bool swallowRightUp_ = false;
};

}  // namespace snow
