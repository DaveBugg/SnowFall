#include "simulation/weather.h"

#include <algorithm>
#include <cmath>

#include "simulation/rng.h"

namespace snow {

namespace {
// density, fall, turbulence, growth, wind, gust, sway
WeatherProfile g_profiles[4] = {
    {0.40f, 55.0f, 6.0f, 0.6f, 120.0f, 40.0f, 1.0f},     // Light
    {0.75f, 85.0f, 14.0f, 1.0f, 180.0f, 70.0f, 1.0f},    // Normal
    {1.20f, 150.0f, 45.0f, 1.8f, 420.0f, 160.0f, 1.3f},  // Blizzard
    {0.30f, 175.0f, 3.0f, 0.05f, 70.0f, 15.0f, 0.3f},    // Thaw: sparse, heavy, straight, barely grows
};
float g_budgets[3] = {400.0f, 1200.0f, 2500.0f};         // Low, Balanced, High
}  // namespace

WeatherProfile& weatherProfile(WeatherPreset preset) {
    return g_profiles[std::clamp(static_cast<int>(preset), 0, 3)];
}

float& particleBudget(PerfProfile profile) {
    return g_budgets[std::clamp(static_cast<int>(profile), 0, 2)];
}

WeatherParams deriveWeather(const WeatherSettings& s) {
    const WeatherProfile& p = weatherProfile(s.preset);
    WeatherParams w;
    const float gust = static_cast<float>(std::clamp(s.gustiness, 0, 100)) / 100.0f;
    w.density = p.density * static_cast<float>(std::clamp(s.intensity, 0, 100)) / 100.0f;
    w.fallSpeed = p.fallSpeed * static_cast<float>(std::clamp(s.fallSpeedPercent, 25, 200)) / 100.0f;
    w.turbulence = p.turbulence * (0.6f + 0.8f * gust);
    w.growth = p.growth;
    w.wind = p.windSpeed * static_cast<float>(std::clamp(s.wind, -100, 100)) / 100.0f;
    w.gustRel = 0.6f * gust;
    w.gustAbs = p.gustSpeed * gust;
    w.sizeScale = static_cast<float>(std::clamp(s.sizePercent, 50, 200)) / 100.0f;
    w.swayScale = p.swayScale;
    w.thaw = s.preset == WeatherPreset::Thaw;
    if (w.thaw) w.sizeScale *= 1.25f;   // wet flakes clump together
    return w;
}

int targetParticleCount(const Settings& s, const RuntimeTuning& tuning, double normalizedArea) {
    if (!tuning.animationsEnabled || !s.performance.animations) return 0;
    const WeatherParams w = deriveWeather(s.weather);
    const double perScreen = particleBudget(s.performance.profile);
    const double scale = std::clamp(static_cast<double>(tuning.particleScale), 0.0, 1.5);
    const double n = perScreen * (normalizedArea / (1920.0 * 1080.0)) * w.density * scale;
    return static_cast<int>(std::clamp(n, 0.0, static_cast<double>(kMaxParticles)));
}

Weather::Weather(std::uint64_t seed) {
    Rng rng(seed ^ 0xA5A5F00Dull);
    for (float& p : phase_) p = rng.range(0.0f, 6.2831853f);
}

void Weather::setScene(float top, float bottom) {
    top_ = top;
    const float h = std::max(bottom - top, 1.0f);
    invBand_ = static_cast<float>(kBands) / h;
}

void Weather::update(float dt) {
    time_ += dt;
    const float t = static_cast<float>(std::fmod(time_, 3600.0));
    // Global gust in [-1, 1]: three incommensurate slow sines.
    const float g = 0.5f * std::sin(0.31f * t + phase_[0]) + 0.3f * std::sin(0.73f * t + phase_[1]) +
                    0.2f * std::sin(1.9f * t + phase_[2]);
    for (int b = 0; b < kBands; ++b) {
        const float fb = static_cast<float>(b);
        // A slow wave travelling through the bands so that gusts are not uniform over height.
        const float local = std::sin(0.9f * t + 0.7f * fb + phase_[3]) * 0.6f +
                            std::sin(0.37f * t - 0.45f * fb + phase_[4]) * 0.4f;
        bands_[b] = params_.wind * (1.0f + params_.gustRel * g) + params_.gustAbs * (0.6f * g + 0.4f * local);
    }
}

}  // namespace snow
