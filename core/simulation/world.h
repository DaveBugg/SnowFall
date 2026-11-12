// World: facade of the whole snow simulation (particles, weather, surfaces, accumulation,
// tools, snowballs, snowmen). Pure C++ (no Win32), deterministic for a given seed and
// sequence of calls. NOT thread-safe: owned and driven by the simulation thread only.
//
// PUBLIC API IS A FIXED CONTRACT used by the platform app layer. Add private state behind Impl.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/input.h"
#include "core/layout.h"
#include "core/settings.h"
#include "core/snapshot.h"

namespace snow {

class World {
public:
    explicit World(std::uint64_t seed = 0x5EED5EEDull);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Live-apply settings (weather, accumulation, tools, performance profile).
    void applySettings(const Settings& settings, const RuntimeTuning& tuning);

    // New desktop layout: updates monitors, surfaces, occlusion; windows that moved carry
    // their snow; closed windows drop it; hidden (minimized) windows keep it.
    void setLayout(const DesktopLayout& layout);

    // Interaction mode on/off (tool cursor visible, hit regions published).
    void setInteractionMode(bool active);
    void handleInput(const InputEvent& ev);

    // false: stop spawning new flakes (existing ones keep falling).
    void setEmissionEnabled(bool on);

    // Remove all drifts (and loose snowballs/snowmen) without touching the weather.
    void clearAllSnow();

    // Mouse position for the "umbrella" effect, fed continuously (also outside interaction
    // mode). valid=false when unknown (e.g. secure desktop).
    void setAmbientPointer(float x, float y, bool valid);

    // Start the fireworks show now (tray «Салют!» and the New Year midnight trigger).
    void triggerFireworks();

    // Local calendar time, fed by the app about once per second (keeps World free of clocks):
    // drives the New Year midnight fireworks (fires once when the date becomes Jan 1, 00:00:xx,
    // only if festive.fireworks). month 1..12, day 1..31, secondsOfDay 0..86399.
    void setLocalTime(int year, int month, int day, int secondsOfDay);

    // Send the toy snowplow along the primary monitor's taskbar now (no-op if it is already
    // running or there is no taskbar surface on the primary monitor).
    void triggerSnowplow();

    // Advance by one fixed step (normally 1/60 s). dt is clamped internally.
    void step(float dt);

    // Fill `out` (fully overwritten) with render data for the current state.
    void buildSnapshot(FrameSnapshot& out) const;

    // Introspection (tests, diagnostics).
    std::size_t particleCount() const;
    std::size_t surfaceCount() const;
    double totalSnowMass() const;   // sum of heightmap area in px^2 over all surfaces (incl. hidden)

private:
    friend struct WorldTestAccess;   // simulation/world_internal.h (unit tests, diagnostics)
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snow
