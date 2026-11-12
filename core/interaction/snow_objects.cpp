#include "interaction/snow_objects.h"

#include <algorithm>
#include <cmath>

#include "interaction/tools.h"
#include "interaction/icicles.h"
#include "interaction/splats.h"
#include "simulation/chunks.h"
#include "simulation/particles.h"
#include "surfaces/geometry.h"

namespace snow {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kGrabPad = 4.0f;

float len(float x, float y) { return std::sqrt(x * x + y * y); }

RectI geomBox(const PileGeom& g, bool withArms) {
    float rmax = 0, topY = g.base, botY = g.base;
    for (int i = 0; i < g.n; ++i) {
        rmax = std::max(rmax, g.r[i]);
        topY = std::min(topY, g.cy[i] - g.r[i]);
        botY = std::max(botY, g.cy[i] + g.r[i]);
    }
    const float reach = (withArms && g.n >= 2) ? rmax * 2.4f : rmax;   // snowmen have arms
    return RectI{static_cast<int>(std::floor(g.cx - reach)) - 2, static_cast<int>(std::floor(topY)) - 2,
                 static_cast<int>(std::ceil(g.cx + reach)) + 2, static_cast<int>(std::ceil(botY)) + 2};
}

SpriteInstance makeSprite(Shape shape, float x, float y, float halfW, float halfH, float rotation) {
    SpriteInstance s;
    s.x = x;
    s.y = y;
    s.halfW = halfW;
    s.halfH = halfH;
    s.rotation = rotation;
    s.opacity = 1.0f;
    s.shape = shape;
    return s;
}

SpriteInstance accessorySprite(Accessory a, float x, float y, const AccessoryPlacement& pl) {
    SpriteInstance sp = makeSprite(Shape::Accessory, x, y, pl.halfW, pl.halfH, pl.rotation);
    sp.variant = static_cast<std::uint8_t>(a);
    return sp;
}

// Sprites of a stack; the whole figure is rotated by `tilt` around its base point.
// `jig`: googly pupils' extra swing (radians).
void appendStackSprites(const PileGeom& g, float tilt, float vx, float vy, std::vector<SpriteInstance>& out,
                        const SnowmanLook& look = SnowmanLook{}, float wave = kArmRestAngle, float jig = 0,
                        bool bare = false) {
    const std::size_t first = out.size();
    for (int i = 0; i < g.n; ++i) out.push_back(makeSprite(Shape::Snowball, g.cx, g.cy[i], g.r[i], g.r[i], 0));
    if (g.n >= 2 && !bare) {
        // Buttons and arms go on the ball below the head.
        const int body = g.n - 2;
        const float rb = g.r[body], yb = g.cy[body];
        const float coal = std::max(1.0f, 0.1f * rb);
        bool hasFlower = false, hasBroom = false, hasHat = look.hat >= 0 || look.hatAcc != kNoPart;
        for (std::uint8_t ac : look.acc) {
            hasFlower = hasFlower || ac == static_cast<std::uint8_t>(Accessory::Flower);
            hasBroom = hasBroom || ac == static_cast<std::uint8_t>(Accessory::Broom);
        }
        for (int k = -1; k <= 1; ++k) {
            if (g.n == 2 && k == -1) continue;   // hidden behind the head overlap
            if (hasFlower && !hasHat && k == -1 + (g.n == 2 ? 1 : 0)) continue;   // the flower takes the top button
            out.push_back(makeSprite(Shape::Coal, g.cx, yb + static_cast<float>(k) * 0.42f * rb, coal, coal, 0));
        }
        const float armHalf = 0.6f * rb, armThick = std::max(1.0f, 0.07f * rb);
        // Shoulders: the ball below the head, at its upper third, mirrored about the centre line.
        const float shY = yb - 0.33f * rb, shDX = 0.94f * rb;
        float angR = kArmRestAngle, angL = kArmRestAngle;   // 0: out (both in +X-arm convention)
        if (look.arms == 1) {
            angR = angL = kArmUpAngle;                       // 1: both up
        } else if (look.arms == 2) {
            angR = wave;                                     // 2: +X arm waving
        }
        if (hasBroom) angR = kArmRestAngle;                  // holding the broom
        // Never across the face: the tip stays outside the head (+ a margin).
        const float headR = g.r[g.n - 1], headY = g.cy[g.n - 1];
        auto clearOfHead = [&](float a) {
            for (int it = 0; it < 30; ++it) {
                const float tx = shDX + std::cos(a) * 2.0f * armHalf, ty = shY + std::sin(a) * 2.0f * armHalf;
                if (std::hypot(tx, ty - headY) >= (1.0f + kArmHeadClearance) * headR) break;
                a += 0.05f;   // lower it
            }
            return a;
        };
        angR = clearOfHead(angR);
        angL = clearOfHead(angL);
        float armEndX = 0, armEndY = 0;
        for (int side = -1; side <= 1; side += 2) {
            const float a = side > 0 ? angR : angL;
            const float fs = static_cast<float>(side);
            const float dx = fs * std::cos(a), dy = std::sin(a);
            const float rot = side > 0 ? a : kPi - a;
            const float sx = g.cx + fs * shDX;
            out.push_back(makeSprite(Shape::Stick, sx + dx * armHalf, shY + dy * armHalf, armHalf, armThick, rot));
            if (side > 0) {
                armEndX = sx + dx * 2.0f * armHalf;
                armEndY = shY + dy * 2.0f * armHalf;
            }
        }
        // Face on the top ball.
        const int head = g.n - 1;
        const float rh = g.r[head], yh = g.cy[head];
        const float k = rh / 22.0f;
        if (look.googly) {
            const AccessoryPlacement pl = accessoryPlacement(Accessory::GooglyEye, rh);
            for (int side = -1; side <= 1; side += 2) {
                AccessoryPlacement e = pl;
                e.dx = static_cast<float>(side) * std::fabs(pl.dx);
                e.rotation = 1.5708f + jig * (side > 0 ? 1.0f : -0.8f);   // pupil direction (down + jiggle)
                out.push_back(accessorySprite(Accessory::GooglyEye, g.cx + e.dx, yh + e.dy, e));
            }
        } else {
            const float eye = std::max(1.0f, 3.0f * k);
            out.push_back(makeSprite(Shape::Coal, g.cx - 8.0f * k, yh - 6.0f * k, eye, eye, 0));
            out.push_back(makeSprite(Shape::Coal, g.cx + 8.0f * k, yh - 6.0f * k, eye, eye, 0));
        }
        if (look.smile) {
            const float c = std::max(0.8f, 0.06f * rh);
            for (int q = 0; q < 5; ++q) {
                const float a = -0.7f + 0.35f * static_cast<float>(q);
                out.push_back(makeSprite(Shape::Coal, g.cx + std::sin(a) * 0.42f * rh, yh + 0.18f * rh + std::cos(a) * 0.3f * rh,
                                         c, c, 0));
            }
        }
        // Parts by layer (renderer convention, lowest first): 0 scarf, 1 neck items, 2 face
        // items, carrot, 3 eyewear, 4 headphones / earmuffs, 5 hats, 6 flower / broom.
        const float nose = look.set ? look.nose : 0.5f;
        for (int layer = 0; layer <= 6; ++layer) {
            if (layer == 0 && look.scarf >= 0) {
                const float neck = yh + 0.85f * rh, sw = 1.25f * rh, sh = 1.1f * rh;
                SpriteInstance sc = makeSprite(Shape::Scarf, g.cx + 0.04f * sw, neck + 0.62f * sh, sw, sh, 0);
                sc.variant = static_cast<std::uint8_t>(look.scarf);
                out.push_back(sc);
            }
            if (layer == 3)   // the carrot goes over beards / moustaches, under glasses
                out.push_back(makeSprite(Shape::Carrot, g.cx + nose * 0.8f * rh, yh + 0.12f * rh, nose * rh, 0.17f * rh, 0.12f));
            if (layer == 5) {
                if (look.hatAcc != kNoPart) {
                    const Accessory a = static_cast<Accessory>(look.hatAcc);
                    const AccessoryPlacement pl = accessoryPlacement(a, rh);
                    out.push_back(accessorySprite(a, g.cx + pl.dx, yh + pl.dy, pl));
                } else if (look.hat >= 0) {   // brim (local +Y) sits on the head
                    static constexpr float kHH[4] = {0.95f, 0.80f, 0.75f, 0.95f};
                    static constexpr float kSink[4] = {0.30f, 0.30f, 0.50f, 0.42f};
                    const int v = look.hat & 3;
                    const float hw = rh / 1.1f, hh = hw * kHH[v];
                    SpriteInstance h = makeSprite(Shape::Hat, g.cx, yh - rh + rh * kSink[v] - hh, hw, hh, 0);
                    h.variant = static_cast<std::uint8_t>(v);
                    out.push_back(h);
                }
            }
            for (std::uint8_t ac : look.acc) {
                if (ac == kNoPart) continue;
                const Accessory a = static_cast<Accessory>(ac);
                if (accessoryLayer(a) != layer) continue;
                const AccessoryPlacement pl = accessoryPlacement(a, rh);
                if (a == Accessory::Broom) {
                    out.push_back(accessorySprite(a, armEndX + pl.dx, armEndY + pl.dy, pl));
                } else if (a == Accessory::Flower && !hasHat) {   // in the top button slot
                    const float by = yb + (g.n == 2 ? 0.0f : -0.42f) * rb;
                    out.push_back(accessorySprite(a, g.cx, by, pl));
                } else {
                    out.push_back(accessorySprite(a, g.cx + pl.dx, yh + pl.dy, pl));
                }
            }
        }
    }
    const float c = std::cos(tilt), s = std::sin(tilt);
    for (std::size_t k = first; k < out.size(); ++k) {
        SpriteInstance& sp = out[k];
        if (tilt != 0.0f) {
            const float dx = sp.x - g.cx, dy = sp.y - g.base;
            sp.x = g.cx + dx * c - dy * s;
            sp.y = g.base + dx * s + dy * c;
            sp.rotation += tilt;
        }
        sp.vx = vx;
        sp.vy = vy;
    }
}

void appendClipped(const RectI& box, const std::vector<RectI>& clip, std::vector<RectI>& out) {
    for (const RectI& c : clip) {
        const RectI x = box.intersect(c);
        if (!x.empty()) out.push_back(x);
    }
}
}  // namespace

PileGeom stackGeom(float cx, float base, const float* radii, int n) {
    PileGeom g;
    g.n = std::clamp(n, 0, 3);
    g.cx = cx;
    g.base = base;
    for (int i = 0; i < g.n; ++i) {
        g.r[i] = radii[i];
        g.cy[i] = i == 0 ? base - radii[0] : g.cy[i - 1] - kStackOverlap * (radii[i - 1] + radii[i]);
    }
    return g;
}

PileGeom pileGeom(const Pile& p, const Surface& s) {
    const float base = static_cast<float>(s.top) - std::min(p.sink, s.hm.heightAtLocalX(p.localX));
    return stackGeom(static_cast<float>(s.left) + p.localX, base, p.r, p.n);
}

CraterResult makeCrater(Surface& s, float localX, float r0) {
    CraterResult res;
    Heightmap& hm = s.hm;
    const int n = hm.columns();
    if (n == 0 || r0 <= 0) return res;
    const float step = hm.step();
    const int c = hm.columnAt(localX);
    const int half = std::max(1, static_cast<int>(std::ceil(r0 / step)));
    for (int i = std::max(0, c - half); i <= std::min(n - 1, c + half); ++i) {
        const float d = (static_cast<float>(i) * step - localX) / r0;
        if (std::fabs(d) > 1.0f) continue;
        const float h = hm.height(i);
        res.removed += hm.lowerTo(i, h * (1.0f - kCraterDepth * (1.0f - d * d)));
    }
    const float perSide = 0.5f * kCraterRimShare * res.removed;
    const int width = std::max(2, half);
    float wsum = 0;
    for (int k = 0; k < width; ++k) wsum += std::sin(kPi * (static_cast<float>(k) + 0.5f) / static_cast<float>(width));
    for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < width; ++k) {
            const int i = c + side * (half + 1 + k);
            if (i < 0 || i >= n) continue;
            const float w = std::sin(kPi * (static_cast<float>(k) + 0.5f) / static_cast<float>(width)) / wsum;
            res.rims += hm.addRaw(i, perSide * w / step, hm.cap(i) * 1.3f + 2.0f * s.dpiScale);
        }
    }
    return res;
}

AccessoryPlacement accessoryPlacement(Accessory a, float rh) {
    // Matches the renderer's reference layout (platform/windows/tests/integration/render/render_demo.cpp,
    // placeAccessory): offsets from the head centre and half sizes in k = head radius / 22.
    // Hat-slot items: bottom edge (local +Y) at headTop + sink * rh.
    constexpr float kHeadTop = -22.0f;
    constexpr float kNeck = 0.85f * 22.0f;
    auto hatSlot = [&](float hw, float hh, float sink) { return AccessoryPlacement{0.0f, kHeadTop + 22.0f * sink - hh, hw, hh, 0}; };
    AccessoryPlacement p;
    switch (a) {
        case Accessory::Sunglasses:
        case Accessory::RoundGlasses: p = {0.0f, -6.0f, 16.0f, 6.2f, 0}; break;
        case Accessory::EyePatch: p = {8.0f, -6.0f, 20.0f, 8.5f, 0}; break;
        case Accessory::Headphones:
        case Accessory::Earmuffs: p = {0.0f, -2.0f, 27.0f, 22.5f, 0}; break;
        case Accessory::BowTie: p = {0.0f, kNeck + 3.0f, 11.0f, 6.0f, 0}; break;
        case Accessory::Crown: p = hatSlot(18.0f, 13.0f, 0.25f); break;
        case Accessory::TricornHat: p = hatSlot(30.0f, 15.0f, 0.35f); break;
        case Accessory::CowboyHat: p = hatSlot(32.0f, 17.0f, 0.35f); break;
        case Accessory::ChefHat: p = hatSlot(16.0f, 18.0f, 0.30f); break;
        case Accessory::CatEars: p = hatSlot(24.0f, 12.0f, 0.45f); break;
        case Accessory::Whiskers: p = {2.0f, 2.0f, 26.0f, 8.0f, 0}; break;
        case Accessory::Mustache: p = {3.0f, 8.5f, 14.0f, 6.0f, 0}; break;
        case Accessory::Beard: p = {0.0f, 12.0f, 22.0f, 20.0f, 0}; break;
        case Accessory::Pipe: p = {17.0f, 11.0f, 13.0f, 8.0f, 0}; break;   // mouthpiece at the -X edge
        case Accessory::GooglyEye: p = {8.0f, -6.0f, 5.5f, 5.5f, 0}; break;   // one eye; the other mirrored
        case Accessory::Flower: p = {-0.75f * 22.0f, kHeadTop + 0.15f * 22.0f, 8.0f, 8.0f, 0}; break;   // hat side
        case Accessory::Broom: p = {0.0f, 0.14f * 30.0f, 8.0f, 30.0f, 0}; break;   // relative to the +X arm end (hand)
        case Accessory::Bandana: p = {0.0f, kNeck + 8.2f, 17.0f, 12.0f, 0}; break;
        case Accessory::Monocle: p = {8.0f, -6.0f, 9.6f, 12.0f, 0}; break;
        default: break;
    }
    const float k = rh / 22.0f;
    p.dx *= k;
    p.dy *= k;
    p.halfW *= k;
    p.halfH *= k;
    return p;
}

int accessoryLayer(Accessory a) {
    switch (a) {
        case Accessory::BowTie:
        case Accessory::Bandana: return 1;
        case Accessory::Beard:
        case Accessory::Mustache:
        case Accessory::Whiskers:
        case Accessory::Pipe: return 2;
        case Accessory::Sunglasses:
        case Accessory::RoundGlasses:
        case Accessory::EyePatch:
        case Accessory::Monocle: return 3;
        case Accessory::Headphones:
        case Accessory::Earmuffs: return 4;
        case Accessory::Crown:
        case Accessory::TricornHat:
        case Accessory::CowboyHat:
        case Accessory::ChefHat:
        case Accessory::CatEars: return 5;
        default: return 6;
    }
}

float waveArmAngle(const Pile& p) {
    if (p.look.arms != 2 || p.waveT <= 0 || p.waveDur <= 0) return kArmRestAngle;
    // Sine ease in / out around -30 deg, starting and ending at the rest angle.
    static const float kPhase0 = std::asin((kArmRestAngle - kWaveCenter) / -kWaveAmp);
    const float t = p.waveDur - p.waveT;
    const float s = std::sin(2.0f * kPi * t / std::max(p.wavePeriod, 0.1f) + kPhase0);
    float a = kWaveCenter - kWaveAmp * s;
    if (s > 0) a -= kWaveFlick * std::pow(s, 12.0f);   // a little flick at the top
    return a;
}

bool isHatSlot(Accessory a) {
    return a == Accessory::Crown || a == Accessory::TricornHat || a == Accessory::CowboyHat || a == Accessory::ChefHat ||
           a == Accessory::CatEars;
}

int skinIndex(const std::string& id) {
    for (int i = 0; i < kSnowmanSkinCount; ++i)
        if (id == kSnowmanSkins[i].id) return i;
    return -1;
}

void ObjectSystem::applySkin(SnowmanLook& l, int skin) {
    if (skin < 0 || skin >= kSnowmanSkinCount) return;
    const SnowmanSkin& sk = kSnowmanSkins[skin];
    l.skin = static_cast<std::int8_t>(skin);
    l.hat = l.scarf = -1;
    l.hatAcc = kNoPart;
    for (std::uint8_t& a : l.acc) a = kNoPart;
    int n = 0;
    for (const SkinPart& part : sk.parts) {
        if (part.kind == SkinPart::Kind::Hat) {
            l.hat = static_cast<std::int8_t>(part.variant);
        } else if (part.kind == SkinPart::Kind::Scarf) {
            l.scarf = static_cast<std::int8_t>(part.variant);
        } else if (part.variant != kNoPart) {
            if (isHatSlot(static_cast<Accessory>(part.variant))) l.hatAcc = part.variant;
            else if (n < 4) l.acc[n++] = part.variant;
        }
    }
    l.googly = sk.googlyEyes;
    for (std::uint8_t a : l.acc)   // no smile under a beard / mustache / pipe
        if (a == static_cast<std::uint8_t>(Accessory::Beard) || a == static_cast<std::uint8_t>(Accessory::Mustache) ||
            a == static_cast<std::uint8_t>(Accessory::Pipe))
            l.smile = false;
}

SnowmanLook ObjectSystem::rollLook() {
    SnowmanLook l;
    l.set = true;
    l.arms = static_cast<std::uint8_t>(rng_.below(3));
    l.smile = rng_.uniform() < kSmileChance;
    l.nose = rng_.range(0.35f, 0.7f);
    int skin = skin_;
    if (skin < 0 && rng_.uniform() < 0.7f) {   // random: mostly a whole skin ...
        skin = static_cast<int>(rng_.below(static_cast<std::uint32_t>(kSnowmanSkinCount)));
        // ... Santa in the festive season more often.
        if (festiveSeason_ && rng_.uniform() < 0.4f) skin = skinIndex("santa");
    }
    if (skin >= 0) {
        applySkin(l, skin);
        return l;
    }
    // ... or random parts.
    if (rng_.uniform() < kHatChance) {
        if (festiveSeason_ && rng_.uniform() < 0.55f) l.hat = 3;   // Santa hat season
        else l.hat = static_cast<std::int8_t>(rng_.below(3));
        if (!festiveSeason_ && rng_.uniform() < 0.1f) l.hat = 3;
    }
    if (rng_.uniform() < kScarfChance) l.scarf = static_cast<std::int8_t>(rng_.below(4));
    return l;
}

float ObjectSystem::radiusFor(float collectedArea, float dpiScale) {
    const float rMin = 0.5f * kBallMinDiameterAt96 * dpiScale;
    const float rMax = 0.5f * kBallMaxDiameterAt96 * dpiScale;
    return std::clamp(std::sqrt(kPackFactor * std::max(collectedArea, 0.0f) / kPi), rMin, rMax);
}

int ObjectSystem::heldIndex() const {
    for (std::size_t i = 0; i < balls_.size(); ++i)
        if (balls_[i].held) return static_cast<int>(i);
    return -1;
}

const Ball* ObjectSystem::heldBall() const {
    const int i = heldIndex();
    return i >= 0 ? &balls_[static_cast<std::size_t>(i)] : nullptr;
}

int ObjectSystem::snowballCount() const {
    int n = static_cast<int>(balls_.size());
    for (const Pile& p : piles_) n += p.n == 1 ? 1 : 0;
    return n;
}

int ObjectSystem::snowmanCount() const {
    int n = 0;
    for (const Pile& p : piles_) n += p.n >= 2 ? 1 : 0;
    return n;
}

int ObjectSystem::autoSnowmenAlive() const {
    int n = static_cast<int>(falling_.size());
    for (const Pile& p : piles_) n += (p.autoSpawned && p.n >= 2) ? 1 : 0;
    return n;
}

void ObjectSystem::clear() {
    sculpt_.active = false;
    balls_.clear();
    piles_.clear();
    falling_.clear();
    debris_.clear();
}

bool ObjectSystem::findDriftAt(const SurfaceRegistry& reg, float x, float y, float tol, int& surfaceIndex, int& column) {
    float best = 1e30f;
    surfaceIndex = -1;
    for (std::size_t i = 0; i < reg.surfaces().size(); ++i) {
        const Surface& s = reg.surfaces()[i];
        if (!s.active || s.hm.peak() < 0.5f || x < static_cast<float>(s.left) || x >= static_cast<float>(s.right)) continue;
        if (!reg.inObjectArea(x, static_cast<float>(s.top) - 1.0f)) continue;
        const int col = s.hm.columnAt(x - static_cast<float>(s.left));
        const float topY = s.topAt(col);
        if (y < topY - tol || y > static_cast<float>(s.top) + tol) continue;
        const float dist = std::fabs(y - topY);
        if (dist < best) {
            best = dist;
            surfaceIndex = static_cast<int>(i);
            column = col;
        }
    }
    return surfaceIndex >= 0;
}

bool ObjectSystem::sculptStart(SurfaceRegistry& reg, float x, float y) {
    if (sculpt_.active) return true;
    int si = -1, col = 0;
    if (!findDriftAt(reg, x, y, 24.0f * reg.dpiScaleAt(x, y), si, col)) return false;
    const Surface& s = reg.surfaces()[static_cast<std::size_t>(si)];
    if (s.hm.height(col) < 1.0f) return false;
    Pile& p = addPile(s, static_cast<float>(col) * s.hm.step(), 0.0f);
    SculptState st;
    st.active = true;
    st.pile = p.id;
    st.surface = s.id;
    st.localX = p.localX;
    st.dpi = s.dpiScale;
    const float r0 = kSculptBaseRadiusAt96 * s.dpiScale;
    st.target[0] = r0;
    st.target[1] = r0 * kSculptMiddleShare;
    st.target[2] = r0 * kSculptHeadShare;
    st.completed = sculpt_.completed;
    sculpt_ = st;
    return true;
}

void ObjectSystem::dressPop(Pile& p, const Surface& s, ParticleSystem* particles) {
    if (!p.look.set) p.look = rollLook();
    if (!particles) return;
    const PileGeom g = pileGeom(p, s);
    const int head = g.n - 1;
    particles->burst(g.cx, g.cy[head] - g.r[head], g.r[head], 10, 90.0f * p.dpi, true, p.dpi);   // a little "pop"
}

void ObjectSystem::sculptStep(float dt, SurfaceRegistry& reg, ParticleSystem* particles, float rub) {
    if (!sculpt_.active || dt <= 0) return;
    Pile* p = findPile(sculpt_.pile);
    Surface* s = reg.find(sculpt_.surface);
    if (!p || !s || !s->active) {
        sculpt_.active = false;
        return;
    }
    const float d = sculpt_.dpi;
    const int stage = p->n - 1;
    // Gather snow from the drift around the base, the most from the deepest columns.
    Heightmap& hm = s->hm;
    const float reach = sculpt_.target[0] + kSculptGatherAt96 * d;
    const int i0 = std::max(0, static_cast<int>((sculpt_.localX - reach) / hm.step()));
    const int i1 = std::min(hm.columns() - 1, static_cast<int>((sculpt_.localX + reach) / hm.step()));
    double total = 0;
    for (int i = i0; i <= i1; ++i) total += hm.height(i);
    float want = kSculptRateAt96 * d * d * std::max(rub, 1.0f) * dt;
    want = std::min(want, static_cast<float>(total * hm.step() * 0.5));   // no snow from nothing
    float taken = 0;
    if (want > 0 && total > 0) {
        for (int i = i0; i <= i1; ++i) {
            const float h = hm.height(i);
            taken += hm.lowerTo(i, h - h / static_cast<float>(total) * want / hm.step());
        }
    }
    sculpt_.area[stage] += taken;
    p->r[stage] = std::sqrt(kPackFactor * sculpt_.area[stage] / kPi);
    sculpt_.patAcc += dt;
    if (sculpt_.patAcc >= kSculptPatSeconds) {   // pat, pat
        sculpt_.patAcc = 0;
        if (particles && taken > 0) {
            const PileGeom g = pileGeom(*p, *s);
            particles->burst(g.cx, g.cy[stage] - g.r[stage], std::max(g.r[stage], 3.0f * d), 2, 60.0f * d, true, d);
        }
    }
    if (p->r[stage] >= sculpt_.target[stage]) {
        if (stage < 2) {   // next ball on top
            p->r[p->n] = 0;
            ++p->n;
        } else {           // head done: dressed with a pop
            dressPop(*p, *s, particles);
            sculpt_.active = false;
            ++sculpt_.completed;
        }
    }
}

void ObjectSystem::sculptEnd(SurfaceRegistry& reg, ParticleSystem* particles) {
    if (!sculpt_.active) return;
    sculpt_.active = false;
    Pile* p = findPile(sculpt_.pile);
    Surface* s = reg.find(sculpt_.surface);
    if (!p || !s) return;
    const float d = sculpt_.dpi;
    // A barely started top ball goes back into the drift.
    if (p->n > 1 && p->r[p->n - 1] < 3.0f * d) {
        const float area = sculpt_.area[p->n - 1];
        if (area > 0) s->hm.deposit(p->localX, area);
        --p->n;
    }
    const float rMin = 0.5f * kBallMinDiameterAt96 * d;
    if (p->n == 1 && kPackFactor * sculpt_.area[0] < kBallMinFill * kPi * rMin * rMin) {   // too little: crumbles
        if (particles && sculpt_.area[0] > 0.5f) {
            const PileGeom g = pileGeom(*p, *s);
            particles->burst(g.cx, g.cy[0], std::max(g.r[0], 3.0f * d), 4, 60.0f * d, true, d);
        }
        for (std::size_t k = 0; k < piles_.size(); ++k)
            if (piles_[k].id == p->id) {
                piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
        return;
    }
    if (p->n >= 2) dressPop(*p, *s, particles);
}

int ObjectSystem::detachPilesOn(const Surface& s, float fraction, float wvx, float wvy, float offX, float offY) {
    int count = 0;
    std::vector<SpriteInstance> parts;
    const float vmax = kDetachMaxSpeedAt96;
    for (std::size_t k = 0; k < piles_.size();) {
        const Pile& p = piles_[k];
        if (p.surface != s.id || !(rng_.uniform() < fraction)) {
            ++k;
            continue;
        }
        const float d = p.dpi;
        const float lag = rng_.range(kDetachLagMin, kDetachLagMax);
        const float vx = std::clamp(wvx * lag, -vmax * d, vmax * d);
        const float vy = std::clamp(wvy * lag, -vmax * d, vmax * d) - rng_.range(0.0f, 40.0f) * d;
        parts.clear();
        appendStackSprites(pileGeom(p, s), p.tilt, 0, 0, parts, p.look, waveArmAngle(p), 0, p.bare);
        for (const SpriteInstance& sp : parts) {
            if (sp.shape == Shape::Snowball) {
                Ball b;
                b.id = nextId_++;
                b.x = sp.x + offX;
                b.y = sp.y + offY;
                b.r = sp.halfW;
                b.collected = kPi * b.r * b.r / kPackFactor;
                b.dpi = d;
                b.vx = vx + rng_.range(-20.0f, 20.0f) * d;
                b.vy = vy;
                b.rotation = sp.rotation;
                b.exclude = s.id;
                balls_.push_back(b);
            } else {
                Debris db;
                db.x = sp.x + offX;
                db.y = sp.y + offY;
                db.vx = vx + rng_.range(-40.0f, 40.0f) * d;
                db.vy = vy - rng_.range(0.0f, 60.0f) * d;
                db.rotation = sp.rotation;
                db.angVel = rng_.range(-6.0f, 6.0f);
                db.halfW = sp.halfW;
                db.halfH = sp.halfH;
                db.dpi = d;
                db.shape = sp.shape;
                db.variant = sp.variant;
                db.exclude = s.id;
                if (sp.shape == Shape::Scarf) {
                    db.gravity = 0.3f;
                    db.sway = 40.0f * d;
                    db.baseVx = db.vx;
                }
                debris_.push_back(db);
            }
        }
        piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
        ++count;
    }
    return count;
}

void ObjectSystem::popDecorations(const std::vector<SpriteInstance>& parts, int dir, float strength, float dpi,
                                  SurfaceId exclude) {
    const float fdir = static_cast<float>(dir);
    for (const SpriteInstance& sp : parts) {
        if (sp.shape == Shape::Snowball) continue;
        Debris db;
        db.x = sp.x;
        db.y = sp.y;
        db.rotation = sp.rotation;
        db.halfW = sp.halfW;
        db.halfH = sp.halfH;
        db.dpi = dpi;
        db.shape = sp.shape;
        db.variant = sp.variant;
        db.exclude = exclude;
        const bool big = sp.shape == Shape::Hat || sp.shape == Shape::Accessory;
        if (sp.shape == Shape::Coal) {   // eyes and buttons scatter
            db.vx = fdir * rng_.range(40.0f, 140.0f) * dpi * strength + rng_.range(-120.0f, 120.0f) * dpi;
            db.vy = -rng_.range(100.0f, 250.0f) * dpi * strength;
            db.angVel = rng_.range(-8.0f, 8.0f);
        } else if (sp.shape == Shape::Carrot) {   // spins away
            db.vx = fdir * rng_.range(120.0f, 260.0f) * dpi * strength;
            db.vy = -rng_.range(180.0f, 320.0f) * dpi * strength;
            db.angVel = (rng_.uniform() < 0.5f ? -1.0f : 1.0f) * rng_.range(10.0f, 16.0f);
        } else if (sp.shape == Shape::Scarf) {    // flutters down
            db.vx = fdir * rng_.range(60.0f, 140.0f) * dpi * strength;
            db.vy = -rng_.range(60.0f, 120.0f) * dpi * strength;
            db.gravity = 0.3f;
            db.sway = 40.0f * dpi;
            db.baseVx = db.vx;
            db.angVel = rng_.range(-1.5f, 1.5f);
        } else if (sp.shape == Shape::Stick) {
            db.vx = fdir * rng_.range(80.0f, 200.0f) * dpi * strength;
            db.vy = -rng_.range(120.0f, 260.0f) * dpi * strength;
            db.angVel = rng_.range(-6.0f, 6.0f);
        } else {   // hats and accessories: off with a pop
            db.vx = fdir * rng_.range(150.0f, 300.0f) * dpi * strength;
            db.vy = -rng_.range(250.0f, 420.0f) * dpi * strength;
            db.angVel = (rng_.uniform() < 0.5f ? -1.0f : 1.0f) * rng_.range(6.0f, 10.0f);
            if (sp.shape == Shape::Accessory && sp.variant == static_cast<std::uint8_t>(Accessory::Broom))
                db.angVel = (rng_.uniform() < 0.5f ? -1.0f : 1.0f) * rng_.range(2.0f, 4.0f);
        }
        (void)big;
        debris_.push_back(db);
    }
}

void ObjectSystem::knock(std::size_t index, const Surface& s, int dir, float speed, KnockBy by, ParticleSystem* particles) {
    Pile& p = piles_[index];
    if (p.knock != KnockBy::None) return;
    if (sculpt_.active && sculpt_.pile == p.id) sculpt_.active = false;
    p.knock = by;
    p.knockDir = static_cast<std::int8_t>(dir >= 0 ? 1 : -1);
    p.knockSpeed = std::max(speed, 40.0f * p.dpi);
    p.knockT = p.knockPush = p.squash = 0;
    const float strength = std::clamp(p.knockSpeed / (150.0f * p.dpi), 0.5f, 3.0f);
    const PileGeom g = pileGeom(p, s);
    if (!p.bare && p.n >= 2) {   // decorations fly off at first contact
        std::vector<SpriteInstance> parts;
        appendStackSprites(g, p.tilt, 0, 0, parts, p.look, waveArmAngle(p), 0, false);
        popDecorations(parts, dir, strength, p.dpi, 0);
    }
    p.bare = true;
    if (particles)   // a satisfying burst at the contact point
        particles->burst(g.cx - static_cast<float>(p.knockDir) * g.r[0], g.cy[0], g.r[0], 14, 120.0f * p.dpi * strength,
                         true, p.dpi);
}

bool ObjectSystem::stepKnock(std::size_t index, Surface& s, float dt, ParticleSystem* particles) {
    Pile& p = piles_[index];
    const float d = p.dpi;
    const float fdir = static_cast<float>(p.knockDir);
    p.knockT += dt;
    const float toppleT = kToppleSecondsAt150 * std::clamp(150.0f * d / p.knockSpeed, 0.35f, 1.4f);
    if (p.n > 1) {
        // The bottom is pushed out from under them: the stack tips forward ...
        p.tilt = fdir * kToppleAngle * std::min(1.0f, p.knockT / toppleT);
        if (p.knockT >= toppleT) {   // ... and comes apart into free balls flying / rolling on
            const PileGeom g = pileGeom(p, s);
            const float omega = kToppleAngle / toppleT;
            const float c = std::cos(p.tilt), sn = std::sin(p.tilt);
            for (int i = 1; i < p.n; ++i) {
                const float hx = 0, hy = g.cy[i] - g.base;   // relative to the base point
                Ball b;
                b.id = nextId_++;
                b.x = g.cx + hx * c - hy * sn;
                b.y = g.base + hx * sn + hy * c;
                b.r = g.r[i];
                b.dpi = d;
                b.collected = kPi * b.r * b.r / kPackFactor;
                const float lever = -hy;   // height above the pivot
                b.vx = fdir * (omega * lever + 0.6f * p.knockSpeed) + rng_.range(-20.0f, 20.0f) * d;
                b.vy = -rng_.range(40.0f, 120.0f) * d * std::clamp(p.knockSpeed / (150.0f * d), 0.5f, 2.0f);
                b.rotation = p.tilt;
                b.fragile = true;
                balls_.push_back(b);
            }
            p.n = 1;
            p.tilt = 0;
        }
    }
    if (p.knock != KnockBy::Plow) {   // shoved by a swing: slides on by its own momentum
        const float v = 0.5f * p.knockSpeed * std::exp(-4.0f * p.knockT);
        const float dx = fdir * v * dt;
        p.localX = std::clamp(p.localX + dx, 0.0f, s.width());
        p.knockPush += std::fabs(dx);
    }
    p.squash = std::min(1.0f, p.knockPush / (kCrushPushAt96 * d));
    const bool done = p.knockPush >= kCrushPushAt96 * d || (p.knock != KnockBy::Plow && p.knockT > 0.5f);
    if (!done || p.n > 1) return false;
    // The base crumbles: pieces tumble ahead, a puff, the rest goes to the plow heap / shovel.
    const PileGeom g = pileGeom(p, s);
    const float m = kPi * p.r[0] * p.r[0] / kPackFactor;
    float pieces = m * kCrumblePieces;
    const float dust = m * kCrumbleDust;
    float rest = m - pieces - dust;
    if (p.knock == KnockBy::Hit) {   // nothing to catch it: all of it tumbles as pieces
        pieces += rest;
        rest = 0;
    }
    if (chunks_ && particles) {
        const int n = 3 + static_cast<int>(rng_.below(3));
        for (int i = 0; i < n; ++i) {
            const float pm = pieces / static_cast<float>(n);
            const float hw = std::clamp(0.6f * std::sqrt(pm), 4.0f * d, 0.7f * p.r[0]);
            Chunk* c = chunks_->spawnFree(g.cx + fdir * rng_.range(0.0f, 0.6f) * p.r[0], g.cy[0] - rng_.range(0.0f, 0.6f) * p.r[0],
                                          hw, std::max(3.0f * d, 0.6f * hw), pm,
                                          fdir * (p.knockSpeed * rng_.range(0.6f, 1.3f) + rng_.range(10.0f, 60.0f) * d),
                                          -rng_.range(60.0f, 200.0f) * d, d, 0);
            if (c) {
                c->soft = true;
                c->angVel = rng_.range(-5.0f, 5.0f);
            }
        }
        particles->burst(g.cx, g.cy[0], g.r[0], 8, 90.0f * d, true, d);
    } else {
        rest += pieces;
    }
    crumbleDust_ += dust;
    pending_[static_cast<int>(p.knock)] += rest;
    ++crumbled_;
    piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void ObjectSystem::shatter(const Ball& b, SurfaceRegistry& reg, const SurfaceHit& hit, ParticleSystem* particles) {
    ++shattered_;
    if (chunks_ && particles) {
        Chunk c;
        c.x = hit.x;
        c.y = hit.y - 0.6f * b.r;
        c.vx = b.vx;
        c.vy = b.vy;
        c.halfW = b.r;
        c.halfH = std::max(3.0f * b.dpi, 0.6f * b.r);
        c.mass = b.collected;
        c.dpi = b.dpi;
        c.rotation = b.rotation;
        c.exclude = b.exclude;
        chunks_->shatterAt(c, reg, hit.surfaceIndex, hit.x, hit.y, particles);
    } else {
        reg.deposit(hit, 0.5f * b.collected);   // animations off: no pieces, half of it stays
    }
}

void ObjectSystem::stepDebris(float dt, const SurfaceRegistry& reg) {
    const RectI prim = reg.sceneBounds();
    for (std::size_t k = 0; k < debris_.size();) {
        Debris& db = debris_[k];
        bool remove = false;
        if (db.fade >= 0) {
            db.fade -= dt;
            db.rotation += db.angVel * dt;   // googly eyes keep jiggling a moment
            db.angVel *= std::exp(-4.0f * dt);
            remove = db.fade <= 0;
        } else {
            const float y0 = db.y + db.halfH;
            db.vy += kBallGravityAt96 * db.dpi * db.gravity * dt;
            if (db.sway > 0) {   // scarf: flutters
                db.swayPhase += 6.0f * dt;
                db.vx = db.baseVx * std::exp(-1.5f * db.swayPhase / 6.0f) + db.sway * std::sin(db.swayPhase);
                db.vy = std::min(db.vy, 160.0f * db.dpi);
            }
            db.x += db.vx * dt;
            db.y += db.vy * dt;
            db.rotation += db.angVel * dt;
            SurfaceHit hit;
            if (reg.sweep(db.x, y0, db.x, db.y + db.halfH, true, hit, db.exclude)) {
                db.y = hit.y - db.halfH;
                if (db.bounces > 0 && db.vy > 120.0f * db.dpi) {   // one bounce
                    --db.bounces;
                    db.vy = -0.35f * db.vy;
                    db.vx *= 0.6f;
                    db.angVel *= 0.5f;
                } else {
                    db.vx = db.vy = 0;
                    if (db.shape == Shape::Stick) {   // sticks lie down on their side
                        db.rotation = std::cos(db.rotation) >= 0 ? 0.0f : kPi;
                    } else if (db.shape == Shape::Accessory && db.variant == static_cast<std::uint8_t>(Accessory::Broom)) {
                        db.rotation = std::sin(db.rotation) >= 0 ? 0.5f * kPi : -0.5f * kPi;
                    }
                    if (!(db.shape == Shape::Accessory && db.variant == static_cast<std::uint8_t>(Accessory::GooglyEye)))
                        db.angVel = 0;
                    db.fade = kDebrisFadeSeconds;
                }
            } else if (prim.empty() || db.y > static_cast<float>(prim.b) + 100.0f ||
                       db.x < static_cast<float>(prim.l) - 100.0f || db.x > static_cast<float>(prim.r) + 100.0f) {
                remove = true;
            }
        }
        if (remove) debris_.erase(debris_.begin() + static_cast<std::ptrdiff_t>(k));
        else ++k;
    }
}

bool ObjectSystem::stackable(const Pile& p, float radius) const {
    if (p.n <= 0 || p.n >= 3) return false;
    const float below = p.r[p.n - 1];
    return radius >= kStackMinRatio * below && radius <= kStackMaxRatio * below;
}

Pile& ObjectSystem::addPile(const Surface& s, float localX, float radius) {
    piles_.emplace_back();
    Pile& p = piles_.back();
    p.id = nextId_++;
    p.surface = s.id;
    p.localX = localX;
    p.sink = s.hm.heightAtLocalX(localX);
    p.dpi = s.dpiScale;
    p.n = 1;
    p.r[0] = radius;
    return p;
}

bool ObjectSystem::grabAt(float x, float y, const SurfaceRegistry& reg) {
    if (holding()) return true;
    for (std::size_t k = balls_.size(); k-- > 0;) {
        Ball& b = balls_[k];
        if (len(b.x - x, b.y - y) <= b.r + kGrabPad) {
            b.held = true;
            b.rolling = 0;
            b.x = x;
            b.y = y;
            b.vx = b.vy = 0;
            return true;
        }
    }
    for (std::size_t k = piles_.size(); k-- > 0;) {
        Pile& p = piles_[k];
        const Surface* s = reg.find(p.surface);
        if (!s || !s->active) continue;
        const PileGeom g = pileGeom(p, *s);
        bool hitAny = false;
        for (int i = 0; i < g.n; ++i) hitAny = hitAny || len(g.cx - x, g.cy[i] - y) <= g.r[i] + kGrabPad;
        if (!hitAny) continue;
        Ball b;
        b.id = nextId_++;
        b.r = p.r[p.n - 1];
        b.collected = kPi * b.r * b.r / kPackFactor;
        b.dpi = p.dpi;
        b.x = x;
        b.y = y;
        b.held = true;
        if (--p.n == 0) piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
        else if (p.n < 2) p.autoSpawned = false;
        balls_.push_back(b);
        return true;
    }
    return false;
}

void ObjectSystem::startBall(float x, float y, float dpiScale) {
    if (holding()) return;
    Ball b;
    b.id = nextId_++;
    b.dpi = dpiScale;
    b.r = radiusFor(0, dpiScale);
    b.x = x;
    b.y = y;
    b.held = true;
    balls_.push_back(b);
}

void ObjectSystem::moveHeld(float x0, float y0, float x1, float y1, SurfaceRegistry& reg) {
    const int hi = heldIndex();
    if (hi < 0) return;
    Ball& b = balls_[static_cast<std::size_t>(hi)];
    const float rMax = 0.5f * kBallMaxDiameterAt96 * b.dpi;
    const float capacity = kPi * rMax * rMax / kPackFactor;
    const float dist = len(x1 - x0, y1 - y0);
    const int n = std::clamp(static_cast<int>(std::ceil(dist / std::max(2.0f, b.r * 0.5f))), 1, 256);
    for (int k = 1; k <= n; ++k) {
        const float room = capacity - b.collected;
        if (room <= 0) break;
        const float t = static_cast<float>(k) / static_cast<float>(n);
        const float px = x0 + (x1 - x0) * t, py = y0 + (y1 - y0) * t;
        if (!reg.inObjectArea(px, py)) break;   // snowballs only gather snow in the desktop layer
        // The ball packs the snow it rolls over: everything inside its outline (+ a small rim).
        b.collected += brushDisc(reg, px, py, b.r + 3.0f * b.dpi, room, nullptr);
        b.r = radiusFor(b.collected, b.dpi);
    }
    if (icicles_) {   // dragged through icicles: they snap, gently
        b.vx = (x1 - x0) * 60.0f;
        b.vy = (y1 - y0) * 60.0f;
        ballIcicles(b, x0, y0, x1, y1, 0.0f, 0.3f, reg, nullptr);
        b.vx = b.vy = 0;
    }
    b.rotation += (x1 - x0) / std::max(b.r, 1.0f);
    b.x = x1;
    b.y = y1;
}

void ObjectSystem::puff(const Ball& b, ParticleSystem* particles) {
    if (particles && b.collected > 0.5f)
        particles->burst(b.x, b.y, b.r, std::clamp(static_cast<int>(b.collected / 8.0f), 2, 14), 70.0f * b.dpi, true, b.dpi);
}

void ObjectSystem::ballIcicles(Ball& b, float x0, float y0, float x1, float y1, float minSpeed, float knockShare,
                               const SurfaceRegistry& reg, ParticleSystem* particles) {
    if (!icicles_) return;
    bool bumped = false;
    const int n = icicles_->hitSegment(reg, x0, y0, x1, y1, b.r, b.vx * knockShare, b.vy * knockShare, minSpeed, particles,
                                       &bumped);
    for (int i = 0; i < n; ++i) {
        const float k = 1.0f - rng_.range(0.2f, 0.35f);
        b.vx *= k;
        b.vy *= k;
    }
    if (n == 0 && bumped && !b.held) {   // a slow ball just bumps off
        b.vx *= -0.3f;
        b.vy *= 0.5f;
    }
}

void ObjectSystem::release(float vx, float vy, const SurfaceRegistry& reg, ParticleSystem* particles) {
    const int hi = heldIndex();
    if (hi < 0) return;
    Ball& b = balls_[static_cast<std::size_t>(hi)];
    b.held = false;
    const float rMin = 0.5f * kBallMinDiameterAt96 * b.dpi;
    if (kPackFactor * b.collected < kBallMinFill * kPi * rMin * rMin) {
        // Not enough snow for a ball: it just crumbles.
        puff(b, particles);
        balls_.erase(balls_.begin() + hi);
        return;
    }
    // Dropped right onto a resting ball / snowman?
    for (Pile& p : piles_) {
        const Surface* s = reg.find(p.surface);
        if (!s || !s->active || !stackable(p, b.r)) continue;
        const PileGeom g = pileGeom(p, *s);
        const float tr = g.r[g.n - 1], ty = g.cy[g.n - 1];
        if (std::fabs(b.x - g.cx) < tr && b.y < ty + 0.5f * tr && len(b.x - g.cx, b.y - ty) < 1.2f * (tr + b.r)) {
            p.r[p.n++] = b.r;
            ensureLook(p);
            balls_.erase(balls_.begin() + hi);
            return;
        }
    }
    // Icicles along the first stretch of the throw break before it reaches a window pane.
    if (icicles_) {
        b.vx = vx;
        b.vy = vy;
        ballIcicles(b, b.x, b.y, b.x + vx * kSplatLookAhead, b.y + vy * kSplatLookAhead, kIcicleBreakSpeedAt96 * b.dpi, 1.0f,
                    reg, particles);
        vx = b.vx;
        vy = b.vy;
    }
    // Thrown hard at a window: it splats on the glass.
    if (splats_ && splats_->tryThrow(reg, b, vx, vy, particles)) {
        balls_.erase(balls_.begin() + hi);
        return;
    }
    const float vmax = kBallMaxThrowAt96 * b.dpi;
    const float v = len(vx, vy);
    const float k = v > vmax ? vmax / v : 1.0f;
    b.vx = vx * k;
    b.vy = vy * k;
    b.rolling = 0;
}

void ObjectSystem::breakPile(std::size_t index, const SurfaceRegistry& reg, ParticleSystem* particles) {
    const Pile& p = piles_[index];
    const Surface* s = reg.find(p.surface);
    if (s && s->active && particles) {
        const PileGeom g = pileGeom(p, *s);
        for (int i = 0; i < g.n; ++i) {
            const int count = std::clamp(static_cast<int>(kPi * g.r[i] * g.r[i] / (45.0f * p.dpi * p.dpi)), 4, 24);
            particles->burst(g.cx, g.cy[i], g.r[i], count, 170.0f * p.dpi, false, p.dpi);
        }
    }
    piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(index));
}

void ObjectSystem::breakPilesOn(SurfaceId id, const SurfaceRegistry& reg, ParticleSystem* particles) {
    for (std::size_t k = 0; k < piles_.size();) {
        if (piles_[k].surface == id) breakPile(k, reg, particles);
        else ++k;
    }
    for (Ball& b : balls_)
        if (b.rolling == id) b.rolling = 0;
}

void ObjectSystem::settle(Ball& b, const Surface& s) {
    Pile& p = addPile(s, b.x - static_cast<float>(s.left), b.r);
    p.dpi = b.dpi;
}

ObjectSystem::Contact ObjectSystem::pileContact(Ball& b, float x1, float y1, const SurfaceRegistry& reg,
                                                ParticleSystem* particles) {
    for (std::size_t k = 0; k < piles_.size(); ++k) {
        Pile& p = piles_[k];
        const Surface* s = reg.find(p.surface);
        if (!s || !s->active) continue;
        const PileGeom g = pileGeom(p, *s);
        for (int i = g.n - 1; i >= 0; --i) {
            const float dx = x1 - g.cx, dy = y1 - g.cy[i];
            const float d = len(dx, dy);
            if (d >= b.r + g.r[i]) continue;
            if (len(b.vx, b.vy) > kPileBreakSpeedAt96 * b.dpi) {
                knock(k, *s, b.vx >= 0 ? 1 : -1, len(b.vx, b.vy) * 0.3f, KnockBy::Hit, particles);
                b.vx *= 0.5f;
                b.vy *= 0.5f;
                return Contact::None;
            }
            if (i == g.n - 1 && b.vy >= 0 && dy < 0 && std::fabs(dx) < 0.9f * g.r[i] && stackable(p, b.r)) {
                p.r[p.n++] = b.r;
                ensureLook(p);
                return Contact::Consumed;
            }
            // Glance off sideways.
            const float dir = dx >= 0 ? 1.0f : -1.0f;
            b.vx = dir * std::max(std::fabs(b.vx) * 0.5f, 90.0f * b.dpi);
            b.vy = -0.25f * std::fabs(b.vy);
            const float inv = d > 1e-3f ? 1.0f / d : 0.0f;
            const float push = b.r + g.r[i] + 0.5f;
            b.x = g.cx + (inv > 0 ? dx * inv : dir) * push;
            b.y = g.cy[i] + (inv > 0 ? dy * inv : 0.0f) * push;
            return Contact::Deflected;
        }
    }
    return Contact::None;
}

void ObjectSystem::dropSnowman(const Surface& target, float localX, int n, const float* radii, float startBottomY,
                               float dpi, Rng& rng) {
    FallingSnowman f;
    f.id = nextId_++;
    f.n = std::clamp(n, 1, 3);
    for (int i = 0; i < f.n; ++i) f.r[i] = radii[i];
    f.target = target.id;
    f.targetLocalX = localX;
    f.x = static_cast<float>(target.left) + localX;
    f.y = f.startY = startBottomY;
    f.dpi = dpi;
    f.swayAmp = rng.range(10.0f, 26.0f) * dpi;
    f.swayPhase = rng.range(0.0f, 6.2831853f);
    if (f.n >= 2) f.look = rollLook();
    falling_.push_back(f);
}

void ObjectSystem::land(FallingSnowman& f, Surface& s, int column, ParticleSystem* particles) {
    const float localX = static_cast<float>(column) * s.hm.step();
    lastCrater_ = makeCrater(s, localX, f.r[0]);
    Pile p;
    p.id = nextId_++;
    p.surface = s.id;
    p.localX = localX;
    p.sink = s.hm.height(column);
    p.dpi = f.dpi;
    p.tilt = f.tilt;
    p.autoSpawned = true;
    p.n = f.n;
    for (int i = 0; i < f.n; ++i) p.r[i] = f.r[i];
    p.look = f.look;
    piles_.push_back(p);
    lastLanded_ = p.id;
    if (!particles) return;
    // The landing lifts snow around it: clumps and dust splashing up and sideways.
    Rng& rng = particles->rng();
    const float d = f.dpi;
    const float baseX = static_cast<float>(s.left) + localX, baseY = s.topAt(column);
    const float splash = std::max(0.0f, lastCrater_.removed - lastCrater_.rims);
    const int clumps = std::clamp(static_cast<int>(splash / (10.0f * d * d)), 8, 40);
    for (int i = 0; i < clumps; ++i) {
        const float side = rng.uniform() < 0.5f ? -1.0f : 1.0f;
        particles->addLoose(baseX + side * rng.range(0.6f, 1.2f) * f.r[0], baseY - rng.range(0.0f, 4.0f) * d,
                            side * rng.range(60.0f, 260.0f) * d, -rng.range(120.0f, 380.0f) * d,
                            rng.range(1.4f, 3.0f) * d);
    }
    for (int i = 0; i < 18; ++i) {
        const float side = rng.uniform() < 0.5f ? -1.0f : 1.0f;
        particles->addDust(baseX + side * rng.range(0.3f, 1.3f) * f.r[0], baseY - rng.range(0.0f, 6.0f) * d,
                           side * rng.range(40.0f, 220.0f) * d, -rng.range(80.0f, 300.0f) * d,
                           rng.range(0.9f, 2.0f) * d, rng.range(0.5f, 1.2f));
    }
}

void ObjectSystem::stepFalling(float dt, SurfaceRegistry& reg, ParticleSystem* particles) {
    const RectI prim = reg.sceneBounds();
    for (std::size_t k = 0; k < falling_.size();) {
        FallingSnowman& f = falling_[k];
        Surface* s = f.target ? reg.find(f.target) : nullptr;
        if (s && !s->active) {
            s = nullptr;
            f.target = 0;
        }
        const float y0 = f.y;
        f.vy = std::min(f.vy + kFallGravityAt96 * f.dpi * dt, kFallMaxSpeedAt96 * f.dpi);
        f.y += f.vy * dt;
        f.swayPhase += 2.4f * dt;
        bool remove = false;
        if (s) {
            const int col = s->hm.columnAt(f.targetLocalX);
            const float ty = s->topAt(col);
            const float progress = std::clamp((ty - f.y) / std::max(ty - f.startY, 1.0f), 0.0f, 1.0f);
            f.x = static_cast<float>(s->left) + f.targetLocalX + f.swayAmp * std::sin(f.swayPhase) * progress;
            f.tilt = 0.3f * std::sin(0.8f * f.swayPhase + 1.0f) * progress;
            if (f.y >= ty) {   // lands even if a window now covers the spot (desktop layer)
                f.y = ty;
                land(f, *s, col, particles);
                remove = true;
            }
        } else {
            SurfaceHit hit;
            if (reg.sweep(f.x, y0, f.x, f.y, true, hit)) {
                Surface& hs = reg.surfaces()[static_cast<std::size_t>(hit.surfaceIndex)];
                land(f, hs, hit.column, particles);
                remove = true;
            } else if (prim.empty() || f.y > static_cast<float>(prim.b) + 300.0f) {
                remove = true;
            }
        }
        if (remove) falling_.erase(falling_.begin() + static_cast<std::ptrdiff_t>(k));
        else ++k;
    }
}

void ObjectSystem::step(float dt, SurfaceRegistry& reg, ParticleSystem* particles) {
    if (dt <= 0) return;
    const RectI scene = reg.sceneBounds();
    // Objects exist only in the desktop layer: piles whose surface left it break apart.
    for (std::size_t k = 0; k < piles_.size();) {
        Pile& p = piles_[k];
        const Surface* s = reg.find(p.surface);
        if (s && s->active) {
            const PileGeom g = pileGeom(p, *s);
            const bool on = reg.inObjectArea(g.cx, g.base - 1.0f);
            if (!on) {
                if (particles) {
                    for (int i = 0; i < g.n; ++i)
                        particles->burst(g.cx, g.cy[i], g.r[i], 10, 80.0f * p.dpi, true, p.dpi);
                }
                piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
        }
        if (p.knock != KnockBy::None && s && s->active) {
            if (stepKnock(k, *reg.find(p.surface), dt, particles)) continue;
            ++k;
            continue;
        }
        p.tilt *= std::exp(-kTiltRecovery * dt);
        if (std::fabs(p.tilt) < 1e-3f) p.tilt = 0;
        if (p.look.googly && s && s->active) {   // googly pupils jiggle when the snowman is moved
            const float cx = static_cast<float>(s->left) + p.localX;
            if (p.lastCx > -1e29f) p.jiggle = std::min(1.2f, p.jiggle + std::fabs(cx - p.lastCx) * 0.05f / p.dpi);
            p.lastCx = cx;
            p.jigPhase += 14.0f * dt;
            p.jiggle *= std::exp(-2.0f * dt);
        }
        if (p.look.arms == 2) {   // a friendly wave every now and then
            if (p.waveT > 0) {   // 2-3 swings, then a pause of 4-8 s
                p.waveT -= dt;
                if (p.waveT <= 0) {
                    p.waveT = 0;
                    p.waveIn = rng_.range(4.0f, 8.0f);
                }
            } else if ((p.waveIn -= dt) <= 0) {
                p.wavePeriod = rng_.range(1.4f, 1.8f);
                p.waveDur = p.waveT = p.wavePeriod * static_cast<float>(2 + rng_.below(2));
            }
        }
        if (melt_ > 0 && s && s->active && !(sculpt_.active && sculpt_.pile == p.id)) {   // thaw: it slowly shrinks, then slumps into the drift
            const float dr = kMeltRadiusPerSecAt96 * p.dpi * melt_ * dt;
            for (int i = 0; i < p.n; ++i) p.r[i] = std::max(0.0f, p.r[i] - dr);
            if (p.r[0] < kCollapseRadiusAt96 * p.dpi || p.r[p.n - 1] < 2.5f * p.dpi) {
                Surface* ms = reg.find(p.surface);
                float area = 0;
                for (int i = 0; i < p.n; ++i) area += kPi * p.r[i] * p.r[i] / kPackFactor;
                const PileGeom g = pileGeom(p, *ms);
                const int parts = 5;
                for (int q = 0; q < parts; ++q) {
                    SurfaceHit h;
                    h.surfaceIndex = static_cast<int>(ms - reg.surfaces().data());
                    h.x = g.cx + (-1.0f + 2.0f * (static_cast<float>(q) + 0.5f) / parts) * p.r[0];
                    h.column = ms->hm.columnAt(h.x - static_cast<float>(ms->left));
                    h.y = ms->topAt(h.column);
                    reg.deposit(h, area / parts);
                }
                ++collapsed_;
                piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
        }
        ++k;
    }
    if (!falling_.empty()) stepFalling(dt, reg, particles);
    if (!debris_.empty()) stepDebris(dt, reg);
    if (balls_.empty()) return;
    for (std::size_t bi = 0; bi < balls_.size();) {
        Ball& b = balls_[bi];
        const bool inside = reg.inObjectArea(b.x, b.y - b.r, 800.0f * b.dpi);
        if (!inside) {   // dragged / rolled / thrown out of the desktop layer
            lostBalls_ += b.collected;
            if (b.held || b.y - b.r <= static_cast<float>(scene.b)) puff(b, particles);
            balls_.erase(balls_.begin() + static_cast<std::ptrdiff_t>(bi));
            continue;
        }
        if (b.held) {
            ++bi;
            continue;
        }
        bool remove = false;
        if (b.rolling != 0) {
            const Surface* s = reg.find(b.rolling);
            if (!s || !s->active) {
                b.rolling = 0;
            } else {
                b.x += b.vx * dt;
                b.rotation += b.vx * dt / std::max(b.r, 1.0f);
                b.vx *= std::exp(-kBallRollFriction * dt);
                const float lx = b.x - static_cast<float>(s->left);
                const int col = s->hm.columnAt(lx);
                if (lx < 0 || lx > s->width() || s->hm.blocked(col)) {
                    b.rolling = 0;   // rolled off the edge (or behind another window): fall
                } else {
                    b.y = s->topAt(col) - b.r;
                    b.vy = 0;
                    for (const Pile& p : piles_) {   // bump into something standing there
                        if (p.surface != s->id) continue;
                        const float gap = (static_cast<float>(s->left) + p.localX) - b.x;
                        if (std::fabs(gap) < 0.9f * (p.r[0] + b.r) && gap * b.vx > 0) b.vx = 0;
                    }
                    if (std::fabs(b.vx) < kBallRestSpeedAt96 * b.dpi) {
                        settle(b, *s);
                        remove = true;
                    }
                }
            }
        } else {
            b.vy += kBallGravityAt96 * b.dpi * dt;
            const float travel = len(b.vx, b.vy) * dt;
            const int n = std::clamp(static_cast<int>(std::ceil(travel / std::max(2.0f, 0.5f * b.r))), 1, 16);
            const float h = dt / static_cast<float>(n);
            for (int k = 0; k < n && !remove; ++k) {
                const float x0 = b.x, y0 = b.y;
                const float x1 = x0 + b.vx * h, y1 = y0 + b.vy * h;
                const Contact c = pileContact(b, x1, y1, reg, particles);
                if (c == Contact::Consumed) {
                    remove = true;
                    break;
                }
                if (c == Contact::Deflected) continue;
                if (icicles_) {
                    const float before = b.vx;
                    ballIcicles(b, x0, y0, x1, y1, kIcicleBreakSpeedAt96 * b.dpi, 1.0f, reg, particles);
                    if (b.vx * before < 0) continue;   // bumped back
                }
                SurfaceHit hit;
                if (b.vy > 0 && reg.sweep(x0, y0 + b.r, x1, y1 + b.r, true, hit, b.exclude)) {
                    b.x = hit.x;
                    b.y = hit.y - b.r;
                    if (b.vy > (b.fragile ? kToppleShatterSpeedAt96 : kBallShatterSpeedAt96) * b.dpi) {   // hard impact: it breaks apart
                        shatter(b, reg, hit, particles);
                        remove = true;
                        break;
                    }
                    b.exclude = 0;
                    if (b.vy > kBallBounceSpeedAt96 * b.dpi) {
                        b.vy = -b.vy * kBallRestitution;
                        b.vx *= 0.7f;
                    } else {
                        b.vy = 0;
                        b.rolling = reg.surfaces()[static_cast<std::size_t>(hit.surfaceIndex)].id;
                    }
                    break;
                }
                b.x = x1;
                b.y = y1;
            }
            b.rotation += b.vx * dt / std::max(b.r, 1.0f);
        }
        if (remove) {
            balls_.erase(balls_.begin() + static_cast<std::ptrdiff_t>(bi));
        } else {
            ++bi;
        }
    }
}

void ObjectSystem::onSurfacesRemoved(const std::vector<Surface>& removed, ParticleSystem* /*particles*/, bool drop) {
    for (const Surface& s : removed) {
        if (drop && s.active) detachPilesOn(s, 1.0f, 0, 0, 0, 0);   // the window is gone: they fall
        for (std::size_t k = 0; k < piles_.size();) {
            if (piles_[k].surface == s.id) piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
            else ++k;
        }
        for (Ball& b : balls_)
            if (b.rolling == s.id) b.rolling = 0;
        for (FallingSnowman& f : falling_)
            if (f.target == s.id) f.target = 0;
    }
}

void ObjectSystem::onSurfaceMoved(const LayoutEvents::Moved& ev, bool teleport, const SurfaceRegistry& reg,
                                  ParticleSystem* particles) {
    for (Ball& b : balls_) {
        if (b.rolling == ev.id) {
            b.x += static_cast<float>(ev.dx);
            b.y += static_cast<float>(ev.dy);
        }
    }
    if (teleport) return;
    for (std::size_t k = 0; k < piles_.size();) {
        if (piles_[k].surface == ev.id && ev.speed > kSurfaceBreakSpeedAt96 * piles_[k].dpi) breakPile(k, reg, particles);
        else ++k;
    }
}

void ObjectSystem::onSurfaceResized(const Surface& s) {
    for (std::size_t k = 0; k < piles_.size();) {
        const Pile& p = piles_[k];
        if (p.surface != s.id || p.localX <= s.width()) {
            ++k;
            continue;
        }
        // The edge under it is gone: the balls fall.
        const PileGeom g = pileGeom(p, s);
        for (int i = 0; i < g.n; ++i) {
            Ball b;
            b.id = nextId_++;
            b.x = g.cx;
            b.y = g.cy[i];
            b.r = g.r[i];
            b.dpi = p.dpi;
            b.collected = kPi * b.r * b.r / kPackFactor;
            balls_.push_back(b);
        }
        piles_.erase(piles_.begin() + static_cast<std::ptrdiff_t>(k));
    }
    for (FallingSnowman& f : falling_)
        if (f.target == s.id && f.targetLocalX > s.width()) f.target = 0;
}

void ObjectSystem::onSurfaceRescaled(SurfaceId id, float ratio) {
    for (Pile& p : piles_) {
        if (p.surface != id) continue;
        p.localX *= ratio;
        p.sink *= ratio;
        p.dpi *= ratio;
        for (float& r : p.r) r *= ratio;
    }
    for (FallingSnowman& f : falling_)
        if (f.target == id) f.targetLocalX *= ratio;
}

void ObjectSystem::appendSprites(const SurfaceRegistry& reg, std::vector<SpriteInstance>& out) const {
    for (const Pile& p : piles_) {
        const Surface* s = reg.find(p.surface);
        if (!s || !s->active) continue;
        const float wave = waveArmAngle(p);
        const std::size_t base = out.size();
        appendStackSprites(pileGeom(p, *s), p.tilt, 0, 0, out, p.look, wave, p.jiggle * std::sin(p.jigPhase), p.bare);
        if (p.squash > 0 && base < out.size()) {   // the shoved base squashes against the blade
            SpriteInstance& b0 = out[base];
            const float r = b0.halfH;
            b0.halfW = r * (1.0f + 0.25f * p.squash);
            b0.halfH = r * (1.0f - 0.2f * p.squash);
            b0.y += r - b0.halfH;
        }
    }
    for (const FallingSnowman& f : falling_)
        appendStackSprites(stackGeom(f.x, f.y, f.r, f.n), f.tilt, 0, f.vy, out, f.look, kArmRestAngle,
                           0.9f * std::sin(3.0f * f.swayPhase));
    for (const Ball& b : balls_) {
        SpriteInstance sp = makeSprite(Shape::Snowball, b.x, b.y, b.r, b.r, b.rotation);
        if (!b.held) {
            sp.vx = b.vx;
            sp.vy = b.vy;
        }
        out.push_back(sp);
    }
    for (const Debris& db : debris_) {
        SpriteInstance sp = makeSprite(db.shape, db.x, db.y, db.halfW, db.halfH, db.rotation);
        sp.variant = db.variant;
        sp.vx = db.vx;
        sp.vy = db.vy;
        if (db.fade >= 0) sp.opacity = std::clamp(db.fade / kDebrisFadeSeconds, 0.0f, 1.0f);
        out.push_back(sp);
    }
}

void ObjectSystem::appendHitRects(const SurfaceRegistry& reg, std::vector<RectI>& out) const {
    const std::vector<RectI>& clip = reg.desktopClip();
    for (const Pile& p : piles_) {
        const Surface* s = reg.find(p.surface);
        if (!s || !s->active) continue;
        appendClipped(geomBox(pileGeom(p, *s), false), clip, out);   // arms are not grabbable
    }
    for (const Ball& b : balls_) {
        if (b.held) continue;
        const RectI box{static_cast<int>(std::floor(b.x - b.r)) - 2, static_cast<int>(std::floor(b.y - b.r)) - 2,
                        static_cast<int>(std::ceil(b.x + b.r)) + 2, static_cast<int>(std::ceil(b.y + b.r)) + 2};
        appendClipped(box, clip, out);
    }
}

}  // namespace snow
