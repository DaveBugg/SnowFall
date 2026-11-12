#include "simulation/festive.h"

#include <algorithm>
#include <cmath>

#include "surfaces/surface_registry.h"

namespace snow {

namespace {
constexpr float kTwoPi = 6.2831853f;

float hash01(std::uint64_t seed, long long idx) {
    std::uint64_t z = seed ^ (static_cast<std::uint64_t>(idx) * 0xD1B54A32D192ED03ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}

constexpr std::uint8_t kBulbColors[6] = {1, 2, 0, 3, 4, 6};   // red, green, gold, blue, violet, orange
}  // namespace

int garlandPattern(float time) {
    return static_cast<int>(std::floor(std::max(time, 0.0f) / kGarlandPatternSeconds)) % kGarlandPatterns;
}

bool garlandBulbOn(int pattern, int k, float time, std::uint64_t seed, float* brightness) {
    float b = 1.0f;
    bool on = true;
    switch (pattern) {
        case 0: {   // chase: every 4th bulb, running along
            const int step = static_cast<int>(std::floor(time * 8.0f));
            on = ((k - step) % 4 + 4) % 4 == 0;
            break;
        }
        case 1:     // alternate halves
            on = ((k + static_cast<int>(std::floor(time * 1.5f))) & 1) == 0;
            break;
        case 2:     // random twinkle
            on = hash01(seed, static_cast<long long>(k) * 100003 + static_cast<long long>(std::floor(time * 4.0f))) < 0.55f;
            break;
        default: {  // slow fade wave
            const float w = std::sin(time * 1.3f - static_cast<float>(k) * 0.45f);
            on = w > -0.2f;
            b = std::clamp(0.5f + 0.5f * w, 0.0f, 1.0f);
            break;
        }
    }
    if (brightness) *brightness = on ? b : 0.0f;
    return on;
}

void buildGarland(const Surface& s, float time, float plowX, std::vector<SpriteInstance>& out) {
    const float d = s.dpiScale;
    const float width = s.width();
    if (width < 40.0f * d) return;
    const float top = static_cast<float>(s.top);
    const float left = static_cast<float>(s.left);
    auto driftTop = [&](float lx) { return top - s.hm.heightAtLocalX(std::clamp(lx, 0.0f, width)); };
    const int pattern = garlandPattern(time);
    const float bulbHalfH = 4.2f * d;
    const float clearance = 2.0f * bulbHalfH + 2.0f * d;   // the hanging bulbs clear the snow / the edge
    const float bulbStep = kGarlandBulbSpacingAt96 * d;
    float a = 0;
    long long anchorIdx = 0;
    int bulb = 0;
    float nextBulb = 0.5f * bulbStep;
    while (a < width - 1.0f) {
        const float span = (kGarlandAnchorMinAt96 + (kGarlandAnchorMaxAt96 - kGarlandAnchorMinAt96) * hash01(s.capSeed, anchorIdx)) * d;
        const float b = std::min(width, a + span);
        const float len = b - a;
        const float sag = std::clamp(0.06f * len, 5.0f * d, 10.0f * d);
        // Anchors ride on the drift, high enough that the swag's lowest point clears it.
        const float ya = driftTop(a) - sag - clearance, yb = driftTop(b) - sag - clearance;
        auto wireY = [&](float lx) {
            const float u = (lx - a) / std::max(len, 1.0f);
            return ya + (yb - ya) * u + 4.0f * sag * u * (1.0f - u);
        };
        const int segs = std::max(3, static_cast<int>(len / (20.0f * d)));
        for (int k = 0; k < segs; ++k) {
            const float x0 = a + len * static_cast<float>(k) / static_cast<float>(segs);
            const float x1 = a + len * static_cast<float>(k + 1) / static_cast<float>(segs);
            const float y0 = wireY(x0), y1 = wireY(x1);
            const float mx = 0.5f * (x0 + x1), my = 0.5f * (y0 + y1);
            if (my > driftTop(mx) + 1.0f) continue;   // buried in a lump of snow
            SpriteInstance w;
            w.x = left + mx;
            w.y = my;
            w.halfW = 0.5f * std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
            w.halfH = 1.0f * d;
            w.rotation = std::atan2(y1 - y0, x1 - x0);
            w.shape = Shape::GarlandWire;
            out.push_back(w);
        }
        while (nextBulb < b) {
            const float lx = nextBulb;
            const float halfH = bulbHalfH;
            const float cy = wireY(lx) + halfH;
            const int k = bulb++;
            nextBulb += bulbStep;
            if (cy > driftTop(lx) + 1.0f) continue;   // buried
            float bright = 1;
            const bool on = garlandBulbOn(pattern, k, time, s.capSeed, &bright);
            SpriteInstance sp;
            sp.x = left + lx;
            sp.y = cy;
            sp.halfW = 2.8f * d;
            sp.halfH = halfH;
            sp.shape = Shape::Bulb;
            sp.variant = static_cast<std::uint8_t>(kBulbColors[k % 6] + (on ? 8 : 0));
            sp.opacity = on ? 0.55f + 0.45f * bright : 0.85f;
            if (plowX == plowX) {   // a plow passing by makes the bulbs swing
                const float dist = std::fabs(sp.x - plowX);
                if (dist < 90.0f * d) sp.rotation = 0.5f * (1.0f - dist / (90.0f * d)) * std::sin(time * 18.0f + static_cast<float>(k));
            }
            out.push_back(sp);
        }
        a = b;
        ++anchorIdx;
    }
}

void Fireworks::start(float seconds, const RectI& area, float dpi) {
    if (area.empty()) return;
    area_ = area;
    dpi_ = dpi;
    if (showLeft_ <= 0) {
        ++shows_;
        finale_ = false;
        // A few colours per show.
        colors_[0] = static_cast<std::uint8_t>(rng_.below(8));
        colors_[1] = static_cast<std::uint8_t>((colors_[0] + 1 + rng_.below(7)) % 8);
        colors_[2] = static_cast<std::uint8_t>((colors_[1] + 1 + rng_.below(7)) % 8);
        launchAcc_ = 0.8f;   // first rocket right away
    }
    showLeft_ = std::max(showLeft_, seconds);
}

void Fireworks::launch() {
    Rocket r;
    const float w = static_cast<float>(area_.width()), h = static_cast<float>(area_.height());
    r.dpi = dpi_;
    r.x = static_cast<float>(area_.l) + w * rng_.range(0.1f, 0.9f);
    r.y = static_cast<float>(area_.b);
    r.burstY = static_cast<float>(area_.b) - h * rng_.range(0.4f, 0.7f);
    const float g = 380.0f * dpi_;
    r.vy = -std::sqrt(2.0f * g * (r.y - r.burstY));
    r.vx = rng_.range(-30.0f, 30.0f) * dpi_;
    r.color = colors_[rng_.below(3)];
    r.type = static_cast<std::uint8_t>(rng_.below(4));
    rockets_.push_back(r);
    ++launched_;
}

void Fireworks::burst(const Rocket& r) {
    ++bursts_;
    const float d = r.dpi;
    int n = 0;
    float speed = 220.0f * d, life0 = 1.2f, life1 = 2.0f, drag = 1.2f, grav = 140.0f * d;
    switch (r.type) {
        case 0: n = 60 + static_cast<int>(rng_.below(61)); break;                       // peony
        case 1: n = 40 + static_cast<int>(rng_.below(31)); speed = 230.0f * d; life0 = 1.0f; life1 = 1.6f; break;   // ring
        case 2: n = 60 + static_cast<int>(rng_.below(31)); speed = 130.0f * d; life0 = 2.0f; life1 = 2.5f;           // willow
                drag = 0.5f; grav = 60.0f * d; break;
        default: n = 40 + static_cast<int>(rng_.below(21)); speed = 200.0f * d; life0 = 0.8f; life1 = 1.2f; break;   // crackle
    }
    for (int i = 0; i < n; ++i) {
        Spark s;
        const float a = r.type == 1 ? kTwoPi * static_cast<float>(i) / static_cast<float>(n) : rng_.range(0.0f, kTwoPi);
        const float v = r.type == 1 ? speed : speed * (r.type == 2 ? rng_.range(0.3f, 1.0f) : rng_.range(0.75f, 1.0f));
        s.x = r.x;
        s.y = r.y;
        s.vx = std::cos(a) * v + r.vx;
        s.vy = std::sin(a) * v + 0.3f * r.vy;
        s.life = s.maxLife = rng_.range(life0, life1);
        s.size = (r.type == 2 ? 1.3f : 1.7f) * d;
        s.drag = drag;
        s.gravity = grav;
        s.dpi = d;
        s.color = r.type == 2 ? std::uint8_t{0} : r.color;
        if (r.type == 3) s.crackleAt = s.maxLife * rng_.range(0.15f, 0.4f);
        addSpark(s);
    }
}

void Fireworks::step(float dt) {
    if (dt <= 0) return;
    if (showLeft_ > 0) {
        showLeft_ -= dt;
        rate_ = 1.0f + 2.0f * (0.5f + 0.5f * std::sin(showLeft_ * 0.7f));   // 1..3 rockets per second
        launchAcc_ += rate_ * dt;
        while (launchAcc_ >= 1.0f) {
            launchAcc_ -= 1.0f;
            launch();
        }
        if (!finale_ && showLeft_ < 2.5f) {   // finale: a big volley
            finale_ = true;
            for (int i = 0; i < 8; ++i) launch();
        }
        if (showLeft_ < 0) showLeft_ = 0;
    }
    for (std::size_t k = 0; k < rockets_.size();) {
        Rocket& r = rockets_[k];
        r.vy += 380.0f * r.dpi * dt;
        r.x += r.vx * dt;
        r.y += r.vy * dt;
        r.trailAcc += dt;
        while (r.trailAcc >= 0.02f) {   // dim smoke trail
            r.trailAcc -= 0.02f;
            Spark s;
            s.x = r.x + rng_.range(-1.0f, 1.0f) * r.dpi;
            s.y = r.y;
            s.vx = rng_.range(-10.0f, 10.0f) * r.dpi;
            s.vy = rng_.range(5.0f, 25.0f) * r.dpi;
            s.life = s.maxLife = rng_.range(0.3f, 0.5f);
            s.size = 1.0f * r.dpi;
            s.drag = 2.0f;
            s.gravity = 20.0f * r.dpi;
            s.color = 6;
            s.smoke = true;
            s.dpi = r.dpi;
            addSpark(s);
        }
        if (r.y <= r.burstY || r.vy >= -20.0f * r.dpi) {
            const Rocket copy = r;
            rockets_[k] = rockets_.back();
            rockets_.pop_back();
            burst(copy);
            continue;
        }
        ++k;
    }
    for (std::size_t k = 0; k < sparks_.size();) {
        Spark& s = sparks_[k];
        const float damp = std::exp(-s.drag * dt);
        s.vx *= damp;
        s.vy = s.vy * damp + s.gravity * dt;
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        s.life -= dt;
        if (s.crackleAt > 0 && s.maxLife - s.life >= s.crackleAt) {   // pop into micro sparks
            s.crackleAt = -1;
            const Spark parent = s;
            const int n = 4 + static_cast<int>(rng_.below(3));
            for (int i = 0; i < n; ++i) {
                Spark m = parent;
                const float a = rng_.range(0.0f, kTwoPi);
                m.vx = parent.vx * 0.3f + std::cos(a) * 80.0f * parent.dpi;
                m.vy = parent.vy * 0.3f + std::sin(a) * 80.0f * parent.dpi;
                m.life = m.maxLife = rng_.range(0.2f, 0.35f);
                m.size = 1.0f * parent.dpi;
                m.color = 5;
                m.crackleAt = -1;
                addSpark(m);
            }
        }
        Spark& cur = sparks_[k];
        if (cur.life <= 0) {
            cur = sparks_.back();
            sparks_.pop_back();
            continue;
        }
        ++k;
    }
}

void Fireworks::appendSprites(std::vector<SpriteInstance>& out) const {
    for (const Rocket& r : rockets_) {
        SpriteInstance sp;
        sp.x = r.x;
        sp.y = r.y;
        sp.vx = r.vx;
        sp.vy = r.vy;
        sp.halfW = sp.halfH = 2.2f * r.dpi;
        sp.shape = Shape::Spark;
        sp.variant = 0;
        out.push_back(sp);
    }
    for (const Spark& s : sparks_) {
        SpriteInstance sp;
        const float f = std::clamp(s.life / s.maxLife, 0.0f, 1.0f);
        sp.x = s.x;
        sp.y = s.y;
        sp.vx = s.vx;
        sp.vy = s.vy;
        sp.halfW = sp.halfH = s.size * (0.6f + 0.4f * f);
        sp.opacity = (s.smoke ? 0.35f : 1.0f) * f;
        sp.shape = Shape::Spark;
        sp.variant = s.color;
        out.push_back(sp);
    }
}

}  // namespace snow
