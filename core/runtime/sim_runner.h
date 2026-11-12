// SimRunner: owns the simulation thread. Fixed 1/60 s physics step with bounded catch-up,
// latest-value mailboxes for layout/settings (so bursts of window events coalesce) and a
// small queue for pointer input. Publishes immutable FrameSnapshots to SnapshotExchange.
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/input.h"
#include "core/layout.h"
#include "core/settings.h"
#include "core/snapshot.h"

namespace snow {

enum class SimMode {
    Running,   // normal
    Paused,    // user pause: emission stops, flakes in the air settle, drifts/snowmen stay
    Frozen,    // nothing visible (overlays hidden): no stepping at all
};

// How the simulation thread sleeps between steps and is woken by commands. The default
// (std::condition_variable) is fine on macOS/Linux; Windows supplies a high-resolution
// waitable-timer version because MSVC's condition_variable timeouts have ~15 ms granularity.
class SimWaiter {
public:
    virtual ~SimWaiter() = default;
    virtual void wake() = 0;                     // thread-safe, callable from any thread
    virtual void wait(double timeoutSeconds) = 0; // < 0: until woken
};
std::unique_ptr<SimWaiter> makeDefaultSimWaiter();

// OS services the simulation thread needs. Every member is optional.
struct SimPlatformHooks {
    std::function<void()> onThreadStart;                 // e.g. name the thread
    std::function<bool(float& x, float& y)> cursor;      // mouse position, physical px (umbrella)
    // Local calendar time: year, month 1..12, day 1..31, seconds of day. Default: <ctime>.
    std::function<void(int& y, int& m, int& d, int& secondsOfDay)> localTime;
    std::function<std::unique_ptr<SimWaiter>()> makeWaiter;   // default: makeDefaultSimWaiter
};

class SimRunner {
public:
    explicit SimRunner(SnapshotExchange& exchange, SimPlatformHooks hooks = {});
    ~SimRunner();

    void start(const Settings& settings, const RuntimeTuning& tuning);
    void stop();

    // All setters are thread-safe and cheap; they wake the simulation thread.
    void setSettings(const Settings& settings, const RuntimeTuning& tuning);
    void setLayout(std::shared_ptr<const DesktopLayout> layout);
    void setMode(SimMode mode);
    void setInteractionMode(bool active);
    void setPointer(float x, float y);          // coalesced pointer position
    void postInput(const InputEvent& ev);       // PointerDown / PointerUp
    void clearSnow();
    void triggerSnowplow();
    void triggerFireworks();

private:
    void threadMain();

    SnapshotExchange& exchange_;
    std::thread thread_;
    SimPlatformHooks hooks_;
    std::unique_ptr<SimWaiter> waiter_;
    std::atomic<bool> quit_{false};

    std::mutex m_;
    // --- guarded by m_ ---
    bool settingsDirty_ = false;
    Settings settings_;
    RuntimeTuning tuning_;
    std::shared_ptr<const DesktopLayout> layout_;   // non-null = pending
    std::vector<InputEvent> inputs_;
    bool pointerDirty_ = false;
    float pointerX_ = 0, pointerY_ = 0;
    bool clear_ = false;
    bool plow_ = false;
    bool fireworks_ = false;
    SimMode mode_ = SimMode::Running;
    bool interaction_ = false;
};

}  // namespace snow
