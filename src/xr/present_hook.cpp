#include "depth_probe.h"
#include "present_hook.h"

#include <windows.h>
#include <d3d11.h>

#include "../core/hooks.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../fix/fix_shader_nan.h"

namespace ggdpvr::xr {
namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn o_Present = nullptr;
PresentCallback g_cb = nullptr;

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (g_cb && !(flags & DXGI_PRESENT_TEST) && g_cb(sc)) return S_OK;  // consumed by VR only
    return o_Present(sc, sync, flags);
}

std::string ModuleOf(void* p) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(p), &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    return name;
}
}  // namespace

bool InstallPresentHook(PresentCallback cb) {
    g_cb = cb;
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                              GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))
                        : nullptr;
    if (!create) {
        LOG("[present] d3d11.dll / D3D11CreateDeviceAndSwapChain unavailable");
        return false;
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GGDPVR_DummyWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev,
                        nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        void* present = vtbl[8];
        LOG("[present] IDXGISwapChain::Present = %p in %s", present, ModuleOf(present).c_str());
        ok = hooks::CreateAt("IDXGISwapChain::Present", present, (void*)&hk_Present, (void**)&o_Present);
        if (config::Get().fixShaderNanClamp && dev) fix::InstallShaderNanFix(*reinterpret_cast<void***>(dev));
        if (dev) InstallDepthProbe(*reinterpret_cast<void***>(dev));  // diagnostic: the game's depth buffers
        ok = ok && hooks::EnableAll();
    } else {
        LOG("[present] dummy device creation failed hr=0x%08lX", hr);
    }
    if (ctx) ctx->Release();
    if (sc) sc->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
}  // namespace ggdpvr::xr
