// Heightmap: snow profile of one horizontal edge in LOCAL coordinates. Column i sits at
// local x = i * step and holds a height in px (rising towards -Y). Because nothing in here
// depends on where the surface is on the desktop, moving a window never touches it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace snow {

struct RelaxParams {
    float maxSlope = 0.9f;     // critical slope (rise/run); steeper snow avalanches
    float avalanche = 0.25f;   // fraction of the excess moved per step (<= 0.5 for stability)
    float smooth = 0.003f;     // mild diffusion coefficient (applied for kSmoothPasses after a change)
    float settle = 0.0015f;    // px: largest avalanche transfer below which the map goes clean
    bool openLeft = true;      // snow may fall off the left / right end
    bool openRight = true;
};

struct RelaxResult {
    float lostLeft = 0;    // area (px^2) that fell off each end during this pass
    float lostRight = 0;
    float maxMove = 0;     // largest height transfer of the pass (px)
};

// Multiplier in [kNaturalCapMin, 1] of the natural snow-cap profile at x96 (local px at 96 DPI).
// 3 octaves of smooth value noise, wavelengths 190 / 85 / 38 px.
inline constexpr float kNaturalCapMin = 0.55f;
float naturalCapFactor(std::uint64_t seed, float x96);

class Heightmap {
public:
    inline static constexpr int kRollRange = 6;   // columns searched when a column is full
    // Smoothing runs only for this many passes after the last change (deposit, brush...), so
    // an undisturbed drift comes to rest instead of slowly diffusing flat forever.
    inline static constexpr int kSmoothPasses = 90;

    void reset(int columns, float step);
    // Keep columns anchored to the left edge; new columns start empty.
    void resizeColumns(int columns);
    // DPI change: new column spacing, positions and heights scaled by `scale`.
    void resample(int columns, float newStep, float scale);
    // Per-column height limit: maxHeight, tapered linearly to 0 over cornerRadius at both
    // ends of a surface `width` px wide. With `natural`, the limit is further multiplied by
    // naturalCapFactor(seed, localX / dpiScale): an irregular, lumpy snow-cap profile that
    // depends only on the LOCAL x, so it is stable under moves and resizes.
    void setLimits(float maxHeight, float cornerRadius, float width, bool natural = false,
                   std::uint64_t seed = 0, float dpiScale = 1.0f);

    int columns() const { return static_cast<int>(h_.size()); }
    float step() const { return step_; }
    float maxHeight() const { return maxHeight_; }
    const std::vector<float>& heights() const { return h_; }
    float height(int i) const { return h_[static_cast<std::size_t>(i)]; }
    float cap(int i) const { return cap_[static_cast<std::size_t>(i)]; }
    int columnAt(float localX) const;            // nearest column, clamped
    float heightAtLocalX(float localX) const { return h_.empty() ? 0.0f : height(columnAt(localX)); }
    float peak() const { return peak_; }         // upper bound of the tallest column
    double mass() const;                         // area in px^2
    std::uint32_t version() const { return version_; }
    bool dirty() const { return dirty_; }

    // Columns that currently cannot receive or exchange snow (occluded / off-screen).
    bool blocked(int i) const { return blocked_[static_cast<std::size_t>(i)] != 0; }
    void setBlocked(int i, bool b);

    // Adds `area` px^2 around localX (3-column kernel), respecting caps and blocked columns;
    // overflow rolls to the lowest reachable column within kRollRange, the rest is discarded.
    // Returns the area actually deposited.
    float deposit(float localX, float area);
    // Avalanche + smoothing pass. Conserves mass except for what falls off open ends.
    RelaxResult relax(const RelaxParams& p);

    // Lower column i to at most newHeight; returns the removed area.
    float lowerTo(int i, float newHeight);
    void setHeight(int i, float h);              // tests / tools
    void scaleAll(float factor);
    // Adds dh to column i ignoring the cap (crater rims); the result never exceeds `limit`.
    // Returns the area actually added.
    float addRaw(int i, float dh, float limit);
    void clear();

private:
    void touch() {
        ++version_;
        dirty_ = true;
        smoothBudget_ = kSmoothPasses;
    }
    std::vector<float> h_;
    std::vector<float> cap_;
    std::vector<std::uint8_t> blocked_;
    float step_ = 3;
    float maxHeight_ = 0;
    float cornerRadius_ = 0;
    float width_ = 0;
    float peak_ = 0;
    // setLimits cache key
    bool capsValid_ = false;
    bool natural_ = false;
    std::uint64_t seed_ = 0;
    float dpi_ = 1;
    std::uint32_t version_ = 1;
    bool dirty_ = false;
    bool flip_ = false;
    int smoothBudget_ = 0;
};

}  // namespace snow
