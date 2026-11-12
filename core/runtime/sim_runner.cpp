#include "runtime/sim_runner.h"

#include <chrono>
#include <condition_variable>
#include <ctime>

#include "diagnostics/log.h"
#include "simulation/world.h"

namespace snow {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kStep = 1.0 / 60.0;
constexpr int kMaxCatchUpSteps = 3;          // never simulate more than 3 steps per wake-up
constexpr double kSettleSeconds = 14.0;      // after a user pause, let airborne flakes land

}  // namespace

namespace {
class CvWaiter final : public SimWaiter {
public:
    void wake() override {
        {
            std::lock_guard<std::mutex> lk(m_);
            signaled_ = true;
        }
        cv_.notify_one();
    }
    void wait(double timeoutSeconds) override {
        std::unique_lock<std::mutex> lk(m_);
        if (timeoutSeconds < 0) {
            cv_.wait(lk, [this] { return signaled_; });
        } else {
            cv_.wait_for(lk, std::chrono::duration<double>(timeoutSeconds), [this] { return signaled_; });
        }
        signaled_ = false;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool signaled_ = false;
};
}  // namespace

std::unique_ptr<SimWaiter> makeDefaultSimWaiter() { return std::make_unique<CvWaiter>(); }

SimRunner::SimRunner(SnapshotExchange& exchange, SimPlatformHooks hooks)
    : exchange_(exchange), hooks_(std::move(hooks)) {}

SimRunner::~SimRunner() { stop(); }

void SimRunner::start(const Settings& settings, const RuntimeTuning& tuning) {
    if (thread_.joinable()) return;
    settings_ = settings;
    tuning_ = tuning;
    settingsDirty_ = true;
    quit_ = false;
    waiter_ = hooks_.makeWaiter ? hooks_.makeWaiter() : makeDefaultSimWaiter();
    thread_ = std::thread([this] { threadMain(); });
}

void SimRunner::stop() {
    if (!thread_.joinable()) return;
    quit_ = true;
    waiter_->wake();
    thread_.join();
    waiter_.reset();
}

void SimRunner::setSettings(const Settings& settings, const RuntimeTuning& tuning) {
    {
        std::lock_guard<std::mutex> lk(m_);
        settings_ = settings;
        tuning_ = tuning;
        settingsDirty_ = true;
    }
    if (waiter_) waiter_->wake();
}

void SimRunner::setLayout(std::shared_ptr<const DesktopLayout> layout) {
    {
        std::lock_guard<std::mutex> lk(m_);
        layout_ = std::move(layout);
    }
    if (waiter_) waiter_->wake();
}

void SimRunner::setMode(SimMode mode) {
    {
        std::lock_guard<std::mutex> lk(m_);
        mode_ = mode;
    }
    if (waiter_) waiter_->wake();
}

void SimRunner::setInteractionMode(bool active) {
    {
        std::lock_guard<std::mutex> lk(m_);
        interaction_ = active;
    }
    if (waiter_) waiter_->wake();
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
    if (waiter_) waiter_->wake();
}

void SimRunner::clearSnow() {
    {
        std::lock_guard<std::mutex> lk(m_);
        clear_ = true;
    }
    if (waiter_) waiter_->wake();
}

void SimRunner::triggerSnowplow() {
    {
        std::lock_guard<std::mutex> lk(m_);
        plow_ = true;
    }
    if (waiter_) waiter_->wake();
}

void SimRunner::triggerFireworks() {
    {
        std::lock_guard<std::mutex> lk(m_);
        fireworks_ = true;
    }
    if (waiter_) waiter_->wake();
}

void SimRunner::threadMain() {
    if (hooks_.onThreadStart) hooks_.onThreadStart();
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
            waiter_->wait(-1.0);
            last = Clock::now();
            accumulator = 0;
            continue;
        }

        // ---- ambient inputs: pointer ("umbrella") and local calendar time -----
        {
            float px = 0, py = 0;
            const bool ok = hooks_.cursor && hooks_.cursor(px, py);
            world.setAmbientPointer(px, py, ok);
            const auto tnow = Clock::now();
            if (tnow - lastClock >= std::chrono::seconds(1)) {
                lastClock = tnow;
                int y = 0, mo = 0, d = 0, sec = 0;
                if (hooks_.localTime) {
                    hooks_.localTime(y, mo, d, sec);
                } else {
                    const std::time_t tt = std::time(nullptr);
                    std::tm lt{};
#ifdef _WIN32
                    localtime_s(&lt, &tt);
#else
                    localtime_r(&tt, &lt);
#endif
                    y = lt.tm_year + 1900;
                    mo = lt.tm_mon + 1;
                    d = lt.tm_mday;
                    sec = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
                }
                world.setLocalTime(y, mo, d, sec);
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
        waiter_->wait(remaining < 0.0001 ? 0.0001 : remaining);
    }
    SNOW_LOGI("simulation thread stopped after %llu frames", static_cast<unsigned long long>(frame));
}

}  // namespace snow
