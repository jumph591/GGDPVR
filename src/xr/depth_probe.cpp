// Diagnostic: the game's depth buffers. Logs every depth-stencil texture the game creates (size, format, whether it
// can also be read by a shader), its depth-stencil views and any shader-readable views made of it - to see whether
// the scene depth can be sampled when the HUD is composited (HUD behind a grabbing girl).
#include "depth_probe.h"
#include <d3d11.h>
#include <atomic>
#include "../core/hooks.h"
#include "../core/log.h"

namespace ggdpvr::xr {
namespace {
using CreateTex2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using CreateDsvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
CreateTex2DFn o_CreateTex2D = nullptr;
CreateSrvFn o_CreateSrv = nullptr;
CreateDsvFn o_CreateDsv = nullptr;
std::atomic<int> g_logs{0};
constexpr int kMaxLogs = 60;

bool DepthDesc(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    return (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
}

HRESULT STDMETHODCALLTYPE hk_CreateTex2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init,
                                         ID3D11Texture2D** out) {
    const HRESULT hr = o_CreateTex2D(dev, d, init, out);
    if (d && (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) && g_logs.fetch_add(1) < kMaxLogs)
        LOG("[depth] texture %ux%u format %d samples %u mips %u array %u bind 0x%X (shader-readable %s) misc 0x%X -> %p hr 0x%08lX",
            d->Width, d->Height, int(d->Format), d->SampleDesc.Count, d->MipLevels, d->ArraySize, d->BindFlags,
            (d->BindFlags & D3D11_BIND_SHADER_RESOURCE) ? "YES" : "no", d->MiscFlags, out ? (void*)*out : nullptr, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateSrv(ID3D11Device* dev, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* v,
                                       ID3D11ShaderResourceView** out) {
    const HRESULT hr = o_CreateSrv(dev, r, v, out);
    D3D11_TEXTURE2D_DESC d;
    if (DepthDesc(r, d) && g_logs.fetch_add(1) < kMaxLogs)
        LOG("[depth] shader view of depth texture %p (%ux%u) as format %d dim %d -> hr 0x%08lX", (void*)r, d.Width, d.Height,
            v ? int(v->Format) : -1, v ? int(v->ViewDimension) : -1, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateDsv(ID3D11Device* dev, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* v,
                                       ID3D11DepthStencilView** out) {
    const HRESULT hr = o_CreateDsv(dev, r, v, out);
    D3D11_TEXTURE2D_DESC d{};
    DepthDesc(r, d);
    if (g_logs.fetch_add(1) < kMaxLogs)
        LOG("[depth] depth view of %p (%ux%u) as format %d flags 0x%X -> hr 0x%08lX", (void*)r, d.Width, d.Height,
            v ? int(v->Format) : -1, v ? v->Flags : 0, hr);
    return hr;
}
}  // namespace

bool InstallDepthProbe(void** vt) {
    // ID3D11Device: 5 CreateTexture2D, 7 CreateShaderResourceView, 10 CreateDepthStencilView
    const bool ok = hooks::CreateAt("ID3D11Device::CreateTexture2D", vt[5], (void*)&hk_CreateTex2D, (void**)&o_CreateTex2D) &&
                    hooks::CreateAt("ID3D11Device::CreateShaderResourceView", vt[7], (void*)&hk_CreateSrv, (void**)&o_CreateSrv) &&
                    hooks::CreateAt("ID3D11Device::CreateDepthStencilView", vt[10], (void*)&hk_CreateDsv, (void**)&o_CreateDsv);
    LOG("[depth] probe hooks %s", ok ? "created" : "FAILED");
    return ok;
}
}  // namespace ggdpvr::xr
