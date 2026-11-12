// System state for the PerformanceGovernor: screen lock, fast user switching, sleep,
// display sleep, battery and Low Power Mode. Main thread; `onChange` is called on the main
// thread whenever any value changed. Objective-C++ header (no ObjC types exposed).
#pragma once
#include <functional>
#include <memory>

namespace snow::mac {

struct SystemState {
    bool locked = false;       // com.apple.screenIsLocked / session resigned (user switch)
    bool suspended = false;    // NSWorkspaceWillSleep .. DidWake, or all displays asleep
    bool onBattery = false;    // IOPowerSources: providing power source is the battery
    bool lowPower = false;     // NSProcessInfo.lowPowerModeEnabled
};

class SystemEvents {
public:
    SystemEvents();
    ~SystemEvents();
    void start(std::function<void()> onChange);
    void stop();
    SystemState state() const;
    std::function<void()> onWake;   // after sleep / display wake (tracker resync)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow::mac
