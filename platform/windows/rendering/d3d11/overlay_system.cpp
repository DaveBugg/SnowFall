// OverlaySystem implementation.
//
// Threads
//   UI thread      owns the overlay HWNDs (create / move / show / destroy) and publishes the
//                  desired overlay list ("targets") under a mutex.
//   render thread  owns EVERY D3D11 / DXGI / DirectComposition object. Between frames it
//                  applies the published target list (create / resize / release swap chains).
//
// Hand-off: the UI thread never blocks on the render thread except for one short handshake
// before DestroyWindow (the render thread must have released the swap chain and DComp target
// first). While it waits it keeps dispatching sent messages, so a cross-thread SendMessage
// from the render thread can never deadlock it.
#include "rendering/d3d11/overlay_system.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "diagnostics/log.h"
#include "rendering/d3d11/drift_mesh.h"
#include "rendering/d3d11/overlay_test_hooks.h"
#include "rendering/d3d11/shaders.h"

using Microsoft::WRL::ComPtr;

namespace snow {
namespace {

const wchar_t kClassName[] = L"SnowFallOverlay";
using Clock = std::chrono::steady_clock;

std::atomic<int> g_testLoss{0};
std::atomic<int> g_testFailAttempts{0};
std::atomic<unsigned long> g_lastDeviceRefs{~0ul};

static_assert(sizeof(SpriteInstance) == 36, "SpriteInstance is uploaded verbatim as instance data");
static_assert(offsetof(SpriteInstance, halfW) == 16 && offsetof(SpriteInstance, shape) == 32 &&
              offsetof(SpriteInstance, variant) == 33,
              "SpriteInstance layout changed: update the input layout");
static_assert(sizeof(DriftVertex) == 36, "DriftVertex layout changed: update the input layout");

struct FrameConstants {
    float originX, originY;
    float invHalfX, invHalfY;
    float dt;
    float opacity;
    float pad[2];
};

struct Target {
    std::uint64_t key = 0;
    HWND hwnd = nullptr;
    RectI bounds;
    float dpi = 1.0f;
};

struct GpuOverlay {
    std::uint64_t key = 0;
    HWND hwnd = nullptr;
    RectI bounds;
    float dpi = 1.0f;
    int w = 0, h = 0;   // current swap chain size
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11RenderTargetView> rtv;

    void releaseGpu() {
        rtv.Reset();
        if (target) target->SetRoot(nullptr);
        visual.Reset();
        target.Reset();
        swapChain.Reset();
        w = h = 0;
    }
};

struct Range {
    UINT start = 0, count = 0;
};

bool isDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// Waits for `h` while still dispatching messages SENT to this thread (never posted ones).
bool waitPumpingSent(HANDLE h, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD remaining = INFINITE;
        if (timeoutMs != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) return false;
            remaining = static_cast<DWORD>(timeoutMs - elapsed);
        }
        const DWORD r = MsgWaitForMultipleObjectsEx(1, &h, remaining, QS_SENDMESSAGE, 0);
        if (r == WAIT_OBJECT_0) return true;
        if (r == WAIT_OBJECT_0 + 1) {
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            continue;
        }
        return false;
    }
}

LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_CLOSE:
            return 0;   // only OverlaySystem destroys overlays
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

namespace render_test {
void forceDeviceLoss(int failRecreateAttempts) {
    g_testFailAttempts.store(std::max(failRecreateAttempts, 0));
    g_testLoss.store(1);
}
unsigned long lastDeviceRefCount() { return g_lastDeviceRefs.load(); }
}  // namespace render_test

struct OverlaySystem::Impl {
    // ---- set once in initialize() ------------------------------------------------------
    HINSTANCE inst = nullptr;
    const SnapshotExchange* source = nullptr;
    HWND notifyHwnd = nullptr;
    UINT failedMsg = 0;
    bool initialized = false;
    bool classRegistered = false;

    // ---- UI thread only ----------------------------------------------------------------
    struct UiOverlay {
        std::uint64_t key = 0;
        HWND hwnd = nullptr;
        MonitorInfo mon;
    };
    std::vector<UiOverlay> wins;
    std::uint64_t nextKey = 1;

    // ---- shared ------------------------------------------------------------------------
    mutable std::mutex mtx;            // guards targets / targetsGen / appliedGen
    std::vector<Target> targets;
    std::uint64_t targetsGen = 0;
    std::uint64_t appliedGen = 0;
    HANDLE wakeEvent = nullptr;        // auto-reset: something changed, render thread should look
    HANDLE appliedEvent = nullptr;     // manual: appliedGen == targetsGen
    HANDLE readyEvent = nullptr;       // manual: render thread finished device creation
    std::thread thread;
    bool initOk = false;
    std::atomic<bool> quit{false};
    std::atomic<bool> visibleFlag{true};
    std::atomic<bool> failed{false};
    std::atomic<int> fpsCap{60};
    std::atomic<float> opacity{1.0f};
    std::atomic<float> statFps{0.0f};
    std::atomic<float> statFrameMs{0.0f};
    std::atomic<int> statResets{0};
    std::atomic<int> statOverlays{0};
    std::atomic<long long> lastPresentMs{0};

    // ---- render thread only ------------------------------------------------------------
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<ID3DBlob> spriteVsBlob, spritePsBlob, driftVsBlob, driftPsBlob;   // survive device loss
    ComPtr<ID3D11VertexShader> spriteVs, driftVs;
    ComPtr<ID3D11PixelShader> spritePs, driftPs;
    ComPtr<ID3D11InputLayout> spriteLayout, driftLayout;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Buffer> instanceVb, driftVb;
    std::size_t instanceCap = 0, driftCap = 0;   // bytes
    std::vector<GpuOverlay> overlays;
    DriftMeshCache driftCache;
    std::shared_ptr<const FrameSnapshot> lastSnap;
    UINT driftVertexCount = 0;
    std::vector<Range> objectRanges;
    std::vector<RectI> objectBounds;   // conservative screen bounds of each objects batch
    Range particleRange, cursorRange;
    bool forceDraw = true;
    bool pendingLoss = false;
    bool failNotified = false;
    int quickLosses = 0;
    std::uint64_t framesSinceRecovery = 1000000;
    std::uint64_t totalFrames = 0;
    std::uint64_t envLossFrame = 0;
    HANDLE sleepTimer = nullptr;
    double refreshHz = 60.0;
    Clock::time_point refreshQueried{};

    // ====================================================================================
    // UI thread
    // ====================================================================================
    HWND createOverlayWindow(const RectI& b) {
        const DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                         WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        HWND hwnd = CreateWindowExW(ex, kClassName, L"", WS_POPUP, b.l, b.t, b.width(), b.height(), nullptr,
                                    nullptr, inst, nullptr);
        if (!hwnd) {
            SNOW_LOGE("overlay: CreateWindowEx failed, error=%lu", GetLastError());
            return nullptr;
        }
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &on, sizeof on);
        DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof on);
        return hwnd;
    }

    void publishTargets(bool waitUntilApplied) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            targets.clear();
            for (const UiOverlay& o : wins) targets.push_back(Target{o.key, o.hwnd, o.mon.bounds, o.mon.dpiScale});
            ++targetsGen;
            ResetEvent(appliedEvent);
        }
        SetEvent(wakeEvent);
        if (waitUntilApplied && !waitPumpingSent(appliedEvent, 3000)) {
            SNOW_LOGW("overlay: render thread did not confirm reconfiguration within 3 s");
        }
    }

    // ====================================================================================
    // render thread: device
    // ====================================================================================
    bool compile(const char* name, const char* body, const char* profile, ComPtr<ID3DBlob>& out) {
        if (out) return true;
        std::string src = std::string(shaders::kCommon) + body;
        ComPtr<ID3DBlob> err;
        const HRESULT hr = D3DCompile(src.data(), src.size(), name, nullptr, nullptr, "main", profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
        if (FAILED(hr)) {
            SNOW_LOGE("render: shader %s failed to compile hr=0x%08lx: %s", name, static_cast<unsigned long>(hr),
                      err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            out.Reset();
            return false;
        }
        return true;
    }

    bool createDevice() {
        if (g_testFailAttempts.load() > 0) {
            g_testFailAttempts.fetch_sub(1);
            SNOW_LOGW("render: device creation failed (forced by test hook)");
            return false;
        }
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
        const char* kind = "hardware";
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4,
                                       D3D11_SDK_VERSION, &device, &fl, &context);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 3,
                                   D3D11_SDK_VERSION, &device, &fl, &context);
        }
        if (FAILED(hr)) {
            SNOW_LOGW("render: hardware device failed hr=0x%08lx, trying WARP", static_cast<unsigned long>(hr));
            kind = "WARP";
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION,
                                   &device, &fl, &context);
        }
        if (FAILED(hr)) {
            SNOW_LOGE("render: D3D11CreateDevice failed hr=0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        auto fail = [&](const char* what, HRESULT h) {
            SNOW_LOGE("render: %s failed hr=0x%08lx", what, static_cast<unsigned long>(h));
            return false;
        };
        if (FAILED(hr = device.As(&dxgiDevice))) return fail("QueryInterface(IDXGIDevice1)", hr);
        dxgiDevice->SetMaximumFrameLatency(1);
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(hr = dxgiDevice->GetAdapter(&adapter))) return fail("GetAdapter", hr);
        if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) return fail("GetParent(IDXGIFactory2)", hr);
        if (FAILED(hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp))))
            return fail("DCompositionCreateDevice", hr);

        if (!compile("sprite_vs", shaders::kSpriteVS, "vs_4_0", spriteVsBlob) ||
            !compile("sprite_ps", shaders::kSpritePS, "ps_4_0", spritePsBlob) ||
            !compile("drift_vs", shaders::kDriftVS, "vs_4_0", driftVsBlob) ||
            !compile("drift_ps", shaders::kDriftPS, "ps_4_0", driftPsBlob)) {
            return false;
        }
        if (FAILED(hr = device->CreateVertexShader(spriteVsBlob->GetBufferPointer(), spriteVsBlob->GetBufferSize(),
                                                   nullptr, &spriteVs)))
            return fail("CreateVertexShader(sprite)", hr);
        if (FAILED(hr = device->CreatePixelShader(spritePsBlob->GetBufferPointer(), spritePsBlob->GetBufferSize(),
                                                  nullptr, &spritePs)))
            return fail("CreatePixelShader(sprite)", hr);
        if (FAILED(hr = device->CreateVertexShader(driftVsBlob->GetBufferPointer(), driftVsBlob->GetBufferSize(),
                                                   nullptr, &driftVs)))
            return fail("CreateVertexShader(drift)", hr);
        if (FAILED(hr = device->CreatePixelShader(driftPsBlob->GetBufferPointer(), driftPsBlob->GetBufferSize(),
                                                  nullptr, &driftPs)))
            return fail("CreatePixelShader(drift)", hr);

        const D3D11_INPUT_ELEMENT_DESC spriteIl[] = {
            {"POSVEL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"SIZEROT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"SHAPE", 0, DXGI_FORMAT_R8G8_UINT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        if (FAILED(hr = device->CreateInputLayout(spriteIl, 3, spriteVsBlob->GetBufferPointer(),
                                                  spriteVsBlob->GetBufferSize(), &spriteLayout)))
            return fail("CreateInputLayout(sprite)", hr);
        const D3D11_INPUT_ELEMENT_DESC driftIl[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"PROFILE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"DPIOP", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(hr = device->CreateInputLayout(driftIl, 3, driftVsBlob->GetBufferPointer(),
                                                  driftVsBlob->GetBufferSize(), &driftLayout)))
            return fail("CreateInputLayout(drift)", hr);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = device->CreateBlendState(&bd, &blend))) return fail("CreateBlendState", hr);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(hr = device->CreateRasterizerState(&rd, &raster))) return fail("CreateRasterizerState", hr);

        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = sizeof(FrameConstants);
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(hr = device->CreateBuffer(&cd, nullptr, &constants))) return fail("CreateBuffer(constants)", hr);

        SNOW_LOGI("render: %s device created, feature level 0x%x", kind, static_cast<unsigned>(fl));
        return true;
    }

    void releaseDevice() {
        lastSnap.reset();
        driftCache.clear();
        driftVertexCount = 0;
        objectRanges.clear();
        objectBounds.clear();
        particleRange = cursorRange = Range{};
        for (GpuOverlay& o : overlays) o.releaseGpu();
        if (dcomp) dcomp->Commit();
        instanceVb.Reset();
        driftVb.Reset();
        instanceCap = driftCap = 0;
        constants.Reset();
        raster.Reset();
        blend.Reset();
        spriteLayout.Reset();
        driftLayout.Reset();
        spriteVs.Reset();
        driftVs.Reset();
        spritePs.Reset();
        driftPs.Reset();
        dcomp.Reset();
        factory.Reset();
        dxgiDevice.Reset();
        if (context) {
            context->ClearState();
            context->Flush();
        }
        context.Reset();
        if (device) {
            const unsigned long refs = device.Reset();
            g_lastDeviceRefs.store(refs);
            SNOW_LOGI("render: device released, remaining references=%lu", refs);
        }
    }

    // Creates / resizes swap chains so every overlay matches its bounds. Returns false when
    // the device is lost (or, with `strict`, when any overlay could not be set up).
    bool ensureOverlayResources(bool strict) {
        bool ok = true;
        bool commit = false;
        for (GpuOverlay& o : overlays) {
            const int w = o.bounds.width(), h = o.bounds.height();
            if (w <= 0 || h <= 0) {
                o.releaseGpu();
                continue;
            }
            HRESULT hr = S_OK;
            if (!o.swapChain) {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = static_cast<UINT>(w);
                sd.Height = static_cast<UINT>(h);
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                sd.Scaling = DXGI_SCALING_STRETCH;
                hr = factory->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &o.swapChain);
                if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(o.hwnd, TRUE, &o.target);
                if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&o.visual);
                if (SUCCEEDED(hr)) hr = o.visual->SetContent(o.swapChain.Get());
                if (SUCCEEDED(hr)) hr = o.target->SetRoot(o.visual.Get());
                if (SUCCEEDED(hr)) {
                    o.w = w;
                    o.h = h;
                    commit = true;
                    SNOW_LOGI("render: overlay %llu swap chain %dx%d created", static_cast<unsigned long long>(o.key),
                              w, h);
                }
            } else if (o.w != w || o.h != h) {
                o.rtv.Reset();
                context->OMSetRenderTargets(0, nullptr, nullptr);
                context->Flush();
                hr = o.swapChain->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr)) {
                    o.w = w;
                    o.h = h;
                    SNOW_LOGI("render: overlay %llu resized to %dx%d", static_cast<unsigned long long>(o.key), w, h);
                }
            }
            if (SUCCEEDED(hr) && !o.rtv) {
                ComPtr<ID3D11Texture2D> back;
                hr = o.swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
                if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(back.Get(), nullptr, &o.rtv);
            }
            if (FAILED(hr)) {
                SNOW_LOGE("render: overlay %llu setup failed hr=0x%08lx (%dx%d)",
                          static_cast<unsigned long long>(o.key), static_cast<unsigned long>(hr), w, h);
                o.releaseGpu();
                if (strict || isDeviceLost(hr) || FAILED(device->GetDeviceRemovedReason())) ok = false;
            }
        }
        if (commit) dcomp->Commit();
        return ok;
    }

    // Applies the overlay list published by the UI thread.
    void syncTargets() {
        std::vector<Target> want;
        std::uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (targetsGen == appliedGen) return;
            want = targets;
            gen = targetsGen;
        }
        // Drop overlays that are gone (this releases their swap chain and DComp target).
        for (auto it = overlays.begin(); it != overlays.end();) {
            const bool keep = std::any_of(want.begin(), want.end(), [&](const Target& t) { return t.key == it->key; });
            if (keep) {
                ++it;
            } else {
                if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
                it->releaseGpu();
                it = overlays.erase(it);
            }
        }
        if (dcomp) dcomp->Commit();
        if (context) context->Flush();
        for (const Target& t : want) {
            auto it = std::find_if(overlays.begin(), overlays.end(), [&](const GpuOverlay& o) { return o.key == t.key; });
            if (it == overlays.end()) {
                GpuOverlay o;
                o.key = t.key;
                o.hwnd = t.hwnd;
                overlays.push_back(std::move(o));
                it = overlays.end() - 1;
            }
            it->bounds = t.bounds;
            it->dpi = t.dpi;
        }
        if (device && !ensureOverlayResources(false)) pendingLoss = true;
        statOverlays.store(static_cast<int>(overlays.size()));
        forceDraw = true;
        {
            std::lock_guard<std::mutex> lk(mtx);
            appliedGen = gen;
            if (appliedGen == targetsGen) SetEvent(appliedEvent);
        }
    }

    // ====================================================================================
    // render thread: frame
    // ====================================================================================
    bool ensureBuffer(ComPtr<ID3D11Buffer>& buf, std::size_t& cap, std::size_t need, std::size_t initial) {
        if (buf && cap >= need) return true;
        std::size_t n = std::max(cap, initial);
        while (n < need) n *= 2;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = static_cast<UINT>(n);
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        buf.Reset();
        cap = 0;
        const HRESULT hr = device->CreateBuffer(&d, nullptr, &buf);
        if (FAILED(hr)) {
            SNOW_LOGE("render: CreateBuffer(%zu bytes) failed hr=0x%08lx", n, static_cast<unsigned long>(hr));
            return false;
        }
        cap = n;
        return true;
    }

    // Copies the snapshot's geometry to the GPU (once per snapshot, shared by all overlays).
    void uploadSnapshot(const FrameSnapshot& s) {
        // Drifts: only re-uploaded when some strip was re-tessellated or the set changed.
        if (driftCache.build(s.drifts) || (!driftVb && !driftCache.vertices().empty())) {
            const std::vector<DriftVertex>& v = driftCache.vertices();
            driftVertexCount = 0;
            if (!v.empty() && ensureBuffer(driftVb, driftCap, v.size() * sizeof(DriftVertex), 64 * 1024)) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(context->Map(driftVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    std::memcpy(m.pData, v.data(), v.size() * sizeof(DriftVertex));
                    context->Unmap(driftVb.Get(), 0);
                    driftVertexCount = static_cast<UINT>(v.size());
                }
            }
        }

        // Sprites: all batches concatenated into one instance buffer, one Map per snapshot.
        std::size_t total = s.particles.sprites.size() + s.cursor.sprites.size();
        for (const SpriteBatch& b : s.objects) total += b.sprites.size();
        objectRanges.assign(s.objects.size(), Range{});
        objectBounds.assign(s.objects.size(), RectI{});
        for (std::size_t i = 0; i < s.objects.size(); ++i) {
            // Bounds incl. rotation, AA padding and up to 50 ms of extrapolation, so clip rects
            // that cannot touch any sprite of the batch are skipped without a draw call.
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
                objectBounds[i] = RectI{clampI(std::floor(l)), clampI(std::floor(t)), clampI(std::ceil(r)) + 1,
                                        clampI(std::ceil(b)) + 1};
            }
        }
        particleRange = cursorRange = Range{};
        if (total == 0) return;
        if (!ensureBuffer(instanceVb, instanceCap, total * sizeof(SpriteInstance), 4096 * sizeof(SpriteInstance)))
            return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(instanceVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        auto* dst = static_cast<std::uint8_t*>(m.pData);
        UINT at = 0;
        auto put = [&](const std::vector<SpriteInstance>& v) {
            Range r{at, static_cast<UINT>(v.size())};
            if (!v.empty()) std::memcpy(dst + static_cast<std::size_t>(at) * sizeof(SpriteInstance), v.data(),
                                        v.size() * sizeof(SpriteInstance));
            at += r.count;
            return r;
        };
        for (std::size_t i = 0; i < s.objects.size(); ++i) objectRanges[i] = put(s.objects[i].sprites);
        particleRange = put(s.particles.sprites);
        cursorRange = put(s.cursor.sprites);
        context->Unmap(instanceVb.Get(), 0);
    }

    void drawOverlay(GpuOverlay& o, const FrameSnapshot* snap, float dt, float globalOpacity) {
        ID3D11RenderTargetView* rtv = o.rtv.Get();
        const float clear[4] = {0, 0, 0, 0};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        if (!snap) return;

        D3D11_VIEWPORT vp{0, 0, static_cast<float>(o.w), static_cast<float>(o.h), 0, 1};
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster.Get());
        context->OMSetBlendState(blend.Get(), nullptr, 0xffffffffu);
        const D3D11_RECT full{0, 0, o.w, o.h};
        context->RSSetScissorRects(1, &full);

        FrameConstants fc{};
        fc.originX = static_cast<float>(o.bounds.l);
        fc.originY = static_cast<float>(o.bounds.t);
        fc.invHalfX = 2.0f / static_cast<float>(o.w);
        fc.invHalfY = 2.0f / static_cast<float>(o.h);
        fc.dt = dt;
        fc.opacity = globalOpacity;
        context->UpdateSubresource(constants.Get(), 0, nullptr, &fc, 0, 0);
        ID3D11Buffer* cb = constants.Get();
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        if (driftVertexCount && driftVb) {
            UINT stride = sizeof(DriftVertex), offset = 0;
            ID3D11Buffer* vb = driftVb.Get();
            context->IASetInputLayout(driftLayout.Get());
            context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            context->VSSetShader(driftVs.Get(), nullptr, 0);
            context->PSSetShader(driftPs.Get(), nullptr, 0);
            context->Draw(driftVertexCount, 0);
        }

        if (!instanceVb) return;
        UINT stride = sizeof(SpriteInstance), offset = 0;
        ID3D11Buffer* vb = instanceVb.Get();
        context->IASetInputLayout(spriteLayout.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->VSSetShader(spriteVs.Get(), nullptr, 0);
        context->PSSetShader(spritePs.Get(), nullptr, 0);

        const std::size_t batches = std::min(objectRanges.size(), snap->objects.size());
        for (std::size_t i = 0; i < batches; ++i) {
            const Range r = objectRanges[i];
            if (!r.count) continue;
            const std::vector<RectI>& clip = snap->objects[i].clip;
            const RectI area = i < objectBounds.size() ? objectBounds[i].intersect(o.bounds) : o.bounds;
            if (area.empty()) continue;
            if (clip.empty()) {
                context->RSSetScissorRects(1, &full);
                context->DrawInstanced(4, r.count, 0, r.start);
                continue;
            }
            for (const RectI& c : clip) {
                const RectI x = c.intersect(area);
                if (x.empty()) continue;
                const D3D11_RECT sc{x.l - o.bounds.l, x.t - o.bounds.t, x.r - o.bounds.l, x.b - o.bounds.t};
                context->RSSetScissorRects(1, &sc);
                context->DrawInstanced(4, r.count, 0, r.start);
            }
        }
        context->RSSetScissorRects(1, &full);
        if (particleRange.count) {
            // Particles honour their clip too: with excluded apps in "behind" mode the sim
            // clips falling snow away from those windows. Empty clip = unclipped.
            const std::vector<RectI>& clip = snap->particles.clip;
            if (clip.empty()) {
                context->DrawInstanced(4, particleRange.count, 0, particleRange.start);
            } else {
                for (const RectI& c : clip) {
                    const RectI x = c.intersect(o.bounds);
                    if (x.empty()) continue;
                    const D3D11_RECT sc{x.l - o.bounds.l, x.t - o.bounds.t, x.r - o.bounds.l, x.b - o.bounds.t};
                    context->RSSetScissorRects(1, &sc);
                    context->DrawInstanced(4, particleRange.count, 0, particleRange.start);
                }
                context->RSSetScissorRects(1, &full);
            }
        }
        if (cursorRange.count) context->DrawInstanced(4, cursorRange.count, 0, cursorRange.start);
    }

    // Presents every overlay; only the first one waits for vertical sync so the frame rate
    // does not get divided by the number of monitors.
    HRESULT presentAll(UINT interval) {
        HRESULT result = S_OK;
        bool first = true;
        for (GpuOverlay& o : overlays) {
            if (!o.swapChain || !o.rtv) continue;
            const HRESULT hr = o.swapChain->Present(first ? interval : 0, 0);
            first = false;
            if (FAILED(hr)) result = hr;
        }
        if (first && SUCCEEDED(result)) Sleep(16);   // nothing presentable: do not spin
        return result;
    }

    void preciseSleep(double seconds) {
        if (seconds <= 0) return;
        if (sleepTimer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7);
            if (due.QuadPart < 0 && SetWaitableTimer(sleepTimer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(sleepTimer, 1000);
                return;
            }
        }
        Sleep(static_cast<DWORD>(seconds * 1000.0 + 0.5));
    }

    void queryRefresh(Clock::time_point now) {
        if (refreshQueried != Clock::time_point{} && now - refreshQueried < std::chrono::seconds(2)) return;
        refreshQueried = now;
        DWM_TIMING_INFO ti{};
        ti.cbSize = sizeof ti;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator != 0) {
            const double hz = static_cast<double>(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator;
            if (hz >= 20.0 && hz <= 1000.0) {
                if (std::fabs(hz - refreshHz) > 0.5) SNOW_LOGI("render: composition refresh rate %.2f Hz", hz);
                refreshHz = hz;
            }
        }
    }

    // Sleeps `ms` while still applying overlay-list changes (keeps the UI handshake fast).
    void sleepServicing(DWORD ms) {
        const ULONGLONG end = GetTickCount64() + ms;
        while (!quit.load()) {
            const ULONGLONG now = GetTickCount64();
            if (now >= end) break;
            WaitForSingleObject(wakeEvent, static_cast<DWORD>(end - now));
            syncTargets();
        }
    }

    void giveUp() {
        failed.store(true);
        statFps.store(0);
        for (const GpuOverlay& o : overlays) ShowWindowAsync(o.hwnd, SW_HIDE);
        SNOW_LOGE("render: giving up after repeated device failures; overlays disabled");
        if (!failNotified) {
            failNotified = true;
            if (notifyHwnd) PostMessageW(notifyHwnd, failedMsg, 0, 0);
        }
    }

    void recover() {
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
        SNOW_LOGW("render: device lost (reason=0x%08lx), recreating", static_cast<unsigned long>(reason));
        quickLosses = framesSinceRecovery < 60 ? quickLosses + 1 : 0;
        releaseDevice();
        bool ok = false;
        if (quickLosses < 3) {
            static const DWORD backoffMs[3] = {100, 400, 1000};
            for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
                sleepServicing(backoffMs[attempt]);
                if (quit.load()) return;
                ok = createDevice() && ensureOverlayResources(true);
                if (!ok) {
                    SNOW_LOGW("render: recreation attempt %d failed", attempt + 1);
                    releaseDevice();
                }
            }
        }
        if (ok) {
            statResets.fetch_add(1);
            framesSinceRecovery = 0;
            forceDraw = true;
            SNOW_LOGI("render: device recreated (%d resets so far)", statResets.load());
        } else {
            giveUp();
        }
    }

    void threadMain() {
        initOk = createDevice();
        SetEvent(readyEvent);
        if (!initOk) {
            releaseDevice();
            return;
        }
        sleepTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!sleepTimer) sleepTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

        bool wasActive = false;
        int lastCap = 0;
        float lastOpacity = -1.0f;
        float lastDt = -1.0f;
        Clock::time_point prevPresentEnd = Clock::now();
        Clock::time_point windowStart = prevPresentEnd;
        int windowFrames = 0;
        double windowCpuMs = 0;

        while (!quit.load()) {
            syncTargets();
            bool lost = pendingLoss;
            pendingLoss = false;
            if (g_testLoss.exchange(0) > 0) {
                SNOW_LOGW("render: simulating device loss (test hook)");
                lost = true;
            }
            if (envLossFrame && totalFrames >= envLossFrame) {
                envLossFrame = 0;
                SNOW_LOGW("render: simulating device loss (SNOW_TEST_DEVICE_LOSS)");
                lost = true;
            }
            if (lost && !failed.load()) {
                recover();
                continue;
            }

            const bool active = visibleFlag.load() && !failed.load() && device && !overlays.empty();
            if (!active) {
                if (wasActive && device && !failed.load()) {
                    // Leave transparent frames behind so nothing stale flashes on the next show.
                    for (GpuOverlay& o : overlays) {
                        if (o.rtv) drawOverlay(o, nullptr, 0, 1);
                    }
                    if (FAILED(presentAll(0))) pendingLoss = true;
                }
                wasActive = false;
                lastSnap.reset();
                statFps.store(0);
                if (pendingLoss) continue;
                WaitForSingleObject(wakeEvent, INFINITE);
                continue;
            }
            if (!wasActive) {
                wasActive = true;
                forceDraw = true;
                windowStart = Clock::now();
                windowFrames = 0;
                windowCpuMs = 0;
            }

            const int cap = fpsCap.load() <= 30 ? 30 : 60;
            if (cap != lastCap) {
                lastCap = cap;
                windowStart = Clock::now();
                windowFrames = 0;
                windowCpuMs = 0;
            }
            const float globalOpacity = opacity.load();

            std::shared_ptr<const FrameSnapshot> snap = source->latest();
            const Clock::time_point now = Clock::now();
            float dt = 0;
            bool moving = false;
            if (snap && snap->taken != Clock::time_point{}) {
                dt = std::clamp(std::chrono::duration<float>(now - snap->taken).count(), 0.0f, 0.05f);
                moving = !snap->particles.sprites.empty() || !snap->objects.empty() || !snap->cursor.sprites.empty();
            }
            const bool newSnap = snap != lastSnap;
            const bool need = forceDraw || newSnap || globalOpacity != lastOpacity || (moving && dt != lastDt);
            if (!need) {
                WaitForSingleObject(wakeEvent, snap ? 4 : 16);
                continue;
            }

            // ---- build + submit -----------------------------------------------------
            const Clock::time_point t0 = Clock::now();
            if (newSnap) {
                if (snap) uploadSnapshot(*snap);
                lastSnap = snap;
            }
            for (GpuOverlay& o : overlays) {
                if (o.rtv) drawOverlay(o, snap.get(), dt, globalOpacity);
            }
            forceDraw = false;
            lastOpacity = globalOpacity;
            lastDt = dt;
            const double cpuMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

            // ---- pacing + present ---------------------------------------------------
            queryRefresh(now);
            const double ratio = refreshHz / cap;
            UINT interval = static_cast<UINT>(std::clamp(std::lround(ratio), 1L, 4L));
            double minPeriod;
            if (std::fabs(refreshHz / interval - cap) <= cap * 0.08) {
                minPeriod = 0.5 * interval / refreshHz;   // vsync paces us; this is only a safety net
            } else {
                interval = 1;                             // odd refresh rate: time-based cap
                minPeriod = 1.0 / cap - 0.0015;
            }
            const double sincePrev = std::chrono::duration<double>(Clock::now() - prevPresentEnd).count();
            if (sincePrev < minPeriod) preciseSleep(minPeriod - sincePrev);

            const HRESULT hr = presentAll(interval);
            prevPresentEnd = Clock::now();
            if (FAILED(hr)) {
                SNOW_LOGW("render: Present failed hr=0x%08lx", static_cast<unsigned long>(hr));
                pendingLoss = true;
                continue;
            }
            ++totalFrames;
            ++framesSinceRecovery;
            if ((totalFrames & 63) == 0 && FAILED(device->GetDeviceRemovedReason())) pendingLoss = true;

            // ---- stats --------------------------------------------------------------
            ++windowFrames;
            windowCpuMs += cpuMs;
            lastPresentMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    prevPresentEnd.time_since_epoch()).count());
            const double windowSec = std::chrono::duration<double>(prevPresentEnd - windowStart).count();
            if (windowSec >= 1.0) {
                statFps.store(static_cast<float>(windowFrames / windowSec));
                statFrameMs.store(static_cast<float>(windowCpuMs / windowFrames));
                windowStart = prevPresentEnd;
                windowFrames = 0;
                windowCpuMs = 0;
            }
        }

        overlays.clear();
        releaseDevice();
        spriteVsBlob.Reset();
        spritePsBlob.Reset();
        driftVsBlob.Reset();
        driftPsBlob.Reset();
        if (sleepTimer) CloseHandle(sleepTimer);
        sleepTimer = nullptr;
    }
};

// ========================================================================================
// Public API (UI thread)
// ========================================================================================
OverlaySystem::OverlaySystem() : impl_(std::make_unique<Impl>()) {}

OverlaySystem::~OverlaySystem() { shutdown(); }

bool OverlaySystem::initialize(HINSTANCE instance, const SnapshotExchange* source, HWND notifyHwnd, UINT failedMsg) {
    Impl& d = *impl_;
    if (d.initialized) return true;
    if (!source) return false;
    d.inst = instance ? instance : GetModuleHandleW(nullptr);
    d.source = source;
    d.notifyHwnd = notifyHwnd;
    d.failedMsg = failedMsg;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = d.inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        SNOW_LOGE("overlay: RegisterClassEx failed, error=%lu", GetLastError());
        return false;
    }
    d.classRegistered = true;

    d.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d.appliedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    d.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d.quit.store(false);
    d.failed.store(false);
    d.failNotified = false;
    d.pendingLoss = false;
    d.quickLosses = 0;
    d.framesSinceRecovery = 1000000;
    d.totalFrames = 0;
    d.statFps.store(0);
    d.statFrameMs.store(0);
    d.statResets.store(0);
    d.statOverlays.store(0);
    d.targets.clear();
    d.targetsGen = d.appliedGen = 0;
    d.envLossFrame = 0;
    if (const char* env = std::getenv("SNOW_TEST_DEVICE_LOSS")) {
        const long n = std::atol(env);
        if (n > 0) d.envLossFrame = static_cast<std::uint64_t>(n);
    }

    d.initOk = false;
    d.thread = std::thread([&d] { d.threadMain(); });
    WaitForSingleObject(d.readyEvent, INFINITE);
    if (!d.initOk) {
        SNOW_LOGE("overlay: renderer initialisation failed");
        d.thread.join();
        CloseHandle(d.wakeEvent);
        CloseHandle(d.appliedEvent);
        CloseHandle(d.readyEvent);
        d.wakeEvent = d.appliedEvent = d.readyEvent = nullptr;
        UnregisterClassW(kClassName, d.inst);
        d.classRegistered = false;
        return false;
    }
    d.initialized = true;
    return true;
}

void OverlaySystem::setMonitors(const std::vector<MonitorInfo>& monitors) {
    Impl& d = *impl_;
    if (!d.initialized) return;

    std::vector<Impl::UiOverlay> old = std::move(d.wins);
    d.wins.clear();
    std::vector<bool> used(old.size(), false);
    std::vector<int> pick(monitors.size(), -1);
    // Prefer the overlay that already covers exactly this monitor, then recycle any other.
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        for (std::size_t j = 0; j < old.size(); ++j) {
            if (!used[j] && old[j].mon.bounds == monitors[i].bounds) {
                used[j] = true;
                pick[i] = static_cast<int>(j);
                break;
            }
        }
    }
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        if (pick[i] >= 0) continue;
        for (std::size_t j = 0; j < old.size(); ++j) {
            if (!used[j]) {
                used[j] = true;
                pick[i] = static_cast<int>(j);
                break;
            }
        }
    }

    const bool show = d.visibleFlag.load() && !d.failed.load();
    bool changed = false;
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const MonitorInfo& m = monitors[i];
        if (m.bounds.empty()) {
            if (pick[i] >= 0) used[static_cast<std::size_t>(pick[i])] = false;   // goes to the destroy list
            continue;
        }
        Impl::UiOverlay o;
        if (pick[i] >= 0) {
            o = old[static_cast<std::size_t>(pick[i])];
            if (!(o.mon.bounds == m.bounds)) {
                SetWindowPos(o.hwnd, HWND_TOPMOST, m.bounds.l, m.bounds.t, m.bounds.width(), m.bounds.height(),
                             SWP_NOACTIVATE);
                changed = true;
            }
            if (!(o.mon == m)) changed = true;
        } else {
            o.hwnd = d.createOverlayWindow(m.bounds);
            if (!o.hwnd) continue;
            o.key = d.nextKey++;
            if (show) ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
            changed = true;
        }
        o.mon = m;
        d.wins.push_back(o);
    }
    std::vector<HWND> doomed;
    for (std::size_t j = 0; j < old.size(); ++j) {
        if (!used[j]) doomed.push_back(old[j].hwnd);
    }
    if (!changed && doomed.empty()) return;

    // The render thread must drop its swap chain / DComp target before the HWND dies.
    d.publishTargets(!doomed.empty());
    for (HWND h : doomed) DestroyWindow(h);
    SNOW_LOGI("overlay: %zu monitor(s), %zu overlay(s), %zu destroyed", monitors.size(), d.wins.size(),
              doomed.size());
}

void OverlaySystem::setVisible(bool visible) {
    Impl& d = *impl_;
    d.visibleFlag.store(visible);
    if (!d.initialized) return;
    const bool show = visible && !d.failed.load();
    for (const Impl::UiOverlay& o : d.wins) ShowWindow(o.hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    SetEvent(d.wakeEvent);
}

bool OverlaySystem::visible() const { return impl_->visibleFlag.load() && !impl_->failed.load(); }

void OverlaySystem::setFpsCap(int fps) {
    impl_->fpsCap.store(fps <= 30 ? 30 : 60);
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

void OverlaySystem::setGlobalOpacity(float o) {
    impl_->opacity.store(std::clamp(o, 0.1f, 1.0f));
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

void OverlaySystem::raise() {
    for (const Impl::UiOverlay& o : impl_->wins) {
        SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

std::vector<HWND> OverlaySystem::overlayWindows() const {
    std::vector<HWND> v;
    v.reserve(impl_->wins.size());
    for (const Impl::UiOverlay& o : impl_->wins) v.push_back(o.hwnd);
    return v;
}

RenderStats OverlaySystem::stats() const {
    const Impl& d = *impl_;
    RenderStats s;
    s.overlays = static_cast<int>(d.wins.size());
    s.deviceResets = d.statResets.load();
    s.failed = d.failed.load();
    s.frameMs = d.statFrameMs.load();
    const long long nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    const bool stale = nowMs - d.lastPresentMs.load() > 1500;   // nothing presented recently
    s.fps = (stale || s.failed) ? 0.0f : d.statFps.load();
    return s;
}

void OverlaySystem::shutdown() {
    Impl& d = *impl_;
    if (!d.initialized) return;
    d.initialized = false;
    d.quit.store(true);
    SetEvent(d.wakeEvent);
    if (d.thread.joinable()) {
        // Keep answering sent messages while the render thread releases its DComp targets.
        waitPumpingSent(static_cast<HANDLE>(d.thread.native_handle()), INFINITE);
        d.thread.join();
    }
    for (const Impl::UiOverlay& o : d.wins) DestroyWindow(o.hwnd);
    d.wins.clear();
    {
        std::lock_guard<std::mutex> lk(d.mtx);
        d.targets.clear();
    }
    if (d.classRegistered) {
        UnregisterClassW(kClassName, d.inst);
        d.classRegistered = false;
    }
    CloseHandle(d.wakeEvent);
    CloseHandle(d.appliedEvent);
    CloseHandle(d.readyEvent);
    d.wakeEvent = d.appliedEvent = d.readyEvent = nullptr;
    SNOW_LOGI("overlay: shut down");
}

}  // namespace snow
