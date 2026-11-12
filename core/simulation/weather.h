// Weather: preset profile table + sliders -> effective parameters, and the time-varying
// wind field (base wind + smooth gusts, slightly different per horizontal band).
#pragma once
#include <cstdint>

#include "core/settings.h"

namespace snow {

// One row per WeatherPreset. All speeds are px/s at 96 DPI. Tunable at runtime through
// weatherProfile() (the table is plain mutable data, not baked into the code paths).
struct WeatherProfile {
    float density;        // fraction of the quality budget used at intensity 100 %
    float fallSpeed;      // mean terminal fall speed
    float turbulence;     // amplitude of the per-flake turbulent velocity
    float growth;         // drift growth multiplier (deposited area per flake)
    float windSpeed;      // horizontal wind speed at the wind slider's +-100
    float gustSpeed;      // extra gust amplitude at gustiness 100 %
    float swayScale;      // per-flake sway multiplier
};
WeatherProfile& weatherProfile(WeatherPreset preset);

// Particle budget of a performance profile: flakes per 1920x1080 (at 96 DPI) of monitor area.
float& particleBudget(PerfProfile profile);
inline constexpr int kMaxParticles = 12000;   // hard cap for ambient flakes

struct WeatherParams {
    float density = 0;
    float fallSpeed = 0;
    float turbulence = 0;
    float growth = 1;
    float wind = 0;        // signed base wind, px/s at 96 DPI (negative = to the left)
    float gustRel = 0;     // relative modulation of the base wind (0..~0.6)
    float gustAbs = 0;     // absolute gust amplitude, px/s
    float sizeScale = 1;
    float swayScale = 1;
    bool thaw = false;     // wet, heavy flakes; drifts melt (see World)
};
WeatherParams deriveWeather(const WeatherSettings& s);

// Ambient flake count for the given settings and DPI-normalised monitor area (px^2).
int targetParticleCount(const Settings& s, const RuntimeTuning& tuning, double normalizedArea);

class Weather {
public:
    inline static constexpr int kBands = 16;

    explicit Weather(std::uint64_t seed);
    void configure(const WeatherSettings& s) { params_ = deriveWeather(s); }
    void setScene(float top, float bottom);
    void update(float dt);

    const WeatherParams& params() const { return params_; }
    // Wind (px/s at 96 DPI) at height y.
    float windAt(float y) const {
        int b = static_cast<int>((y - top_) * invBand_);
        b = b < 0 ? 0 : (b >= kBands ? kBands - 1 : b);
        return bands_[b];
    }
    float time() const { return static_cast<float>(time_); }

private:
    WeatherParams params_;
    double time_ = 0;
    float phase_[5]{};
    float bands_[kBands]{};
    float top_ = 0;
    float invBand_ = 0;
};

}  // namespace snow
