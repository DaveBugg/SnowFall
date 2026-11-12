#include "surfaces/heightmap.h"

#include <algorithm>
#include <cmath>

namespace snow {

namespace {
float hash01(std::uint64_t seed, int octave, long long idx) {
    std::uint64_t z = seed ^ (static_cast<std::uint64_t>(octave + 1) * 0x9E3779B97F4A7C15ull) ^
                      (static_cast<std::uint64_t>(idx) * 0xD1B54A32D192ED03ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}
}  // namespace

float naturalCapFactor(std::uint64_t seed, float x96) {
    static constexpr float kWave[3] = {190.0f, 85.0f, 38.0f};
    static constexpr float kAmp[3] = {0.5f, 0.32f, 0.18f};
    float n = 0;
    for (int o = 0; o < 3; ++o) {
        const float u = x96 / kWave[o] + 17.0f * static_cast<float>(o);
        const float fl = std::floor(u);
        const long long i = static_cast<long long>(fl);
        const float f = u - fl;
        const float sm = f * f * (3.0f - 2.0f * f);
        const float a = hash01(seed, o, i), b = hash01(seed, o, i + 1);
        n += kAmp[o] * (a + (b - a) * sm);
    }
    // The sum clusters around 0.5: stretch the contrast so mounds and dips are visible.
    n = std::clamp((n - 0.5f) * 1.9f + 0.5f, 0.0f, 1.0f);
    return kNaturalCapMin + (1.0f - kNaturalCapMin) * n;
}

void Heightmap::reset(int columns, float step) {
    const std::size_t n = static_cast<std::size_t>(std::max(columns, 0));
    h_.assign(n, 0.0f);
    cap_.assign(n, maxHeight_);
    blocked_.assign(n, 0);
    step_ = std::max(step, 1.0f);
    peak_ = 0;
    capsValid_ = false;
    touch();
}

void Heightmap::resizeColumns(int columns) {
    const std::size_t n = static_cast<std::size_t>(std::max(columns, 0));
    if (n == h_.size()) return;
    h_.resize(n, 0.0f);
    cap_.resize(n, 0.0f);
    blocked_.resize(n, 0);
    capsValid_ = false;
    touch();
}

void Heightmap::resample(int columns, float newStep, float scale) {
    const std::size_t n = static_cast<std::size_t>(std::max(columns, 0));
    std::vector<float> old;
    old.swap(h_);
    h_.assign(n, 0.0f);
    newStep = std::max(newStep, 1.0f);
    if (!old.empty() && scale > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            const float oldX = static_cast<float>(i) * newStep / scale / step_;   // in old columns
            const std::size_t i0 = static_cast<std::size_t>(oldX);
            if (i0 >= old.size()) break;
            const std::size_t i1 = std::min(i0 + 1, old.size() - 1);
            const float f = oldX - static_cast<float>(i0);
            h_[i] = (old[i0] * (1 - f) + old[i1] * f) * scale;
        }
    }
    step_ = newStep;
    cap_.assign(n, 0.0f);
    blocked_.assign(n, 0);
    peak_ *= std::max(scale, 0.0f);
    capsValid_ = false;
    touch();
}

void Heightmap::setLimits(float maxHeight, float cornerRadius, float width, bool natural,
                          std::uint64_t seed, float dpiScale) {
    maxHeight = std::max(maxHeight, 0.0f);
    cornerRadius = std::max(cornerRadius, 0.0f);
    dpiScale = dpiScale > 0.1f ? dpiScale : 1.0f;
    if (capsValid_ && cap_.size() == h_.size() && maxHeight == maxHeight_ && cornerRadius == cornerRadius_ &&
        width == width_ && natural == natural_ && seed == seed_ && dpiScale == dpi_)
        return;   // unchanged (layouts arrive at 60 Hz while dragging)
    maxHeight_ = maxHeight;
    cornerRadius_ = cornerRadius;
    width_ = width;
    natural_ = natural;
    seed_ = seed;
    dpi_ = dpiScale;
    capsValid_ = true;
    const float invDpi = 1.0f / dpiScale;
    for (std::size_t i = 0; i < cap_.size(); ++i) {
        float c = maxHeight_;
        if (natural_) c *= naturalCapFactor(seed_, static_cast<float>(i) * step_ * invDpi);
        if (cornerRadius_ > 0.5f) {
            const float x = static_cast<float>(i) * step_;
            const float d = std::min(x, width_ - x);
            c *= std::clamp(d / cornerRadius_, 0.0f, 1.0f);
        }
        cap_[i] = c;
    }
}

int Heightmap::columnAt(float localX) const {
    const int n = columns();
    if (n <= 0) return 0;
    const int c = static_cast<int>(localX / step_ + 0.5f);
    return std::clamp(c, 0, n - 1);
}

double Heightmap::mass() const {
    double s = 0;
    for (float v : h_) s += v;
    return s * step_;
}

void Heightmap::setBlocked(int i, bool b) {
    std::uint8_t& v = blocked_[static_cast<std::size_t>(i)];
    const std::uint8_t nv = b ? std::uint8_t{1} : std::uint8_t{0};
    if (v != nv) {
        v = nv;
        dirty_ = true;   // neighbours may now exchange snow
    }
}

float Heightmap::deposit(float localX, float area) {
    const int n = columns();
    if (n == 0 || area <= 0) return 0;
    const int c = columnAt(localX);
    if (blocked(c)) return 0;
    static constexpr float kKernel[3] = {0.25f, 0.5f, 0.25f};
    const float invStep = 1.0f / step_;
    float left = 0, done = 0;
    for (int k = -1; k <= 1; ++k) {
        const int i = c + k;
        const float a = area * kKernel[k + 1];
        if (i < 0 || i >= n || blocked(i)) {
            left += a;
            continue;
        }
        const std::size_t u = static_cast<std::size_t>(i);
        const float room = cap_[u] - h_[u];
        if (room <= 0) {
            left += a;
            continue;
        }
        float dh = a * invStep;
        if (dh > room) {
            left += (dh - room) * step_;
            dh = room;
        }
        h_[u] += dh;
        peak_ = std::max(peak_, h_[u]);
        done += dh * step_;
    }
    for (int pass = 0; pass < 2 && left > 1e-5f; ++pass) {
        // Roll to the lowest column with room that is reachable without crossing a blocked one.
        int best = -1;
        float bestH = 0;
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int k = (dir < 0 ? 0 : 1); k <= kRollRange; ++k) {
                const int i = c + dir * k;
                if (i < 0 || i >= n || blocked(i)) break;
                const std::size_t u = static_cast<std::size_t>(i);
                if (cap_[u] - h_[u] > 1e-3f && (best < 0 || h_[u] < bestH)) {
                    best = i;
                    bestH = h_[u];
                }
            }
        }
        if (best < 0) break;
        const std::size_t u = static_cast<std::size_t>(best);
        const float dh = std::min(left * invStep, cap_[u] - h_[u]);
        h_[u] += dh;
        peak_ = std::max(peak_, h_[u]);
        done += dh * step_;
        left -= dh * step_;
    }
    if (done > 0) touch();
    return done;
}

RelaxResult Heightmap::relax(const RelaxParams& p) {
    RelaxResult res;
    const int n = columns();
    if (n == 0) {
        dirty_ = false;
        return res;
    }
    const float limit = p.maxSlope * step_;
    const float smooth = smoothBudget_ > 0 ? p.smooth : 0.0f;
    if (smoothBudget_ > 0) --smoothBudget_;
    float moved = 0, avalanched = 0;
    auto exchange = [&](int i) {
        const std::size_t a = static_cast<std::size_t>(i), b = a + 1;
        if (blocked_[a] | blocked_[b]) return;
        const float d = h_[a] - h_[b];
        float t = smooth * d;
        bool steep = false;
        if (d > limit) {
            t += p.avalanche * (d - limit);
            steep = true;
        } else if (d < -limit) {
            t += p.avalanche * (d + limit);
            steep = true;
        }
        // Never push a column above its cap (keeps rounded-corner tapers and maxHeight).
        if (t > 0) t = std::min(t, std::max(0.0f, cap_[b] - h_[b]));
        else t = -std::min(-t, std::max(0.0f, cap_[a] - h_[a]));
        h_[a] -= t;
        h_[b] += t;
        if (steep) avalanched = std::max(avalanched, std::fabs(t));
        moved = std::max(moved, std::fabs(t));
    };
    // Gauss-Seidel sweep, alternating direction every pass to avoid a directional bias.
    flip_ = !flip_;
    if (flip_) {
        for (int i = 0; i + 1 < n; ++i) exchange(i);
    } else {
        for (int i = n - 2; i >= 0; --i) exchange(i);
    }
    if (p.openLeft && !blocked_.front() && h_.front() > limit) {
        const float out = p.avalanche * (h_.front() - limit);
        h_.front() -= out;
        res.lostLeft = out * step_;
        moved = std::max(moved, out);
        avalanched = std::max(avalanched, out);
    }
    if (p.openRight && !blocked_.back() && h_.back() > limit) {
        const float out = p.avalanche * (h_.back() - limit);
        h_.back() -= out;
        res.lostRight = out * step_;
        moved = std::max(moved, out);
        avalanched = std::max(avalanched, out);
    }
    float pk = 0;
    for (float v : h_) pk = std::max(pk, v);
    peak_ = pk;
    res.maxMove = moved;
    if (moved > 1e-6f) ++version_;
    dirty_ = avalanched > p.settle || smoothBudget_ > 0;
    return res;
}

float Heightmap::lowerTo(int i, float newHeight) {
    float& v = h_[static_cast<std::size_t>(i)];
    newHeight = std::max(newHeight, 0.0f);
    if (v <= newHeight) return 0;
    const float removed = (v - newHeight) * step_;
    v = newHeight;
    touch();
    return removed;
}

void Heightmap::setHeight(int i, float h) {
    h = std::max(h, 0.0f);
    h_[static_cast<std::size_t>(i)] = h;
    peak_ = std::max(peak_, h);
    touch();
}

void Heightmap::scaleAll(float factor) {
    for (float& v : h_) v *= factor;
    peak_ *= factor;
    touch();
}

float Heightmap::addRaw(int i, float dh, float limit) {
    float& v = h_[static_cast<std::size_t>(i)];
    const float nv = std::min(v + std::max(dh, 0.0f), std::max(limit, v));
    const float added = (nv - v) * step_;
    if (added <= 0) return 0;
    v = nv;
    peak_ = std::max(peak_, v);
    touch();
    return added;
}

void Heightmap::clear() {
    std::fill(h_.begin(), h_.end(), 0.0f);
    peak_ = 0;
    touch();
    dirty_ = false;
}

}  // namespace snow
