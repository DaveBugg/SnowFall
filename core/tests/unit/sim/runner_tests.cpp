// The portable simulation thread (core/runtime): starts, publishes snapshots at ~60 Hz,
// applies layouts/commands, honours modes and stops cleanly. Uses the default
// condition_variable waiter, i.e. exactly what macOS/Linux run.
#include <chrono>
#include <thread>

#include "runtime/performance_governor.h"
#include "runtime/sim_runner.h"
#include "sim_test_util.h"

using namespace simtest;
using namespace std::chrono_literals;

TEST(sim_runner_publishes_and_stops) {
    SnapshotExchange ex;
    int cursorCalls = 0;
    SimPlatformHooks hooks;
    hooks.cursor = [&cursorCalls](float& x, float& y) {
        ++cursorCalls;
        x = 500;
        y = 300;
        return true;
    };
    SimRunner runner(ex, hooks);
    Settings s;
    s.weather.preset = WeatherPreset::Blizzard;
    s.weather.intensity = 100;
    runner.start(s, RuntimeTuning{});
    runner.setLayout(std::make_shared<DesktopLayout>(basicLayout()));
    std::this_thread::sleep_for(600ms);
    auto a = ex.latest();
    CHECK(a != nullptr);
    std::this_thread::sleep_for(500ms);
    auto b = ex.latest();
    CHECK(b != nullptr);
    if (a && b) {
        const auto frames = b->frame - a->frame;
        printf("  frames in 0.5 s: %llu, particles %u\n", static_cast<unsigned long long>(frames), b->stats.particles);
        CHECK(frames >= 15 && frames <= 40);   // ~30 at 60 Hz, generous for loaded CI machines
        CHECK(b->stats.particles > 0);
    }
    CHECK(cursorCalls > 10);

    // Frozen: nothing is published any more.
    runner.setMode(SimMode::Frozen);
    std::this_thread::sleep_for(200ms);
    const auto f1 = ex.latest()->frame;
    std::this_thread::sleep_for(300ms);
    CHECK(ex.latest()->frame - f1 <= 1);

    runner.setMode(SimMode::Running);
    runner.clearSnow();
    std::this_thread::sleep_for(200ms);
    CHECK(ex.latest()->frame > f1);
    const auto t0 = std::chrono::steady_clock::now();
    runner.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
    runner.stop();   // idempotent
}

TEST(governor_pauses_for_fullscreen_and_lock) {
    Settings s;
    GovernorInputs in;
    CHECK(decide(s, in).simMode == SimMode::Running);
    in.fullscreenApp = true;
    CHECK(decide(s, in).simMode == SimMode::Frozen && !decide(s, in).overlaysVisible);
    in.fullscreenApp = false;
    in.sessionLocked = true;
    CHECK(decide(s, in).reason == PauseReason::Locked);
    in.sessionLocked = false;
    in.userPaused = true;   // AppHost derives it from settings.enabled
    CHECK(decide(s, in).simMode == SimMode::Paused && decide(s, in).overlaysVisible);
}
