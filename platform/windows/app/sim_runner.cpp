#include "app/sim_runner.h"

#include <chrono>

#include "diagnostics/log.h"
#include "simulation/world.h"

namespace snow {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kStep = 1.0 / 60.0;
constexpr int kMaxCatchUpSteps = 3;          // never simulate more than 3 steps per wake-up
constexpr double kSettleSeconds = 14.0;      // after a user pause, let airborne flakes land

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
}  // namespace

SimRunner::SimRunner(SnapshotExchange& exchange) : exchange_(exchange) {}

SimRunner::~SimRunner() { stop(); }

void SimRunner::start(const Settings& settings, const RuntimeTuning& tuning) {
    if (thread_.joinable()) return;
    settings_ = settings;
    tuning_ = tuning;
    settingsDirty_ = true;
    quit_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    thread_ = std::thread([this] { threadMain(); });
}

void SimRunner::stop() {
    if (!thread_.joinable()) return;
    quit_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    CloseHandle(timer_);
    wake_ = timer_ = nullptr;
}

void SimRunner::setSettings(const Settings& settings, const RuntimeTuning& tuning) {
    {
        std::lock_guard<std::mutex> lk(m_);
        settings_ = settings;
        tuning_ = tuning;
        settingsDirty_ = true;
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::setLayout(std::shared_ptr<const DesktopLayout> layout) {
    {
        std::lock_guard<std::mutex> lk(m_);
        layout_ = std::move(layout);
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::setMode(SimMode mode) {
    {
        std::lock_guard<std::mutex> lk(m_);
        mode_ = mode;
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::setInteractionMode(bool active) {
    {
        std::lock_guard<std::mutex> lk(m_);
        interaction_ = active;
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::setPointer(float x, float y) {
    std::lock_guard<std::mutex> lk(m_);
    pointerX_ = x;
    pointerY_ = y;
    pointerDirty_ = true;   // picked up on the next step; no wake needed
}

void SimRunner::postInput(const InputEvent& ev) {
    {
        std::lock_guard<std::mutex> lk(m_);
        inputs_.push_back(ev);
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::clearSnow() {
    {
        std::lock_guard<std::mutex> lk(m_);
        clear_ = true;
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::triggerSnowplow() {
    {
        std::lock_guard<std::mutex> lk(m_);
        plow_ = true;
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::triggerFireworks() {
    {
        std::lock_guard<std::mutex> lk(m_);
        fireworks_ = true;
    }
    if (wake_) SetEvent(wake_);
}

void SimRunner::threadMain() {
    SetThreadDescription(GetCurrentThread(), L"SnowSimulation");
    World world;

    // Small pool of snapshot buffers: a buffer is reused once nobody else holds it, so the
    // steady state allocates nothing.
    std::vector<std::shared_ptr<FrameSnapshot>> pool;
    auto acquire = [&pool]() {
        for (auto& p : pool)
            if (p.use_count() == 1) return p;
        pool.push_back(std::make_shared<FrameSnapshot>());
        return pool.back();
    };

    std::uint64_t frame = 0;
    double stepMsAvg = 0;
    SimMode mode = SimMode::Running;
    bool interaction = false;
    bool emission = true;
    Clock::time_point pausedAt{};
    Clock::time_point lastClock{};
    Clock::time_point last = Clock::now();
    double accumulator = 0;
    std::vector<InputEvent> inputs;

    auto publish = [&](bool moving) {
        auto snap = acquire();
        world.buildSnapshot(*snap);
        snap->frame = ++frame;
        // A default time point tells the renderer not to extrapolate (state is at rest).
        snap->taken = moving ? Clock::now() : Clock::time_point{};
        snap->stats.stepMs = static_cast<float>(stepMsAvg);
        exchange_.publish(snap);
    };

    while (!quit_) {
        // ---- drain mailboxes --------------------------------------------------
        bool changed = false;
        {
            std::shared_ptr<const DesktopLayout> layout;
            bool haveSettings = false, clear = false, pointer = false, plow = false, fireworks = false;
            Settings settings;
            RuntimeTuning tuning;
            float px = 0, py = 0;
            SimMode newMode;
            bool newInteraction;
            {
                std::lock_guard<std::mutex> lk(m_);
                layout.swap(layout_);
                if (settingsDirty_) {
                    settings = settings_;
                    tuning = tuning_;
                    haveSettings = true;
                    settingsDirty_ = false;
                }
                clear = clear_;
                clear_ = false;
                plow = plow_;
                plow_ = false;
                fireworks = fireworks_;
                fireworks_ = false;
                pointer = pointerDirty_;
                pointerDirty_ = false;
                px = pointerX_;
                py = pointerY_;
                inputs.clear();
                inputs.swap(inputs_);
                newMode = mode_;
                newInteraction = interaction_;
            }
            if (haveSettings) {
                world.applySettings(settings, tuning);
                changed = true;
            }
            if (layout) {
                world.setLayout(*layout);
                changed = true;
            }
            if (clear) {
                world.clearAllSnow();
                changed = true;
            }
            if (plow) {
                world.triggerSnowplow();
                changed = true;
            }
            if (fireworks) {
                world.triggerFireworks();
                changed = true;
            }
            if (newInteraction != interaction) {
                interaction = newInteraction;
                world.setInteractionMode(interaction);
                changed = true;
            }
            if (pointer && interaction) {
                InputEvent ev;
                ev.type = InputEvent::Type::PointerMove;
                ev.x = px;
                ev.y = py;
                world.handleInput(ev);
                changed = true;
            }
            for (const auto& ev : inputs) {
                world.handleInput(ev);
                changed = true;
            }
            if (newMode != mode) {
                if (mode == SimMode::Frozen || newMode == SimMode::Running) {
                    // Coming back: never simulate the time we were away.
                    last = Clock::now();
                    accumulator = 0;
                }
                if (newMode == SimMode::Paused) pausedAt = Clock::now();
                mode = newMode;
                changed = true;
            }
        }

        const bool wantEmission = (mode == SimMode::Running);
        if (wantEmission != emission) {
            emission = wantEmission;
            world.setEmissionEnabled(emission);
        }

        // Does physics need to advance right now?
        bool active = false;
        if (mode == SimMode::Running) {
            active = true;
        } else if (mode == SimMode::Paused) {
            const double since = std::chrono::duration<double>(Clock::now() - pausedAt).count();
            active = interaction || (since < kSettleSeconds && world.particleCount() > 0);
        }

        if (!active) {
            if (changed && mode != SimMode::Frozen) {
                world.step(0.0f);   // let layout/clear/settings changes take effect
                publish(false);
            } else if (changed) {
                publish(false);
            }
            WaitForSingleObject(wake_, INFINITE);
            last = Clock::now();
            accumulator = 0;
            continue;
        }

        // ---- ambient inputs: pointer ("umbrella") and local calendar time -----
        {
            POINT pt;
            const bool ok = GetCursorPos(&pt) != FALSE;   // fails on the secure desktop
            world.setAmbientPointer(static_cast<float>(pt.x), static_cast<float>(pt.y), ok);
            const auto tnow = Clock::now();
            if (tnow - lastClock >= std::chrono::seconds(1)) {
                lastClock = tnow;
                SYSTEMTIME st;
                GetLocalTime(&st);
                world.setLocalTime(st.wYear, st.wMonth, st.wDay, st.wHour * 3600 + st.wMinute * 60 + st.wSecond);
            }
        }

        // ---- fixed-step advance ----------------------------------------------
        const auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - last).count();
        last = now;
        if (elapsed > 0.25) elapsed = kStep;   // sleep/resume, debugger, long stall: do not catch up
        accumulator += elapsed;

        int steps = 0;
        while (accumulator >= kStep && steps < kMaxCatchUpSteps) {
            const auto t0 = Clock::now();
            world.step(static_cast<float>(kStep));
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            stepMsAvg += (ms - stepMsAvg) * 0.05;
            accumulator -= kStep;
            ++steps;
        }
        if (accumulator >= kStep) accumulator = 0;   // drop the backlog instead of spiralling

        if (steps > 0 || changed) publish(true);

        // ---- wait for the next tick or a command ------------------------------
        const double remaining = kStep - accumulator;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(remaining * 1e7);
        if (due.QuadPart > -1000) due.QuadPart = -1000;
        SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
        HANDLE handles[2] = {timer_, wake_};
        WaitForMultipleObjects(2, handles, FALSE, 100);
    }
    SNOW_LOGI("simulation thread stopped after %llu frames", static_cast<unsigned long long>(frame));
}

}  // namespace snow
