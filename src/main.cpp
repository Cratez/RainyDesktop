// raindesk: rain on the glass of your monitor.
//
// A topmost, click-through, per-pixel-alpha window covers the primary monitor.
// Every frame we grab the desktop image underneath with Desktop Duplication
// (our own window is excluded from capture), advance the rain simulation, and
// draw water that refracts that image. Pixels with no water are fully
// transparent, so the desktop shows through and input goes straight to it.
//
// Behind the glass, Sky (sky.cpp) runs showers, lightning and the rain falling
// outside, and RainSound (sound.cpp) plays it all.
//
// A drop icon in the tray opens the settings and menu (see controls.cpp).
//
// Hotkeys: Ctrl+Alt+Q quit, Ctrl+Alt+W run the wiper, Ctrl+Alt+Up/Down rain harder/softer,
// Ctrl+Alt+S save a screenshot with the rain in it (ordinary screenshots can't see the overlay).
// Holding Ctrl+Alt and moving the mouse wipes a finger through the water.
// The shaders (rain.hlsl) are built into the exe. A rain.hlsl next to the exe
// takes their place and is recompiled live whenever it changes, for tuning;
// errors go to raindesk.log next to the exe.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "controls.h"
#include "rain_sim.h"
#include "resource.h"
#include "sky.h"
#include "sound.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace {

enum Hotkey { kHotkeyQuit = 1, kHotkeyWipe, kHotkeyHarder, kHotkeySofter, kHotkeyScreenshot };
constexpr UINT_PTR kTimerTopmost = 1;

struct FrameConstants {
    float resolution[2];
    float time;
    float hasDesktop;
    float decay;
    float wiperActive;
    float wiperAngle;  // radians from straight up
    float wiperInner;
    float wiperPivot[2];
    float wiperOuter;
    float pad;
    float flash;       // lightning brightness, after the user's flash setting
    float flashDir[2]; // toward the flash, on the screen
    float refraction;
    float sky[4];      // colour of the light from the sky, and daylight 0..1
};
static_assert(sizeof(FrameConstants) % 16 == 0, "constant buffers come in 16-byte rows");

// A dynamic structured buffer of splats that grows as needed.
struct SplatBuffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT capacity = 0;
    UINT count = 0;
};

struct WaterTarget {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct Shaders {
    ComPtr<ID3D11VertexShader> fullscreen, splat, streak;
    ComPtr<ID3D11PixelShader> drop, mist, erase, composite, desktop, decay, wiper, flash, streakPs;
};

struct App {
    HWND hwnd = nullptr;
    RECT bounds{};  // monitor rect in physical pixels
    UINT width = 0, height = 0;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;

    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> desktopTex;
    ComPtr<ID3D11ShaderResourceView> desktopSrv;
    bool haveDesktop = false;
    std::chrono::steady_clock::time_point nextDuplRetry{};

    ComPtr<IDXGISwapChain1> swap;
    HANDLE frameWait = nullptr;  // signalled when the compositor can take another frame
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;

    Shaders shaders;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blendAdd, blendMax, blendErase, blendOver;

    WaterTarget water;  // redrawn every frame from the drop list
    WaterTarget mist;   // persistent: tiny droplets accumulate here
    SplatBuffer dropSplats, mistSplats, eraseSplats, streaks;
    bool clearMist = true;

    // The simulation and rendering run on their own thread, so the window
    // thread can sit in a modal loop (dragging the settings window, the tray
    // menu) without freezing the rain. The window thread only talks to it
    // through these.
    std::atomic<bool> wipeRequested{false}, clearRequested{false}, screenshotRequested{false};
    std::mutex settingsLock;
    Settings shared;  // copy of settings for the render thread, under settingsLock

    RainSim sim;       // render thread only
    Sky sky;           // render thread only
    RainSound sound;   // fed by the render thread, played by soundOut's thread
    SoundOutput soundOut;
    Settings settings; // window thread only; the tray and settings window edit it
    bool shownHidden = false;  // overlay currently hidden from the screen

    fs::path exeDir;
    fs::path shaderPath;               // rain.hlsl next to the exe, used instead of the built-in shaders if present
    fs::file_time_type shaderTime{};   // its last change, or {} when there is none
    std::chrono::steady_clock::time_point nextShaderCheck{};

    std::atomic<bool> running{true};
    std::mutex logLock;
};

App g;

void Log(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    std::lock_guard lock(g.logLock);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g.exeDir / L"raindesk.log").c_str(), L"a") == 0 && f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

void Fatal(const wchar_t* what, HRESULT hr) {
    wchar_t msg[512];
    swprintf_s(msg, L"%s failed (0x%08lX).", what, static_cast<unsigned long>(hr));
    Log("FATAL: %ls", msg);
    MessageBoxW(nullptr, msg, L"raindesk", MB_ICONERROR);
    ExitProcess(1);
}

#define CHECK(expr)                               \
    do {                                          \
        HRESULT hr_ = (expr);                     \
        if (FAILED(hr_)) Fatal(L## #expr, hr_);   \
    } while (0)

// Picks the primary monitor's output and the adapter that drives it.
// Desktop Duplication only works on a device created on that adapter.
void FindPrimaryOutput() {
    ComPtr<IDXGIFactory1> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            out->GetDesc(&desc);
            const RECT& r = desc.DesktopCoordinates;
            if (desc.AttachedToDesktop && r.left <= 0 && r.top <= 0 && r.right > 0 && r.bottom > 0) {
                g.adapter = adapter;
                CHECK(out.As(&g.output));
                g.bounds = r;
                g.width = static_cast<UINT>(r.right - r.left);
                g.height = static_cast<UINT>(r.bottom - r.top);
                Log("Primary output %ls: %ux%u at (%ld,%ld), rotation %d", desc.DeviceName, g.width,
                    g.height, r.left, r.top, static_cast<int>(desc.Rotation));
                return;
            }
        }
    }
    Fatal(L"Finding the primary monitor", E_FAIL);
}

ComPtr<ID3D11BlendState> MakeBlend(D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op) {
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    // *_COLOR factors are not allowed for the alpha channel. Our water targets
    // have no alpha channel anyway, so alpha just mirrors with the ALPHA variant.
    auto alphaFactor = [](D3D11_BLEND f) {
        switch (f) {
            case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
            case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
            case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
            case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
            default: return f;
        }
    };
    rt.SrcBlend = src;
    rt.DestBlend = dst;
    rt.SrcBlendAlpha = alphaFactor(src);
    rt.DestBlendAlpha = alphaFactor(dst);
    rt.BlendOp = rt.BlendOpAlpha = op;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> state;
    CHECK(g.device->CreateBlendState(&bd, &state));
    return state;
}

void CreateWaterTarget(WaterTarget& t) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g.width;
    td.Height = g.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CHECK(g.device->CreateTexture2D(&td, nullptr, &t.tex));
    CHECK(g.device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv));
    CHECK(g.device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv));
}

void CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(g.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &g.device, nullptr, &g.ctx);
#ifdef _DEBUG
    if (FAILED(hr)) {  // debug layer not installed
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(g.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &g.device, nullptr, &g.ctx);
    }
#endif
    CHECK(hr);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FrameConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CHECK(g.device->CreateBuffer(&cb, nullptr, &g.constants));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(g.device->CreateSamplerState(&sd, &g.sampler));

    g.blendAdd = MakeBlend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD);
    g.blendMax = MakeBlend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX);
    g.blendErase = MakeBlend(D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_OP_ADD);
    g.blendOver = MakeBlend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD);

    CreateWaterTarget(g.water);
    CreateWaterTarget(g.mist);
}

void CreateSwapChainAndComposition() {
    ComPtr<IDXGIDevice> dxgiDevice;
    CHECK(g.device.As(&dxgiDevice));
    ComPtr<IDXGIFactory2> factory;
    CHECK(g.adapter->GetParent(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = g.width;
    sc.Height = g.height;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    CHECK(factory->CreateSwapChainForComposition(g.device.Get(), &sc, nullptr, &g.swap));

    // Keep at most one frame queued. By default the CPU can run up to three
    // frames ahead, and each queued frame shows a desktop image that much
    // older, which shows as a ghost of a window's old position as it moves.
    ComPtr<IDXGISwapChain2> swap2;
    CHECK(g.swap.As(&swap2));
    CHECK(swap2->SetMaximumFrameLatency(1));
    g.frameWait = swap2->GetFrameLatencyWaitableObject();

    ComPtr<ID3D11Texture2D> back;
    CHECK(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)));
    CHECK(g.device->CreateRenderTargetView(back.Get(), nullptr, &g.rtv));

    CHECK(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g.dcomp)));
    CHECK(g.dcomp->CreateTargetForHwnd(g.hwnd, TRUE, &g.dcompTarget));
    CHECK(g.dcomp->CreateVisual(&g.dcompVisual));
    CHECK(g.dcompVisual->SetContent(g.swap.Get()));
    CHECK(g.dcompTarget->SetRoot(g.dcompVisual.Get()));
    CHECK(g.dcomp->Commit());
}

// Uploads splats or streaks (both 32 bytes) into a structured buffer.
template <typename T>
void Upload(SplatBuffer& b, const std::vector<T>& splats) {
    static_assert(sizeof(T) == sizeof(Splat), "one buffer layout for both");
    b.count = static_cast<UINT>(splats.size());
    if (b.count == 0) return;
    if (b.count > b.capacity) {
        b.capacity = std::max(b.count, b.capacity * 2u);
        b.capacity = std::max(b.capacity, 256u);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = b.capacity * sizeof(Splat);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(Splat);
        b.buffer.Reset();
        b.srv.Reset();
        CHECK(g.device->CreateBuffer(&bd, nullptr, &b.buffer));
        CHECK(g.device->CreateShaderResourceView(b.buffer.Get(), nullptr, &b.srv));
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g.ctx->Map(b.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, splats.data(), splats.size() * sizeof(Splat));
        g.ctx->Unmap(b.buffer.Get(), 0);
    }
}

void TryCreateDuplication() {
    auto now = std::chrono::steady_clock::now();
    if (now < g.nextDuplRetry) return;
    HRESULT hr = g.output->DuplicateOutput(g.device.Get(), &g.dupl);
    if (FAILED(hr)) {
        // Common while the secure desktop (UAC, lock screen) is up. Try again shortly.
        g.dupl.Reset();
        g.nextDuplRetry = now + std::chrono::milliseconds(500);
        Log("DuplicateOutput failed (0x%08lX), retrying", static_cast<unsigned long>(hr));
    }
}

// Copies the newest desktop image into desktopTex. Keeps the previous image
// when nothing changed on screen.
void UpdateDesktop() {
    if (!g.dupl) {
        TryCreateDuplication();
        if (!g.dupl) return;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = g.dupl->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (FAILED(hr)) {
        // DXGI_ERROR_ACCESS_LOST on mode changes, fullscreen switches, desktop switches.
        Log("AcquireNextFrame failed (0x%08lX), recreating duplication", static_cast<unsigned long>(hr));
        g.dupl.Reset();
        return;
    }

    ComPtr<ID3D11Texture2D> frame;
    if (SUCCEEDED(resource.As(&frame))) {
        D3D11_TEXTURE2D_DESC fd{};
        frame->GetDesc(&fd);
        D3D11_TEXTURE2D_DESC cur{};
        if (g.desktopTex) g.desktopTex->GetDesc(&cur);
        if (!g.desktopTex || cur.Width != fd.Width || cur.Height != fd.Height || cur.Format != fd.Format) {
            // Full mip chain: drops show a blurred view, sampled from a lower mip.
            D3D11_TEXTURE2D_DESC td = fd;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.CPUAccessFlags = 0;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            td.MipLevels = 0;
            td.ArraySize = 1;
            g.desktopTex.Reset();
            g.desktopSrv.Reset();
            CHECK(g.device->CreateTexture2D(&td, nullptr, &g.desktopTex));
            CHECK(g.device->CreateShaderResourceView(g.desktopTex.Get(), nullptr, &g.desktopSrv));
            Log("Desktop texture %ux%u format %d", fd.Width, fd.Height, static_cast<int>(fd.Format));
        }
        g.ctx->CopySubresourceRegion(g.desktopTex.Get(), 0, 0, 0, 0, frame.Get(), 0, nullptr);
        g.ctx->GenerateMips(g.desktopSrv.Get());
        g.haveDesktop = true;
    }
    g.dupl->ReleaseFrame();
}

// The shader source: rain.hlsl next to the exe if there is one (so the look
// can be tuned while it runs), otherwise the copy built into the exe.
std::string LoadShaderSource(bool builtIn, std::wstring& from) {
    if (!builtIn) {
        std::ifstream file(g.shaderPath, std::ios::binary);
        if (file) {
            from = g.shaderPath.wstring();
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
    }
    from = L"the copy built into raindesk.exe";
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_SHADER), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
    return bytes ? std::string(bytes, SizeofResource(nullptr, res)) : std::string();
}

template <typename T>
bool CompileEntry(const std::string& source, const char* entry, const char* target, ComPtr<T>& out) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompile(source.data(), source.size(), "rain.hlsl", nullptr, nullptr, entry, target, flags, 0,
                            &blob, &errors);
    if (FAILED(hr)) {
        Log("Shader %s failed to compile (0x%08lX):\n%s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no compiler output)");
        return false;
    }
    if constexpr (std::is_same_v<T, ID3D11VertexShader>)
        hr = g.device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out);
    else
        hr = g.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out);
    if (FAILED(hr)) Log("Creating shader %s failed (0x%08lX)", entry, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

// All or nothing: a broken edit keeps the last working shaders running.
bool CompileShaders(bool builtIn = false) {
    std::wstring from;
    const std::string src = LoadShaderSource(builtIn, from);
    Shaders s;
    bool ok = CompileEntry(src, "VSFullscreen", "vs_5_0", s.fullscreen) &&
              CompileEntry(src, "VSSplat", "vs_5_0", s.splat) && CompileEntry(src, "PSDrop", "ps_5_0", s.drop) &&
              CompileEntry(src, "PSMist", "ps_5_0", s.mist) && CompileEntry(src, "PSErase", "ps_5_0", s.erase) &&
              CompileEntry(src, "PSComposite", "ps_5_0", s.composite) &&
              CompileEntry(src, "PSDesktop", "ps_5_0", s.desktop) && CompileEntry(src, "PSDecay", "ps_5_0", s.decay) &&
              CompileEntry(src, "PSWiper", "ps_5_0", s.wiper) && CompileEntry(src, "PSFlash", "ps_5_0", s.flash) &&
              CompileEntry(src, "VSStreak", "vs_5_0", s.streak) && CompileEntry(src, "PSStreak", "ps_5_0", s.streakPs);
    if (!ok) {
        Log("Shaders from %ls failed to compile", from.c_str());
        return false;
    }
    g.shaders = s;
    Log("Shaders loaded from %ls", from.c_str());
    return true;
}

void ReloadShadersIfChanged() {
    auto now = std::chrono::steady_clock::now();
    if (now < g.nextShaderCheck) return;
    g.nextShaderCheck = now + std::chrono::milliseconds(500);
    // A rain.hlsl appearing, changing or going away (back to the built-in shaders).
    std::error_code ec;
    auto t = fs::last_write_time(g.shaderPath, ec);
    if (ec) t = {};
    if (t == g.shaderTime) return;
    g.shaderTime = t;
    CompileShaders();
}

void DrawSplats(const SplatBuffer& b, ID3D11PixelShader* ps, ID3D11BlendState* blend) {
    if (b.count == 0) return;
    g.ctx->VSSetShader(g.shaders.splat.Get(), nullptr, 0);
    g.ctx->VSSetShaderResources(0, 1, b.srv.GetAddressOf());
    g.ctx->PSSetShader(ps, nullptr, 0);
    g.ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
    g.ctx->DrawInstanced(4, b.count, 0, 0);
}

bool WritePng(const fs::path& path, const D3D11_MAPPED_SUBRESOURCE& m) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(g.width, g.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WritePixels(g.height, m.RowPitch, m.RowPitch * g.height, static_cast<BYTE*>(m.pData))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// Draws everything that goes on the screen, each layer blended over the last:
// the lightning flash and the rain outside (behind the glass), then the water
// on the glass, then the wiper. Expects the render target, the fullscreen
// vertex shader, the triangle list topology and the desktop, water and mist
// textures to be bound.
void DrawOverlay() {
    g.ctx->OMSetBlendState(g.blendOver.Get(), nullptr, 0xffffffff);
    if (g.sky.Flash() > 0.001f) {
        g.ctx->PSSetShader(g.shaders.flash.Get(), nullptr, 0);
        g.ctx->Draw(3, 0);
    }
    if (g.streaks.count) {
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g.ctx->VSSetShader(g.shaders.streak.Get(), nullptr, 0);
        g.ctx->VSSetShaderResources(3, 1, g.streaks.srv.GetAddressOf());
        g.ctx->PSSetShader(g.shaders.streakPs.Get(), nullptr, 0);
        g.ctx->DrawInstanced(4, g.streaks.count, 0, 0);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(g.shaders.fullscreen.Get(), nullptr, 0);
    }
    g.ctx->PSSetShader(g.shaders.composite.Get(), nullptr, 0);
    g.ctx->Draw(3, 0);
    if (g.sim.GetWiper().active) {
        g.ctx->PSSetShader(g.shaders.wiper.Get(), nullptr, 0);
        g.ctx->Draw(3, 0);
    }
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

// Our window is hidden from screen capture (so we can refract what is under
// it), which also hides the rain from normal screenshots. This renders the
// desktop with the rain on top and saves it next to the exe.
void SaveScreenshot() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g.width;
    td.Height = g.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &target)) ||
        FAILED(g.device->CreateRenderTargetView(target.Get(), nullptr, &rtv)))
        return;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &staging))) return;

    // Same pipeline state as the composite pass that just ran.
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g.ctx->PSSetShader(g.shaders.desktop.Get(), nullptr, 0);
    g.ctx->Draw(3, 0);
    DrawOverlay();
    g.ctx->CopyResource(staging.Get(), target.Get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"raindesk-%04d%02d%02d-%02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    fs::path dir = g.exeDir / L"screenshots";
    std::error_code ec;
    fs::create_directories(dir, ec);
    bool ok = WritePng(dir / name, m);
    g.ctx->Unmap(staging.Get(), 0);
    Log("Screenshot %ls: %s", (dir / name).c_str(), ok ? "saved" : "FAILED");
    MessageBeep(ok ? MB_OK : MB_ICONERROR);
}

void Render(float seconds, float dt, const Settings& s) {
    UpdateDesktop();

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g.ctx->Map(g.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* c = static_cast<FrameConstants*>(m.pData);
        c->resolution[0] = static_cast<float>(g.width);
        c->resolution[1] = static_cast<float>(g.height);
        c->time = seconds;
        c->hasDesktop = g.haveDesktop ? 1.0f : 0.0f;
        c->decay = 1.0f - std::exp(-dt / 60.0f);  // mist dries over about a minute
        const RainSim::Wiper& w = g.sim.GetWiper();
        c->wiperActive = w.active ? 1.0f : 0.0f;
        c->wiperAngle = w.angle;
        c->wiperInner = w.inner;
        c->wiperPivot[0] = w.pivotX;
        c->wiperPivot[1] = w.pivotY;
        c->wiperOuter = w.outer;
        // A flash stands out far more against a night sky than a grey day.
        c->flash = g.sky.Flash() * s.flash * (1.5f - 0.5f * g.sky.Daylight());
        c->flashDir[0] = g.sky.FlashDirX();
        c->flashDir[1] = g.sky.FlashDirY();
        c->refraction = s.refraction;
        SkyColor sky = g.sky.Light();
        c->sky[0] = sky.r;
        c->sky[1] = sky.g;
        c->sky[2] = sky.b;
        c->sky[3] = g.sky.Daylight();
        g.ctx->Unmap(g.constants.Get(), 0);
    }

    Upload(g.dropSplats, g.sim.Drops());
    Upload(g.mistSplats, g.sim.Mist());
    Upload(g.eraseSplats, g.sim.Erase());
    Upload(g.streaks, g.sky.Streaks());

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g.width), static_cast<float>(g.height), 0, 1};
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetConstantBuffers(0, 1, g.constants.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, g.constants.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, g.sampler.GetAddressOf());

    const float zero[4] = {0, 0, 0, 0};
    if (g.clearMist) {
        g.ctx->ClearRenderTargetView(g.mist.rtv.Get(), zero);
        g.clearMist = false;
    }
    g.ctx->ClearRenderTargetView(g.water.rtv.Get(), zero);
    g.ctx->ClearRenderTargetView(g.rtv.Get(), zero);

    if (g.shaders.composite && g.desktopSrv) {
        // Mist: dry a little, stamp new droplets and streaks, then wipe where drops ran through.
        g.ctx->OMSetRenderTargets(1, g.mist.rtv.GetAddressOf(), nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(g.shaders.fullscreen.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.shaders.decay.Get(), nullptr, 0);
        g.ctx->OMSetBlendState(g.blendErase.Get(), nullptr, 0xffffffff);
        g.ctx->Draw(3, 0);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        DrawSplats(g.mistSplats, g.shaders.mist.Get(), g.blendMax.Get());
        DrawSplats(g.eraseSplats, g.shaders.erase.Get(), g.blendErase.Get());

        // Drops: summed so close drops fuse.
        g.ctx->OMSetRenderTargets(1, g.water.rtv.GetAddressOf(), nullptr);
        DrawSplats(g.dropSplats, g.shaders.drop.Get(), g.blendAdd.Get());

        // Onto the screen.
        ID3D11ShaderResourceView* noVs = nullptr;
        g.ctx->VSSetShaderResources(0, 1, &noVs);
        g.ctx->OMSetRenderTargets(1, g.rtv.GetAddressOf(), nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(g.shaders.fullscreen.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = {g.desktopSrv.Get(), g.water.srv.Get(), g.mist.srv.Get()};
        g.ctx->PSSetShaderResources(0, 3, srvs);
        DrawOverlay();

        if (g.screenshotRequested.exchange(false)) {
            SaveScreenshot();
        }

        // Unbind: next frame writes to all three (desktop copy, render targets).
        ID3D11ShaderResourceView* none[3] = {};
        g.ctx->PSSetShaderResources(0, 3, none);
    }

    HRESULT hr = g.swap->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Fatal(L"Present (GPU device lost)", g.device->GetDeviceRemovedReason());
    }
}

// Hands the current settings to the render thread. Window thread only.
void PublishSettings() {
    {
        std::lock_guard lock(g.settingsLock);
        g.shared = g.settings;
    }
    // Hidden: take the overlay off screen; the render thread idles meanwhile.
    if (g.settings.hidden != g.shownHidden) {
        g.shownHidden = g.settings.hidden;
        ShowWindow(g.hwnd, g.shownHidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_HOTKEY:
            switch (wp) {
                case kHotkeyQuit:
                    DestroyWindow(hwnd);
                    break;
                case kHotkeyWipe:
                    g.wipeRequested = true;
                    break;
                case kHotkeyScreenshot:
                    g.screenshotRequested = true;
                    break;
                case kHotkeyHarder:
                case kHotkeySofter:
                    g.settings.rain *= wp == kHotkeyHarder ? 1.5f : 1.0f / 1.5f;
                    g.settings.rain = std::clamp(g.settings.rain, 0.1f, 6.0f);
                    controls::Save();
                    PublishSettings();
                    Log("Rain amount %.2f", g.settings.rain);
                    break;
            }
            return 0;
        case WM_TIMER:
            if (wp == kTimerTopmost) {
                // The taskbar and other topmost windows can end up above us; stay on top.
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_DESTROY:
            g.running = false;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CreateOverlayWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"RaindeskOverlay";
    RegisterClassExW(&wc);

    // LAYERED + TRANSPARENT makes the whole window click-through.
    // NOREDIRECTIONBITMAP lets DirectComposition supply per-pixel alpha content.
    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                     WS_EX_NOREDIRECTIONBITMAP;
    g.hwnd = CreateWindowExW(ex, wc.lpszClassName, L"raindesk", WS_POPUP, g.bounds.left, g.bounds.top,
                             static_cast<int>(g.width), static_cast<int>(g.height), nullptr, nullptr, inst,
                             nullptr);
    if (!g.hwnd) Fatal(L"CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));
    SetLayeredWindowAttributes(g.hwnd, 0, 255, LWA_ALPHA);

    // Hide our window from screen capture, including our own Desktop Duplication,
    // so the drops refract the real desktop instead of themselves. Windows 10 2004+.
    if (!SetWindowDisplayAffinity(g.hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        Fatal(L"Excluding the overlay from capture (needs Windows 10 version 2004 or newer)",
              HRESULT_FROM_WIN32(GetLastError()));
    }

    struct {
        int id;
        UINT vk;
    } keys[] = {{kHotkeyQuit, 'Q'}, {kHotkeyWipe, 'W'}, {kHotkeyHarder, VK_UP}, {kHotkeySofter, VK_DOWN},
                {kHotkeyScreenshot, 'S'}};
    for (auto& k : keys) {
        if (!RegisterHotKey(g.hwnd, k.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, k.vk))
            Log("Could not register hotkey %d (another app owns it)", k.id);
    }
    SetTimer(g.hwnd, kTimerTopmost, 1000, nullptr);
}

// The latitude of the country in Windows' region settings (its middle), so
// daylight can follow the sun without asking where you are.
float RegionLatitude() {
    GEOID id = GetUserGeoID(GEOCLASS_NATION);
    wchar_t buf[32];
    if (id != GEOID_NOT_AVAILABLE && GetGeoInfoW(id, GEO_LATITUDE, buf, 32, 0) > 0)
        return static_cast<float>(_wtof(buf));
    return 40.0f;
}

// Tells the sky the date, the time and roughly where you are. The longitude
// comes from the time zone (15 degrees an hour from Greenwich), which is
// close to where most people in it live.
void UpdateClock() {
    static const float latitude = RegionLatitude();
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    TIME_ZONE_INFORMATION tz;
    GetTimeZoneInformation(&tz);
    float longitude = -static_cast<float>(tz.Bias) / 4.0f;
    static const int kDaysBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    bool leap = utc.wYear % 4 == 0 && (utc.wYear % 100 != 0 || utc.wYear % 400 == 0);
    int day = kDaysBefore[(utc.wMonth - 1) % 12] + utc.wDay + (leap && utc.wMonth > 2 ? 1 : 0);
    float hours = utc.wHour + utc.wMinute / 60.0f + utc.wSecond / 3600.0f;
    g.sky.SetClock(day, hours, longitude, latitude);
}

void RenderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // for WIC screenshots
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last = start;
    auto lastWipe = start;
    auto lastClock = start - std::chrono::minutes(1);
    while (g.running) {
        Settings s;
        {
            std::lock_guard lock(g.settingsLock);
            s = g.shared;
        }
        if (s.hidden) {
            g.sound.SetLevels(0, 0, 0, 0);
            Sleep(100);
            last = clock::now();
            continue;
        }

        // Wait until the compositor can take a frame, then capture the desktop
        // as late as possible so the drops show what is behind them now.
        WaitForSingleObjectEx(g.frameWait, 100, TRUE);
        ReloadShadersIfChanged();

        auto now = clock::now();
        float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.05f);
        last = now;

        if (g.clearRequested.exchange(false)) {
            g.sim.Clear();
            g.clearMist = true;
        }
        if (g.wipeRequested.exchange(false)) g.sim.StartWipe();

        // Passing showers set how hard it rains right now.
        g.sky.rain = s.rain;
        g.sky.showers = s.weather;
        g.sky.outside = s.outside;
        g.sky.lightning = s.lightning;
        g.sky.gusts = s.gusts;
        g.sky.daylight = s.daylight;
        g.sky.raining = !s.paused;
        if (now - lastClock > std::chrono::seconds(10)) {
            UpdateClock();
            lastClock = now;
        }
        g.sim.intensity = s.paused ? 0.0f : s.rain * g.sky.Showers();
        g.sim.mist = s.mist;
        g.sim.friction = s.friction;
        g.sim.runSpeed = s.runSpeed;
        g.sim.minDrop = s.minDrop;
        g.sim.maxDrop = s.maxDrop;
        g.sim.streakLife = s.streakLife;
        g.sim.trailSpeed = s.trailSpeed;
        g.sim.beadSpeed = s.beadSpeed;
        g.sim.splash = s.splash;
        g.sim.burst = s.burst;
        g.sim.wind = s.wind;
        g.sim.gusts = s.gusts;
        g.sim.shake = g.sky.Shake() * s.shake;
        // A fingertip on the glass while Ctrl+Alt are held: the overlay
        // never takes the mouse, so ask where it is.
        {
            POINT cursor{};
            bool down = s.finger && GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(VK_MENU) < 0 &&
                        GetCursorPos(&cursor);
            g.sim.SetFinger(down, static_cast<float>(cursor.x - g.bounds.left),
                            static_cast<float>(cursor.y - g.bounds.top));
        }
        if (s.autoWipeMinutes > 0 && now - lastWipe > std::chrono::minutes(s.autoWipeMinutes)) {
            g.sim.StartWipe();
        }
        if (g.sim.GetWiper().active) lastWipe = now;

        g.sim.Update(dt);
        g.sky.Update(dt, g.sim.WindNow());

        // Sound: a tap for every raindrop that hit the glass, thunder for every flash.
        g.sound.SetLevels(s.volume, s.glassSound, s.outsideSound, s.thunder);
        g.sound.SetAmbience(s.windSound, s.drips, s.cars, s.windowOpen);
        g.sound.SetWeather(g.sky.RainRate(), g.sim.WindNow(), g.sky.Daylight());
        const float mmPerPixel = 336.0f / static_cast<float>(g.height);  // a 27" panel is 336 mm tall
        for (const RainSim::Hit& h : g.sim.Hits())
            g.sound.Tap(h.x / static_cast<float>(g.width) * 2.0f - 1.0f, h.r * mmPerPixel, h.glance, h.splash,
                        h.burst);
        for (const ThunderEvent& t : g.sky.TakeThunder())
            g.sound.Thunder(t.delay, t.distance, t.power, t.pan, std::min(t.rattle * s.shake, 1.0f));

        Render(std::chrono::duration<float>(now - start).count(), dt, s);
    }
    CoUninitialize();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // for WIC screenshots

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g.exeDir = fs::path(exePath).parent_path();
    g.shaderPath = g.exeDir / L"rain.hlsl";
    Log("---- raindesk starting ----");

    FindPrimaryOutput();
    CreateDevice();
    CreateOverlayWindow(inst);
    CreateSwapChainAndComposition();
    g.sim.Reset(static_cast<float>(g.width), static_cast<float>(g.height));
    g.sky.Reset(static_cast<float>(g.width), static_cast<float>(g.height));

    std::error_code ec;
    g.shaderTime = fs::last_write_time(g.shaderPath, ec);
    if (ec) g.shaderTime = {};
    // A rain.hlsl next to the exe that doesn't compile falls back to the built-in shaders.
    if (!CompileShaders() && !CompileShaders(true)) {
        MessageBoxW(nullptr, L"The shaders failed to compile. See raindesk.log next to the exe.", L"raindesk",
                    MB_ICONERROR);
        return 1;
    }

    ControlCallbacks cb;
    cb.wipe = [] { g.wipeRequested = true; };
    cb.clearNow = [] { g.clearRequested = true; };
    cb.screenshot = [] { g.screenshotRequested = true; };
    cb.changed = PublishSettings;
    cb.quit = [] { DestroyWindow(g.hwnd); };
    controls::Init(inst, g.exeDir / L"raindesk.ini", g.settings, cb);

    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    PublishSettings();

    std::thread renderThread(RenderLoop);
    g.soundOut.Start(&g.sound);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g.running = false;
    renderThread.join();
    g.soundOut.Stop();
    controls::Shutdown();
    Log("---- raindesk exiting ----");
    return 0;
}
