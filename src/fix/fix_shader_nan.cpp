#include "fix_shader_nan.h"

#include <d3d11.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "../core/hooks.h"
#include "../core/log.h"

namespace ggdpvr::fix {
namespace {
// ---- DXBC checksum: MD5 over everything after the 20-byte header, with D3D's own final block ----
// (verified against d3dcompiler_47 output by tools/dxbc_checksum.py)
void Md5Blocks(uint32_t st[4], const uint8_t* data, size_t len) {
    static const uint8_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static uint32_t K[64];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 64; ++i) K[i] = uint32_t(std::fabs(std::sin(double(i + 1))) * 4294967296.0);
        init = true;
    }
    for (size_t off = 0; off + 64 <= len; off += 64) {
        uint32_t M[16];
        memcpy(M, data + off, 64);
        uint32_t A = st[0], B = st[1], C = st[2], D = st[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16) F = (B & C) | (~B & D), g = i;
            else if (i < 32) F = (D & B) | (~D & C), g = (5 * i + 1) % 16;
            else if (i < 48) F = B ^ C ^ D, g = (3 * i + 5) % 16;
            else F = C ^ (B | ~D), g = (7 * i) % 16;
            F = F + A + K[i] + M[g];
            A = D, D = C, C = B;
            B = B + ((F << S[i]) | (F >> (32 - S[i])));
        }
        st[0] += A, st[1] += B, st[2] += C, st[3] += D;
    }
}
void DxbcChecksum(const uint8_t* blob, size_t size, uint8_t out[16]) {
    const uint8_t* data = blob + 20;
    const size_t n = size - 20;
    const uint32_t bits = uint32_t(n * 8);
    uint32_t st[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
    const size_t full = n - n % 64;
    Md5Blocks(st, data, full);
    const size_t rest = n - full;
    uint8_t blk[64]{};
    const uint32_t tail = (bits >> 2) | 1;
    if (rest >= 56) {
        memcpy(blk, data + full, rest);
        blk[rest] = 0x80;
        Md5Blocks(st, blk, 64);
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        memcpy(blk + 60, &tail, 4);
    } else {
        memcpy(blk, &bits, 4);
        memcpy(blk + 4, data + full, rest);
        blk[4 + rest] = 0x80;
        memcpy(blk + 60, &tail, 4);
    }
    Md5Blocks(st, blk, 64);
    memcpy(out, st, 16);
}

uint32_t U32(const std::vector<uint8_t>& b, size_t o) {
    uint32_t v;
    memcpy(&v, b.data() + o, 4);
    return v;
}
}  // namespace

int PatchGammaSaturate(std::vector<uint8_t>& b) {
    constexpr uint32_t kLog = 47, kMul = 56, kExp = 25, kMin = 51, kCustom = 53, kSat = 1u << 13;
    auto satOk = [](uint32_t op) { return op == 0 || op == 50 || op == 51 || op == 52 || op == 54 || op == 56; };
    if (b.size() < 32 || memcmp(b.data(), "DXBC", 4) != 0 || U32(b, 24) != b.size()) return 0;
    int patched = 0;
    const uint32_t nChunks = U32(b, 28);
    for (uint32_t c = 0; c < nChunks && 32 + 4 * c + 4 <= b.size(); ++c) {
        const size_t co = U32(b, 32 + 4 * c);
        if (co + 8 > b.size()) return 0;
        if (memcmp(b.data() + co, "SHDR", 4) != 0 && memcmp(b.data() + co, "SHEX", 4) != 0) continue;
        const size_t end = co + 8 + U32(b, co + 4);
        if (end > b.size()) return 0;
        struct Ins { size_t p; uint32_t op, n; };
        std::vector<Ins> ins;
        for (size_t p = co + 16; p + 4 <= end;) {
            const uint32_t tok = U32(b, p), op = tok & 0x7FF;
            const uint32_t n = op == kCustom ? (p + 8 <= end ? U32(b, p + 4) : 0) : (tok >> 24) & 0x7F;
            if (!n || p + size_t(n) * 4 > end) break;
            ins.push_back({p, op, n});
            p += size_t(n) * 4;
        }
        for (size_t k = 1; k + 3 < ins.size(); ++k) {
            const Ins &i0 = ins[k - 1], &i1 = ins[k], &i2 = ins[k + 1], &i3 = ins[k + 2], &i4 = ins[k + 3];
            if (i1.op != kLog || i2.op != kMul || i3.op != kExp || i4.op != kMin || !satOk(i0.op)) continue;
            if (i0.n < 3 || i1.n < 3 || memcmp(b.data() + i0.p + 4, b.data() + i1.p + 4, 8) != 0) continue;
            bool clampsToOne = false;  // the min must be against an immediate 1.0
            for (uint32_t w = 1; w < i4.n; ++w) clampsToOne |= U32(b, i4.p + 4 * w) == 0x3F800000u;
            if (!clampsToOne) continue;
            const uint32_t tok = U32(b, i0.p);
            if (tok & kSat) continue;
            const uint32_t nt = tok | kSat;
            memcpy(b.data() + i0.p, &nt, 4);
            ++patched;
        }
    }
    if (patched) DxbcChecksum(b.data(), b.size(), b.data() + 4);
    return patched;
}

namespace {
using CreatePsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11PixelShader**);
CreatePsFn o_CreatePs = nullptr;
std::atomic<int> g_seen{0}, g_patched{0};

HRESULT STDMETHODCALLTYPE hk_CreatePs(ID3D11Device* dev, const void* code, SIZE_T len, ID3D11ClassLinkage* link,
                                      ID3D11PixelShader** out) {
    ++g_seen;
    // cheap pre-filter: only shaders that bind the colour-grading LUT
    static const char kTag[] = "ColorGradingLUT";
    const auto* c = static_cast<const uint8_t*>(code);
    bool candidate = false;
    for (SIZE_T i = 0; code && len > sizeof kTag && i + sizeof kTag - 1 < len && !candidate; ++i)
        candidate = c[i] == 'C' && memcmp(c + i, kTag, sizeof kTag - 1) == 0;
    if (candidate) {
        std::vector<uint8_t> b(c, c + len);
        const int n = PatchGammaSaturate(b);
        if (n > 0) {
            const HRESULT hr = o_CreatePs(dev, b.data(), b.size(), link, out);
            if (SUCCEEDED(hr)) {
                LOG("[fix.shader] colour-grading shader #%d patched (%d instruction(s) saturated, %zu bytes)",
                    ++g_patched, n, size_t(len));
                return hr;
            }
            LOG("[fix.shader] patched shader rejected (hr=0x%08lX); using the original", hr);
        }
    }
    return o_CreatePs(dev, code, len, link, out);
}
}  // namespace

bool InstallShaderNanFix(void** vt) {
    // ID3D11Device vtable: IUnknown (3), CreateBuffer, CreateTexture1D/2D/3D, CreateSRV, CreateUAV, CreateRTV,
    // CreateDSV, CreateInputLayout, CreateVertexShader, CreateGeometryShader, CreateGSWithStreamOutput,
    // CreatePixelShader = slot 15
    const bool ok = hooks::CreateAt("ID3D11Device::CreatePixelShader", vt[15], (void*)&hk_CreatePs, (void**)&o_CreatePs);
    LOG("[fix.shader] CreatePixelShader hook %s", ok ? "created" : "FAILED");
    return ok;
}
}  // namespace ggdpvr::fix
