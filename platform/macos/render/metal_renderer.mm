#include "render/metal_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "diagnostics/log.h"
#include "render/msl_translate.h"
// Portable tessellation shared with the Windows renderer (no D3D dependencies).
#include "../../windows/rendering/d3d11/drift_mesh.h"

namespace snow::mac {
namespace {

static_assert(sizeof(SpriteInstance) == 36, "SpriteInstance is uploaded verbatim (9 x 4 bytes)");
static_assert(offsetof(SpriteInstance, shape) == 32 && offsetof(SpriteInstance, variant) == 33,
              "SpriteInstance layout changed: update sf_sprite_vertex in msl_translate.cpp");
static_assert(sizeof(DriftVertex) == 36, "DriftVertex is uploaded verbatim (9 floats)");

// Mirrors SFFrame in msl_translate.cpp.
struct FrameConstants {
    float originX, originY;   // pixels
    float invHalfX, invHalfY;
    float dt;
    float opacity;
    float scale;
    float pad;
};
static_assert(sizeof(FrameConstants) == 32, "FrameConstants must match SFFrame");

constexpr int kSlots = 3;

struct Range {
    NSUInteger start = 0, count = 0;
};

struct Slot {
    id<MTLBuffer> inst;
    std::size_t instCap = 0;
    id<MTLBuffer> drift;
    std::size_t driftCap = 0;
    std::uint64_t driftGen = 0;   // DriftMeshCache generation copied into `drift`
    NSUInteger driftCount = 0;
    std::vector<Range> objects;
    std::vector<RectI> objectBounds;
    Range particles, cursor;
    std::shared_ptr<const FrameSnapshot> snap;
};

void logLong(const char* prefix, NSString* text) {
    const char* s = text.UTF8String ?: "";
    const std::size_t n = std::strlen(s);
    for (std::size_t at = 0; at < n && at < 8000; at += 900) {
        SNOW_LOGE("%s%.*s", prefix, static_cast<int>(std::min<std::size_t>(900, n - at)), s + at);
    }
}

bool cursorBadged(Shape s) { return s == Shape::Mitten || s == Shape::Shovel || s == Shape::SnowChunk; }

}  // namespace

struct MetalRenderer::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> spritePso, driftPso;
    dispatch_semaphore_t inflight = nullptr;
    Slot slots[kSlots];
    int cur = -1;                       // slot of the last prepared snapshot
    DriftMeshCache driftCache;
    std::uint64_t driftGen = 0;
    std::shared_ptr<const FrameSnapshot> lastSnap;
    std::vector<SpriteInstance> cursorScratch;
    float badgeDx = 0, badgeDy = 0;
    bool ready = false;

    bool ensure(id<MTLBuffer> __strong& buf, std::size_t& cap, std::size_t need, std::size_t initial) {
        if (buf && cap >= need) return true;
        std::size_t n = std::max(cap, initial);
        while (n < need) n *= 2;
        buf = [device newBufferWithLength:n options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        cap = buf ? n : 0;
        if (!buf) SNOW_LOGE("render: newBuffer(%zu bytes) failed", n);
        return buf != nil;
    }

    id<MTLFunction> compile(ShaderStage stage, std::string* error) {
        const std::string src = buildMslLibrary(stage);
        MTLCompileOptions* opts = [MTLCompileOptions new];
        // Same results as the HLSL (the procedural noise hashes rely on precise sin/fract).
        if (@available(macOS 15.0, *)) {
            opts.mathMode = MTLMathModeSafe;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                                  options:opts
                                                    error:&err];
        if (!lib) {
            SNOW_LOGE("render: shader %s failed to compile", mslEntryPoint(stage));
            if (err) logLong("render:   ", err.localizedDescription);
            if (error) *error = std::string("shader ") + mslEntryPoint(stage) + " failed to compile";
            return nil;
        }
        id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:mslEntryPoint(stage)]];
        if (!fn && error) *error = std::string("missing entry point ") + mslEntryPoint(stage);
        return fn;
    }

    id<MTLRenderPipelineState> pipeline(id<MTLFunction> vs, id<MTLFunction> fs, const char* name, std::string* error) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.label = [NSString stringWithUTF8String:name];
        pd.vertexFunction = vs;
        pd.fragmentFunction = fs;
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
        ca.pixelFormat = MetalRenderer::pixelFormat();
        // Premultiplied alpha "over", identical to the D3D11 blend state.
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorOne;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        NSError* err = nil;
        id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) {
            SNOW_LOGE("render: pipeline %s failed", name);
            if (err) logLong("render:   ", err.localizedDescription);
            if (error) *error = std::string("pipeline ") + name + " failed";
        }
        return pso;
    }

    void upload(const FrameSnapshot& s, Slot& slot) {
        // Drifts: re-tessellated only for strips whose content changed; copied into this
        // slot whenever the slot's copy is older than the cache.
        if (driftCache.build(s.drifts)) ++driftGen;
        if (slot.driftGen != driftGen || (!slot.drift && !driftCache.vertices().empty())) {
            const std::vector<DriftVertex>& v = driftCache.vertices();
            slot.driftCount = 0;
            if (!v.empty() && ensure(slot.drift, slot.driftCap, v.size() * sizeof(DriftVertex), 64 * 1024)) {
                std::memcpy(slot.drift.contents, v.data(), v.size() * sizeof(DriftVertex));
                slot.driftCount = v.size();
            }
            slot.driftGen = driftGen;
        }

        std::size_t total = s.particles.sprites.size() + s.cursor.sprites.size();
        for (const SpriteBatch& b : s.objects) total += b.sprites.size();
        slot.objects.assign(s.objects.size(), Range{});
        slot.objectBounds.assign(s.objects.size(), RectI{});
        for (std::size_t i = 0; i < s.objects.size(); ++i) {
            // Conservative bounds incl. rotation, AA padding and 50 ms of extrapolation, so
            // clip rects that cannot touch the batch cost no draw call.
            float l = 1e30f, t = 1e30f, r = -1e30f, b = -1e30f;
            for (const SpriteInstance& sp : s.objects[i].sprites) {
                const float ext = std::sqrt(sp.halfW * sp.halfW + sp.halfH * sp.halfH) + 3.0f;
                const float ex = ext + std::fabs(sp.vx) * 0.05f, ey = ext + std::fabs(sp.vy) * 0.05f;
                l = std::min(l, sp.x - ex);
                r = std::max(r, sp.x + ex);
                t = std::min(t, sp.y - ey);
                b = std::max(b, sp.y + ey);
            }
            if (r > l && b > t && std::isfinite(l) && std::isfinite(r) && std::isfinite(t) && std::isfinite(b)) {
                auto clampI = [](float v) { return static_cast<int>(std::clamp(v, -1e9f, 1e9f)); };
                slot.objectBounds[i] =
                    RectI{clampI(std::floor(l)), clampI(std::floor(t)), clampI(std::ceil(r)) + 1, clampI(std::ceil(b)) + 1};
            }
        }
        slot.particles = slot.cursor = Range{};
        slot.snap.reset();
        if (total == 0) return;
        if (!ensure(slot.inst, slot.instCap, total * sizeof(SpriteInstance), 4096 * sizeof(SpriteInstance))) return;
        auto* dst = static_cast<std::uint8_t*>(slot.inst.contents);
        NSUInteger at = 0;
        auto put = [&](const std::vector<SpriteInstance>& v) {
            Range rg{at, v.size()};
            if (!v.empty())
                std::memcpy(dst + at * sizeof(SpriteInstance), v.data(), v.size() * sizeof(SpriteInstance));
            at += rg.count;
            return rg;
        };
        for (std::size_t i = 0; i < s.objects.size(); ++i) slot.objects[i] = put(s.objects[i].sprites);
        slot.particles = put(s.particles.sprites);
        if ((badgeDx != 0 || badgeDy != 0) && !s.cursor.sprites.empty()) {
            // The held-ball marker (ring + ball centred on the pointer) stays where it is; the
            // tool sprites move next to the visible system arrow.
            const bool hasRing = std::any_of(s.cursor.sprites.begin(), s.cursor.sprites.end(),
                                             [](const SpriteInstance& sp) { return sp.shape == Shape::Ring; });
            cursorScratch = s.cursor.sprites;
            for (SpriteInstance& sp : cursorScratch) {
                if (cursorBadged(sp.shape) || (sp.shape == Shape::Snowball && !hasRing)) {
                    sp.x += badgeDx;
                    sp.y += badgeDy;
                }
            }
            slot.cursor = put(cursorScratch);
        } else {
            slot.cursor = put(s.cursor.sprites);
        }
    }
};

MetalRenderer::MetalRenderer() : d_(std::make_unique<Impl>()) {}

MetalRenderer::~MetalRenderer() {
    // Wait for frames still on the GPU (their completion handlers signal the semaphore).
    // libdispatch aborts when a semaphore is released below its initial value: give back
    // every slot we took.
    if (d_->inflight) {
        int taken = 0;
        for (int i = 0; i < kSlots; ++i)
            if (dispatch_semaphore_wait(d_->inflight, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)) == 0) ++taken;
        for (int i = 0; i < taken; ++i) dispatch_semaphore_signal(d_->inflight);
    }
}

bool MetalRenderer::initialize(id<MTLDevice> device, std::string* error) {
    Impl& d = *d_;
    if (d.ready) return true;
    if (!device) {
        if (error) *error = "no Metal device";
        return false;
    }
    d.device = device;
    d.queue = [device newCommandQueue];
    if (!d.queue) {
        if (error) *error = "newCommandQueue failed";
        return false;
    }
    d.queue.label = @"SnowFall";
    id<MTLFunction> svs = d.compile(ShaderStage::SpriteVertex, error);
    id<MTLFunction> sfs = svs ? d.compile(ShaderStage::SpriteFragment, error) : nil;
    id<MTLFunction> dvs = sfs ? d.compile(ShaderStage::DriftVertex, error) : nil;
    id<MTLFunction> dfs = dvs ? d.compile(ShaderStage::DriftFragment, error) : nil;
    if (!dfs) return false;
    d.spritePso = d.pipeline(svs, sfs, "sprites", error);
    d.driftPso = d.spritePso ? d.pipeline(dvs, dfs, "drifts", error) : nil;
    if (!d.driftPso) return false;
    d.inflight = dispatch_semaphore_create(kSlots);
    d.ready = true;
    return true;
}

bool MetalRenderer::ready() const { return d_->ready; }
id<MTLDevice> MetalRenderer::device() const { return d_->device; }
id<MTLCommandQueue> MetalRenderer::queue() const { return d_->queue; }

void MetalRenderer::beginFrame() {
    if (d_->inflight) dispatch_semaphore_wait(d_->inflight, DISPATCH_TIME_FOREVER);
}

void MetalRenderer::endFrame(id<MTLCommandBuffer> cb) {
    dispatch_semaphore_t sem = d_->inflight;
    if (!sem) return;
    if (cb) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
          dispatch_semaphore_signal(sem);
        }];
    } else {
        dispatch_semaphore_signal(sem);
    }
}

void MetalRenderer::prepare(const std::shared_ptr<const FrameSnapshot>& snap) {
    Impl& d = *d_;
    if (!d.ready || snap == d.lastSnap) return;
    d.lastSnap = snap;
    // Every frame holds one semaphore slot, so the slot written here was last read by a
    // frame at least kSlots frames ago, which has completed (see beginFrame()).
    d.cur = (d.cur + 1) % kSlots;
    Slot& slot = d.slots[d.cur];
    if (snap) {
        d.upload(*snap, slot);
        slot.snap = snap;
    } else {
        slot.objects.clear();
        slot.objectBounds.clear();
        slot.particles = slot.cursor = Range{};
        slot.driftCount = 0;
        slot.driftGen = ~0ull;
        slot.snap.reset();
    }
}

void MetalRenderer::reset() {
    Impl& d = *d_;
    d.lastSnap.reset();
    d.driftCache.clear();
    ++d.driftGen;
}

void MetalRenderer::setCursorBadgeOffset(float dx, float dy) {
    d_->badgeDx = dx;
    d_->badgeDy = dy;
}

void MetalRenderer::encode(id<MTLRenderCommandEncoder> enc, const RenderTarget& t, float dt, float opacity) {
    Impl& d = *d_;
    if (!d.ready || d.cur < 0 || t.widthPx <= 0 || t.heightPx <= 0) return;
    const Slot& slot = d.slots[d.cur];
    const FrameSnapshot* snap = slot.snap.get();
    if (!snap) return;

    const float S = t.scale > 0 ? t.scale : 1.0f;
    FrameConstants fc{};
    fc.originX = static_cast<float>(t.bounds.l) * S;
    fc.originY = static_cast<float>(t.bounds.t) * S;
    fc.invHalfX = 2.0f / static_cast<float>(t.widthPx);
    fc.invHalfY = 2.0f / static_cast<float>(t.heightPx);
    fc.dt = dt;
    fc.opacity = opacity;
    fc.scale = S;

    MTLViewport vp{0, 0, static_cast<double>(t.widthPx), static_cast<double>(t.heightPx), 0, 1};
    [enc setViewport:vp];
    const MTLScissorRect full{0, 0, static_cast<NSUInteger>(t.widthPx), static_cast<NSUInteger>(t.heightPx)};
    [enc setScissorRect:full];
    [enc setVertexBytes:&fc length:sizeof fc atIndex:1];
    [enc setFragmentBytes:&fc length:sizeof fc atIndex:0];

    // Clip rect (points, global) -> scissor (pixels, target), clamped to the target.
    auto scissor = [&](const RectI& c, MTLScissorRect& out) {
        const double l = std::clamp(std::floor((c.l - t.bounds.l) * static_cast<double>(S)), 0.0, static_cast<double>(t.widthPx));
        const double tp = std::clamp(std::floor((c.t - t.bounds.t) * static_cast<double>(S)), 0.0, static_cast<double>(t.heightPx));
        const double r = std::clamp(std::ceil((c.r - t.bounds.l) * static_cast<double>(S)), 0.0, static_cast<double>(t.widthPx));
        const double b = std::clamp(std::ceil((c.b - t.bounds.t) * static_cast<double>(S)), 0.0, static_cast<double>(t.heightPx));
        if (!(r > l) || !(b > tp)) return false;
        out = MTLScissorRect{static_cast<NSUInteger>(l), static_cast<NSUInteger>(tp), static_cast<NSUInteger>(r - l),
                             static_cast<NSUInteger>(b - tp)};
        return true;
    };

    if (slot.driftCount && slot.drift) {
        [enc setRenderPipelineState:d.driftPso];
        [enc setVertexBuffer:slot.drift offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:slot.driftCount];
    }
    if (!slot.inst) return;
    [enc setRenderPipelineState:d.spritePso];
    [enc setVertexBuffer:slot.inst offset:0 atIndex:0];
    auto draw = [&](const Range& r) {
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:r.count baseInstance:r.start];
    };
    const std::size_t batches = std::min(slot.objects.size(), snap->objects.size());
    for (std::size_t i = 0; i < batches; ++i) {
        const Range r = slot.objects[i];
        if (!r.count) continue;
        const RectI area = i < slot.objectBounds.size() ? slot.objectBounds[i].intersect(t.bounds) : t.bounds;
        if (area.empty()) continue;
        const std::vector<RectI>& clip = snap->objects[i].clip;
        if (clip.empty()) {
            [enc setScissorRect:full];
            draw(r);
            continue;
        }
        for (const RectI& c : clip) {
            const RectI x = c.intersect(area);
            MTLScissorRect sc;
            if (x.empty() || !scissor(x, sc)) continue;
            [enc setScissorRect:sc];
            draw(r);
        }
    }
    [enc setScissorRect:full];
    if (slot.particles.count) {
        // Particles honour their clip too (excluded apps in "behind" mode); empty = unclipped.
        const std::vector<RectI>& clip = snap->particles.clip;
        if (clip.empty()) {
            draw(slot.particles);
        } else {
            for (const RectI& c : clip) {
                const RectI x = c.intersect(t.bounds);
                MTLScissorRect sc;
                if (x.empty() || !scissor(x, sc)) continue;
                [enc setScissorRect:sc];
                draw(slot.particles);
            }
            [enc setScissorRect:full];
        }
    }
    if (slot.cursor.count) draw(slot.cursor);
}

id<MTLDevice> pickMetalDevice() {
    id<MTLDevice> best = nil;
    NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
    for (id<MTLDevice> dev in all) {
        if (dev.isLowPower && !dev.isRemovable) {
            best = dev;
            break;
        }
    }
    if (!best) best = MTLCreateSystemDefaultDevice();
    if (!best && all.count) best = all.firstObject;
    return best;
}

}  // namespace snow::mac
