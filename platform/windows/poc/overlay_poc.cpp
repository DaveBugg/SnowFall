// Proof-of-concept: transparent click-through GPU overlay on Windows 11.
// Path under test: WS_EX_NOREDIRECTIONBITMAP + WS_EX_LAYERED|WS_EX_TRANSPARENT,
// D3D11 flip-model swap chain with premultiplied alpha, presented via DirectComposition.
//
// Self-test: places an own magenta window under the overlay, then reads back ONLY the
// pixels of that own window to check (a) transparency, (b) drawn content, (c) click-through.
// Result is written to poc_result.txt next to the exe.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

static const char* kVS = R"(
struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };
VSOut main(float2 p : POSITION, float4 c : COLOR) { VSOut o; o.pos = float4(p, 0, 1); o.col = c; return o; }
)";
static const char* kPS = R"(
float4 main(float4 pos : SV_Position, float4 col : COLOR) : SV_Target { return col; }
)";

struct Vtx { float x, y; float r, g, b, a; };

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { return 0; }
    return DefWindowProcW(h, m, w, l);
}
static LRESULT CALLBACK BgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) {
        RECT rc; GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
        FillRect((HDC)w, &rc, b); DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::string log;
    auto L = [&](const char* fmt, auto... a) { char b[512]; snprintf(b, sizeof b, fmt, a...); log += b; log += "\n"; };

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"SnowPocOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BgProc; bc.hInstance = hi; bc.lpszClassName = L"SnowPocBg";
    bc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&bc);

    // Own test background window (magenta), 400x300 at (200,200).
    const int bx = 200, by = 200, bw = 400, bh = 300;
    HWND bg = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnowPocBg", L"bg", WS_POPUP | WS_VISIBLE, bx, by, bw, bh, nullptr, nullptr, hi, nullptr);
    SetWindowPos(bg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
    int W = mi.rcMonitor.right - mi.rcMonitor.left, H = mi.rcMonitor.bottom - mi.rcMonitor.top;

    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    HWND hwnd = CreateWindowExW(ex, L"SnowPocOverlay", L"overlay", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top, W, H, nullptr, nullptr, hi, nullptr);
    L("overlay hwnd=%p size=%dx%d", (void*)hwnd, W, H);

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    L("D3D11CreateDevice hr=0x%08lx fl=0x%x", hr, (unsigned)fl);
    if (FAILED(hr)) goto done;
    {
        ComPtr<IDXGIDevice> dxgiDev; dev.As(&dxgiDev);
        ComPtr<IDXGIFactory2> fac; hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> sc;
        hr = fac->CreateSwapChainForComposition(dxgiDev.Get(), &sd, nullptr, &sc);
        L("CreateSwapChainForComposition hr=0x%08lx", hr);
        if (FAILED(hr)) goto done;

        ComPtr<IDCompositionDevice> dc; hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dc));
        L("DCompositionCreateDevice hr=0x%08lx", hr);
        ComPtr<IDCompositionTarget> tgt; hr = dc->CreateTargetForHwnd(hwnd, TRUE, &tgt);
        L("CreateTargetForHwnd hr=0x%08lx", hr);
        ComPtr<IDCompositionVisual> vis; dc->CreateVisual(&vis);
        vis->SetContent(sc.Get()); tgt->SetRoot(vis.Get()); hr = dc->Commit();
        L("Commit hr=0x%08lx", hr);

        ComPtr<ID3D11Texture2D> bb; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        ComPtr<ID3D11RenderTargetView> rtv; dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);

        ComPtr<ID3DBlob> vsb, psb, err;
        D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err);
        D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err);
        ComPtr<ID3D11VertexShader> vs; dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        ComPtr<ID3D11PixelShader> ps; dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ComPtr<ID3D11InputLayout> layout; dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);

        // Opaque white quad covering screen px (250..350, 250..350); half-alpha quad at (400..500, 250..350).
        auto nx = [&](float px) { return (px - mi.rcMonitor.left) / W * 2.f - 1.f; };
        auto ny = [&](float py) { return 1.f - (py - mi.rcMonitor.top) / H * 2.f; };
        auto quad = [&](Vtx* v, float x0, float y0, float x1, float y1, float a) {
            Vtx q[6] = {{nx(x0), ny(y0), a, a, a, a}, {nx(x1), ny(y0), a, a, a, a}, {nx(x0), ny(y1), a, a, a, a},
                        {nx(x1), ny(y0), a, a, a, a}, {nx(x1), ny(y1), a, a, a, a}, {nx(x0), ny(y1), a, a, a, a}};
            memcpy(v, q, sizeof q);
        };
        Vtx verts[12]; quad(verts, 250, 250, 350, 350, 1.f); quad(verts + 6, 400, 250, 500, 350, 0.5f);
        D3D11_BUFFER_DESC bd{sizeof verts, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
        D3D11_SUBRESOURCE_DATA sdv{verts};
        ComPtr<ID3D11Buffer> vb; dev->CreateBuffer(&bd, &sdv, &vb);

        D3D11_BLEND_DESC bld{}; bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> bs; dev->CreateBlendState(&bld, &bs);
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        ComPtr<ID3D11RasterizerState> rs; dev->CreateRasterizerState(&rd, &rs);

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        auto render = [&] {
            float clear[4] = {0, 0, 0, 0};
            ID3D11RenderTargetView* r = rtv.Get();
            ctx->OMSetRenderTargets(1, &r, nullptr);
            ctx->ClearRenderTargetView(r, clear);
            D3D11_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1}; ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs.Get());
            ctx->OMSetBlendState(bs.Get(), nullptr, 0xffffffff);
            ctx->IASetInputLayout(layout.Get()); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride = sizeof(Vtx), off = 0; ID3D11Buffer* b = vb.Get();
            ctx->IASetVertexBuffers(0, 1, &b, &stride, &off);
            ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(12, 0);
            return sc->Present(1, 0);
        };
        for (int i = 0; i < 60; i++) { hr = render(); Pump(10); }
        L("Present hr=0x%08lx", hr);

        // Read back pixels of OUR magenta window region only.
        HDC sdc = GetDC(nullptr);
        auto px = [&](int x, int y) { COLORREF c = GetPixel(sdc, x, y); return c; };
        COLORREF cT = px(220, 220), cW = px(300, 300), cH = px(450, 300);
        ReleaseDC(nullptr, sdc);
        L("transparent px (expect ff00ff): %02x%02x%02x", GetRValue(cT), GetGValue(cT), GetBValue(cT));
        L("opaque white px (expect ffffff): %02x%02x%02x", GetRValue(cW), GetGValue(cW), GetBValue(cW));
        L("half white px (expect ~ff80ff): %02x%02x%02x", GetRValue(cH), GetGValue(cH), GetBValue(cH));
        HWND hit = WindowFromPoint({300, 300});
        L("WindowFromPoint(300,300)=%p bg=%p overlay=%p -> clickthrough=%s", (void*)hit, (void*)bg, (void*)hwnd, hit == bg ? "YES" : "NO");
        L("foreground is overlay: %s", GetForegroundWindow() == hwnd ? "YES(bad)" : "no(good)");
        bool ok = cT == RGB(255, 0, 255) && cW == RGB(255, 255, 255) && hit == bg;
        L("RESULT: %s", ok ? "PASS" : "FAIL");
    }
done:
    DestroyWindow(hwnd); DestroyWindow(bg);
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path); p = p.substr(0, p.find_last_of(L'\\')) + L"\\poc_result.txt";
    FILE* f = _wfopen(p.c_str(), L"wb"); if (f) { fwrite(log.data(), 1, log.size(), f); fclose(f); }
    return 0;
}
