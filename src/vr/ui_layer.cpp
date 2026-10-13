#include "ui_layer.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../ue3/objects.h"
#include "../xr/xr_shared.h"
#include "head_tracking.h"
#include "controllers.h"
#include "menu_pointer.h"
#include "../xr/xr_input.h"
#include "menu_pointer.h"
#include "gfx_vars.h"

namespace ggdpvr::vr::ui {
namespace {
// FGFxRenderUI (UE3 render command, vtable rva 0x2633DD8): Execute = slot 1, returns the command size.
constexpr uint32_t kGfxRenderUiExecute = 0xD77A60;
constexpr uint8_t kGfxExecBytes[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x51, 0x10, 0x48, 0x8B, 0x49, 0x08};
constexpr uint32_t kGfxRenderUiCmdSize = 0x28;

using ExecFn = uint32_t(__fastcall*)(void*);
ExecFn o_Exec = nullptr;
std::atomic<bool> g_enabled{true};

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release(), p = nullptr;
}

// ---- render-thread resources ----
struct Target {  // RGBA8 texture usable as render target and shader input
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0;
    bool Create(ID3D11Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt, bool rt, bool mips = false) {
        Release();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width, d.Height = height, d.MipLevels = mips ? 0 : 1, d.ArraySize = 1, d.Format = fmt;
        d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0);
        d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        const bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &tex)) &&
                        SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &srv)) &&
                        (!rt || SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv)));
        if (!ok) Release();
        w = width, h = height;
        return ok;
    }
    void Release() { SafeRelease(srv), SafeRelease(rtv), SafeRelease(tex); }
};
Target g_save, g_black, g_white;  // backbuffer-sized: saved eye image, UI over black, UI over white
Target g_ui;                      // panel image: unpremultiplied RGBA, the 16:9 HUD band (+ rows above / below: tall)
std::atomic<bool> g_tallPanel{false};   // minigames: the panel shows the whole canvas height
std::atomic<bool> g_hudHidden{false};   // the player hid the gameplay HUD (hud_toggle_button)
std::atomic<bool> g_hudToggleCtx{false}; // ...which only applies in stages / boss battles in play
// Doki Doki Mode's "Double Peace" finish (every girl's meter overfilled: a white border with flowers, _root.effWPeace in
// GG2HudDokiLightMovie): while it shows, that movie goes to the full-screen layer - the headset's periphery, like the
// damage effect - instead of the panel (0.9.25)
std::atomic<bool> g_dokiPeaceFull{false};
std::atomic<float> g_panelExtFrac{0.f};  // rows above (and below) the band on the panel, as a fraction of the band
Target g_fs;                      // full-screen effects: unpremultiplied RGBA, the 16:9 band
Target g_eo;                      // eye overlay (weak-spot pop-ups etc.): unpremultiplied RGBA, the whole frame
Target g_sco;                     // the game's zoom scope movie (kScope): unpremultiplied RGBA, the whole frame
// reticle_style=1 (0.9.86): the game's sight movie (kSight), drawn at the band centre (its SetSightPosition replaced),
// captured as a square around that centre and drawn on the reticle point in each eye
Target g_sgt;
bool g_sgtValid = false;
// name_tags_3d (0.9.113): the targeted girl's name tag (slot 0) when the panel can't show her (beside / behind the game
// camera, or beyond the panel's edges): the movie draws it at a reserved spot under the HUD band, that region is cut out
// of the panel image and drawn into each eye at her head as that eye sees it
constexpr float kTagParkX = 960.f, kTagParkY = 1380.f;           // stage units (rows under the band: empty in stages)
constexpr float kTagBoxW = 900.f, kTagBoxUp = 250.f, kTagBoxDown = 100.f;  // the region around that spot (stage units)
std::atomic<ULONGLONG> g_tagParkT{0};       // game thread: the tag was parked for 3D (last time)
std::atomic<float> g_tagEyeX[2]{}, g_tagEyeY[2]{};  // her tag's position in each eye's view (stage units, as the game sent)
std::atomic<ULONGLONG> g_tagEyeT[2]{};
std::atomic<bool> g_tagTestOn{false};       // dev: !tagtest <x> <y>
std::atomic<float> g_tagTestX{960.f}, g_tagTestY{400.f};
Target g_tag, g_tagZero;                    // render thread: the cut-out tag; zeros to blank its spot on the panel
bool g_tagValid = false;
uint32_t g_sgtX0 = 0, g_sgtY0 = 0;  // the square's top-left in the frame
Target g_tintB, g_tintW;          // 1x1: a tint movie's corner pixel over black / white
Target g_scv;                     // zoom scope render (third draw, narrow FOV towards the reticle), as drawn
// Layered menu movies (title screen): parts of one movie rendered separately (others hidden through the movie's own
// _visible flags): layer 0 = its background on a sphere around the player, 1..3 = cut-outs at their own depths.
Target g_lay[4];
bool g_layValid[4] = {};
float g_layDist[4] = {};
const float* LayerCropOf(int l);
bool TitleLayersActive();  // the title screen's layered menu (below)
extern std::atomic<float> g_bgCropU, g_bgCropV;
int LayerBgMode();
const float* LayerPan();
struct LayerSpec;
const LayerSpec& ActiveLayerSpec();
bool LayerSplit(int& layer, float crop[4], float& dist);
// an angular span adjusted so a whole number of repeats (period = span x periodsPerSpan) fits around 360 degrees
float WholeTurns(float span, float periodsPerSpan) {
    const float period = span * periodsPerSpan, n = std::fmax(1.f, std::round(6.2831853f / period));
    return 6.2831853f / n / periodsPerSpan;
}
extern std::atomic<float> g_bgCaptureTime;
extern std::atomic<uintptr_t> g_drawnPlayers[24];
extern std::atomic<bool> g_roomDebug;
extern std::atomic<bool> g_cutoutsOn;
extern std::atomic<uint32_t> g_cutMask;
extern std::atomic<uint32_t> g_layMeasured[4];
extern std::atomic<float> g_layDiff[4];
extern std::atomic<uint64_t> g_layMeasT[4];
std::atomic<bool> g_shopVoid{false};  // Academy Store: its 3D booth stands in a black void
uint64_t g_scvFrame = ~0ull;
float g_lastRx = 0, g_lastRy = 0;  // dev: last reticle pixel
D3D11_TEXTURE2D_DESC g_desc{};
ID3D11RenderTargetView* g_bbRtv = nullptr;  // created per use: a held view would block backbuffer resizes
bool g_fsValid = false, g_tintValid = false, g_eoValid = false, g_scoValid = false;
// The game's full-screen movies (fades, flashes) are drawn over the panel's content too - but only when it draws them
// above a panel movie: the prologue's closing text box is drawn on top of a black fade (0.9.44: in VR the fade
// hid it). Set by Classify (render thread).
bool g_fsOverPanel = true;
bool g_cursorMovieUp = false;  // the game's mouse cursor movie is drawn (Classify, render thread)
// The per-eye effect movie (weak-spot pop-ups, boss "!" and kiss marks) takes eye-image positions; Scaleform draws its stage
// from the HUD band's top, so the composite used to shift the overlay up by that much - and anything meant for the
// bottom band-offset of the eye image was drawn below the backbuffer, lost (0.9.64: weak-spot pop-ups sometimes missing when
// hitting girls outside the flat screen area). The movie's root is moved up by the band offset instead (EffectRootTick);
// with it moved, the composite takes the overlay as is (pixel row = eye-image row).
// effect_stereo (0.9.70 A): the game places the effect movie's 3D items (weak-spot pop-ups, boss marks) once per eye draw, each at
// that eye's projection, but the layer is one Scaleform snapshot - the last (second) eye's positions - drawn into both
// eyes, so the items had no disparity (at infinity: "weak-spot pop-ups on distant girls very large"). The per-eye x the game sends
// is recorded here (game thread, values untouched) and the composite shifts the layer sideways per eye by the
// difference (render only).
struct StereoItem {
    double x[2]{};
    bool has[2]{};
    ULONGLONG t = 0;
    float distM = 0.f;  // smoothed measured distance (weak_spot_distance_scale)
};
std::unordered_map<uint32_t, StereoItem> g_stItems;  // game thread; key: kind << 16 | id
std::atomic<float> g_eoShiftD{0.f};                  // stage units: left-eye x minus right-eye x of the live 3D items
std::atomic<ULONGLONG> g_eoItemsT{0};                // when live 3D items last gave it (0 = none up)
std::atomic<float> g_eoRest{0.f};                    // effect_rest_depth: the HUD panel depth's disparity
std::atomic<bool> g_stageRows{false};                // hud_extend_rows (0.9.70 B)
std::atomic<bool> g_aeResultUp{false};  // a minigame results screen is up (the panel stops following the gaze)
// "Mom is coming" (mamakita: the retro-game cover screen, Back key): in the flat game it covers the whole screen - a
// black heart grows over the view, then the retro screen. The movie stays on the panel (transparent around the heart)
// and the world around it takes the panel's corner colour as the corners get covered (0.9.46)
bool g_coverWorld = false;
uint32_t g_uiExtRows = 0, g_uiBandW = 0, g_uiBandH = 0;  // the panel image's band (for the corner samples)
uint64_t g_fsFrame = ~0ull;
uint64_t g_captures = 0;

// pipeline, run in a private device-context state so UE3's cached D3D state is untouched
ID3DDeviceContextState* g_state = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader *g_psCombine = nullptr, *g_psComposite = nullptr;
ID3D11Buffer *g_cbCombine = nullptr, *g_cbComposite = nullptr;
int g_backdrop = 0;  // Collection backdrop this frame: 1 = the background movie on a sphere, 2 = white (profile)
std::atomic<uint64_t> g_asTraceUntil{0};  // dev: log every ActionScript call until then  // Collection background shown as a backdrop this frame
ID3D11RasterizerState* g_rs = nullptr;
ID3D11BlendState* g_blendOver = nullptr;
ID3D11SamplerState* g_linear = nullptr;
bool g_pipelineFailed = false;

const char kShader[] = R"(
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

// UI drawn over black (B) and over white (W) -> unpremultiplied colour + coverage.
// Tint movies (a uniform full-stage background colour T, sampled at the stage corner into TB/TW) are handed
// over from the panel to the world across a feathered band at the panel's edge: in the band the panel keeps
// only a share ap of the tint and the world tint (Composite) supplies aw, with (1-aw)(1-ap) = 1-Ta, so the
// total darkening is exactly the game's and there is no hard edge to misalign.
cbuffer C : register(b0) {
    int2 off; int tintOn; float feather; float2 band; float2 pad;  // pad.x: apply the full-screen overlays (FS);
                                                                   // pad.y: rows of canvas above the band (tall panel)
    float4 zoom;   // x = factor, y = on
    float4 zp0;    // panel centre (eye space), half-width and half-height vectors, eye projection
    float4 zpu;
    float4 zpv;
    float4 zproj;  // tan(halfH), tan(halfV), eye image width, height
};
SamplerState S : register(s0);
Texture2D<float4> ZEYE : register(t4);  // the eye image (no UI) for the zoomed view
Texture2D<float4> CFS : register(t5);   // the full-screen overlays (fades, flashes) of this frame, band-sized
Texture2D<float4> B : register(t0);
Texture2D<float4> W : register(t1);
Texture2D<float4> CTB : register(t2);
Texture2D<float4> CTW : register(t3);
float EdgeWeight(float2 uv, float f) {  // uv in [-1,1]: 1 inside, falling to 0 at the outline over width f
    float e = min(1 - abs(uv.x), 1 - abs(uv.y));
    return saturate(e / f);
}
float4 Combine(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(int2(pos.xy) + off, 0);
    float2 bp = pos.xy - float2(0, pad.y);  // position in the band
    float3 b = B.Load(p).rgb, w = W.Load(p).rgb;
    float a = saturate(1.0 - dot(w - b, float3(1, 1, 1) / 3.0));
    if (tintOn != 0) {
        float wt = EdgeWeight(bp / band * 2 - 1, feather);
        if (wt < 1) {
            float3 tb = CTB.Load(int3(0, 0, 0)).rgb, tw = CTW.Load(int3(0, 0, 0)).rgb;
            float ta = min(saturate(1.0 - dot(tw - tb, float3(1, 1, 1) / 3.0)), 0.98);
            if (ta > 0.004) {
                // un-composite the uniform tint (premultiplied tb, alpha ta) from under the rest, then re-add a share
                float xa = saturate((a - ta) / (1 - ta));
                float3 xp = max(b - tb * (1 - xa), 0);
                float ap = 1 - pow(1 - ta, wt);
                b = xp + (tb / ta) * ap * (1 - xa);
                a = xa + ap * (1 - xa);
            }
        }
    }
    float3 c = a > 0.004 ? saturate(b / a) : float3(0, 0, 0);
    if (pad.x != 0) {  // the game draws its fades and flashes over the HUD too: so does the panel (its content only;
                       // the world behind already has them)
        float4 f = CFS.Load(int3(int2(bp), 0));
        c = lerp(c, f.rgb, f.a);
    }
    if (zoom.y != 0) {  // right-click zoom: the panel becomes a window onto the view magnified about its centre
        float2 uv = float2(bp.x / band.x * 2 - 1, 1 - bp.y / band.y * 2) / zoom.x;
        float3 d = zp0.xyz + zpu.xyz * uv.x + zpv.xyz * uv.y;
        float2 t = float2(d.x / -d.z / zproj.x, d.y / -d.z / zproj.y);
        float3 scene = ZEYE.SampleLevel(S, float2(0.5 * (1 + t.x), 0.5 * (1 - t.y)), 0).rgb;
        return float4(c * a + scene * (1 - a), 1);
    }
    return float4(c, a);
}

// Over an eye image (a copy of it in EYE): the tint (world share, blended in linear light like the headset
// compositor blends the panel), then the full-screen effects mapped onto the rect `vis` (the same rect in
// both eyes, so they sit at infinity), blended as the game blends them.
cbuffer K : register(b0) {
    float4 vis;
    float4 p0;     // panel centre, eye space (xyz); w = panel valid
    float4 pu;     // panel half-width vector, eye space
    float4 pv;     // panel half-height vector, eye space
    float4 proj;   // tan(halfH), tan(halfV), image width, image height
    float4 misc;   // feather, reticle x, y, radius
    int4 flags;    // fs valid, tint valid, reveal radius, reticle on
    float4 scope;  // zoom scope: magnification, radius px, centre x, y (magnification <= 1: off)
    float4 bdrop;  // x = backdrop: 1 the full-screen layer's HUD band [y, z] (fractions) on a sphere, 2 plain white
    float4 eo;     // eye overlay: x = valid, z = rows to shift up (the HUD band's top), w = image height
    float4 sco;    // game scope movie: x = valid, yz = offset moving its centre onto the reticle
    float4 rx;     // eye -> recentered frame rotation (columns), for the backdrop sphere
    float4 ry;
    float4 rz;
    float4 scv;    // x = the zoom scope render (SCV, same size as the eye image, centred on the reticle) is valid
    float4 vbd;    // Dressing Room: xyz = world up in the recentered frame, w = on
    float4 tm;     // x = seconds (animation), y = room wall height (uu)
    float4 vbc;    // room centre on the floor (recentered frame, metres), w = Unreal units per metre
    float4 vbe;    // this eye's position (recentered frame, metres), w = room radius (uu)
    float4 lbg;    // x = layered menu: background layer (L0) on a sphere
    float4 lc[3];  // cut-out layers 1..3: plane centre (eye space), w = valid
    float4 lu[3];  // half-width vectors
    float4 lv[3];  // half-height vectors
    float4 lcrop[4];  // per layer: the part used (u0, v0, u1, v1)
    float4 lpan;      // background pan mode: xy = scroll (band fractions/s), z = seconds since its capture
    float4 sc, su, sv, scrop;  // split-off part of a layer (its own cut-out): plane, crop; sc.w = layer (1..3, 0 = none)
    float4 vbf;    // Academy Store: horizontal direction into the booth (recentered frame); w = set
    float4 eyeh;   // x = eye height (m): the title's floor below the recentered head; y = Academy Store void white;
                   // z = the booth's visible base above the store floor (uu)
    float4 cov;    // Mom is coming: x = on, y z = band width / height in UI, w = the band's top row in UI
    float4 dk;     // Doki Doki's Double Peace border: x = on, y = image v where its flower strip begins, z = the strip's
                   // top on screen (px): the strip keeps its true shape, the white glow above stretches to fill
    float4 sgt;    // the game's sight (reticle_style=1): x = on, yz = this eye's offset into its square, w = square size
    float4 sgt2;   // its centre squeeze (sight_center_scale): x = scale, y = R0, z = R1 (px)
    float4 tag;    // name_tags_3d: x = on, yz = her tag's anchor in this eye (px), w = eye px per tag texel
    float4 tag2;   // the anchor inside the tag image (px) xy, its size zw
};
Texture2D<float4> FS : register(t0);
Texture2D<float4> TB : register(t1);
Texture2D<float4> TW : register(t2);
Texture2D<float4> EYE : register(t3);
Texture2D<float4> EO : register(t4);
Texture2D<float4> SCO : register(t5);
Texture2D<float4> SCV : register(t6);
Texture2D<float4> L0 : register(t7);
Texture2D<float4> L1 : register(t8);
Texture2D<float4> L2 : register(t9);
Texture2D<float4> L3 : register(t10);
Texture2D<float4> UI : register(t11);  // the panel image (unpremultiplied)
Texture2D<float4> SGT : register(t12);  // the game's sight, a square around its centre (unpremultiplied)
Texture2D<float4> TAG : register(t13);  // name_tags_3d: the cut-out name tag (unpremultiplied)
float4 Cutout(Texture2D<float4> T, float4 c, float4 u, float4 v, float4 crop, float2 px) {  // a layer on a plane
    if (c.w == 0) return float4(0, 0, 0, 0);
    float3 d = float3((px.x / proj.z * 2 - 1) * proj.x, (1 - px.y / proj.w * 2) * proj.y, -1);
    float3 n = cross(u.xyz, v.xyz);
    float den = dot(d, n);
    if (abs(den) < 1e-6) return float4(0, 0, 0, 0);
    float t = dot(c.xyz, n) / den;
    if (t <= 0) return float4(0, 0, 0, 0);
    float3 h = t * d - c.xyz;
    float2 uv = float2(dot(h, u.xyz) / dot(u.xyz, u.xyz), dot(h, v.xyz) / dot(v.xyz, v.xyz));
    float2 tc = float2(uv.x * 0.5 + 0.5, 0.5 - uv.y * 0.5);
    if (abs(uv.x) >= 1 || abs(uv.y) >= 1 || any(tc < crop.xy) || any(tc > crop.zw)) return float4(0, 0, 0, 0);
    return T.SampleLevel(S, tc, 0);
}
float Mirror(float u) { return 1 - abs(fmod(abs(u), 2.0) - 1); }
float3 ToLin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 ToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float PanelWeight(float2 px) {  // the panel's edge weight at this eye pixel (0 outside the panel)
    if (p0.w == 0) return 0;
    float3 d = float3((px.x / proj.z * 2 - 1) * proj.x, (1 - px.y / proj.w * 2) * proj.y, -1);
    float3 n = cross(pu.xyz, pv.xyz);
    float den = dot(d, n);
    if (abs(den) < 1e-6) return 0;
    float t = dot(p0.xyz, n) / den;
    if (t <= 0) return 0;
    float3 h = t * d - p0.xyz;
    float2 uv = float2(dot(h, pu.xyz) / dot(pu.xyz, pu.xyz), dot(h, pv.xyz) / dot(pv.xyz, pv.xyz));
    if (abs(uv.x) >= 1 || abs(uv.y) >= 1) return 0;
    return saturate(min(1 - abs(uv.x), 1 - abs(uv.y)) / misc.x);
}
)" R"(
// Dressing Room: the vanilla backdrop (a bubble-wallpaper wall on a floor slab, in a black void) is hidden and
// this room is drawn around the player instead, in every exactly-black pixel (the props have none). Room frame in
// Unreal units: origin = the room centre on the floor (under the game camera), y up, z the camera's horizontal
// forward. A floor; a round wall of the vanilla wall's distance and height; above it a dome cap. Colours follow the
// vanilla backdrop: wall and floor light where they meet, the wall darkening in its top rows (bubble rows on per-row
// bands, light lines between the dark ones, rows drifting in alternate directions), the floor deepening to orange
// toward the player with three light stripes.
)" R"(
float3 RoomWall(float s, float az, float rloc) {
    const float rowP = 82, rad = 36, cellP = 108, TAU = 6.2831853;
    float row = floor(s / rowP);
    // bubbles per row from the radius at the row's centre (per pixel, the count changed partway up a row on the
    // dome and sliced its bubbles)
    float sc = (row + 0.5) * rowP, Rw = vbe.w, Hw = tm.y;
    float rc = sc <= Hw ? Rw : Rw * cos(min((sc - Hw) / Rw, 1.5707));
    float n = max(1.0, round(TAU * rc / cellP));
    float dirn = fmod(row, 2.0) < 0.5 ? 1.0 : -1.0;
    float u = az / TAU * n + dirn * tm.x * 0.18;
    float dx = (frac(u) - 0.5) * TAU * rloc / n, dy = s - (row + 0.5) * rowP;
    float dist = length(float2(dx, dy));
    float aa = max(fwidth(dist), 0.5);
    float bub = (1 - smoothstep(rad - aa, rad + aa, dist)) * saturate((rc - 120) / 200);
    float3 bg = row < 3.5 ? float3(1.0, 0.824, 0.761)
              : row < 4.5 ? float3(1.0, 0.812, 0.698)
              : row < 5.5 ? float3(1.0, 0.796, 0.596)
              : lerp(float3(1.0, 0.694, 0.49), float3(1.0, 0.62, 0.42), saturate((s - 6 * rowP) / 1200));
    float3 c = lerp(bg, float3(1.0, 0.851, 0.761), bub);
    if (row > 4.5) {  // thin light lines between the dark rows
        float ld = abs(s - round(s / rowP) * rowP), la = max(fwidth(s), 0.5);
        c = lerp(c, float3(1.0, 0.86, 0.79), 0.75 * (1 - smoothstep(2.0 - la, 2.0 + la, ld)));
    }
    return c;
}
float3 RoomFloor(float w) {  // w = distance in from the wall
    const float k[8] = {0, 350, 432, 491, 538, 564, 597, 616};
    const float3 c[8] = {float3(1, .824, .761), float3(1, .824, .761), float3(1, .808, .643), float3(1, .776, .565),
                         float3(1, .729, .514), float3(1, .706, .482), float3(1, .671, .451), float3(1, .651, .459)};
    float3 col = lerp(c[7], float3(1, .62, .42), saturate((w - 616) / 600));
    [unroll] for (int i = 6; i >= 0; --i)
        if (w < k[i + 1]) col = lerp(c[i], c[i + 1], saturate((w - k[i]) / (k[i + 1] - k[i])));
    float la = max(fwidth(w), 0.4);
    float st = max(max(1 - smoothstep(1.6 - la, 1.6 + la, abs(w - 479)), 1 - smoothstep(1.6 - la, 1.6 + la, abs(w - 521))),
                   1 - smoothstep(1.6 - la, 1.6 + la, abs(w - 576)));
    return lerp(col, float3(1, .812, .733), st);
}
)" R"(
// Academy Store: the booth (a wall section with Aoi's window) stands in a black void. The corridor around it: its
// front wall continued left and right (the booth's grey-green with its green trim), a wall behind the player, the
// school corridor's teal floor and a pale ceiling with light panels. Room frame (uu): origin = the booth front's centre
// at the floor, y up, z toward the booth (the front wall is z = 0, the wall behind z = -radius).
float3 CorridorWall(float x, float y, float Hc) {
    const float3 wall = float3(159, 173, 162) / 255, trim = float3(37, 115, 98) / 255;
    float aa = max(fwidth(y), 0.5);
    float base = 1 - smoothstep(14 - aa, 14 + aa, y);       // baseboard
    float top = smoothstep(Hc - 22 - aa, Hc - 22 + aa, y);   // band under the ceiling
    float3 c = wall * (0.97 + 0.03 * saturate(y / Hc));
    float seam = 1 - smoothstep(0.6 - max(fwidth(x), 0.3), 0.6 + max(fwidth(x), 0.3), abs(frac(x / 160 + 0.5) - 0.5) * 160);
    c *= 1 - 0.06 * seam;                                     // panel joints
    return lerp(c, trim, max(base, top));
}
// Academy Store: the floor seen along a ray (room frame), ignoring the booth: a pool of the corridor's teal tiles in
// front of the booth (running on under it), fading into the void
float3 StoreVoid() { return eyeh.y != 0 ? float3(0.985, 0.99, 1.0) : float3(0, 0, 0); }
float3 StoreFloor(float3 o, float3 d, float Wx, float Db) {
    const float3 vd = StoreVoid();
    float tf = d.y < -1e-5 ? -o.y / d.y : 1e9;
    if (!(tf > 0 && tf < 1e8)) return vd;
    float3 q = o + d * tf;
    // in front: an oval reaching back to the player; behind the booth's front (z > 0) it runs on further, so the
    // booth's faded base shows the floor continuing under it
    float zr = q.z > -Db * 0.45 ? Db * 1.6 : Db * 0.75;
    float r = length(float2(q.x / max(Wx * 1.25, 1), (q.z + Db * 0.45) / max(zr, 1)));
    float k = 1 - smoothstep(0.55, 1.05, r);
    float2 g = abs(frac(q.xz / 60 + 0.5) - 0.5) * 60;
    float aa = max(max(fwidth(q.x), fwidth(q.z)), 0.4);
    float joint = 1 - smoothstep(0.5 - aa, 0.5 + aa, min(g.x, g.y));
    return lerp(vd, lerp(float3(30, 104, 88) / 255, float3(22, 82, 69) / 255, joint * 0.8), k);
}
// Academy Store: on the booth's own front wall, a darkening toward its outer sides (outside the window opening, which
// spans the middle ~55%), so it fades into the dark surround instead of ending in a hard edge. 1 elsewhere.
static float g_dbgQx = 1e9;
float StoreEdgeShade(float2 px) {
    float3 de = normalize(float3((px.x / proj.z * 2 - 1) * proj.x, (1 - px.y / proj.w * 2) * proj.y, -1));
    float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
    float3 U = normalize(vbd.xyz);
    float3 F0 = vbf.w != 0 ? vbf.xyz : float3(0, 0, -1);
    float3 F = F0 - U * dot(F0, U);
    F = dot(F, F) > 1e-6 ? normalize(F) : float3(0, 0, -1);
    float3 Rt = cross(F, U);
    float3 e = (vbe.xyz - vbc.xyz) * vbc.w;
    float3 o = float3(dot(e, Rt), dot(e, U), dot(e, F));
    float3 d = float3(dot(dr, Rt), dot(dr, U), dot(dr, F));
    float Wx = tm.w;
    float tw = d.z > 1e-5 ? -o.z / d.z : 1e9, tf = d.y < -1e-5 ? -o.y / d.y : 1e9;
    // every non-black pixel here is the booth's own mesh: shade by where its ray meets the front plane, also for the
    // baseboard below the drawn floor's line (it counted as floor and stayed bright - 0.9.6)
    if (!(tw > 0 && tw < 1e8)) return 1;
    float3 q = o + d * tw;
    if (q.y < -120) return 1;
    g_dbgQx = q.x;
    return 1 - 0.95 * smoothstep(0.56 * Wx, 0.94 * Wx, abs(q.x));  // from just outside the window frame (shutter +-101)
}
// Academy Store: the booth's wall carried down to the floor, which lies below its base (eyeh.z uu): on the front
// plane, from the floor up to its base + B + P, the game's own pixels are re-sampled - the baseboard (B) moved down
// onto the floor, the plain wall above it (P) stretched over the gap (0.9.14: the floor's depth felt right, the
// booth fading out at the bottom odd). False elsewhere (or where the source pixel isn't the booth).
bool StoreWallDown(float2 px, out float3 colOut) {
    colOut = float3(0, 0, 0);
    if (eyeh.z <= 0) return false;
    float3 de = normalize(float3((px.x / proj.z * 2 - 1) * proj.x, (1 - px.y / proj.w * 2) * proj.y, -1));
    float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
    float3 U = normalize(vbd.xyz);
    float3 F0 = vbf.w != 0 ? vbf.xyz : float3(0, 0, -1);
    float3 F = F0 - U * dot(F0, U);
    F = dot(F, F) > 1e-6 ? normalize(F) : float3(0, 0, -1);
    float3 Rt = cross(F, U);
    float3 o = float3(dot((vbe.xyz - vbc.xyz) * vbc.w, Rt), dot((vbe.xyz - vbc.xyz) * vbc.w, U), dot((vbe.xyz - vbc.xyz) * vbc.w, F));
    float3 d = float3(dot(dr, Rt), dot(dr, U), dot(dr, F));
    if (d.z <= 1e-5) return false;
    float3 q = o + d * (-o.z / d.z);
    const float B = 16, P = 14;
    float top = eyeh.z + B + P;
    if (q.y < 0 || q.y >= top || abs(q.x) > tm.w) return false;
    float ys = q.y < B ? eyeh.z + q.y : eyeh.z + B + (q.y - B) * P / (top - B);
    float3 v = float3(q.x, ys, 0) - o;
    float3 sr = Rt * v.x + U * v.y + F * v.z;
    float3 se = float3(dot(rx.xyz, sr), dot(ry.xyz, sr), dot(rz.xyz, sr));
    if (se.z > -1e-5) return false;
    float2 sp = float2((se.x / -se.z / proj.x + 1) * 0.5 * proj.z, (1 - se.y / -se.z / proj.y) * 0.5 * proj.w);
    if (!(sp.x >= 0 && sp.y >= 0 && sp.x < proj.z && sp.y < proj.w)) return false;  // (NaN fails too)
    float3 c = EYE.Load(int3(int2(sp), 0)).rgb;
    if (max(c.r, max(c.g, c.b)) < 0.5 / 255) return false;
    colOut = c;
    return true;
}
float3 Corridor(float2 px) {
    float3 de = normalize(float3((px.x / proj.z * 2 - 1) * proj.x, (1 - px.y / proj.w * 2) * proj.y, -1));
    float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
    float3 U = normalize(vbd.xyz);
    float3 F0 = vbf.w != 0 ? vbf.xyz : float3(0, 0, -1);  // the booth's facing, not the camera's
    float3 F = F0 - U * dot(F0, U);
    F = dot(F, F) > 1e-6 ? normalize(F) : float3(0, 0, -1);
    float3 Rt = cross(F, U);
    float3 e = (vbe.xyz - vbc.xyz) * vbc.w;
    float3 o = float3(dot(e, Rt), dot(e, U), dot(e, F));
    float3 d = float3(dot(dr, Rt), dot(dr, U), dot(dr, F));
    float Db = vbe.w, Hc = tm.y, Wx = tm.w;
    float t = 1e9;
    int hit = 0;
    if (d.z > 1e-5) { float tt = -o.z / d.z; if (tt > 0 && tt < t) t = tt, hit = 1; }          // front wall
    if (d.z < -1e-5) { float tt = (-Db - o.z) / d.z; if (tt > 0 && tt < t) t = tt, hit = 4; }  // wall behind (door)
    if (d.x > 1e-5) { float tt = (Wx - o.x) / d.x; if (tt > 0 && tt < t) t = tt, hit = 5; }    // side walls
    if (d.x < -1e-5) { float tt = (-Wx - o.x) / d.x; if (tt > 0 && tt < t) t = tt, hit = 5; }
    if (d.y < -1e-5) { float tt = -o.y / d.y; if (tt > 0 && tt < t) t = tt, hit = 2; }         // floor
    if (d.y > 1e-5) { float tt = (Hc - o.y) / d.y; if (tt > 0 && tt < t) t = tt, hit = 3; }    // ceiling
    float3 p = o + d * t;
    {  // Academy Store: only the booth's wall (continued sideways) and the floor in front of it, fading to black
        float tw = d.z > 1e-5 ? -o.z / d.z : 1e9, tf = d.y < -1e-5 ? -o.y / d.y : 1e9;
        const float3 vd = StoreVoid();  // the void: black / white (option)
        float3 c = vd;
        if (tw < tf && tw > 0 && o.y + d.y * tw >= eyeh.z) {
            c = vd;  // beside / above the booth: the void (its own wall fades into it: StoreEdgeShade)
        } else {
            c = StoreFloor(o, d, Wx, Db);  // the floor, also under the booth (below its base)
        }
        if (lpan.w == 0) return c;
    }
    // (dev !roomdebug: the full room below, in flat colours)
    // soft shading into the room's edges and corners
    float edge = min(min(min(hit == 2 ? 1e4 : p.y, hit == 3 ? 1e4 : Hc - p.y), min(hit == 5 ? 1e4 : Wx - abs(p.x),
                     hit == 4 ? 1e4 : p.z + Db)), hit == 1 ? 1e4 : -p.z);  // distance to the nearest other surface
    float ao = lerp(0.78, 1.0, smoothstep(0, 70, edge));
    if (lpan.w > 0)  // dev: surfaces in flat colours
        return hit == 1 ? float3(1, 0, 0) : hit == 4 ? float3(1, 1, 0) : hit == 5 ? float3(1, 0, 1) : hit == 2 ? float3(0, 1, 0)
             : hit == 3 ? float3(0, 0, 1) : float3(0, 0, 0);
    if (hit == 1) return CorridorWall(p.x, p.y, Hc) * ao;
    if (hit == 5) return CorridorWall(p.z, p.y, Hc) * ao;
    if (hit == 4) {  // the wall behind, with a door in it (behind the viewpoint)
        float3 c = CorridorWall(p.x, p.y, Hc);
        float dx = abs(p.x - o.x), aa = max(max(fwidth(p.x), fwidth(p.y)), 0.4);
        float frame = (1 - smoothstep(52 - aa, 52 + aa, dx)) * (1 - smoothstep(206 - aa, 206 + aa, p.y));
        float leaf = (1 - smoothstep(46 - aa, 46 + aa, dx)) * (1 - smoothstep(200 - aa, 200 + aa, p.y));
        float glass = (1 - smoothstep(30 - aa, 30 + aa, dx)) * smoothstep(120 - aa, 120 + aa, p.y) *
                      (1 - smoothstep(185 - aa, 185 + aa, p.y));
        float handle = 1 - smoothstep(3 - aa, 3 + aa, length(float2(p.x - o.x - 34, p.y - 95)));
        c = lerp(c, float3(37, 115, 98) / 255, frame);
        c = lerp(c, float3(0.80, 0.78, 0.72), leaf);
        c = lerp(c, float3(0.62, 0.74, 0.80), glass);
        c = lerp(c, float3(0.35, 0.35, 0.33), handle * leaf);
        return c * ao;
    }
    if (hit == 2) {  // teal floor, large tiles
        float2 g = abs(frac(p.xz / 60 + 0.5) - 0.5) * 60;
        float aa = max(max(fwidth(p.x), fwidth(p.z)), 0.4);
        float joint = 1 - smoothstep(0.5 - aa, 0.5 + aa, min(g.x, g.y));  // ("line" is an HLSL keyword)
        return lerp(float3(30, 104, 88) / 255, float3(22, 82, 69) / 255, joint * 0.8) * ao;
    }
    if (hit == 3) {  // ceiling with light panels
        float2 g = abs(frac(float2(p.x / 220, p.z / 160) + 0.5) - 0.5) * float2(220, 160);
        float lamp = (1 - smoothstep(55, 58, g.x)) * (1 - smoothstep(9, 11, g.y));
        return lerp(float3(0.86, 0.88, 0.85), float3(0.98, 0.99, 0.98), lamp) * ao;
    }
    return float3(0.8, 0.82, 0.78);
}
float3 Room(float2 px) {
    float3 de = normalize(float3((px.x / proj.z * 2 - 1) * proj.x, (1 - px.y / proj.w * 2) * proj.y, -1));
    float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
    float3 U = normalize(vbd.xyz);
    float3 F = float3(0, 0, -1) - U * dot(float3(0, 0, -1), U);
    F = dot(F, F) > 1e-6 ? normalize(F) : float3(0, 0, -1);
    float3 Rt = cross(F, U);
    float3 e = (vbe.xyz - vbc.xyz) * vbc.w;
    float3 o = float3(dot(e, Rt), dot(e, U), dot(e, F));
    float3 d = float3(dot(dr, Rt), dot(dr, U), dot(dr, F));
    float R = vbe.w, Hw = tm.y;
    float a = dot(d.xz, d.xz), b = dot(o.xz, d.xz), c = dot(o.xz, o.xz) - R * R;
    float tw = a > 1e-8 ? (-b + sqrt(max(b * b - a * c, 0))) / a : 1e9;
    float yw = o.y + d.y * tw;
    if (yw < 0 && d.y < 0) {  // floor first
        float3 p = o + d * (-o.y / d.y);
        return RoomFloor((R - length(p.xz)) * 935.0 / R);  // the vanilla floor's look, scaled to this room's size
    }
    if (yw <= Hw) {
        float3 p = o + d * tw;
        return RoomWall(max(p.y, 0), atan2(p.x, p.z), R);
    }
    float3 oc = o - float3(0, Hw, 0);  // dome cap: a sphere of the wall's radius on top of it
    float bb = dot(oc, d), cc = dot(oc, oc) - R * R;
    float3 p = oc + d * (-bb + sqrt(max(bb * bb - cc, 0)));
    float phi = asin(clamp(p.y / R, 0, 1));
    return RoomWall(Hw + R * phi, atan2(p.x, p.z), R * cos(phi));
}
)" R"(
float4 Composite(float4 pos : SV_Position) : SV_Target {
    float3 col = EYE.Load(int3(int2(pos.xy), 0)).rgb;  // sRGB-encoded
    [branch] if (cov.x != 0) {  // Mom is coming: the world takes the cover screen's corner colour, as covered as they are
        int x0 = 6, x1 = int(cov.y) - 7, y0 = int(cov.w) + 6, y1 = int(cov.w + cov.z) - 7;
        float4 c0 = UI.Load(int3(x0, y0, 0)), c1 = UI.Load(int3(x1, y0, 0)), c2 = UI.Load(int3(x0, y1, 0)), c3 = UI.Load(int3(x1, y1, 0));
        float a = (c0.a + c1.a + c2.a + c3.a) * 0.25;
        float3 rgb = (c0.rgb * c0.a + c1.rgb * c1.a + c2.rgb * c2.a + c3.rgb * c3.a) / max(a * 4, 1e-4);
        // only the dark cover (the black heart, the black screen) spreads around: the 8-bit white strobe before the
        // heart stays on the panel - a full-view strobe in a headset is a photosensitivity risk, and it never matched
        // the panel's white anyway (0.9.48)
        float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        a *= saturate((0.6 - lum) / 0.2);
        // in linear light, as the headset blends the panel over it
        col = ToSrgb(lerp(ToLin(col), ToLin(rgb), saturate(a)));
    }
    // Nested branches: HLSL's && does not short-circuit, so testing the store and calling StoreWallDown in one condition ran the store's wall code (an
    // eye-image read at a computed spot) for every pixel of every scene - since 0.9.15 the NVIDIA driver logged GPU
    // engine resets (event 153) at each of the user's 1-4 s freezes (stage, title, store, Dressing Room).
    bool wallDone = false;
    [branch] if (vbd.w > 1.5 && lpan.w == 0 && eyeh.z > 0) {
        float3 wallDown;
        if (StoreWallDown(pos.xy, wallDown)) col = lerp(StoreVoid(), wallDown, StoreEdgeShade(pos.xy)), wallDone = true;
    }
    if (wallDone) {
    } else if (vbd.w != 0 && max(col.r, max(col.g, col.b)) < 0.5 / 255) col = vbd.w > 1.5 ? Corridor(pos.xy) : Room(pos.xy);
    else if (vbd.w > 1.5) {
        float sh = StoreEdgeShade(pos.xy);
        if (lpan.w == 0) col = lerp(StoreVoid(), col, sh);
        else if (g_dbgQx < 1e8) {  // dev: bands every 50 uu along the wall (red left of centre, blue right)
            float band = frac(g_dbgQx / 50.0) < 0.5 ? 1.0 : 0.6;
            col = lerp(col, (g_dbgQx < 0 ? float3(1, 0.2, 0.2) : float3(0.2, 0.4, 1)) * band, 0.6);
        }
    }

    if (flags.y != 0) {
        float3 b = TB.Load(int3(0, 0, 0)).rgb, w = TW.Load(int3(0, 0, 0)).rgb;
        float ta = min(saturate(1.0 - dot(w - b, float3(1, 1, 1) / 3.0)), 0.98);
        if (ta > 0.004) {
            float aw = 1 - pow(1 - ta, 1 - PanelWeight(pos.xy));
            col = ToSrgb(ToLin(saturate(b / ta)) * aw + ToLin(col) * (1 - aw));
        }
    }
    if (flags.x != 0) {
        // clamped outside the rect: edge colour extends to the rest of the view (fades fill it, vignettes reach the periphery)
        float2 fuv = (pos.xy - vis.xy) / vis.zw;
        if (bdrop.x == 1) {
            // Collection background on a sphere fixed in the room: 120 x 68 degrees in front, mirrored beyond
            float3 de = float3((pos.x / proj.z * 2 - 1) * proj.x, (1 - pos.y / proj.w * 2) * proj.y, -1);
            float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
            float yaw = atan2(dr.x, -dr.z), pitch = atan2(dr.y, length(dr.xz));
            fuv = float2(Mirror(0.5 + yaw / 2.094), lerp(bdrop.y, bdrop.z, Mirror(0.5 - pitch / 1.187)));
        }
        if (dk.x != 0 && bdrop.x != 1) {
            float yTop = vis.y, yBot = vis.y + vis.w;
            fuv.y = pos.y >= dk.z ? dk.y + (pos.y - dk.z) / max(yBot - dk.z, 1) * (1 - dk.y)
                                  : (pos.y - yTop) / max(dk.z - yTop, 1) * dk.y;
        }
        float4 f = FS.SampleLevel(S, fuv, 0);
        col = f.rgb * f.a + col * (1 - f.a);
    }
    if (lbg.x != 0 && lbg.w == 1) {  // layered menu, a picture background: continued beyond the panel's edges
        float3 d = float3((pos.x / proj.z * 2 - 1) * proj.x, (1 - pos.y / proj.w * 2) * proj.y, -1);
        float3 n = cross(pu.xyz, pv.xyz);
        float den = dot(d, n), t = abs(den) > 1e-6 ? dot(p0.xyz, n) / den : -1;
        float2 uv = float2(0, 0);
        float far = 4;  // behind the panel plane: the picture's overall colour
        if (t > 0) {
            float3 h = t * d - p0.xyz;
            uv = float2(dot(h, pu.xyz) / dot(pu.xyz, pu.xyz), dot(h, pv.xyz) / dot(pv.xyz, pv.xyz));
            far = max(abs(uv.x), abs(uv.y)) - 1;
        }
        float lod = far <= 0 ? 0 : min(5.5 + far * 7.0, 11.0);  // soft from the edge on (a clamped edge streaks)
        float2 tc = saturate(float2(uv.x * 0.5 + 0.5, 0.5 - uv.y * 0.5));
        float4 b = L0.SampleLevel(S, tc, lod);
        col = lerp(col, b.rgb, b.a);
    } else if (lbg.x != 0 && lbg.w == 2) {  // a periodic pattern: wrapped without seams, scrolling like the movie's
        float3 de = float3((pos.x / proj.z * 2 - 1) * proj.x, (1 - pos.y / proj.w * 2) * proj.y, -1);
        float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
        float yaw = atan2(dr.x, -dr.z), pitch = atan2(dr.y, length(dr.xz));
        float2 cw = lcrop[0].zw - lcrop[0].xy;
        float2 mu = float2(yaw / lbg.y, -pitch / lbg.z) - lpan.xy * lpan.z / cw;
        float2 tc = lcrop[0].xy + frac(mu) * cw;
        // gradients of the unwrapped coordinate: no mip jump (a line) at the wrap
        float4 b = L0.SampleGrad(S, tc, ddx(mu) * cw, ddy(mu) * cw);
        // the title's background fades to white toward the girls (measured: flat to 14% of the width, then about 40%
        // toward white at 35%): undone, so the wrapped window has one brightness
        float fw = clamp((tc.x - 0.14) / 0.21 * 0.39, 0, 0.65);  // measured: 0.38 at 35%, 0.59 at 45% of the width
        b.rgb = saturate((b.rgb - fw) / (1 - fw));
        // the wall's lower part fades to white toward the horizon, and the floor to white toward its outer edge: they
        // meet in white, seamless (like the Dressing Room's light wall-floor junction)
        b.rgb = lerp(b.rgb, float3(0.985, 0.99, 1.0), 1 - smoothstep(0.0, 0.38, pitch));
        b.rgb = lerp(b.rgb, float3(0.985, 0.99, 1.0), smoothstep(0.62, 1.12, pitch));  // and toward the top of the dome
        if (dr.y < -0.01) {  // a floor eye-height below the recentered head: the same tiles laid flat (still)
            float3 er = vbe.xyz;
            float t = (-eyeh.x - er.y) / dr.y;
            if (t > 0) {
                float2 fp = er.xz + dr.xz * t;
                float2 fm = fp / 2.4;  // the window's 4 tiles per 2.4 m
                float2 ftc = lcrop[0].xy + frac(fm) * cw;
                float4 f = L0.SampleGrad(S, ftc, ddx(fm) * cw, ddy(fm) * cw);
                float ffw = clamp((ftc.x - 0.14) / 0.21 * 0.39, 0, 0.65);
                f.rgb = saturate((f.rgb - ffw) / (1 - ffw));
                // the tiles under the player under a light white wash, turning fully white toward the outer edge
                float dist = length(fp - er.xz);
                float wash = lerp(0.35, 1.0, smoothstep(1.5, 11.0, dist));
                b.rgb = lerp(f.rgb, float3(0.985, 0.99, 1.0), wash);
            }
        }
        col = lerp(col, b.rgb, b.a);
    } else if (lbg.x != 0) {  // layered menu: its background on a sphere fixed in the room (tiled, mirrored beyond)
        float3 de = float3((pos.x / proj.z * 2 - 1) * proj.x, (1 - pos.y / proj.w * 2) * proj.y, -1);
        float3 dr = rx.xyz * de.x + ry.xyz * de.y + rz.xyz * de.z;
        float yaw = atan2(dr.x, -dr.z), pitch = atan2(dr.y, length(dr.xz));
        // the cropped part keeps the angular size it has on the panel (lbg.yz = its yaw / pitch span), mirrored beyond
        float2 m = float2(Mirror(0.5 + yaw / lbg.y), Mirror(0.5 - pitch / lbg.z));
        float4 b = L0.SampleLevel(S, lerp(lcrop[0].xy, lcrop[0].zw, m), 0);
        col = lerp(col, b.rgb, b.a);
    }
    {  // layered menu cut-outs, far to near (the panel with the rest of the movie is composited in front by the runtime)
        float4 c1 = Cutout(L1, lc[0], lu[0], lv[0], lcrop[1], pos.xy);
        col = lerp(col, c1.rgb, c1.a);
        float4 c2 = Cutout(L2, lc[1], lu[1], lv[1], lcrop[2], pos.xy);
        col = lerp(col, c2.rgb, c2.a);
        float4 c3 = Cutout(L3, lc[2], lu[2], lv[2], lcrop[3], pos.xy);
        col = lerp(col, c3.rgb, c3.a);
        if (sc.w != 0) {
            float4 sp = float4(sc.xyz, 1);
            float4 c4 = sc.w < 1.5 ? Cutout(L1, sp, su, sv, scrop, pos.xy)
                      : sc.w < 2.5 ? Cutout(L2, sp, su, sv, scrop, pos.xy) : Cutout(L3, sp, su, sv, scrop, pos.xy);
            col = lerp(col, c4.rgb, c4.a);
        }
    }
    // girl profile: plain white surroundings; inside the panel the game's own white backdrop and the 3D girl show
    if (bdrop.x == 2) col = lerp(float3(0.97, 0.97, 0.97), col, PanelWeight(pos.xy) > 0 ? 1.0 : 0.0);
    if (scope.x > 1.001) {  // right-click zoom: the game's scope circle around the reticle, magnified inside
        float2 dd = pos.xy - scope.zw;
        float rs = length(dd) / scope.y;
        // inside: the scope render 1:1 (its frustum is the eye's narrowed by the zoom factor), else the eye image magnified
        if (rs < 1.0) col = scv.x != 0 ? SCV.SampleLevel(S, (0.5 * proj.zw + dd) / proj.zw, 0).rgb
                                      : EYE.SampleLevel(S, (scope.zw + dd / scope.x) / proj.zw, 0).rgb;
        if (sco.x != 0) {  // the game's own scope graphics (dark surroundings, tinted circle), moved onto the reticle
            float2 sp = pos.xy - sco.yz;  // its darkness extends past the HUD band it was drawn in
            sp = clamp(sp, float2(1, bdrop.y * proj.w), float2(proj.z - 1, bdrop.z * proj.w));
            float4 o = SCO.SampleLevel(S, sp / proj.zw, 0);
            col = lerp(col, o.rgb, o.a);
        } else {
            col *= rs < 1.0 ? 1.0 : 0.45;
        }
        // rim: a light ring with a dark outline so the circle's edge reads clearly in VR
        float rim = smoothstep(0.978, 0.986, rs) * (1 - smoothstep(0.998, 1.006, rs));
        float rimEdge = smoothstep(0.955, 0.972, rs) * (1 - smoothstep(1.008, 1.03, rs));
        col = lerp(col, float3(0.07, 0.02, 0.09), saturate(rimEdge) * 0.75);
        col = lerp(col, float3(0.97, 0.86, 1.0), saturate(rim) * 0.9);
    }
    if (eo.x != 0) {  // eye overlay (weak-spot pop-ups): placed by the game at eye-image positions + the band's top
        int2 q = int2(pos.xy) + int2(eo.yz);
        if (q.y >= 0 && q.y < int(eo.w) && q.x >= 0) {
            float4 o = EO.Load(int3(q, 0));
            col = lerp(col, o.rgb, o.a);
        }
    }
    if (sgt.x != 0) {  // the game's own sight, centred on the reticle point
        // sight_center_scale: inside R0 drawn at that scale, back to 1:1 by R1 (continuous) - the aura is untouched
        float2 c = 0.5 * sgt.w - sgt.yz;  // the reticle point in this eye
        float2 dv = pos.xy - c;
        float dd = length(dv), k = sgt2.x, f = dd;
        if (k > 0.05 && k < 0.999 && dd < sgt2.z && dd > 0.001)
            f = dd < sgt2.y * k ? dd / k : sgt2.y + (dd - sgt2.y * k) * (sgt2.z - sgt2.y) / (sgt2.z - sgt2.y * k);
        float2 sp = c + (dd > 0.001 ? dv * (f / dd) : float2(0, 0)) + sgt.yz;
        if (sp.x >= 0 && sp.y >= 0 && sp.x < sgt.w && sp.y < sgt.w) {
            float4 o = (k > 0.05 && k < 0.999 && dd < sgt2.z) ? SGT.SampleLevel(S, sp / sgt.w, 0) : SGT.Load(int3(int2(sp), 0));
            col = lerp(col, o.rgb, o.a);
        }
    }
    if (tag.x != 0) {  // name_tags_3d: the targeted girl's name tag at her head as this eye sees it
        float2 tp = (pos.xy - tag.yz) / tag.w + tag2.xy;
        if (tp.x >= 0 && tp.y >= 0 && tp.x < tag2.z && tp.y < tag2.w) {
            float4 o = TAG.SampleLevel(S, tp / tag2.zw, 0);
            col = lerp(col, o.rgb, o.a);
        }
    }
    if (flags.w != 0 && flags.z > 0) {  // the sight's reveal area (mini demons, Mr. Happiness): faint outer ring
        float r2 = length(pos.xy - misc.yz) / float(flags.z);
        float ring2 = smoothstep(0.955, 0.985, r2) * (1 - smoothstep(0.985, 1.015, r2));
        col = lerp(col, float3(1, 0.9, 0.97), saturate(ring2) * 0.55);
    }
    if (flags.w != 0) {  // controller aim reticle: white ring + dot with a dark outline
        float r = length(pos.xy - misc.yz) / misc.w;
        float ring = smoothstep(0.62, 0.70, r) * (1 - smoothstep(0.92, 1.0, r)) + (1 - smoothstep(0.14, 0.2, r));
        float edge = smoothstep(0.52, 0.62, r) * (1 - smoothstep(1.0, 1.12, r)) + (1 - smoothstep(0.2, 0.3, r));
        col = lerp(col, float3(0.05, 0.02, 0.08), saturate(edge) * 0.6);
        col = lerp(col, float3(1, 0.85, 0.95), saturate(ring));
    }
    return float4(col, 1);
}
)";

bool CreatePipeline(ID3D11Device* dev) {
    if (g_state) return true;
    if (g_pipelineFailed) return false;
    g_pipelineFailed = true;  // until proven otherwise
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) {
        LOG("[ui] d3dcompiler_47.dll / D3DCompile not available");
        return false;
    }
    ID3DBlob *vs = nullptr, *psA = nullptr, *psB = nullptr, *err = nullptr;
    auto cc = [&](const char* entry, const char* target, ID3DBlob** out) {
        if (SUCCEEDED(compile(kShader, sizeof kShader - 1, "ggdpvr_ui", nullptr, nullptr, entry, target, 0, 0, out, &err)))
            return true;
        LOG("[ui] shader %s compile failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        SafeRelease(err);
        return false;
    };
    if (!cc("VS", "vs_4_0", &vs) || !cc("Combine", "ps_4_0", &psA) || !cc("Composite", "ps_4_0", &psB)) {
        SafeRelease(vs), SafeRelease(psA), SafeRelease(psB);
        return false;
    }
    HRESULT hr[10]{};
    hr[0] = dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    hr[1] = dev->CreatePixelShader(psA->GetBufferPointer(), psA->GetBufferSize(), nullptr, &g_psCombine);
    hr[2] = dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &g_psComposite);
    SafeRelease(vs), SafeRelease(psA), SafeRelease(psB);
    D3D11_BUFFER_DESC bd{112, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    hr[3] = dev->CreateBuffer(&bd, nullptr, &g_cbCombine);
    bd.ByteWidth = 736;  // = the Composite cbuffer (46 float4s; grow with it - a short buffer leaves new fields zero)
    hr[4] = dev->CreateBuffer(&bd, nullptr, &g_cbComposite);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    hr[5] = dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = FALSE;  // the composite computes the final colour itself
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE, bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO, bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                               D3D11_COLOR_WRITE_ENABLE_BLUE;
    hr[6] = dev->CreateBlendState(&bl, &g_blendOver);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr[7] = dev->CreateSamplerState(&sd, &g_linear);
    ID3D11Device1* dev1 = nullptr;
    hr[8] = dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1));
    const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    if (dev1) {
        // UE3 creates its device single-threaded; the context state must match
        const UINT stFlags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                 ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        hr[9] = dev1->CreateDeviceContextState(stFlags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                               &g_state);
        dev1->Release();
    }
    bool ok = true;
    for (HRESULT h : hr) ok = ok && SUCCEEDED(h);
    if (!ok) {
        LOG("[ui] pipeline creation failed: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", hr[0], hr[1],
            hr[2], hr[3], hr[4], hr[5], hr[6], hr[7], hr[8], hr[9]);
        SafeRelease(g_vs), SafeRelease(g_psCombine), SafeRelease(g_psComposite), SafeRelease(g_cbCombine);
        SafeRelease(g_cbComposite), SafeRelease(g_rs), SafeRelease(g_blendOver), SafeRelease(g_linear);
        SafeRelease(g_state);
        return false;
    }
    g_pipelineFailed = false;
    LOG("[ui] pipeline ready (feature level %x)", unsigned(fl));
    return true;
}

// The 16:9 HUD band (the UI's "show all" layout of its 1920x1080 stage) inside a w x h view.
void Band(uint32_t w, uint32_t h, uint32_t& x0, uint32_t& y0, uint32_t& bw, uint32_t& bh) {
    if (w * 9 <= h * 16) {
        bw = w, bh = (w * 9 + 8) / 16, x0 = 0, y0 = (h - bh) / 2;
    } else {
        bh = h, bw = (h * 16 + 4) / 9, y0 = 0, x0 = (w - bw) / 2;
    }
}

bool Ensure(ID3D11Device* dev, ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return false;
    if (!g_save.tex || d.Width != g_desc.Width || d.Height != g_desc.Height || d.Format != g_desc.Format) {
        uint32_t x0, y0, bw, bh;
        Band(d.Width, d.Height, x0, y0, bw, bh);
        const bool ok = g_save.Create(dev, d.Width, d.Height, d.Format, false) &&
                        g_black.Create(dev, d.Width, d.Height, d.Format, false) &&
                        g_white.Create(dev, d.Width, d.Height, d.Format, false) &&
                        g_ui.Create(dev, bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM, true) &&
                        g_fs.Create(dev, bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM, true) &&
                        g_tintB.Create(dev, 1, 1, d.Format, false) && g_tintW.Create(dev, 1, 1, d.Format, false);
        if (!ok) {
            LOG("[ui] capture resources failed (%ux%u fmt %u)", d.Width, d.Height, d.Format);
            g_desc = {};
            return false;
        }
        // optional ones (eye overlay, zoom scope movie, menu layers: up to ~300 MB at 3640x3782) are created on first
        // use (LazyTarget) - video memory, 0.9.16 - and released here so they follow the new size
        g_eo.Release(), g_sco.Release(), g_sgt.Release(), g_tag.Release(), g_tagZero.Release();
        for (auto& l : g_lay) l.Release();
        g_desc = d;
        g_fsValid = g_tintValid = g_eoValid = g_scoValid = g_sgtValid = g_tagValid = false;
        g_layValid[0] = g_layValid[1] = g_layValid[2] = g_layValid[3] = false;  // recreated: empty
        LOG("[ui] capture %ux%u fmt %u; UI band %ux%u at y %u", d.Width, d.Height, d.Format, bw, bh, y0);
    }
    SafeRelease(g_bbRtv);
    return SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &g_bbRtv));
}

// an optional capture target, created on first use: the whole frame, or the 16:9 HUD band
bool LazyTarget(ID3D11Device* dev, Target& t, bool band, bool mips = false) {
    if (t.tex) return true;
    uint32_t x0, y0, bw, bh;
    Band(g_desc.Width, g_desc.Height, x0, y0, bw, bh);
    const bool ok = t.Create(dev, band ? bw : g_desc.Width, band ? bh : g_desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM, true, mips);
    LOG("[ui] %s target %ux%u created on first use%s", band ? "band" : "frame", t.w, t.h, ok ? "" : " - FAILED");
    return ok;
}
struct Pass {  // swaps in the private context state for the scope
    ID3D11DeviceContext1* ctx1 = nullptr;
    ID3DDeviceContextState* prev = nullptr;
    explicit Pass(ID3D11DeviceContext* ctx) {
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return;
        ctx1->SwapDeviceContextState(g_state, &prev);
        ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx1->VSSetShader(g_vs, nullptr, 0);
        ctx1->RSSetState(g_rs);
        ctx1->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);  // the private state persists between passes
    }
    ~Pass() {
        if (!ctx1) return;
        ID3D11ShaderResourceView* none[5] = {};
        ctx1->PSSetShaderResources(0, 5, none);
        ctx1->OMSetRenderTargets(0, nullptr, nullptr);
        ctx1->SwapDeviceContextState(prev, nullptr);
        SafeRelease(prev);
        ctx1->Release();
    }
};

// g_black / g_white (backbuffer-sized) -> dst (band-sized, unpremultiplied RGBA)
struct ZoomGeo {
    float factor = 1.f;
    float p0[4]{}, pu[4]{}, pv[4]{}, proj[4]{};
};
bool Combine(ID3D11Device* dev, ID3D11DeviceContext* ctx, Target& dst, bool tintHandover = false,
             const ZoomGeo* zg = nullptr, bool wholeFrame = false, bool overlays = false, uint32_t ext = 0) {
    if (!CreatePipeline(dev)) return false;
    Pass p(ctx);
    if (!p.ctx1) return false;
    uint32_t x0, y0, bw, bh;
    Band(g_desc.Width, g_desc.Height, x0, y0, bw, bh);
    if (wholeFrame) x0 = y0 = 0, bw = g_desc.Width, bh = g_desc.Height;
    ext = wholeFrame ? 0 : std::min(ext, y0);
    y0 -= ext;
    struct {
        int32_t off[2];
        int32_t tintOn;
        float feather;
        float band[2];
        float pad[2];
        float zoom[4], p0[4], pu[4], pv[4], proj[4];
    } cb{{int32_t(x0), int32_t(y0)}, tintHandover && g_tintValid ? 1 : 0, config::Get().vrUiTintFeather,
         {float(bw), float(bh)}, {overlays && g_fsValid && g_backdrop == 0 && g_fsOverPanel ? 1.f : 0.f, float(ext)}, {zg ? zg->factor : 1.f, zg ? 1.f : 0.f, 0, 0},
         {}, {}, {}, {}};
    if (zg) {
        memcpy(cb.p0, zg->p0, 16), memcpy(cb.pu, zg->pu, 16), memcpy(cb.pv, zg->pv, 16), memcpy(cb.proj, zg->proj, 16);
    }
    p.ctx1->UpdateSubresource(g_cbCombine, 0, nullptr, &cb, 0, 0);
    p.ctx1->PSSetShader(g_psCombine, nullptr, 0);
    ID3D11ShaderResourceView* srvs[6] = {g_black.srv, g_white.srv, g_tintB.srv, g_tintW.srv, g_save.srv, g_fs.srv};
    p.ctx1->PSSetShaderResources(0, 6, srvs);
    p.ctx1->PSSetConstantBuffers(0, 1, &g_cbCombine);
    p.ctx1->PSSetSamplers(0, 1, &g_linear);
    D3D11_VIEWPORT vp{0, 0, float(dst.w), float(dst.h), 0, 1};
    p.ctx1->RSSetViewports(1, &vp);
    p.ctx1->OMSetRenderTargets(1, &dst.rtv, nullptr);
    p.ctx1->Draw(3, 0);
    return true;
}

// Combine of a square region (g_black / g_white hold it at x0, y0) into dst (unpremultiplied, no tint / overlays)
bool CombineRegion(ID3D11Device* dev, ID3D11DeviceContext* ctx, Target& dst, uint32_t x0, uint32_t y0) {
    if (!CreatePipeline(dev)) return false;
    Pass p(ctx);
    if (!p.ctx1) return false;
    struct {
        int32_t off[2];
        int32_t tintOn;
        float feather;
        float band[2];
        float pad[2];
        float zoom[4], p0[4], pu[4], pv[4], proj[4];
    } cb{{int32_t(x0), int32_t(y0)}, 0, 0.f, {float(dst.w), float(dst.h)}, {0.f, 0.f}, {1.f, 0.f, 0, 0}, {}, {}, {}, {}};
    p.ctx1->UpdateSubresource(g_cbCombine, 0, nullptr, &cb, 0, 0);
    p.ctx1->PSSetShader(g_psCombine, nullptr, 0);
    ID3D11ShaderResourceView* srvs[6] = {g_black.srv, g_white.srv, g_tintB.srv, g_tintW.srv, g_save.srv, g_fs.srv};
    p.ctx1->PSSetShaderResources(0, 6, srvs);
    p.ctx1->PSSetConstantBuffers(0, 1, &g_cbCombine);
    p.ctx1->PSSetSamplers(0, 1, &g_linear);
    D3D11_VIEWPORT vp{0, 0, float(dst.w), float(dst.h), 0, 1};
    p.ctx1->RSSetViewports(1, &vp);
    p.ctx1->OMSetRenderTargets(1, &dst.rtv, nullptr);
    p.ctx1->Draw(3, 0);
    return true;
}

XrVector3f QRotInv(const XrQuaternionf& q, XrVector3f v) {  // rotate by the conjugate of q
    const XrQuaternionf c{-q.x, -q.y, -q.z, q.w};
    // v' = c * v * q  (expanded: t = 2 * cross(c.xyz, v); v' = v + c.w * t + cross(c.xyz, t))
    const float tx = 2.f * (c.y * v.z - c.z * v.y), ty = 2.f * (c.z * v.x - c.x * v.z), tz = 2.f * (c.x * v.y - c.y * v.x);
    return {v.x + c.w * tx + (c.y * tz - c.z * ty), v.y + c.w * ty + (c.z * tx - c.x * tz),
            v.z + c.w * tz + (c.x * ty - c.y * tx)};
}
XrVector3f QRot(const XrQuaternionf& q, XrVector3f v) { return QRotInv({-q.x, -q.y, -q.z, q.w}, v); }

// Full-screen effects and the tint over the current eye image (in g_bbRtv).
// The UI panel in the eye space of `rec`: centre, half-width and half-height vectors.
void PanelInEye(const xr::FrameRecord& rec, XrVector3f& c0, XrVector3f& cu, XrVector3f& cv) {
    const XrPosef pp = xr::UiPanelPoseRT();
    const float pw = PanelWidth(), ph = pw * 9.f / 16.f;
    const XrVector3f rel{pp.position.x - rec.pose.position.x, pp.position.y - rec.pose.position.y,
                         pp.position.z - rec.pose.position.z};
    c0 = QRotInv(rec.pose.orientation, rel);
    cu = QRotInv(rec.pose.orientation, QRot(pp.orientation, {pw / 2, 0, 0}));
    cv = QRotInv(rec.pose.orientation, QRot(pp.orientation, {0, ph / 2, 0}));
}
// right-click zoom: "scope" (default) = magnified circle around the reticle; "panel" = the panel becomes a window
// onto the view magnified about its centre (pre-0.8.2 behaviour, and the fallback without controller aim)
// the game's scope circle: radius 0.201 x the HUD band width (measured from its movie: 183 px of 910)
constexpr float kScopeRadiusFrac = 0.2011f;
bool ScopeMode() { return config::Get().vrZoomScope && vr::ctrl::AimActive(); }
bool ZoomActive() { return config::Get().vrZoomInPanel && vr::ZoomFactor() > 1.001f && !ScopeMode(); }

// the effect layer's sideways move in this eye (stage units): the live 3D items' disparity, else (effect_rest_depth)
// the HUD panel's - kiss / kick marks sat at infinity whenever no weak-spot pop-up was up (0.9.83: "slightly further away").
// effect_stereo=2: the other assumption (the snapshot holds the FIRST eye's positions: the right eye moves instead)
float EffectEyeShift(int eye) {
    const auto& c = config::Get();
    if (!c.vrEffectStereo || (eye != 0 && eye != 1)) return 0.f;
    const ULONGLONG t = g_eoItemsT.load();
    const float d = t && GetTickCount64() - t < 250 ? g_eoShiftD.load() : c.vrEffectRestDepth ? g_eoRest.load() : 0.f;
    if (c.vrEffectStereo == 2) return eye == 1 ? -d : 0.f;
    return eye == 0 ? d : 0.f;
}
void Composite(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    vr::ctrl::Aim aim;
    const bool reticle = vr::ctrl::GetAim(aim) && config::Get().vrControllers;
    const bool scopeOn = config::Get().vrZoomInPanel && ScopeMode() && vr::ZoomFactor() > 1.001f;
    vr::DressingRoom room;
    const bool voidOn = vr::GetDressingRoom(room);
    const bool layers = g_layValid[0] || g_layValid[1] || g_layValid[2] || g_layValid[3];
    if (!(g_fsValid || g_tintValid || g_eoValid || reticle || scopeOn || g_backdrop || voidOn || layers || g_coverWorld || g_tagValid) ||
        !CreatePipeline(dev))
        return;
    xr::FrameRecord rec;
    if (!xr::CurrentRecordRT(rec) || rec.halfH <= 0 || rec.halfV <= 0) return;
    // reticle: the point reticle_distance along the aim ray, projected into this eye
    float rx = 0, ry = 0, rr = 0;
    bool reticleOn = false;
    // reveal radius: the flat game's sight circle is ~280 px of a 1920-wide screen; the native test runs in the
    // eye image scaled the same way (HUD band width = eye image width)
    // (not while the controller is a menu pointer)
    const bool pointing = vr::pointer::Active();
    // the auras (the reveal ring, the game's sight) are gameplay only: not in menu scenes wherever the ray points, not
    // on the Mom-is-coming cover (0.9.86: "the menu plain rings shouldn't have an aura")
    // (nor in Final Doki Doki Mode: nothing to reveal there, and its own sight only shows moving between body parts -
    // 0.9.89: the old aura ring was still there)
    const bool gameplayAim = !pointing && !vr::pointer::MenuScene() && !g_coverWorld && !vr::InFinalDoki();
    int32_t revealPx = config::Get().vrRevealRing && gameplayAim ? int32_t(280.f * float(g_desc.Width) / 1920.f) : 0;
    if (reticle) {
        // on the menu surface while pointing (the hand ray), else on what the shot ray (vr::AimRayRaw) hits
        XrVector3f ro = aim.raw.position, f = QRot(aim.raw.orientation, {0, 0, -1});
        if (!pointing) vr::AimRayRaw(aim.raw, ro, f);
        const float maxDist = config::Get().vrReticleDistance,
                    hitDist = pointing ? vr::pointer::HitDistance() : vr::ctrl::HitDistance();
        const float dist = hitDist > 0.1f ? std::fmin(hitDist, maxDist) : maxDist;
        const XrVector3f pt{ro.x + f.x * dist - rec.pose.position.x, ro.y + f.y * dist - rec.pose.position.y,
                            ro.z + f.z * dist - rec.pose.position.z};
        const XrVector3f e = QRotInv(rec.pose.orientation, pt);
        if (e.z < -0.05f) {
            const float tH0 = std::tan(rec.halfH), tV0 = std::tan(rec.halfV);
            rx = 0.5f * float(g_desc.Width) * (1.f + (e.x / -e.z) / tH0);
            ry = 0.5f * float(g_desc.Height) * (1.f - (e.y / -e.z) / tV0);
            rr = std::tan(config::Get().vrReticleSizeDeg * 3.14159265f / 180.f) / tH0 * 0.5f * float(g_desc.Width);
            // menus with the game's own mouse cursor: that cursor is the pointer, no reticle under it (0.9.46)
            reticleOn = rr > 1.f && !(pointing && g_cursorMovieUp);
            g_lastRx = rx, g_lastRy = ry;
        }
        static int s_retLogs = 0;
        static uint64_t s_retFrame = 0;
        if (++s_retFrame % 600 == 1 && s_retLogs < 8) {
            ++s_retLogs;
            LOG("[ui] reticle eye-space (%.2f,%.2f,%.2f) -> px (%.0f,%.0f) r %.1f on %d", e.x, e.y, e.z, rx, ry, rr, reticleOn);
        }
    }
    // reticle_style=1: the game's sight (with its aura = the reveal area) instead of the ring, dot and reveal ring
    const bool sightOn = g_sgtValid && reticleOn && gameplayAim && !scopeOn && config::Get().vrReticleStyle == 1;
    if (sightOn) revealPx = 0;
    const float W = float(g_desc.Width), H = float(g_desc.Height);
    const float tH = std::tan(rec.halfH), tV = std::tan(rec.halfV);
    // the binocular field of view (union of both eyes), scaled: the same pixel rect in both eye images
    const float k = g_dokiPeaceFull.load() && config::Get().vrUiFullscreenFovDoki > 0.f ? config::Get().vrUiFullscreenFovDoki
                                                                                         : config::Get().vrUiFullscreenFov;
    const XrFovf &a = rec.fov[0], &b = rec.fov[1];
    const float l = std::tan(std::fmin(a.angleLeft, b.angleLeft)) * k, r = std::tan(std::fmax(a.angleRight, b.angleRight)) * k;
    const float u = std::tan(std::fmax(a.angleUp, b.angleUp)) * k, d = std::tan(std::fmin(a.angleDown, b.angleDown)) * k;
    const float x0 = 0.5f * W * (1.f + l / tH), x1 = 0.5f * W * (1.f + r / tH);
    const float y0 = 0.5f * H * (1.f - u / tV), y1 = 0.5f * H * (1.f - d / tV);
    // the panel in eye space (the world tint hands over to the panel's own tint across its feathered edge)
    XrVector3f c0, cu, cv;
    PanelInEye(rec, c0, cu, cv);
    const float panelValid = ZoomActive() ? 0.f : 1.f;  // zoom: the panel is opaque, the world takes the full tint
    uint32_t bx0, by0, bbw, bbh;
    Band(g_desc.Width, g_desc.Height, bx0, by0, bbw, bbh);
    // eye -> recentered frame (backdrop sphere)
    const XrPosef ref = xr::RecenterRefRT();
    const XrQuaternionf eq = rec.pose.orientation;
    auto toRef = [&](XrVector3f v) { return QRotInv(ref.orientation, QRot(eq, v)); };
    const XrVector3f ex = toRef({1, 0, 0}), ey = toRef({0, 1, 0}), ez = toRef({0, 0, 1});
    const XrVector3f eyeRef = QRotInv(ref.orientation, {rec.pose.position.x - ref.position.x, rec.pose.position.y - ref.position.y,
                                                        rec.pose.position.z - ref.position.z});
    const float bandY0 = (float(by0) + 1.f) / H, bandY1 = (float(by0 + bbh) - 1.f) / H;
    struct {
        float vis[4], p0[4], pu[4], pv[4], proj[4], misc[4];
        int32_t flags[4];
        float scope[4];
        float bdrop[4];
        float eo[4];
        float sco[4];
        float rx[4], ry[4], rz[4];
        float scv[4];
        float vbd[4];
        float tm[4];
        float vbc[4];
        float vbe[4];
        float lbg[4];
        float lc[3][4], lu[3][4], lv[3][4];
        float lcrop[4][4];
        float lpan[4];
        float sc[4], su[4], sv[4], scrop[4];
        float vbf[4];
        float eyeh[4];
        float cov[4];
        float dk[4];
        float sgt[4];  // the game's sight (reticle_style=1)
        float sgt2[4];  // its centre squeeze
        float tag[4];   // name_tags_3d
        float tag2[4];
    } cb{{x0, y0, x1 - x0, y1 - y0},
         {c0.x, c0.y, c0.z, panelValid},
         {cu.x, cu.y, cu.z, 0},
         {cv.x, cv.y, cv.z, 0},
         {tH, tV, W, H},
         {config::Get().vrUiTintFeather, rx, ry, rr},
         // zoomed: the scope's own centre mark is the reticle (ours and the reveal ring are hidden)
         {g_fsValid ? 1 : 0, g_tintValid ? 1 : 0, revealPx, reticleOn && !scopeOn && !sightOn ? 1 : 0},
         {scopeOn ? vr::ZoomFactor() : 1.f,
          config::Get().vrZoomScopeDeg > 0 ? std::tan(config::Get().vrZoomScopeDeg * 3.14159265f / 180.f) / tH * 0.5f * W
                                            : kScopeRadiusFrac * float(bbw),
          reticleOn ? rx : 0.5f * W, reticleOn ? ry : 0.5f * H},
         {g_backdrop == 2 ? 2.f : (g_backdrop == 1 && g_fsValid ? 1.f : 0.f), bandY0, bandY1, 0},
         {g_eoValid ? 1.f : 0.f,
          std::round(-EffectEyeShift(rec.eye) * float(bbw) / 1920.f),
          config::Get().vrEffectFullView ? 0.f : float(by0), H},
         {g_scoValid && scopeOn ? 1.f : 0.f, (reticleOn ? rx : 0.5f * W) - 0.5f * W, (reticleOn ? ry : 0.5f * H) - (float(by0) + 0.5f * float(bbh)), 0},
         {ex.x, ex.y, ex.z, 0}, {ey.x, ey.y, ey.z, 0}, {ez.x, ez.y, ez.z, 0},
         {scopeOn && g_scvFrame == rec.frameId && g_scv.w == g_desc.Width && g_scv.h == g_desc.Height ? 1.f : 0.f, 0, 0, 0},
         {room.up[0], room.up[1], room.up[2], voidOn ? float(room.kind) : 0.f},
         {float(GetTickCount64() % 3600000ull) / 1000.f, room.wallH, g_shopVoid.load() ? 1.f : 0.f, room.halfWidth},
         {room.centre[0], room.centre[1], room.centre[2], room.unitsPerMetre},
         {eyeRef.x, eyeRef.y, eyeRef.z, room.radius},
         {g_layValid[0] && !g_shopVoid.load() ? 1.f : 0.f,  // the store is a 3D scene: not the hub's backdrop
          WholeTurns(2.f * std::atan(0.5f * config::Get().vrUiWidth / config::Get().vrUiDistance) * (LayerCropOf(0)[2] - LayerCropOf(0)[0]),
                     LayerBgMode() == 2 ? 1.f : 2.f),
          2.f * std::atan(0.5f * config::Get().vrUiWidth * 9.f / 16.f / config::Get().vrUiDistance) * (LayerCropOf(0)[3] - LayerCropOf(0)[1]),
          float(LayerBgMode())},
         {}, {}, {}, {}};
    for (int l = 0; l < 4; ++l) memcpy(cb.lcrop[l], LayerCropOf(l), 16);
    if (sightOn)  // the square's centre onto this eye's reticle point
        cb.sgt[0] = 1.f, cb.sgt[1] = float(g_sgt.w) * 0.5f - rx, cb.sgt[2] = float(g_sgt.w) * 0.5f - ry, cb.sgt[3] = float(g_sgt.w),
        cb.sgt2[0] = std::fmax(0.f, std::fmin(1.f, config::Get().vrSightCenterScale)), cb.sgt2[1] = 80.f * W / 3640.f,
        cb.sgt2[2] = 160.f * W / 3640.f;
    {  // name_tags_3d: her tag at that eye's own projection of her head (the game's x, y in eye-image pixels / (band width /
       // 1920) - y from the image top, unlike the movie's own band-relative stage); the panel tags' angular size (the game sends no
       // size: name tags don't shrink with distance)
        const int e = rec.eye;
        const ULONGLONG now = GetTickCount64();
        if (g_tagValid && g_tag.w && config::Get().vrNameTags3d && (e == 0 || e == 1) && now - g_tagParkT.load() < 250 &&
            now - g_tagEyeT[e].load() < 250 && config::Get().vrUiDistance > 0.1f && tH > 0.f) {
            const float s = float(bbw) / 1920.f;
            cb.tag[0] = 1.f, cb.tag[1] = float(bx0) + g_tagEyeX[e].load() * s, cb.tag[2] = g_tagEyeY[e].load() * s,  // the game's tag y counts from the top of the eye image (not the band)
            cb.tag[3] = config::Get().vrUiWidth / config::Get().vrUiDistance / (2.f * tH);
            cb.tag2[0] = kTagBoxW / 2.f * s, cb.tag2[1] = kTagBoxUp * s, cb.tag2[2] = float(g_tag.w), cb.tag2[3] = float(g_tag.h);
        }
    }
    cb.eyeh[0] = 1.6f;  // the title floor, metres below the recentered head
    if (g_coverWorld && g_ui.srv && g_uiBandW > 16 && g_uiBandH > 16)
        cb.cov[0] = 1.f, cb.cov[1] = float(g_uiBandW), cb.cov[2] = float(g_uiBandH), cb.cov[3] = float(g_uiExtRows);
    // Double Peace border: the game's 16:9 picture on the (squarer) view stretched it ~1.4x tall - its flowers looked
    // pulled (0.9.43). Its bottom strip (the flowers) keeps the picture's shape along the bottom of the view; the
    // plain glow above takes up the rest of the height.
    if (g_dokiPeaceFull.load() && config::Get().vrUiDokiFlowerBand > 0.f) {
        const float band = std::fmin(1.f, config::Get().vrUiDokiFlowerBand);
        const float stripTan = band * 9.f / 16.f * (r - l);  // the strip's height at the picture's own aspect
        cb.dk[0] = 1.f, cb.dk[1] = 1.f - band, cb.dk[2] = std::fmax(y0, y1 - stripTan * 0.5f * H / tV);
    }
    cb.eyeh[1] = config::Get().vrShopVoidWhite ? 1.f : 0.f;
    cb.eyeh[2] = voidOn && room.kind == 2 ? room.base : 0.f;
    {
        const bool f = room.fwd[0] != 0 || room.fwd[1] != 0 || room.fwd[2] != 0;
        cb.vbf[0] = room.fwd[0], cb.vbf[1] = room.fwd[1], cb.vbf[2] = room.fwd[2], cb.vbf[3] = f ? 1.f : 0.f;
    }
    {
        const float now = float(GetTickCount64() % 3600000ull) / 1000.f;
        float since = now - g_bgCaptureTime.load();
        if (since < 0) since += 3600.f;
        cb.lpan[0] = LayerPan()[0], cb.lpan[1] = LayerPan()[1], cb.lpan[2] = since, cb.lpan[3] = g_roomDebug.load() ? 1.f : 0.f;
    }
    // title_face_player (0.9.94a): a title girl cut-out (layers 1, 2) turned about the vertical to face the recentered
    // head (the start position) - its half-width vector along up x (toward the head); placement and size unchanged
    auto facePlayer = [&](int layer, const XrVector3f& centreWorld, float halfW, XrVector3f& ueOut, const XrPosef& recPose) {
        const float amt = std::fmax(-2.f, std::fmin(2.f, config::Get().vrTitleFacePlayer));
        if (std::fabs(amt) < 0.001f || !TitleLayersActive() || (layer != 1 && layer != 2)) return false;
        const XrVector3f up = QRot(ref.orientation, {0, 1, 0});
        XrVector3f d{ref.position.x - centreWorld.x, ref.position.y - centreWorld.y, ref.position.z - centreWorld.z};
        const float du = d.x * up.x + d.y * up.y + d.z * up.z;
        d = {d.x - up.x * du, d.y - up.y * du, d.z - up.z * du};  // horizontal
        const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dl < 0.05f) return false;
        XrVector3f r{up.y * d.z - up.z * d.y, up.z * d.x - up.x * d.z, up.x * d.y - up.y * d.x};  // up x d = right
        const float rl = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        if (rl < 1e-4f) return false;
        r = {r.x / rl, r.y / rl, r.z / rl};
        // the panel's own right (horizontal): turned toward r by the chosen fraction of the angle between them
        XrVector3f p = QRot(xr::UiPanelPoseRT().orientation, {1, 0, 0});
        const float pu = p.x * up.x + p.y * up.y + p.z * up.z;
        p = {p.x - up.x * pu, p.y - up.y * pu, p.z - up.z * pu};
        const float pl = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (pl < 1e-4f) return false;
        p = {p.x / pl, p.y / pl, p.z / pl};
        const XrVector3f pxr{p.y * r.z - p.z * r.y, p.z * r.x - p.x * r.z, p.x * r.y - p.y * r.x};
        const float ang = std::atan2(pxr.x * up.x + pxr.y * up.y + pxr.z * up.z, p.x * r.x + p.y * r.y + p.z * r.z) * amt;
        const XrVector3f uxp{up.y * p.z - up.z * p.y, up.z * p.x - up.x * p.z, up.x * p.y - up.y * p.x};
        const float cs = std::cos(ang), sn = std::sin(ang);
        const XrVector3f q{p.x * cs + uxp.x * sn, p.y * cs + uxp.y * sn, p.z * cs + uxp.z * sn};
        ueOut = QRotInv(recPose.orientation, {q.x * halfW, q.y * halfW, q.z * halfW});
        return true;
    };
    {  // cut-out layers: the panel scaled about the recentered head position to each layer's distance (same angular size)
        const XrPosef pp = xr::UiPanelPoseRT();
        const float pw = config::Get().vrUiWidth, ph = pw * 9.f / 16.f, pd = config::Get().vrUiDistance;
        for (int l = 1; l < 4; ++l) {
            if (!g_layValid[l] || g_layDist[l] <= 0.f || !((g_cutMask.load() >> l) & 1)) continue;
            const float k = g_layDist[l] / pd;
            // title screen girls (layers 1, 2): rooted in the floor at their cut-off - the bottom of the part used sits
            // on the floor (eyeh: 1.6 m below the recentered head) at the layer's distance; the scaled panel left the
            // back pair 0.6 m into the floor and the front girl 0.3 m above it (0.9.59)
            // title front girls (layer 2): moved back by title_front_push metres at their own size - smaller from the
            // default spot, so more of the girls behind them shows (0.9.60); the size keeps k, the place kp
            float kp = k;
            if (TitleLayersActive() && l == 2 && config::Get().vrTitleFrontPush > 0.f)
                kp = (g_layDist[l] + config::Get().vrTitleFrontPush) / pd;
            float dy = 0.f;
            if (config::Get().vrTitleGroundGirls && TitleLayersActive() && (l == 1 || l == 2)) {
                const float cy = ref.position.y + kp * (pp.position.y - ref.position.y);
                const float v1 = LayerCropOf(l)[3];
                const float bottom = cy + k * ph / 2 - v1 * k * ph;  // the used part's lower edge (v from the top)
                dy = (ref.position.y - 1.6f) - bottom;
            }
            XrVector3f side{0, 0, 0};  // title front girls: title_front_right metres along the panel's right (0.9.62)
            if (TitleLayersActive() && l == 2 && config::Get().vrTitleFrontRight != 0.f)
                side = QRot(pp.orientation, {config::Get().vrTitleFrontRight, 0, 0});
            const XrVector3f c{ref.position.x + kp * (pp.position.x - ref.position.x) + side.x - rec.pose.position.x,
                               ref.position.y + kp * (pp.position.y - ref.position.y) + dy + side.y - rec.pose.position.y,
                               ref.position.z + kp * (pp.position.z - ref.position.z) + side.z - rec.pose.position.z};
            const XrVector3f ce = QRotInv(rec.pose.orientation, c);
            XrVector3f ue = QRotInv(rec.pose.orientation, QRot(pp.orientation, {k * pw / 2, 0, 0}));
            facePlayer(l, {c.x + rec.pose.position.x, c.y + rec.pose.position.y, c.z + rec.pose.position.z}, k * pw / 2, ue, rec.pose);
            const XrVector3f ve = QRotInv(rec.pose.orientation, QRot(pp.orientation, {0, k * ph / 2, 0}));
            const float cc[4] = {ce.x, ce.y, ce.z, 1}, uu[4] = {ue.x, ue.y, ue.z, 0}, vv[4] = {ve.x, ve.y, ve.z, 0};
            memcpy(cb.lc[l - 1], cc, 16), memcpy(cb.lu[l - 1], uu, 16), memcpy(cb.lv[l - 1], vv, 16);
        }
        int sl = 0;
        float scrop[4], sd = 0;
        if (LayerSplit(sl, scrop, sd) && sl >= 1 && sl <= 3 && g_layValid[sl] && ((g_cutMask.load() >> sl) & 1)) {
            const float k = (sd > 0 ? sd : pd) / pd;
            const XrVector3f c{ref.position.x + k * (pp.position.x - ref.position.x) - rec.pose.position.x,
                               ref.position.y + k * (pp.position.y - ref.position.y) - rec.pose.position.y,
                               ref.position.z + k * (pp.position.z - ref.position.z) - rec.pose.position.z};
            const XrVector3f ce = QRotInv(rec.pose.orientation, c);
            XrVector3f ue = QRotInv(rec.pose.orientation, QRot(pp.orientation, {k * pw / 2, 0, 0}));
            facePlayer(sl, {c.x + rec.pose.position.x, c.y + rec.pose.position.y, c.z + rec.pose.position.z}, k * pw / 2, ue, rec.pose);
            const XrVector3f ve = QRotInv(rec.pose.orientation, QRot(pp.orientation, {0, k * ph / 2, 0}));
            const float cc[4] = {ce.x, ce.y, ce.z, float(sl)}, uu[4] = {ue.x, ue.y, ue.z, 0}, vv[4] = {ve.x, ve.y, ve.z, 0};
            memcpy(cb.sc, cc, 16), memcpy(cb.su, uu, 16), memcpy(cb.sv, vv, 16), memcpy(cb.scrop, scrop, 16);
        }
    }
    Pass p(ctx);
    if (!p.ctx1) return;
    p.ctx1->UpdateSubresource(g_cbComposite, 0, nullptr, &cb, 0, 0);
    p.ctx1->PSSetShader(g_psComposite, nullptr, 0);
    ID3D11ShaderResourceView* srvs[14] = {g_fs.srv,  g_tintB.srv,  g_tintW.srv,  g_save.srv,  g_eo.srv,  g_sco.srv, g_scv.srv,
                                          g_lay[0].srv, g_lay[1].srv, g_lay[2].srv, g_lay[3].srv, g_ui.srv,   g_sgt.srv, g_tag.srv};
    p.ctx1->PSSetShaderResources(0, 14, srvs);
    p.ctx1->PSSetConstantBuffers(0, 1, &g_cbComposite);
    p.ctx1->PSSetSamplers(0, 1, &g_linear);
    const float factor[4] = {};
    p.ctx1->OMSetBlendState(g_blendOver, factor, 0xFFFFFFFF);  // no blending; RGB only (alpha untouched)
    D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};
    p.ctx1->RSSetViewports(1, &vp);
    p.ctx1->OMSetRenderTargets(1, &g_bbRtv, nullptr);
    p.ctx1->Draw(3, 0);
}

// ---- per-movie routing ----
// The command's payload (cmd+0x10) is a TArray of FGFxMovie copies (0x8C bytes each; +0x70 = UGFxMoviePlayer*).
// Execute only reads it, so subsets are rendered by pointing the array at a filtered copy for one call.
bool ScopeMode();
// The shooting effect movie (weak-spot pop-ups, boss "!" markers) is drawn per eye while the controllers aim
bool EffectMovieInEye() { return config::Get().vrWeakSpotAtReticle && vr::ctrl::AimActive(); }
// kEye: drawn 1:1 over each eye image. The shooting effect movie places its weak-spot pop-ups at the sight
// cursor, which (with controller aim) is in eye-image pixels, so drawn this way they appear at the reticle.
// kScope: the game's zoom scope (darkened view, tinted circle, centre mark), drawn per eye moved onto the reticle.
// kSight: the game's sight movie as the reticle (reticle_style=1).
enum Cat : uint8_t { kPanel = 0, kFullscreen = 1, kHidden = 2, kTint = 3, kEye = 4, kScope = 5, kSight = 6, kCatCount = 7 };
constexpr uint32_t kMovieSize = 0x8C, kMoviePlayer = 0x70;
struct MovieArray { uint8_t* data; int32_t num, max; };
// player -> (class, category). Keyed by address, so the class is stored too: a freed movie's address can be
// reused by a different movie class (that once routed the Student Roster as a full-screen effect).
struct CatEntry { uintptr_t cls; uint8_t cat; };
std::unordered_map<uintptr_t, CatEntry> g_catCache;  // render thread
std::vector<uintptr_t> g_players;                    // this command's players
std::vector<uint8_t> g_cats, g_tmp;
uint32_t g_present = 0;  // categories present in this command (bit per Cat)
std::string g_lastSet;
std::atomic<bool> g_fantasize{false};

std::string MovieClass(uintptr_t player) {
    return player && ue3::IsObject(player) ? ue3::ObjName(ue3::ClassOf(player)) : std::string("?");
}
bool InList(const std::vector<std::string>& l, const std::string& cls) {
    for (const auto& n : l)
        if (cls == n) return true;
    return false;
}
uint8_t CategoryOf(const std::string& cls) {
    const auto& c = config::Get();
    if (InList(c.vrUiHiddenMovies, cls)) return kHidden;
    if (InList(c.vrUiFullscreenMovies, cls)) return kFullscreen;
    if (InList(c.vrUiTintMovies, cls)) return kTint;
    return kPanel;
}
void Classify(void* cmd) {
    auto* arr = reinterpret_cast<MovieArray*>(static_cast<uint8_t*>(cmd) + 0x10);
    g_players.clear(), g_cats.clear();
    g_present = 0;
    bool backdrop = false, cursorUp = false, cover = false, aeResult = false;
    std::string set;
    static const char* const kTag[] = {"", "(full)", "(hidden)", "(tint)", "(eye)", "(scope)", "(sight)"};
    // (0.9.86 added kSight without a name here: a read past the end crashed every stage start)
    static_assert(sizeof(kTag) / sizeof(kTag[0]) == kCatCount, "a name per movie category");
    // a girl's profile in the Collection: a plain white backdrop, the pink background movie is not drawn
    bool profile = false;
    for (int32_t i = 0; i < arr->num && i < 64; ++i) {
        uintptr_t pl = 0;
        memcpy(&pl, arr->data + size_t(i) * kMovieSize + kMoviePlayer, sizeof pl);
        if (pl && ue3::IsObject(pl) && MovieClass(pl) == "GG2HudCollectionGirlProfileMovie") profile = true;
    }
    for (int32_t i = 0; i < arr->num && i < 64; ++i) {
        uintptr_t player = 0;
        memcpy(&player, arr->data + size_t(i) * kMovieSize + kMoviePlayer, sizeof player);
        const uintptr_t cls = player && ue3::IsObject(player) ? ue3::ClassOf(player) : 0;
        auto it = g_catCache.find(player);
        uint8_t cat;
        if (it != g_catCache.end() && it->second.cls == cls) {
            cat = it->second.cat;
        } else {
            cat = CategoryOf(MovieClass(player));
            g_catCache[player] = {cls, cat};
        }
        if (cat == kPanel && g_dokiPeaceFull.load() && MovieClass(player) == "GG2HudDokiLightMovie") cat = kFullscreen;
        if (cat == kPanel && vr::ctrl::AimActive() && MovieClass(player) == "GG2HudShootingSightMovie" && !vr::pointer::TouchModeActive())
            cat = config::Get().vrReticleStyle == 1 && config::Get().vrMode == 2 ? kSight : kHidden;  // controller aim: the world reticle replaces the 2D sight (minigames: kept - its aura shows
                            // the reveal range, moved to the pointer: SetSightPosition)
        // menu pointer in immersive scenes: the reticle on the panel is the pointer; the game's own mouse cursor
        // (hotspot at its tip, body down-right) only looked offset next to it
        // (the game's mouse cursor stays visible: hiding it only hid the reticle/cursor mismatch, fixed in menu_pointer)
        // zoom scope: the game's zoom vignette (dark view with a clear centre square) framed the flat screen
        if (ScopeMode() && config::Get().vrZoomInPanel && MovieClass(player) == "GG2HudShootingScopeMovie") cat = kScope;
        // Collection menus (Student Roster etc.): no 3D scene is drawn behind them (stale, smeared image in VR);
        // their background movie becomes a backdrop filling the view
        if (config::Get().vrCollectionBackdrop && MovieClass(player) == "GG2HudCollectionBGMovie")
            cat = profile ? kHidden : kFullscreen, backdrop = true;
        // final Doki Doki Mode's zoom-in overlay (doki marks around the screen edges): a full-screen effect, like the
        // damage border, instead of clipped to the panel (0.9.50)
        if (cat == kPanel && vr::InFinalDoki() && MovieClass(player) == "GG2HudShootingStencilSightMovie") cat = kFullscreen;
        if (cat == kPanel && EffectMovieInEye() && MovieClass(player) == "GG2HudShootingEffectMovie")
            cat = kEye;
        // the player's HUD toggle (0.9.38: to see a girl grabbing you without the HUD over her): the gameplay
        // read-outs only - warnings, route choices, Doki Doki markers, pop-ups at the reticle and menus stay
        if (cat == kPanel && g_hudHidden.load() && g_hudToggleCtx.load()) {
            const std::string& mc = MovieClass(player);
            if (mc == "GG2HudShootingMainMovie" || mc == "GG2HudBossBattleMovie" || mc == "GG2HudEffectScoreMovie") cat = kHidden;
        }
        if (cat != kHidden && MovieClass(player) == "GG2HudMouseCursorMovie") cursorUp = true;
        if (cat != kHidden && MovieClass(player) == "GG2HudResultActionEventMovie") aeResult = true;
        if (cat == kPanel && MovieClass(player) == "GG2HudMamakitaMovie") cover = true;
        g_players.push_back(player), g_cats.push_back(cat);
        if (i < 24) g_drawnPlayers[i] = player;
        g_present |= 1u << cat;
        set += (set.empty() ? "" : ", ") + MovieClass(player) + kTag[cat];
    }
    if (set != g_lastSet) {
        g_lastSet = set;
        g_fantasize = set.find("GG2HudCollectionWildFancyMovie") != std::string::npos;
        LOG("[ui] movies: %s", set.empty() ? "(none)" : set.c_str());
    }
    g_backdrop = !backdrop && !profile ? 0 : (profile && config::Get().vrCollectionBackdrop ? 2 : 1);
    g_cursorMovieUp = cursorUp;
    g_aeResultUp = aeResult;
    if (cover != g_coverWorld) LOG("[ui] Mom is coming screen %s", cover ? "up: the world follows its cover" : "gone");
    g_coverWorld = cover;
    {
        int lastFull = -1, firstPanel = -1;
        for (int i = 0; i < int(g_cats.size()); ++i) {
            if (g_cats[i] == kFullscreen) lastFull = i;
            if (g_cats[i] == kPanel && firstPanel < 0) firstPanel = i;
        }
        const bool over = firstPanel < 0 || lastFull > firstPanel;
        if (over != g_fsOverPanel) {
            g_fsOverPanel = over;
            LOG("[ui] full-screen effects %s the panel's content (game draw order)", over ? "over" : "under");
        }
    }
    // (the Academy Store's movie stays in the list after the store is left: "open" is decided on the game thread)
    for (int32_t i = std::min<int32_t>(arr->num, 64); i < 24; ++i) g_drawnPlayers[i] = 0;
}
// Layered menu movies (title screen). Scaleform 4 renders a snapshot taken when the movie advances (game thread),
// so a movie can't be split into parts within one frame. Instead the frames rotate: the panel (all layer clips
// hidden) every other frame, and in between one layer alone (every other root clip hidden) - its background for
// the sphere, its girls and logo for cut-outs at their own depths. Clips are hidden with _visible, never _alpha: an
// alpha set by script takes the clip off its timeline for good, so a fade caught mid-way froze there ("Press Any
// Button" stuck at 25%, stage select menus too light - 0.9.12). No capture frames around a click (an
// invisible button can't be hit).
struct LayerSpec {
    const char* movie;
    const char* layers[4];  // layer 0 = background sphere ("" = whatever is left with every clip hidden: a
                            // background drawn straight on the movie's root, which can't be hidden on the panel;
                            // "@first" = the first unnamed root clip, "@largest" = the biggest). Unnamed root clips ("instanceN", found at
                            // runtime) are panel content unless named here.
    float dist[4];          // metres (cut-outs)
    float crop[4][4];       // part of each layer used (u0, v0, u1, v1 of the HUD band)
    const char* others;     // the movie's other root clips (hidden while a layer is captured)
    int bgMode;             // background: 0 = tiled on a sphere (mirrored), 1 = a picture: continued beyond the panel's
                            // edges, lined up with it and blurring with distance, 2 = a periodic pattern: crop[0] is
                            // exactly whole tiles, wrapped around without seams and scrolled at pan (band fractions/s)
    float pan[2];
    int unnamedLayer;       // where unnamed root clips (instanceN) go: -1 = the panel, or a layer
    int splitLayer;         // a part of one layer shown as its own cut-out (0 = none): that layer's capture,
    float splitCrop[4];     // cropped to splitCrop, at splitDist metres (0 = the panel's distance)
    float splitDist;
};
const LayerSpec kLayerSpecs[] = {
    // Options (from the title screen): its pink background is root content: on a sphere around, and on the panel too
    {"GG2HudUIOptionMovie",
     {"", "", "", ""},
     {0.f, 0.f, 0.f, 0.f},
     {{0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
     "assignSignalAll,buttonBackAll,filterBlack,gameMenuAll,gameSetUpAll,keyAssignAll,keyAssignWindowAll,keyMenuAll,"
     "languageMenuAll,languageSetUpAll,maskAll,soundMenuAll,tabAll,title,volumeAll", 0, {0.f, 0.f}, -1, 0, {}, 0.f},
    // Title screen: background sphere; the crowd, the two girls and the logo as cut-outs; buttons on the panel
    {"GG2ScreenTitleMovie",
     {"BG_All", "Chara_02,Chara_04", "Chara_01", "logo_back,logo_title,logo_star"},
     {0.f, 7.f, 4.f, 2.6f},
     // background: 4 x 4 of its tiles (period 1018/6 stage px; BG_charaAll slides +2,-2 px per frame at 30 fps, SWF);
     // main girls: their part (the copyright line, part of the same clip, is split off below)
     {{0.f, 0.1f, 4.f * 169.667f / 1920.f, 0.1f + 4.f * 169.667f / 1080.f}, {0.f, 0.f, 1.f, 1.f}, {0.56f, 0.f, 1.f, 1.f},
      {0.f, 0.f, 1.f, 1.f}},
     "MayaPants,PressStartAll,ShinobuPants,buttonDifficultyAll,buttonMenuAll,buttonSaveDataAll,buttonUpDown,confirmAll,"
     "filterAll,processAll,windowInfoAll", 2, {60.f / 1920.f, -60.f / 1080.f},
     3,  // its unnamed root clips are logo animation pieces (the pink "Double Peace" overlay; numbered differently
         // each time the movie is created): with the logo
     2, {0.f, 0.85f, 0.56f, 1.f}, 0.f},  // the copyright line (in the main girls' clip): its own cut-out, with the buttons
    // Story hub / location select (Aoi's): its painted background (the first unnamed root clip) on a sphere
    {"GG2HudBaseMovie",
     {"@first", "", "", ""},
     {0.f, 0.f, 0.f, 0.f},
     {{0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
     "clockAndEpisode,rankAndScore,angelCash,menuMain,menuDestination,buttonDestiny,systemMessage,stageImageAll,back", 1, {0.f, 0.f}, -1, 0, {}, 0.f},
    // Score Attack (mode and stage select): its painted background (the largest unnamed root clip; the other one is
    // the title banner, numbered lower) on a sphere, like the hub's; the banner and the menus (top = the mode cards,
    // selectSt = the stage lists) on the panel
    {"GG2HudScoreAttackMovie",
     {"@largest", "", "", ""},
     {0.f, 0.f, 0.f, 0.f},
     {{0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}},
     "top,selectSt", 1, {0.f, 0.f}, -1, 0, {}, 0.f},
};
constexpr int kNumLayerSpecs = int(sizeof kLayerSpecs / sizeof kLayerSpecs[0]);
std::atomic<int> g_activeSpec{0};
std::vector<std::string> SplitNames(const char* list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* q = list;; ++q) {
        if (*q == ',' || !*q) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (!*q) break;
        } else {
            cur += *q;
        }
    }
    return out;
}
// phase per frame: -1 none, 0 panel, 1..4 = layer 0..3; written by the game thread for the frame whose snapshot it
// affects, read by the render thread
std::atomic<int8_t> g_phaseRing[16];
std::atomic<int> g_layerLag{1};
std::atomic<bool> g_cutoutsOn{false};  // cut-out layers shown (game thread -> composite)
std::atomic<uint32_t> g_cutMask{0};    // which cut-out layers are shown (bit per layer)
std::atomic<uint32_t> g_layMeasured[4];  // render thread: captures compared so far (per layer)
std::atomic<float> g_layDiff[4];         // render thread: the last capture's largest change from the one before
std::atomic<uint64_t> g_layMeasT[4];     // render thread: when the last compared capture was taken (GetTickCount64)
bool g_resetSettle = false;            // game thread: a new layered movie was found
std::atomic<int> g_dumpLayers{0};
std::atomic<bool> g_layersOff{false};  // dev: !menulayers 0
std::atomic<bool> g_roomDebug{false};  // dev: !roomdebug  // dev: bit per layer still to dump  // frames between setting the clips and the snapshot showing them
std::atomic<uintptr_t> g_layerPlayer{0};
float LayerDistOf(int l) { return kLayerSpecs[g_activeSpec.load()].dist[l]; }
// the title's wrap window, moved onto the grid's cell borders after each background capture: the captured image is
// read back (two strips) and the dark gaps between the cells are located (period 169.667 stage px). A window off
// the borders wrapped mid-cell, showing half of one character and half of another (seen in a screenshot);
// the clip's position read from the movie didn't match the captured snapshot (dev).
std::atomic<float> g_bgCropU{-1.f}, g_bgCropV{0.f};
const float* LayerCropOf(int l) {
    static thread_local float c[4];
    const auto& sp = kLayerSpecs[g_activeSpec.load()];
    if (l != 0 || sp.bgMode != 2 || g_bgCropU.load() < 0.f) return sp.crop[l];
    const float w = sp.crop[0][2] - sp.crop[0][0], h = sp.crop[0][3] - sp.crop[0][1];
    c[0] = g_bgCropU.load(), c[1] = g_bgCropV.load(), c[2] = c[0] + w, c[3] = c[1] + h;
    return c;
}
int LayerBgMode() { return kLayerSpecs[g_activeSpec.load()].bgMode; }
const float* LayerPan() { return kLayerSpecs[g_activeSpec.load()].pan; }
const LayerSpec& ActiveLayerSpec() { return kLayerSpecs[g_activeSpec.load()]; }
bool TitleLayersActive() { return !strcmp(kLayerSpecs[g_activeSpec.load()].movie, "GG2ScreenTitleMovie"); }
// title_logo_on_panel: the title logo (layer 3, with its unnamed animation pieces) stays on the panel instead of a
// cut-out. Its looping sparkles kept the layer "live" for ever, re-captured only every few frames (one layer per frame:
// Scaleform renders one snapshot per advance) - the animations looked laggy (0.9.59). On the panel they play every
// frame; the logo is the front-most title element anyway (1.6 m instead of 2.6 m).
int LayerOfClip(const LayerSpec& sp, int l) {
    return l == 3 && config::Get().vrTitleLogoOnPanel && !strcmp(sp.movie, "GG2ScreenTitleMovie") ? -1 : l;
}
bool LayerSplit(int& layer, float crop[4], float& dist) {
    const auto& sp = ActiveLayerSpec();
    if (sp.splitLayer <= 0) return false;
    layer = sp.splitLayer, dist = sp.splitDist;
    memcpy(crop, sp.splitCrop, 16);
    return true;
}
std::atomic<float> g_bgCaptureTime{0.f};  // tm.x clock when the background layer was captured (pan mode)
// movie players drawn in the last UI command (render thread -> game thread; replaces object scans)
std::atomic<uintptr_t> g_drawnPlayers[24];
// ---- title background grid measurement (render thread) ----
ID3D11Texture2D *g_gridA = nullptr, *g_gridB = nullptr;  // staging: a horizontal and a vertical strip of layer 0
bool g_gridPending = false;
uint32_t kGridAW = 1400, kGridAH = 160, kGridBW = 160, kGridBH = 2048;  // sized to the layer (band) on use
void QueueGridMeasure(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const uint32_t aw = g_lay[0].w * 1400 / 3640, ah = std::max(32u, g_lay[0].h * 160 / 2048), bw = std::max(32u, g_lay[0].w * 160 / 3640),
                   bh = g_lay[0].h;
    if (g_gridA && (aw != kGridAW || ah != kGridAH || bw != kGridBW || bh != kGridBH)) SafeRelease(g_gridA), SafeRelease(g_gridB);
    kGridAW = aw, kGridAH = ah, kGridBW = bw, kGridBH = bh;
    if (!g_gridA) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kGridAW, d.Height = kGridAH, d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_STAGING, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&d, nullptr, &g_gridA);
        d.Width = kGridBW, d.Height = kGridBH;
        dev->CreateTexture2D(&d, nullptr, &g_gridB);
        if (!g_gridA || !g_gridB) return;
    }
    if (g_lay[0].w < kGridAW || g_lay[0].h < kGridBH) return;
    const uint32_t ay = g_lay[0].h * 600 / 2048;
    const D3D11_BOX a{0, ay, 0, kGridAW, ay + kGridAH, 1}, b{0, 0, 0, kGridBW, kGridBH, 1};
    ctx->CopySubresourceRegion(g_gridA, 0, 0, 0, 0, g_lay[0].tex, 0, &a);
    ctx->CopySubresourceRegion(g_gridB, 0, 0, 0, 0, g_lay[0].tex, 0, &b);
    g_gridPending = true;
}
// ---- cut-out change detection (render thread): a 1/16-size copy of each capture, compared with the previous one ----
Target g_thumbSrc;                       // band-sized with mips: a capture is copied in and reduced
ID3D11Texture2D* g_thumbSt[4] = {};      // staging: mip 4 of the last capture per layer
bool g_thumbPending[4] = {}, g_thumbPendingAny = false;
uint64_t g_thumbT[4] = {};
std::vector<uint8_t> g_thumbPrev[4];
uint32_t g_thumbW = 0, g_thumbH = 0;
void QueueThumb(ID3D11Device* dev, ID3D11DeviceContext* ctx, int l) {
    const uint32_t w = g_lay[l].w, h = g_lay[l].h;
    if (!g_thumbSrc.tex || g_thumbSrc.w != w || g_thumbSrc.h != h) {
        for (auto& t : g_thumbSt) SafeRelease(t);
        for (auto& p : g_thumbPrev) p.clear();
        if (!g_thumbSrc.Create(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, true)) return;
        g_thumbW = std::max(1u, w >> 4), g_thumbH = std::max(1u, h >> 4);
    }
    if (!g_thumbSt[l]) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = g_thumbW, d.Height = g_thumbH, d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_STAGING, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_thumbSt[l]))) return;
    }
    ctx->CopySubresourceRegion(g_thumbSrc.tex, 0, 0, 0, 0, g_lay[l].tex, 0, nullptr);
    ctx->GenerateMips(g_thumbSrc.srv);
    ctx->CopySubresourceRegion(g_thumbSt[l], 0, 0, 0, 0, g_thumbSrc.tex, 4, nullptr);
    g_thumbPending[l] = g_thumbPendingAny = true;
    g_thumbT[l] = GetTickCount64();
}
void TryThumbs(ID3D11DeviceContext* ctx) {
    bool any = false;
    for (int l = 1; l < 4; ++l) {
        if (!g_thumbPending[l]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_thumbSt[l], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            any = true;
            continue;
        }
        g_thumbPending[l] = false;
        std::vector<uint8_t> cur(size_t(g_thumbW) * g_thumbH * 4);
        for (uint32_t y = 0; y < g_thumbH; ++y) memcpy(&cur[size_t(y) * g_thumbW * 4], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(g_thumbW) * 4);
        ctx->Unmap(g_thumbSt[l], 0);
        float diff = 255.f;
        if (g_thumbPrev[l].size() == cur.size()) {
            int mx = 0;
            for (size_t i = 0; i < cur.size(); ++i) mx = std::max(mx, std::abs(int(cur[i]) - int(g_thumbPrev[l][i])));
            diff = float(mx);
        }
        g_thumbPrev[l].swap(cur);
        g_layDiff[l] = diff;
        g_layMeasT[l] = g_thumbT[l];
        ++g_layMeasured[l];
        static int s_logs = 0;
        if (s_logs++ < 150) LOG("[ui] cut-out %d capture: largest change %.0f", l, diff);
    }
    g_thumbPendingAny = any;
}
// the phase (band px) whose lines, one period apart, are darkest on average
float DarkestPhase(const std::vector<float>& prof, float period) {
    float best = 0, bestV = 1e30f;
    for (float ph = 0; ph < period; ph += 0.5f) {
        float sum = 0;
        int n = 0;
        for (float x = ph; x < float(prof.size()) - 1; x += period) {
            const int i = int(x);
            const float f = x - float(i);
            sum += prof[size_t(i)] * (1 - f) + prof[size_t(i) + 1] * f, ++n;
        }
        if (n && sum / float(n) < bestV) bestV = sum / float(n), best = ph;
    }
    return best;
}
void TryGridMeasure(ID3D11DeviceContext* ctx) {
    D3D11_MAPPED_SUBRESOURCE ma{}, mb{};
    if (FAILED(ctx->Map(g_gridA, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ma))) return;
    if (FAILED(ctx->Map(g_gridB, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mb))) {
        ctx->Unmap(g_gridA, 0);
        return;
    }
    g_gridPending = false;
    std::vector<float> cols(kGridAW, 0.f), rows(kGridBH, 0.f);
    for (uint32_t y = 0; y < kGridAH; ++y) {
        const uint8_t* r = static_cast<const uint8_t*>(ma.pData) + size_t(y) * ma.RowPitch;
        for (uint32_t x = 0; x < kGridAW; ++x) cols[x] += float(r[4 * x] + r[4 * x + 1] + r[4 * x + 2]);
    }
    for (uint32_t y = 0; y < kGridBH; ++y) {
        const uint8_t* r = static_cast<const uint8_t*>(mb.pData) + size_t(y) * mb.RowPitch;
        for (uint32_t x = 0; x < kGridBW; ++x) rows[y] += float(r[4 * x] + r[4 * x + 1] + r[4 * x + 2]);
    }
    ctx->Unmap(g_gridA, 0), ctx->Unmap(g_gridB, 0);
    const float bw = float(g_lay[0].w), bh = float(g_lay[0].h);
    const float period = 169.667f * bw / 1920.f;  // band px
    const float px = DarkestPhase(cols, period), py = DarkestPhase(rows, period);
    float mean = 0;
    for (float v : cols) mean += v;
    mean /= float(cols.size()) * float(kGridAH) * 3.f;
    g_bgCropU = px / bw, g_bgCropV = (py + period) / bh;  // one row down from the top edge
    static int s_logs = 0;
    if (s_logs++ < 30)
        LOG("[ui] title background grid: gaps at band (%.1f, %.1f) -> wrap window (%.4f, %.4f), mean level %.1f", px, py, px / bw,
            (py + period) / bh, mean);
}
// Render the movies whose category bit is in `mask` (or exactly movie `only` if >= 0).
void RunSubset(void* cmd, uint32_t mask, int only = -1) {
    auto* arr = reinterpret_cast<MovieArray*>(static_cast<uint8_t*>(cmd) + 0x10);
    const int32_t num = int32_t(g_cats.size());
    int32_t n = 0;
    g_tmp.resize(size_t(num) * kMovieSize + 16);
    for (int32_t i = 0; i < num; ++i)
        if (only >= 0 ? i == only : ((mask >> g_cats[i]) & 1) != 0)
            memcpy(g_tmp.data() + size_t(n++) * kMovieSize, arr->data + size_t(i) * kMovieSize, kMovieSize);
    if (n == 0) return;
    if (n == arr->num) {
        o_Exec(cmd);
        return;
    }
    const MovieArray saved = *arr;
    arr->data = g_tmp.data(), arr->num = n;
    o_Exec(cmd);
    *arr = saved;
}

// Debug: quarter-size BMP of `tex` if anything was drawn (over black). Returns the covered fraction.
ID3D11Texture2D* g_staging = nullptr;
float DumpSmall(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd{};
    if (g_staging) g_staging->GetDesc(&sd);
    if (!g_staging || sd.Width != d.Width || sd.Height != d.Height || sd.Format != d.Format) {
        SafeRelease(g_staging);
        sd = d;
        sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0;
        sd.MipLevels = 1, sd.ArraySize = 1;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_staging))) return 0;
    }
    ctx->CopyResource(g_staging, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return 0;
    const uint32_t w = d.Width / 4, h = d.Height / 4, row = w * 3, pad = (4 - row % 4) % 4;
    std::vector<uint8_t> img(size_t(row + pad) * h, 0);
    uint64_t covered = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y * 4) * m.RowPitch;
        uint8_t* dst = img.data() + size_t(h - 1 - y) * (row + pad);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* px = src + size_t(x) * 16;
            dst[x * 3 + 0] = px[2], dst[x * 3 + 1] = px[1], dst[x * 3 + 2] = px[0];
            if (px[0] + px[1] + px[2] > 24) ++covered;
        }
    }
    ctx->Unmap(g_staging, 0);
    const float frac = float(covered) / float(w * h);
    if (frac < 0.0005f) return frac;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return frac;
    uint8_t hdr[54]{'B', 'M'};
    auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
    put32(2, 54 + uint32_t(img.size())), put32(10, 54), put32(14, 40), put32(18, w), put32(22, h);
    hdr[26] = 1, hdr[28] = 24, put32(34, uint32_t(img.size()));
    fwrite(hdr, 1, 54, f);
    fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    return frac;
}
int g_dumpSet = 0;
bool g_dumpedThisRequest = false;
void DumpMovies(void* cmd, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* bb) {
    if (!xr::DumpPending()) {
        g_dumpedThisRequest = false;
        return;
    }
    if (g_dumpedThisRequest) return;
    g_dumpedThisRequest = true;
    static const float kBlack[4] = {0, 0, 0, 1};
    wchar_t sub[32];
    swprintf_s(sub, L"%03d", ++g_dumpSet);
    const std::wstring dir = log::Directory() + L"\\ui_dumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring setDir = dir + L"\\" + sub;
    CreateDirectoryW(setDir.c_str(), nullptr);
    std::string visible;
    for (int i = 0; i < int(g_cats.size()); ++i) {
        ctx->ClearRenderTargetView(g_bbRtv, kBlack);
        RunSubset(cmd, 0, i);
        const std::string cls = MovieClass(g_players[i]);
        const float frac =
            DumpSmall(dev, ctx, bb, setDir + L"\\" + std::to_wstring(i) + L"_" + std::wstring(cls.begin(), cls.end()) + L".bmp");
        if (frac >= 0.0005f) {
            char buf[160];
            sprintf_s(buf, "%s%s %.1f%%", visible.empty() ? "" : ", ", cls.c_str(), frac * 100.f);
            visible += buf;
        }
    }
    DumpSmall(dev, ctx, g_save.tex, setDir + L"\\_eye1_view.bmp");  // the 3D view (right eye, no UI)
    for (int l = 0; l < 4; ++l)  // layered menu layers
        if (g_layValid[l] && g_lay[l].tex) DumpSmall(dev, ctx, g_lay[l].tex, setDir + L"\\_layer" + std::to_wstring(l) + L".bmp");
    if (g_scv.tex) {  // the zoom scope render of this frame, full size (test: compare with the eye at the reticle)
        xr::DumpTexture(g_scv.tex, L"dump_scope_view");
        xr::DumpTexture(g_save.tex, L"dump_eye1_noui");
        LOG("[ui] dump: scope view frame %llu (current %llu), reticle eye1 px (%.0f,%.0f)", g_scvFrame, xr::CurrentFrameIdRT(), g_lastRx, g_lastRy);
    }
    if (g_fs.tex) DumpSmall(dev, ctx, g_fs.tex, setDir + L"\\_fs_layer.bmp");
    if (g_ui.tex) DumpSmall(dev, ctx, g_ui.tex, setDir + L"\\_panel_layer.bmp");
    if (g_ui.tex) xr::DumpTexture(g_ui.tex, L"dump_panel_full");  // full size + _alpha
    LOG("[ui] dump set %03d: visible movies: %s (fs valid %d, tint valid %d)", g_dumpSet,
        visible.empty() ? "(none)" : visible.c_str(), g_fsValid, g_tintValid);
}

// Stereo frame, left eye: capture the full-screen effects and the tint (once per frame), then draw them
// over the eye image. Right eye: capture the panel, then draw the effects and tint over the eye image.
uint32_t ExecInner(void* cmd);
uint32_t __fastcall hk_Exec(void* cmd) {  // timed for [perf] (GPU + render thread)
    const int eye = xr::CurrentEyeRT();
    if (eye != 0 && eye != 1) return ExecInner(cmd);
    xr::PerfUiMark(eye, true);
    const uint32_t r = ExecInner(cmd);
    xr::PerfUiMark(eye, false);
    return r;
}
uint32_t ExecInner(void* cmd) {
    const int eye = xr::CurrentEyeRT();
    if (g_thumbPendingAny) {
        ID3D11Device* td = nullptr;
        if (g_thumbSrc.tex) g_thumbSrc.tex->GetDevice(&td);
        if (td) {
            ID3D11DeviceContext* tc = nullptr;
            td->GetImmediateContext(&tc);
            TryThumbs(tc);
            tc->Release(), td->Release();
        }
    }
    if (g_gridPending) {
        ID3D11Device* gd = nullptr;
        if (g_gridA) g_gridA->GetDevice(&gd);
        if (gd) {
            ID3D11DeviceContext* gc = nullptr;
            gd->GetImmediateContext(&gc);
            TryGridMeasure(gc);
            gc->Release(), gd->Release();
        }
    }
    if (!g_enabled || eye < 0) return o_Exec(cmd);
    if (eye == 2) return kGfxRenderUiCmdSize;  // zoom scope render: the world only (kept at its Present)
    Classify(cmd);

    IDXGISwapChain* sc = xr::SwapChainRT();
    ID3D11Texture2D* bb = nullptr;
    if (!sc || FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return kGfxRenderUiCmdSize;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    static const float kBlack[4] = {0, 0, 0, 1}, kWhite[4] = {1, 1, 1, 1};
    if (Ensure(dev, bb)) {
        ctx->CopyResource(g_save.tex, bb);
        const uint64_t frame = xr::CurrentFrameIdRT();
        if (eye == 0 || g_fsFrame != frame) {
            g_fsFrame = frame;
            // full-screen effects -> g_fs
            g_fsValid = false;
            if (g_present & (1u << kFullscreen)) {
                ctx->ClearRenderTargetView(g_bbRtv, kBlack);
                RunSubset(cmd, 1u << kFullscreen);
                ctx->CopyResource(g_black.tex, bb);
                ctx->ClearRenderTargetView(g_bbRtv, kWhite);
                RunSubset(cmd, 1u << kFullscreen);
                ctx->CopyResource(g_white.tex, bb);
                g_fsValid = Combine(dev, ctx, g_fs);
            }
            g_eoValid = false;
            if ((g_present & (1u << kEye)) && LazyTarget(dev, g_eo, false)) {  // eye overlay -> g_eo
                ctx->ClearRenderTargetView(g_bbRtv, kBlack);
                RunSubset(cmd, 1u << kEye);
                ctx->CopyResource(g_black.tex, bb);
                ctx->ClearRenderTargetView(g_bbRtv, kWhite);
                RunSubset(cmd, 1u << kEye);
                ctx->CopyResource(g_white.tex, bb);
                g_eoValid = Combine(dev, ctx, g_eo, false, nullptr, true);
                if (xr::DumpPending()) {  // debug: the full-screen capture pair
                    const std::wstring dir = log::Directory() + L"\\ui_dumps";
                    CreateDirectoryW(dir.c_str(), nullptr);
                    const float fb = DumpSmall(dev, ctx, g_black.tex, dir + L"\\fs_black_eye" + std::to_wstring(eye) + L".bmp");
                    const float fw = DumpSmall(dev, ctx, g_white.tex, dir + L"\\fs_white_eye" + std::to_wstring(eye) + L".bmp");
                    LOG("[ui] fs capture eye %d: over black %.1f%% covered, over white %.1f%%", eye, fb * 100, fw * 100);
                }
            }
            // the game's sight (reticle_style=1): a square around the band centre, where SetSightPosition put it -
            // only that region of the black / white captures is copied (the scope's whole-frame capture cost ~1 ms)
            g_sgtValid = false;
            if (g_present & (1u << kSight)) {
                uint32_t x0, y0, bw, bh;
                Band(g_desc.Width, g_desc.Height, x0, y0, bw, bh);
                const uint32_t sz = std::min(bh, g_desc.Width / 2) & ~1u;
                if (sz >= 64 && (g_sgt.tex || g_sgt.Create(dev, sz, sz, DXGI_FORMAT_R8G8B8A8_UNORM, true))) {
                    g_sgtX0 = x0 + bw / 2 - sz / 2, g_sgtY0 = y0 + bh / 2 - sz / 2;
                    const D3D11_BOX box{g_sgtX0, g_sgtY0, 0, g_sgtX0 + sz, g_sgtY0 + sz, 1};
                    ctx->ClearRenderTargetView(g_bbRtv, kBlack);
                    RunSubset(cmd, 1u << kSight);
                    ctx->CopySubresourceRegion(g_black.tex, 0, g_sgtX0, g_sgtY0, 0, bb, 0, &box);
                    ctx->ClearRenderTargetView(g_bbRtv, kWhite);
                    RunSubset(cmd, 1u << kSight);
                    ctx->CopySubresourceRegion(g_white.tex, 0, g_sgtX0, g_sgtY0, 0, bb, 0, &box);
                    g_sgtValid = CombineRegion(dev, ctx, g_sgt, g_sgtX0, g_sgtY0);
                    static int s_logs = 0;
                    if (s_logs++ < 1) LOG("[ui] game sight as the reticle: %ux%u square at (%u,%u)", sz, sz, g_sgtX0, g_sgtY0);
                }
            }
            g_scoValid = false;
            // (only while zoomed: the composite uses it only then - the movie is present all through a stage, and its
            // capture cost ~1 ms of GPU every frame for nothing, perf 0.9.56)
            if ((g_present & (1u << kScope)) && vr::ZoomFactor() > 1.001f && LazyTarget(dev, g_sco, false)) {  // the game's zoom scope -> g_sco
                ctx->ClearRenderTargetView(g_bbRtv, kBlack);
                RunSubset(cmd, 1u << kScope);
                ctx->CopyResource(g_black.tex, bb);
                ctx->ClearRenderTargetView(g_bbRtv, kWhite);
                RunSubset(cmd, 1u << kScope);
                ctx->CopyResource(g_white.tex, bb);
                g_scoValid = Combine(dev, ctx, g_sco, false, nullptr, true);
            }
            // tint movies: the colour of their stage corner (their full-stage background)
            g_tintValid = false;
            if (g_present & (1u << kTint)) {
                uint32_t x0, y0, bw, bh;
                Band(g_desc.Width, g_desc.Height, x0, y0, bw, bh);
                const D3D11_BOX px{x0 + 6, y0 + 6, 0, x0 + 7, y0 + 7, 1};
                ctx->ClearRenderTargetView(g_bbRtv, kBlack);
                RunSubset(cmd, 1u << kTint);
                ctx->CopySubresourceRegion(g_tintB.tex, 0, 0, 0, 0, bb, 0, &px);
                ctx->ClearRenderTargetView(g_bbRtv, kWhite);
                RunSubset(cmd, 1u << kTint);
                ctx->CopySubresourceRegion(g_tintW.tex, 0, 0, 0, 0, bb, 0, &px);
                g_tintValid = true;
            }
        }
        if (eye == 1) {
            // layered menu movie: this frame shows one phase (see kLayerSpecs): a layer -> g_lay[], or the panel
            const int phase = g_phaseRing[frame % 16].load();
            int layerMovie = -1;
            for (int i = 0; i < int(g_cats.size()); ++i)
                if (g_cats[i] == kPanel && g_players[i] && g_players[i] == g_layerPlayer.load()) layerMovie = i;
            static uintptr_t s_layerOwner = 0;  // a different layered movie: its layers start empty
            if (layerMovie < 0 || phase < 0 || s_layerOwner != g_layerPlayer.load())
                g_layValid[0] = g_layValid[1] = g_layValid[2] = g_layValid[3] = false;
            s_layerOwner = g_layerPlayer.load();
            // menu layer textures (title / hub / Score Attack / Options) go after 30 s without a layered menu
            static ULONGLONG s_layUsed = 0;
            if (layerMovie >= 0) s_layUsed = GetTickCount64();
            else if (g_lay[0].tex && GetTickCount64() - s_layUsed > 30000) {
                for (auto& t : g_lay) t.Release();
                LOG("[ui] menu layer targets released (no layered menu for 30 s)");
            }
            if (layerMovie >= 0 && phase >= 1 && phase <= 4 && LazyTarget(dev, g_lay[phase - 1], true, phase == 1)) {
                const int l = phase - 1;
                ctx->ClearRenderTargetView(g_bbRtv, kBlack);
                RunSubset(cmd, 0, layerMovie);
                ctx->CopyResource(g_black.tex, bb);
                ctx->ClearRenderTargetView(g_bbRtv, kWhite);
                RunSubset(cmd, 0, layerMovie);
                ctx->CopyResource(g_white.tex, bb);
                g_layValid[l] = Combine(dev, ctx, g_lay[l]);
                g_layDist[l] = LayerDistOf(l);
                if (l >= 1 && g_layValid[l]) QueueThumb(dev, ctx, l);
                if (l == 0 && g_layValid[0]) {
                    if (LayerBgMode() == 2) QueueGridMeasure(dev, ctx);
                    ctx->GenerateMips(g_lay[0].srv);  // blurred extension (extend mode)
                    g_bgCaptureTime = float(GetTickCount64() % 3600000ull) / 1000.f;
                }
                if (g_layValid[l] && (g_dumpLayers.fetch_and(~(1 << l)) & (1 << l))) {  // dev: !dumplayers
                    xr::DumpTexture(g_lay[l].tex, (L"dump_layer" + std::to_wstring(l)).c_str());
                }
                if (g_ui.tex) xr::SetUiImageRT(g_ui.tex, frame);  // the panel keeps its last image this frame
                ctx->CopyResource(bb, g_save.tex);
                Composite(dev, ctx);
                SafeRelease(g_bbRtv);
                ctx->Release();
                dev->Release();
                bb->Release();
                return kGfxRenderUiCmdSize;
            }
            // panel (+ tint movies): UI over black / white -> g_ui
            ctx->ClearRenderTargetView(g_bbRtv, kBlack);
            RunSubset(cmd, (1u << kPanel) | (1u << kTint));
            ctx->CopyResource(g_black.tex, bb);
            ctx->ClearRenderTargetView(g_bbRtv, kWhite);
            RunSubset(cmd, (1u << kPanel) | (1u << kTint));
            ctx->CopyResource(g_white.tex, bb);
            DumpMovies(cmd, dev, ctx, bb);
            ZoomGeo zg;
            xr::FrameRecord zr;
            const bool zoom = ZoomActive() && xr::CurrentRecordRT(zr) && zr.halfH > 0 && zr.halfV > 0;
            if (zoom) {
                XrVector3f c0, cu, cv;
                PanelInEye(zr, c0, cu, cv);
                zg.factor = vr::ZoomFactor();
                const float p0[4] = {c0.x, c0.y, c0.z, 0}, pu[4] = {cu.x, cu.y, cu.z, 0}, pv[4] = {cv.x, cv.y, cv.z, 0};
                const float pj[4] = {std::tan(zr.halfH), std::tan(zr.halfV), float(g_desc.Width), float(g_desc.Height)};
                memcpy(zg.p0, p0, 16), memcpy(zg.pu, pu, 16), memcpy(zg.pv, pv, 16), memcpy(zg.proj, pj, 16);
            }
            // minigames: the panel also shows the canvas above and below the band, where the request icons on her
            // body are drawn when they are outside it (0.9.21: the rub / poke icons couldn't be seen or touched)
            uint32_t ext = 0;
            {
                uint32_t x0, y0, bw, bh;
                Band(g_desc.Width, g_desc.Height, x0, y0, bw, bh);
                ext = (g_tallPanel.load() || g_stageRows.load()) && !zoom ? y0 : 0;
                if (g_ui.h != bh + 2 * ext || g_ui.w != bw) {
                    g_ui.Create(dev, bw, bh + 2 * ext, DXGI_FORMAT_R8G8B8A8_UNORM, true);
                    LOG("[ui] panel image %ux%u (%u rows above and below the band)", bw, bh + 2 * ext, ext);
                }
                g_panelExtFrac = bh ? float(ext) / float(bh) : 0.f;
                g_uiExtRows = ext, g_uiBandW = bw, g_uiBandH = bh;
            }
            if (Combine(dev, ctx, g_ui, !zoom, zoom ? &zg : nullptr, false, true, ext)) {
                // name_tags_3d: the parked tag's region of this same capture -> g_tag, and blanked on the panel
                g_tagValid = false;
                if (config::Get().vrNameTags3d && GetTickCount64() - g_tagParkT.load() < 250) {
                    uint32_t x0, y0, bw, bh;
                    Band(g_desc.Width, g_desc.Height, x0, y0, bw, bh);
                    const float s = float(bw) / 1920.f;
                    const int32_t rx = int32_t(float(x0) + (kTagParkX - kTagBoxW / 2) * s), ry = int32_t(float(y0) + (kTagParkY - kTagBoxUp) * s);
                    const uint32_t tw = uint32_t(kTagBoxW * s) & ~1u, th = uint32_t((kTagBoxUp + kTagBoxDown) * s) & ~1u;
                    if (rx >= 0 && ry >= 0 && rx + tw <= g_desc.Width && ry + th <= g_desc.Height && tw >= 16 && th >= 16) {
                        if (!g_tag.tex || !g_tagZero.tex || g_tag.w != tw || g_tag.h != th) {
                            g_tag.Create(dev, tw, th, DXGI_FORMAT_R8G8B8A8_UNORM, true);
                            g_tagZero.Create(dev, tw, th, DXGI_FORMAT_R8G8B8A8_UNORM, true);
                            static const float kClear[4] = {0, 0, 0, 0};
                            if (g_tagZero.rtv) ctx->ClearRenderTargetView(g_tagZero.rtv, kClear);
                            LOG("[ui] 3D name tag region %ux%u at (%d,%d) of the %ux%u capture", tw, th, rx, ry, g_desc.Width, g_desc.Height);
                        }
                        g_tagValid = g_tag.tex && CombineRegion(dev, ctx, g_tag, uint32_t(rx), uint32_t(ry));
                        // the panel image covers rows y0 - ext .. : blank the spot where it overlaps
                        const int32_t uy = ry - int32_t(y0 - ext), ux = rx - int32_t(x0);
                        if (g_tagZero.tex && ux >= 0 && uy >= 0 && uint32_t(ux) + tw <= g_ui.w && uint32_t(uy) < g_ui.h) {
                            const D3D11_BOX box{0, 0, 0, tw, std::min(th, g_ui.h - uint32_t(uy)), 1};
                            ctx->CopySubresourceRegion(g_ui.tex, 0, uint32_t(ux), uint32_t(uy), 0, g_tagZero.tex, 0, &box);
                        }
                    }
                }
                xr::SetUiImageRT(g_ui.tex, frame);
                if (++g_captures == 1) LOG("[ui] first UI layer image (%ux%u)", g_ui.w, g_ui.h);
            }
        }
        ctx->CopyResource(bb, g_save.tex);
        Composite(dev, ctx);
    }
    SafeRelease(g_bbRtv);
    ctx->Release();
    dev->Release();
    bb->Release();
    return kGfxRenderUiCmdSize;
}
// ---- kiss marks / shoe prints ----
// GG2HudShootingEffectMovie.AS_SetEffectKiss / AS_SetEffectKick call ActionScriptVoid, which ends in this wrapper
// (GFx movie view, ActionScript method name, result, args, count) -> movie root Invoke. Only those methods
// are dropped; everything else in that movie (the weak-spot pop-ups, boss markers, ...) is untouched.
constexpr uint32_t kGfxInvoke = 0x1B064A0;
constexpr uint8_t kGfxInvokeBytes[] = {0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x49, 0x18, 0x8B, 0x44, 0x24, 0x60};
using InvokeFn = bool(__fastcall*)(void*, const char*, void*, const void*, uint32_t);
InvokeFn o_Invoke = nullptr;
std::atomic<int> g_invokeLogs{0};
bool IsScreenMark(const char* m) {
    const char* tail = strrchr(m, '.');
    tail = tail ? tail + 1 : m;
    static const char* const kNames[] = {"StartEffKiss", "StartEffKick", "StartEffKickEx",
                                         "SetEffectKiss", "SetEffectKick", "SetEffectKickEx"};
    for (const char* n : kNames)
        if (!strcmp(tail, n)) return true;
    return false;
}
std::atomic<bool> g_continueUp{false};
float g_lastAeIcon[4][2]{};  // dev: the last hidden / request icon's / gesture guide's panel position (stage coords)
// Request kinds showing, by marker serial (AS_SetRequestIconType(serial, touch control type) .. EndRequestIcon(serial)):
// the HUD maps request type -> icon type with m_MarkerRequest_TouchControlType (0 poke, 1 rub, 2 flick, 3 hold)
std::atomic<uint32_t> g_flickSerials{0};  // bit per serial (0..31) of the flick requests showing
std::atomic<bool> g_gesturePhase{false};   // StartResistGauge .. EndResistGauge
std::atomic<float> g_aeMaskScale{100.f};    // the game's reveal circle scale (percent; its shape: radius 278 stage units)
std::atomic<bool> g_aeRequestRevealed{true};
// hidden icons by serial: their panel position (SetTargetIconPosition, every frame while alive)
struct AeIcon { float x = 0, y = 0; ULONGLONG t = 0; int shown = -1; };
AeIcon g_aeIcons[32];
std::atomic<bool> g_aeUnmasked{false};  // the hidden-icon layer's mask removed (this minigame movie)
std::atomic<bool> g_aeNewIcon{false};   // a hidden icon was placed for the first time (or after a gap): a new one
int RequestTypeOfIcon(int icon) {
    static int s_map[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    static bool s_read = false;
    if (!s_read) {
        s_read = true;
        const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBaseHud", "Class");
        const uintptr_t d = ue3::FindObject("GG2Game.Default__GG2ActionEventBaseHud");
        ue3::PropInfo q;
        if (c && d && ue3::FindProperty(c, "m_MarkerRequest_TouchControlType", q)) {
            std::string m;
            for (uint32_t i = 0; i < q.arrayDim && i < 8; ++i) {
                uint8_t v = 0xFF;
                game::Read(d + q.offset + i * q.elementSize, v);
                if (v < 8 && s_map[v] < 0) s_map[v] = int(i);
                m += std::to_string(v) + (i + 1 < q.arrayDim ? "," : "");
            }
            LOG("[ae] request type -> icon type: %s", m.c_str());
        } else {
            LOG("[ae] m_MarkerRequest_TouchControlType not found: icon type = request type");
            for (int i = 0; i < 8; ++i) s_map[i] = i;
        }
    }
    return icon >= 0 && icon < 8 ? s_map[icon] : -1;
}
ULONGLONG g_lastAeIconT[4]{};
// effect_stereo: one 3D item's x as this eye's draw placed it (game thread)
void RecordEffectX(uint32_t key, double x, double y) {
    const int eye = vr::DrawingEye();
    if (eye < 0) return;
    const ULONGLONG now = GetTickCount64();
    if (y <= -100.0) {  // hidden (the game parks it at -200)
        g_stItems.erase(key);
    } else {
        StereoItem& it = g_stItems[key];
        if (now - it.t > 250) it = StereoItem{};
        it.x[eye] = x, it.has[eye] = true, it.t = now;
        if (eye == 1 && it.has[0]) {  // a left / right pair: its distance
            const float dm = vr::StereoDistanceM(it.x[0], it.x[1]);
            if (dm > 0.f) it.distM = it.distM > 0.f ? it.distM * 0.8f + dm * 0.2f : dm;
        }
    }
    // the snapshot holds the second eye's positions (its calls are the last before the movie is captured)
    double s0 = 0;
    int cnt = 0;
    for (auto i = g_stItems.begin(); i != g_stItems.end();) {
        if (now - i->second.t > 250) {
            i = g_stItems.erase(i);
            continue;
        }
        if (i->second.has[0] && i->second.has[1]) s0 += i->second.x[0] - i->second.x[1], ++cnt;
        ++i;
    }
    const float d = cnt ? float(s0 / cnt) : 0.f;
    g_eoShiftD = d, g_eoItemsT = cnt ? now : 0;
    static int s_logs = 0;
    static ULONGLONG s_t = 0;
    if (cnt && (s_logs < 6 || now - s_t > 3000) && std::fabs(d) > 0.5f)
        ++s_logs, s_t = now, LOG("[ui] effect stereo: %d item(s), left eye layer moved %.1f stage units (key %X)", cnt, d, key);
}
double ArgNum(const uint8_t* a, int k) {
    const uint32_t t = *reinterpret_cast<const uint32_t*>(a + k * 48 + 0x18);
    return t == 5 ? *reinterpret_cast<const double*>(a + k * 48 + 0x20) : double(*reinterpret_cast<const int32_t*>(a + k * 48 + 0x20));
}
// effect_full_view: an effect movie position argument moved by dy (double or int)
bool ArgAdd(uint8_t* a, int k, double dy) {
    const uint32_t t = *reinterpret_cast<uint32_t*>(a + k * 48 + 0x18);
    if (t == 5) return *reinterpret_cast<double*>(a + k * 48 + 0x20) += dy, true;
    if (t == 3 || t == 4) return *reinterpret_cast<int32_t*>(a + k * 48 + 0x20) += int32_t(std::lround(dy)), true;
    return false;
}
// a weak-spot pop-up's size factor: weak_spot_scale, and with weak_spot_distance_scale its measured distance (0.9.71: the game asked for
// 100% at any distance - reported: "weak-spot pop-ups still seem massive for girls at a distance")
float WeakSpotFactor(uint32_t id) {
    const auto& c = config::Get();
    float k = (c.vrWeakSpotSize > 0.f ? c.vrWeakSpotSize : 1.f) * vr::FlatProportion();
    if (!c.vrWeakSpotDistScale) return k;
    const auto i = g_stItems.find(0x10000u | (id & 0xFFFF));
    if (i == g_stItems.end() || i->second.distM <= 0.f) return k;
    const float d = i->second.distM, full = c.vrWeakSpotFullSizeM > 0.1f ? c.vrWeakSpotFullSizeM : 1.5f;
    const float f = std::fmax(std::fmin(full / d, 1.f), std::fmin(std::fmax(c.vrWeakSpotMinSize, 0.05f), 1.f));
    static ULONGLONG s_t = 0;
    static int s_n = 0;
    if (s_n < 300 && GetTickCount64() - s_t > 500) ++s_n, s_t = GetTickCount64(), LOG("[ui] weak-spot pop-up #%u at %.1f m -> size x%.2f (x%.2f weak_spot_scale)", id, d, f, k);
    return k * f;
}
// fddm_doki_offsets (0.9.92): FDDM doki effect I's current fly-out offset (GG2DokiSpHeroineHud.UpdateDkdkEffPos:
// position = GetGirlPos2D(part) + m_movePosOffset), in flat-screen stage units
bool FddmDokiOffset(int i, float& ox, float& oy) {
    if (i < 0 || i >= 10) return false;
    const uintptr_t pc = vr::CurrentPc();
    if (!pc || !ue3::IsObject(pc)) return false;
    static uintptr_t s_pcCls = 0, s_hudCls = 0;
    static int s_offHud = -1, s_offTbl = -1, s_elem = 0;
    const uintptr_t pcCls = ue3::ClassOf(pc);
    if (pcCls != s_pcCls) {
        s_pcCls = pcCls, s_offHud = -1;
        ue3::PropInfo pi;
        if (ue3::FindProperty(pcCls, "myHUD", pi)) s_offHud = int(pi.offset);
    }
    uintptr_t hud = 0;
    if (s_offHud < 0 || !game::Read(pc + s_offHud, hud) || !hud || !ue3::IsObject(hud)) return false;
    const uintptr_t hudCls = ue3::ClassOf(hud);
    if (hudCls != s_hudCls) {
        s_hudCls = hudCls, s_offTbl = -1;
        ue3::PropInfo pi;
        if (ue3::FindProperty(hudCls, "m_hudManagerDkdkEffDataTbl", pi) && pi.elementSize == 0x2C) s_offTbl = int(pi.offset), s_elem = int(pi.elementSize);
        LOG("[ui] FDDM doki table on %s: %s", ue3::ObjName(hud).c_str(), s_offTbl >= 0 ? "found" : "not found");
    }
    if (s_offTbl < 0) return false;
    float o[2]{};
    if (!game::Read(hud + s_offTbl + i * s_elem + 0x1C, o)) return false;  // SDkdkHudEffData.m_movePosOffset
    if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || std::fabs(o[0]) > 4000.f || std::fabs(o[1]) > 4000.f) return false;
    ox = o[0], oy = o[1];
    return true;
}
bool EffectFullView() { return config::Get().vrEffectFullView && config::Get().vrMode == 2 && EffectMovieInEye(); }
bool __fastcall hk_Invoke(void* view, const char* method, void* result, const void* args, uint32_t n) {
    if (method) {  // diag: every distinct ActionScript method called (pause menu input, damage events, ...)
        // the Continue prompt's movie stays open (root visible) after Continue: its own calls bound the prompt
        // (0.9.9: the menu pointer stayed on in the stage - the trigger clicked instead of shooting)
        if (!strcmp(method, "StartContinueWipe")) g_continueUp = true;
        else if (!strcmp(method, "EndContinue")) g_continueUp = false;
        static std::mutex s_mu;
        static std::set<std::string> s_seen;
        std::lock_guard l(s_mu);
        if (s_seen.size() < 400 && s_seen.insert(method).second) LOG("[ui.as] %s (%u args)", method, n);
        if (GetTickCount64() < g_asTraceUntil.load()) {
            char av[160] = "";
            int o = 0;
            for (uint32_t i = 0; i < n && i < 4 && args && o < 120; ++i) {
                const uint8_t* g = static_cast<const uint8_t*>(args) + i * 48;
                const uint32_t t = *reinterpret_cast<const uint32_t*>(g + 0x18);
                if (t == 5) o += snprintf(av + o, sizeof av - o, " %.1f", *reinterpret_cast<const double*>(g + 0x20));
                else if (t == 3 || t == 4) o += snprintf(av + o, sizeof av - o, " %d", *reinterpret_cast<const int32_t*>(g + 0x20));
                else o += snprintf(av + o, sizeof av - o, " t%u", t);
            }
            LOG("[ui.trace] %s(%s) view %p draw %llu", method, av, view, vr::DrawCount());
        }
    }
    static std::atomic<int> s_anyLogs{0};
    if (method && ((strstr(method, "Eff") || strstr(method, "Kiss") || strstr(method, "Kick")) ? g_invokeLogs.fetch_add(1) < 40
                                                                                               : s_anyLogs.fetch_add(1) < 8))
        LOG("[ui] ActionScript call: %s (%u args)", method, n);
    // Note: scaling the stencil-sight movie's per-frame SetSightScale(percent) does NOT change which hidden objects
    // are revealed (test: still revealed at 2x the scaled radius); the reveal radius is native and fixed.
    if (method && g_enabled) {
        const char* tail = strrchr(method, '.');
        tail = tail ? tail + 1 : method;
        // the player was hit (the game itself sends no rumble here): a strong buzz on both controllers
        if (!strcmp(tail, "StartDamage") && config::Get().vrHaptics) xr::input::Rumble(2, 0.9f, 0.25f);
        // the girl's name label (main HUD, on the panel) is placed at her eye-image projection: move it to where
        // that direction crosses the panel, so it sits above her head as seen through the panel
        // weak-spot pop-ups (SetEffectWeakMove(no, x, y, scale), every frame): smaller in VR
        const float ks = (config::Get().vrWeakSpotSize > 0.f ? config::Get().vrWeakSpotSize : 1.f) * vr::FlatProportion();
        if (!strcmp(tail, "SetEffectWeakMove") && n == 4 && args) {  // diagnostic (read only): the game's size per weak-spot pop-up
            static ULONGLONG s_t = 0;
            static int s_n = 0;
            const uint8_t* q = static_cast<const uint8_t*>(args);
            auto num = [&](int k) {
                const uint32_t t = *reinterpret_cast<const uint32_t*>(q + k * 48 + 0x18);
                return t == 5 ? *reinterpret_cast<const double*>(q + k * 48 + 0x20)
                              : double(*reinterpret_cast<const int32_t*>(q + k * 48 + 0x20));
            };
            if (num(2) > -100.0 && s_n < 400 && GetTickCount64() - s_t > 250)
                s_t = GetTickCount64(), ++s_n,
                LOG("[ui] weak-spot pop-up #%.0f at (%.0f,%.0f) game size %.1f%%", num(0), num(1), num(2), num(3));
        }
        if (!strcmp(tail, "SetEffectWeakMove") && n == 4 && args && config::Get().vrMode == 2 && EffectMovieInEye()) {
            const uint8_t* q = static_cast<const uint8_t*>(args);
            if (config::Get().vrEffectStereo || config::Get().vrWeakSpotDistScale)
                RecordEffectX(0x10000u | (uint32_t(ArgNum(q, 0)) & 0xFFFF), ArgNum(q, 1), ArgNum(q, 2));
        }
        if (!strcmp(tail, "SetEffectWeakMove") && n == 4 && args && EffectFullView() && ArgNum(static_cast<const uint8_t*>(args), 2) > -100.0) {
            uint8_t a[4 * 48];  // weak-spot pop-up low in the view (0.9.70 C): y - band top, the layer composited unshifted
            memcpy(a, args, sizeof a);
            ArgAdd(a, 2, -double(vr::BandTopStage()));
            const uint32_t t = *reinterpret_cast<uint32_t*>(a + 144 + 0x18);
            const float kk = WeakSpotFactor(uint32_t(ArgNum(a, 0)));
            if (std::fabs(kk - 1.f) > 0.001f) {
                if (t == 5) *reinterpret_cast<double*>(a + 144 + 0x20) *= kk;
                else if (t == 3 || t == 4) *reinterpret_cast<int32_t*>(a + 144 + 0x20) = int32_t(std::lround(double(*reinterpret_cast<int32_t*>(a + 144 + 0x20)) * kk));
            }
            return o_Invoke(view, method, result, a, n);
        }
        if (!strcmp(tail, "SetEffectWeakMove") && n == 4 && args && std::fabs(WeakSpotFactor(uint32_t(ArgNum(static_cast<const uint8_t*>(args), 0))) - 1.f) > 0.001f) {
            uint8_t a[4 * 48];
            memcpy(a, args, sizeof a);
            const float kk = WeakSpotFactor(uint32_t(ArgNum(a, 0)));
            const uint32_t t = *reinterpret_cast<uint32_t*>(a + 144 + 0x18);
            static int s_kLogs = 0;
            if (s_kLogs++ < 4)
                LOG("[ui] weak-spot pop-up scale arg type %u value %.3f -> x%.2f", t,
                    t == 5 ? *reinterpret_cast<double*>(a + 144 + 0x20) : double(*reinterpret_cast<int32_t*>(a + 144 + 0x20)), ks);
            if (t == 5) {
                *reinterpret_cast<double*>(a + 144 + 0x20) *= kk;
                return o_Invoke(view, method, result, a, n);
            }
            if (t == 3 || t == 4) {
                int32_t& v = *reinterpret_cast<int32_t*>(a + 144 + 0x20);
                v = int32_t(std::lround(double(v) * kk));
                return o_Invoke(view, method, result, a, n);
            }
        }
        if (!strcmp(tail, "StartResistGauge") || !strcmp(tail, "EndResistGauge")) {
            const bool on = tail[0] == 'S';
            if (g_gesturePhase.exchange(on) != on) LOG("[ae] gesture phase %s", on ? "on" : "off");
        }
        {  // minigame outcomes (test logs): requests started / ended, icons hit, the satisfaction gauge
            static const char* const kEv[] = {"StartRequestIcon", "EndRequestIcon", "StartTargetIconDamage", "StartTargetIconDelete",
                                              "SetSatisfyGauge", "StartResistGaugeSuccess", "SetResistGauge", "EndTargetIcon",
                                              "SetRequestIconType"};
            static std::atomic<int> s_evLogs{0};
            for (const char* e : kEv)
                if (!strcmp(tail, e) && s_evLogs.load() < 400) {
                    static double s_lastGauge = -1;
                    double v0 = 0;
                    const uint32_t t0 = n && args ? *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(args) + 0x18) : 0;
                    if (t0 == 5) v0 = *reinterpret_cast<const double*>(static_cast<const uint8_t*>(args) + 0x20);
                    if (t0 == 3 || t0 == 4) v0 = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(args) + 0x20);
                    if (!strcmp(e, "SetSatisfyGauge") || !strcmp(e, "SetResistGauge")) {
                        if (std::fabs(v0 - s_lastGauge) < 0.5) break;  // only changes
                        s_lastGauge = v0;
                    }
                    ++s_evLogs;
                    double v1 = 0;  // the second argument (SetRequestIconType: the type)
                    if (n >= 2) {
                        const uint8_t* a1 = static_cast<const uint8_t*>(args) + 48;
                        const uint32_t t1 = *reinterpret_cast<const uint32_t*>(a1 + 0x18);
                        if (t1 == 5) v1 = *reinterpret_cast<const double*>(a1 + 0x20);
                        if (t1 == 3 || t1 == 4) v1 = *reinterpret_cast<const int32_t*>(a1 + 0x20);
                    }
                    if (n >= 2) LOG("[ae] %s(%g, %g)", e, v0, v1);
                    else LOG("[ae] %s(%g)", e, v0);
                    const int serial = int(v0);
                    if (serial >= 0 && serial < 32) {
                        if (!strcmp(e, "SetRequestIconType")) {
                            const int rt = RequestTypeOfIcon(int(v1));
                            if (rt == 2) g_flickSerials |= 1u << serial;
                            else g_flickSerials &= ~(1u << serial);
                            LOG("[ae] request %d: %s", serial, rt == 0 ? "poke" : rt == 1 ? "rub" : rt == 2 ? "flick" : rt == 3 ? "hold" : "?");
                        } else if (!strcmp(e, "EndRequestIcon")) {
                            g_flickSerials &= ~(1u << serial);
                        }
                    }
                    break;
                }
        }
        // minigames (GG2HudActionEventMovie): icons and the gesture guides are placed at 3D projections (the reveal mask
        // follows the sight, the screen centre, and stays)
        // (SetPosX/SetPosY, found per method from the AS wrapper's parameters): drawn where that direction crosses the
        // panel. The reveal mask is enlarged so every hidden icon shows (ae_reveal_scale). The Flash hit tests get the
        // pointer's panel position (the icon is drawn there) instead of the screen-centre sight (0.9.21).
        if (!strncmp(tail, "SetTouchAct", 11) || !strcmp(tail, "SetTargetIconPosition") ||
            !strcmp(tail, "SetRequestIconPosition") || !strcmp(tail, "HitTestTargetIcon") ||
            !strcmp(tail, "HitTestRequestIcon") || !strcmp(tail, "SetTargetMaskScale") ||
            (!strcmp(tail, "SetTargetMaskPosition") && config::Get().vrAeRevealMode == 1)) {
            if (args && n >= 1 && n <= 8 && config::Get().vrAeMinigameFix) {
                struct Slots { int x = -1, y = -1, scale = -1; };
                static std::mutex s_mu;
                static std::unordered_map<std::string, Slots> s_slots;
                Slots sl;
                {
                    std::lock_guard l(s_mu);
                    auto it = s_slots.find(tail);
                    if (it == s_slots.end()) {
                        const uintptr_t fn = ue3::FindObject(("GG2Game.GG2HudActionEventMovie.AS_" + std::string(tail)).c_str(), "Function");
                        int i = 0;
                        if (fn)
                            for (auto& q : ue3::Properties(fn)) {
                                if (q.name == "ReturnValue") continue;
                                if (q.name == "SetPosX") sl.x = i;
                                if (q.name == "SetPosY") sl.y = i;
                                if (q.name == "SetScale") sl.scale = i;
                                ++i;
                            }
                        s_slots[tail] = sl;
                        LOG("[ui] minigame %s: x arg %d, y arg %d, scale arg %d (%s)", tail, sl.x, sl.y, sl.scale, fn ? "found" : "NO WRAPPER");
                    } else {
                        sl = it->second;
                    }
                }
                uint8_t a[8 * 48];
                memcpy(a, args, size_t(n) * 48);
                auto dbl = [&](int k) -> double* {
                    return k >= 0 && k < int(n) && *reinterpret_cast<uint32_t*>(a + k * 48 + 0x18) == 5 ? reinterpret_cast<double*>(a + k * 48 + 0x20) : nullptr;
                };
                bool changed = false;
                // The game masks the hidden-icon layer with the reveal circle; Scaleform only renders masked content
                // inside the 16:9 band, so on the taller VR panel icons below it were cut off (0.9.25). The mask is
                // removed and the reveal done per icon (AeRevealTick); its circle shape is hidden.
                if (!strcmp(tail, "SetTargetMaskScale") || !strcmp(tail, "SetTargetMaskPosition")) {
                    static void* s_view = nullptr;
                    if (s_view != view) {
                        uint8_t nul[48]{};
                        *reinterpret_cast<uint32_t*>(nul + 0x18) = 1;  // GFxValue null
                        uint8_t res[48]{};
                        const bool ok = o_Invoke(view, "_root.target.targetLayer.setMask", res, nul, 1);
                        LOG("[ui] minigame: hidden-icon mask removed (%s)", ok ? "ok" : "FAILED");
                        s_view = view, g_aeUnmasked = true;
                        for (auto& ic : g_aeIcons) ic = {};
                    }
                }
                if (!strcmp(tail, "SetTargetMaskScale")) {
                    // reveal mode 1: the game's size (x minigame_reveal_aura); 0: big enough to show every icon
                    const float k = config::Get().vrAeRevealMode == 1 ? config::Get().vrAeRevealAura : config::Get().vrAeRevealScale;
                    if (double* s = dbl(sl.scale); s && *s > 0.0) {
                        g_aeMaskScale = float(*s);
                        if (k > 0.f && k != 1.f) *s *= k, changed = true;
                    }
                } else if (!strcmp(tail, "SetTargetMaskPosition")) {
                    // reveal mode 1: the circle around the pointer (vanilla: around the sight, the screen centre); off
                    // the panel nothing is revealed
                    double *x = dbl(sl.x), *y = dbl(sl.y);
                    float px, py;
                    if (x && y) {
                        if (vr::pointer::PanelStage(px, py)) *x = px, *y = py;
                        else *x = -10000.0, *y = -10000.0;
                        changed = true;
                    }
                } else if (!strncmp(tail, "HitTest", 7)) {
                    float px, py;
                    double *x = dbl(sl.x), *y = dbl(sl.y);
                    if (x && y && vr::pointer::PanelStage(px, py)) {
                        static int s_hLogs = 0;
                        if (s_hLogs++ < 6) LOG("[ui] minigame %s at (%.0f,%.0f) -> pointer (%.0f,%.0f)", tail, *x, *y, px, py);
                        *x = px, *y = py, changed = true;
                    }
                } else {
                    double *x = dbl(sl.x), *y = dbl(sl.y);
                    float fx = x ? float(*x) : 0.f, fy = y ? float(*y) : 0.f;
                    const bool projected = x && y && fy > -100.f && vr::HudProjectionToPanel(fx, fy);
                    if (!projected) {
                        static int s_fLogs = 0;
                        if (s_fLogs++ < 4) LOG("[ui] minigame %s: not moved (x %d y %d)", tail, x ? 1 : 0, y ? 1 : 0);
                    }
                    if (projected) {
                        const int slot = !strcmp(tail, "SetTargetIconPosition") ? 0 : !strcmp(tail, "SetRequestIconPosition") ? 1
                                         : !strncmp(tail, "SetTouchAct", 11) ? 2 : -1;
                        if (slot == 0) {  // per-icon reveal (AeRevealTick)
                            const uint8_t* a0 = a;
                            const uint32_t t0 = *reinterpret_cast<const uint32_t*>(a0 + 0x18);
                            const int no = t0 == 5 ? int(*reinterpret_cast<const double*>(a0 + 0x20))
                                         : (t0 == 3 || t0 == 4) ? *reinterpret_cast<const int32_t*>(a0 + 0x20) : -1;
                            if (no >= 0 && no < 32) {
                                const ULONGLONG now = GetTickCount64();
                                if (!g_aeIcons[no].t || now - g_aeIcons[no].t > 300) g_aeNewIcon = true;
                                g_aeIcons[no].x = fx, g_aeIcons[no].y = fy, g_aeIcons[no].t = now;
                            }
                        }
                        if (slot >= 0)  // dev: !fakectrl at aeicon / aereq
                            g_lastAeIcon[slot][0] = fx, g_lastAeIcon[slot][1] = fy, g_lastAeIconT[slot] = GetTickCount64();
                        static int s_pLogs = 0;
                        if (s_pLogs++ < 8) LOG("[ui] minigame %s (%.0f,%.0f) -> panel (%.0f,%.0f)", tail, *x, *y, fx, fy);
                        *x = fx, *y = fy, changed = true;
                    }
                }
                if (changed) return o_Invoke(view, method, result, a, n);
            }
        }
        // boss attack markers ("!" on the dolls; SetBossMarkerPosition(no, x, y), every frame): the same correction, so
        // the marker is drawn over its doll as seen through the panel (0.9.20: the dolls couldn't be shot - the hit
        // test itself uses the unmoved position against the cursor, both in the eye view)
        // With the controllers aiming, this movie is drawn into each eye image (kEye) instead: there the game's own
        // projection is already right (the eye overlay is composited shifted up by the band's top, so stage y = eye-image
        // y). The panel conversion there pinned the marks to the head (0.9.35), and an extra band shift drew them
        // a quarter of the view too high (0.9.36).
        // Final Doki Doki Mode: its marks on her body (SetEffectDoki01/02, SetEffectGuard, SetEffectPos), the sight and
        // the parts name board are placed at eye-view projections of 3D points: onto the panel as seen through it, like
        // the stage's Doki Doki markers (0.9.50: the marks were cut off by the screen)
        if (vr::InFinalDoki() && args &&
            (((!strcmp(tail, "SetEffectDoki01") || !strcmp(tail, "SetEffectDoki02") || !strcmp(tail, "SetEffectGuard") ||
               !strcmp(tail, "SetSightPos") || !strcmp(tail, "SetPartsNameBoardPos")) && n == 2) ||
             (!strcmp(tail, "SetEffectPos") && n == 3))) {
            uint8_t a[3 * 48];
            memcpy(a, args, size_t(n) * 48);
            const int ix = n == 3 ? 1 : 0;
            uint8_t* px = a + ix * 48;
            uint8_t* py = a + (ix + 1) * 48;
            if (*reinterpret_cast<uint32_t*>(px + 0x18) == 5 && *reinterpret_cast<uint32_t*>(py + 0x18) == 5) {
                double& x = *reinterpret_cast<double*>(px + 0x20);
                double& y = *reinterpret_cast<double*>(py + 0x20);
                float fx = float(x), fy = float(y);
                if (y <= -100.0 && g_stageRows.load()) {  // parked (hidden): off the taller panel too (0.9.91, as 0.9.73)
                    y = -10000.0;
                    return o_Invoke(view, method, result, a, n);
                }
                // fddm_doki_offsets: a doki's fly-out offset is flat-screen (it grew ~2x converted as part of the
                // projection - 0.9.91: the zoom-in dokis spawned at the panel's edges): only the body point is
                // converted, the offset added back as it is
                float ox = 0, oy = 0;
                const bool off = config::Get().vrFddmDokiOffsets && n == 3 && FddmDokiOffset(int(ArgNum(a, 0)), ox, oy);
                if (off) fx -= ox, fy -= oy;
                if (vr::HudProjectionToPanel(fx, fy)) {
                    if (off) fx += ox, fy += oy;
                    static int s_logs = 0;
                    if (s_logs++ < 8 || (off && s_logs < 40 && (std::fabs(ox) > 1.f || std::fabs(oy) > 1.f)))
                        ++s_logs, LOG("[ui] final Doki Doki %s (%.0f,%.0f)%s -> panel (%.0f,%.0f)", tail, x, y,
                                      off ? " [offset kept]" : "", fx, fy);
                    x = fx, y = fy;
                    return o_Invoke(view, method, result, a, n);
                }
            }
        }
        // route choice ("which route?"): the engine hit-tests the arrows with the sight cursor, which in VR is in eye-view
        // coordinates - the arrows are on the panel: the point the aim ray crosses the panel instead (0.9.59: only
        // the stick moved between the options)
        if (!strcmp(tail, "SetRouteArrowTouch") && n == 2 && args && config::Get().vrMode == 2) {
            float ax = 0, ay = 0;
            if (vr::AimPanelStage(ax, ay)) {
                uint8_t a[2 * 48];
                memcpy(a, args, sizeof a);
                for (int k = 0; k < 2; ++k) {
                    const uint32_t t = *reinterpret_cast<uint32_t*>(a + k * 48 + 0x18);
                    const float v = k ? ay : ax;
                    if (t == 5) *reinterpret_cast<double*>(a + k * 48 + 0x20) = v;
                    else if (t == 3 || t == 4) *reinterpret_cast<int32_t*>(a + k * 48 + 0x20) = int32_t(v);
                }
                static int s_logs = 0;
                if (s_logs++ < 6) {
                    const uint8_t* a0 = static_cast<const uint8_t*>(args);
                    const uint32_t t0 = *reinterpret_cast<const uint32_t*>(a0 + 0x18);
                    const double ox = t0 == 5 ? *reinterpret_cast<const double*>(a0 + 0x20) : double(*reinterpret_cast<const int32_t*>(a0 + 0x20));
                    LOG("[ui] route arrow touch: game (%.0f, type %u) -> aim on the panel (%.0f,%.0f)", ox, t0, ax, ay);
                }
                return o_Invoke(view, method, result, a, n);
            }
        }
        // effect_full_view (0.9.70 C): the boss marks' hit-test query and the book objects move with the layer
        if (((!strcmp(tail, "HitTestBossMarker") || !strcmp(tail, "SetBookObjectPosition")) && n == 3) && args && EffectFullView()) {
            uint8_t a[3 * 48];
            memcpy(a, args, sizeof a);
            if (ArgNum(a, 2) > -100.0 && ArgAdd(a, 2, -double(vr::BandTopStage()))) {
                static int s_logs = 0;
                if (s_logs++ < 6) LOG("[ui] %s y %.0f -> %.0f (effect layer over the whole view)", tail, ArgNum(static_cast<const uint8_t*>(args), 2), ArgNum(a, 2));
                return o_Invoke(view, method, result, a, n);
            }
        }
        if (!strcmp(tail, "SetBossMarkerPosition") && n == 3 && args) {
            uint8_t a[3 * 48];
            memcpy(a, args, sizeof a);
            if (*reinterpret_cast<uint32_t*>(a + 48 + 0x18) == 5 && *reinterpret_cast<uint32_t*>(a + 96 + 0x18) == 5) {
                double& x = *reinterpret_cast<double*>(a + 48 + 0x20);
                double& y = *reinterpret_cast<double*>(a + 96 + 0x20);
                float fx = float(x), fy = float(y);
                if (EffectMovieInEye() && config::Get().vrMode == 2) {
                    static int s_eLogs = 0;
                    if (s_eLogs++ < 6) LOG("[ui] boss marker (%.0f,%.0f) kept (drawn per eye)", x, y);
                    if (config::Get().vrEffectStereo) RecordEffectX(0x20000u | (uint32_t(ArgNum(a, 0)) & 0xFFFF), x, y);
                    if (EffectFullView() && y > -100.0) {  // 0.9.70 C: y - band top (its hit test query moves equally)
                        y -= vr::BandTopStage();
                        return o_Invoke(view, method, result, a, n);
                    }
                    return o_Invoke(view, method, result, args, n);
                }
                if (y > -100.0 && vr::HudProjectionToPanel(fx, fy)) {
                    static int s_bmLogs = 0;
                    static ULONGLONG s_bmDiagT = 0;
                    if (s_bmLogs++ < 6 || (vr::BossMarkerDiag() && GetTickCount64() - s_bmDiagT > 200))
                        s_bmDiagT = GetTickCount64(), LOG("[ui] boss marker (%.0f,%.0f) -> panel (%.0f,%.0f)", x, y, fx, fy);
                    x = fx, y = fy;
                    return o_Invoke(view, method, result, a, n);
                }
            }
        }
        // Doki Doki Mode in stages: the girl selector markers (SetDokiDokiMarkerPosition(no, x, y), at her waist's 3D
        // projection), the selection test (GetTouchDokiDokiMarker(no, x, y), the cursor) and the hand cursor
        // (SetPointerPosition(x, y), ConvScreenPos of the sight cursor - eye-view since its Y hook): all eye-view
        // stage coords, so all onto the panel like the boss markers (reported: the selector wasn't over the girls, the hand
        // cursor was off the real one).
        {
            const bool marker = !strcmp(tail, "SetDokiDokiMarkerPosition") || !strcmp(tail, "GetTouchDokiDokiMarker");
            const bool hand = !strcmp(tail, "SetPointerPosition");
            if (((marker && n == 3) || (hand && n == 2)) && args) {
                uint8_t a[3 * 48];
                memcpy(a, args, size_t(n) * 48);
                const int ix = marker ? 1 : 0;
                uint32_t* tx = reinterpret_cast<uint32_t*>(a + ix * 48 + 0x18);
                uint32_t* ty = reinterpret_cast<uint32_t*>(a + (ix + 1) * 48 + 0x18);
                if (*tx == 5 && *ty == 5) {
                    double& x = *reinterpret_cast<double*>(a + ix * 48 + 0x20);
                    double& y = *reinterpret_cast<double*>(a + (ix + 1) * 48 + 0x20);
                    float fx = float(x), fy = float(y);
                    // the selection test gets the cursor field itself (PC+0xBA4, HUD-band pixels, not through the
                    // sight-cursor Y hook): its Y lacks the band's top offset the projections include
                    if (!strcmp(tail, "GetTouchDokiDokiMarker")) fy += vr::BandTopStage();
                    if (y <= -100.0 && g_stageRows.load() && !strcmp(tail, "SetDokiDokiMarkerPosition")) {
                        y = -10000.0;  // parked (hidden): off the taller stage panel too (0.9.73)
                        return o_Invoke(view, method, result, a, n);
                    }
                    if (y > -100.0 && vr::HudProjectionToPanel(fx, fy)) {
                        static int s_dkLogs = 0;
                        if (s_dkLogs++ < 12) LOG("[ui] doki %s (%.0f,%.0f) -> panel (%.0f,%.0f)", tail, x, y, fx, fy);
                        if (!strcmp(tail, "SetDokiDokiMarkerPosition"))  // dev: !fakectrl at dkmarker
                            g_lastAeIcon[3][0] = fx, g_lastAeIcon[3][1] = fy, g_lastAeIconT[3] = GetTickCount64();
                        x = fx, y = fy;
                        return o_Invoke(view, method, result, a, n);
                    }
                } else {
                    static int s_dkType = 0;
                    if (s_dkType++ < 4) LOG("[ui] doki %s: arg types %u/%u (not converted)", tail, *tx, *ty);
                }
            }
        }
        // minigames: the sight (with its aura = the reveal range) at the pointer, like the reveal circle (0.9.25:
        // the aura around the reticle was missing)
        if (!strcmp(tail, "SetSightPosition") && n == 2 && args && config::Get().vrReticleStyle == 1 && config::Get().vrMode == 2 &&
            vr::ctrl::AimActive() && !vr::pointer::TouchModeActive()) {
            uint8_t a[2 * 48];  // reticle_style=1: drawn at the band centre, captured there, shown on the reticle (visual only)
            memcpy(a, args, sizeof a);
            if (ArgAdd(a, 0, 960.0 - ArgNum(a, 0)) && ArgAdd(a, 1, 540.0 - ArgNum(a, 1))) return o_Invoke(view, method, result, a, n);
        }
        if (!strcmp(tail, "SetSightPosition") && n == 2 && args && config::Get().vrAeMinigameFix && vr::pointer::TouchModeActive()) {
            uint8_t a[2 * 48];
            memcpy(a, args, sizeof a);
            if (*reinterpret_cast<uint32_t*>(a + 0x18) == 5 && *reinterpret_cast<uint32_t*>(a + 48 + 0x18) == 5) {
                float px, py;
                const bool on = vr::pointer::PanelStage(px, py);
                *reinterpret_cast<double*>(a + 0x20) = on ? px : -10000.0;
                *reinterpret_cast<double*>(a + 48 + 0x20) = on ? py : -10000.0;
                return o_Invoke(view, method, result, a, n);
            }
        }
        // minigame timed phases: the controller guide (sticks / triggers - the VR controllers have both) instead of the
        // mouse one, by gesture type (the HUD's own _forPC_Controller choices); the sticks work there (0.9.29)
        // (also while the pointer is still off: the first phase starts straight out of a cutscene - 0.9.32)
        if (!strcmp(tail, "ControlGuideInMouse") && config::Get().vrAeMinigameFix && config::Get().vrAeControllerGuide &&
            (vr::pointer::TouchModeActive() || GesturePhase())) {
            const int type = vr::AeSkinShipType();
            if (type < 0) LOG("[ui] minigame guide: gesture type unknown, mouse guide kept");
            const char* name = nullptr;
            int na = 0, v0 = 0;
            switch (type) {
                case 0: name = "ControlGuideInStickLR", na = 2, v0 = 2; break;  // two-hand drag up
                case 1: name = "ControlGuideInStickLR", na = 2, v0 = 3; break;  // ... down
                case 2: name = "ControlGuideInStickLR", na = 2, v0 = 4; break;  // ... up and down
                case 3: name = "ControlGuideInStickR", na = 1, v0 = 0; break;   // circle anticlockwise
                case 4: name = "ControlGuideInStickR", na = 1, v0 = 1; break;   // circle clockwise
                case 5: name = "ControlGuideInLTRT", na = 0; break;              // taps
                case 6: name = "ControlGuideInStickR", na = 1, v0 = 5; break;   // scratch
                default: break;
            }
            if (name) {
                std::string m(method);
                m.replace(m.size() - strlen(tail), strlen(tail), name);
                uint8_t a[2 * 48]{};
                for (int k = 0; k < na; ++k) {
                    if (args && n >= 1) memcpy(a + k * 48, args, 48);  // the same value type as the game's own argument
                    const uint32_t t = *reinterpret_cast<uint32_t*>(a + k * 48 + 0x18);
                    if (t == 5) *reinterpret_cast<double*>(a + k * 48 + 0x20) = v0;
                    else *reinterpret_cast<uint32_t*>(a + k * 48 + 0x18) = 3, *reinterpret_cast<int32_t*>(a + k * 48 + 0x20) = v0;
                }
                LOG("[ui] minigame guide: %s(%d) for gesture type %d (VR controllers)", name, v0, type);
                return o_Invoke(view, m.c_str(), result, na ? a : nullptr, uint32_t(na));
            }
        }
        if (!strcmp(tail, "SetNameBoxPos") && n == 3 && args) {
            uint8_t a[3 * 48];
            memcpy(a, args, sizeof a);
            if (*reinterpret_cast<uint32_t*>(a + 48 + 0x18) == 5 && *reinterpret_cast<uint32_t*>(a + 96 + 0x18) == 5) {
                double& x = *reinterpret_cast<double*>(a + 48 + 0x20);
                double& y = *reinterpret_cast<double*>(a + 96 + 0x20);
                float fx = float(x), fy = float(y);
                // (converted ONCE: HudProjectionToPanel works in place - 0.9.66 called it twice and the second call threw
                // the labels off the panel: 0.9.68 "name tags are still missing")
                // diagnostic: a SHOWN tag whose position this call can't be trusted (outside the two eye draws, the
                // conversion failed, or the game's projection gave ~0,0) is logged
                const int id = int(ArgNum(a, 0)) & 3, eye = vr::DrawingEye();
                const ULONGLONG now = GetTickCount64();
                static void* s_tagStarted = nullptr;
                if (id == 0 && g_tagTestOn.load()) {  // dev: !tagtest - slot 0 pinned at an eye-view position (~2 m disparity)
                    if (s_tagStarted != view) {  // its box started once (the game starts it only for a targeted girl)
                        s_tagStarted = view;
                        std::string m(method);
                        m.replace(m.size() - strlen(tail), std::string::npos, "StartNameBox");
                        uint8_t r[48]{};
                        o_Invoke(view, m.c_str(), r, a, 1);
                        LOG("[ui] dev: %s started for !tagtest", m.c_str());
                    }
                    x = g_tagTestX.load() + (eye == 1 ? -7.0 : 7.0), y = g_tagTestY.load(), fx = float(x), fy = float(y);
                } else if (id == 0) {
                    s_tagStarted = nullptr;
                }
                const bool shown = y > -100.0, zero = std::fabs(x) < 1.0 && std::fabs(y) < 1.0;
                const bool conv = shown && vr::HudProjectionToPanel(fx, fy);  // (converted ONCE: it works in place)
                // name_tags_3d: the targeted girl's tag where the panel can't show it (its direction misses the panel, or
                // lands beyond its edges, where the movie pinned it): drawn at a reserved spot that the composite cuts
                // out and draws at her head in each eye (her per-eye position recorded here)
                if (shown && id == 0 && config::Get().vrNameTags3d) {
                    const float ext = g_panelExtFrac.load() * 1080.f;
                    const bool offPanel = conv && (fx < 0.f || fx > 1920.f || fy < -ext || fy > 1080.f + ext);
                    if (!conv || offPanel) {
                        if (eye == 0 || eye == 1) g_tagEyeX[eye] = float(x), g_tagEyeY[eye] = float(y), g_tagEyeT[eye] = now;
                        g_tagParkT = now;
                        {
                            static ULONGLONG s_t = 0;
                            static int s_n = 0;
                            if (s_n < 60 && now - s_t > 2000)
                                ++s_n, s_t = now, LOG("[ui] name label #0 eye %d (%.0f,%.0f): %s (panel %.0f,%.0f, rows %.0f) -> drawn in 3D", eye, x, y,
                                                      conv ? "beyond the panel" : "off the panel", fx, fy, ext);
                        }
                        x = kTagParkX, y = kTagParkY;
                        return o_Invoke(view, method, result, a, n);
                    }
                }
                if (shown && (!conv || zero || eye < 0)) {
                    static ULONGLONG s_badT = 0;
                    static int s_bad = 0;
                    if (s_bad < 200 && now - s_badT > 1000)
                        ++s_bad, s_badT = now,
                        LOG("[ui] name label #%d eye %d (%.0f,%.0f): %s -> %s", id, eye, x, y,
                            !conv ? "conversion failed" : zero ? "game position 0,0" : "outside the eye draws",
                            conv ? "converted anyway" : "parked off the panel");
                }
                // hidden: the game parks it at (0, -200) - inside the rows above the band that hud_extend_rows shows,
                // so it showed at the panel's top-left while fading out (0.9.72): parked well off the panel
                if (!shown && g_stageRows.load()) {
                    y = -10000.0;
                    return o_Invoke(view, method, result, a, n);
                }
                // shown, but her direction never crosses the panel (a girl beside / behind the game camera, seen with the
                // head turned): passed unconverted it was a stray tag at a meaningless panel spot - parked off the panel
                if (shown && !conv) {
                    y = -10000.0;
                    return o_Invoke(view, method, result, a, n);
                }
                if (conv) {
                    static int s_nameLogs = 0;
                    static ULONGLONG s_nameT = 0;
                    static int s_pair = 0;
                    if (s_nameLogs++ < 6 || GetTickCount64() - s_nameT > 2000 || s_pair > 0) {
                        if (s_pair > 0) --s_pair;
                        else s_nameT = GetTickCount64(), s_pair = 1;
                        LOG("[ui] name label #%.0f eye %d (%.0f,%.0f) -> panel (%.0f,%.0f)", ArgNum(a, 0), vr::DrawingEye(), x, y, fx, fy);
                    }
                    x = fx, y = fy;
                    return o_Invoke(view, method, result, a, n);
                }
            }
        }
    }
    // kiss / kick marks (when shown): placed at screen positions measured from the top of the 16:9 HUD band, while the
    // per-eye effect layer (kEye) takes eye-image positions (the boss marks' projections): the band's top added
    // back, or they sat a band offset too high (0.9.59: "quite high-up in VR view")
    if (method && g_enabled && config::Get().vrScreenMarks && IsScreenMark(method) && args && n >= 2 && EffectMovieInEye() &&
        config::Get().vrMode == 2 && strstr(method, "SetEffect")) {
        uint8_t a[6 * 48];
        const uint32_t cnt = n > 6 ? 6 : n;
        memcpy(a, args, size_t(cnt) * 48);
        uint8_t* py = a + 48;
        const uint32_t t = *reinterpret_cast<uint32_t*>(py + 0x18);
        const float top = vr::BandTopStage();
        if (EffectFullView()) return o_Invoke(view, method, result, args, n);  // 0.9.70 C: band-relative = layer rows
        if (top > 0.f && (t == 5 || t == 3 || t == 4)) {
            if (t == 5) *reinterpret_cast<double*>(py + 0x20) += top;
            else *reinterpret_cast<int32_t*>(py + 0x20) += int32_t(top);
            static int s_logs = 0;
            if (s_logs++ < 4) LOG("[ui] %s: y + %.0f (band top) for the per-eye layer", strrchr(method, '.') ? strrchr(method, '.') + 1 : method, top);
            return o_Invoke(view, method, result, a, cnt);
        }
    }
    if (method && g_enabled && !config::Get().vrScreenMarks && IsScreenMark(method)) {
        static std::atomic<int> s_dropLogs{0};
        if (s_dropLogs.fetch_add(1) < 10) LOG("[ui] screen mark suppressed: %s", method);
        return true;
    }
    return o_Invoke(view, method, result, args, n);
}
}  // namespace

// freeze watchdog report (render thread): what the UI layer was doing
void ReportFreeze(const char* why) {
    const uint64_t f = xr::CurrentFrameIdRT();
    std::string ring;
    for (int k = 0; k < 6; ++k) ring += std::to_string(int(g_phaseRing[(f + 16 - k) % 16].load())) + (k < 5 ? "," : "");
    const uintptr_t lp = g_layerPlayer.load();
    LOG("[watch] %s: game %s%s; movies %s", why, vr::GameClassDiag(), vr::PausedDiag() ? " (paused)" : "", g_lastSet.c_str());
    LOG("[watch]   layered %s phases(newest first) %s cutouts %d mask 0x%X valid %d%d%d%d spec %d; fs %d tint %d eo %d sco %d "
        "backdrop %d shop %d zoom %.2f scope %d captures %llu",
        lp && ue3::IsObject(lp) ? MovieClass(lp).c_str() : "-", ring.c_str(), g_cutoutsOn.load() ? 1 : 0, g_cutMask.load(),
        g_layValid[0] ? 1 : 0, g_layValid[1] ? 1 : 0, g_layValid[2] ? 1 : 0, g_layValid[3] ? 1 : 0, g_activeSpec.load(), g_fsValid,
        g_tintValid, g_eoValid, g_scoValid, g_backdrop, g_shopVoid.load() ? 1 : 0, vr::ZoomFactor(), ScopeMode() ? 1 : 0,
        (unsigned long long)g_captures);
    vr::DressingRoom room;
    const bool rv = vr::GetDressingRoom(room);
    LOG("[watch]   room %d kind %d; frame %llu", rv ? 1 : 0, room.kind, (unsigned long long)f);
}
bool Install() {
    xr::SetFreezeReporter(&ReportFreeze);
    hooks::Create("GFxMovieView::Invoke(wrapper)", kGfxInvoke, kGfxInvokeBytes, sizeof kGfxInvokeBytes,
                  (void*)&hk_Invoke, (void**)&o_Invoke);
    g_enabled = config::Get().vrUiLayer;
    return hooks::Create("FGFxRenderUI::Execute", kGfxRenderUiExecute, kGfxExecBytes, sizeof kGfxExecBytes,
                         (void*)&hk_Exec, (void**)&o_Exec);
}
void SetEnabled(bool on) {
    g_enabled = on;
    LOG("[ui] UI layer %s", on ? "ON" : "off");
}
bool Enabled() { return g_enabled; }
bool FantasizeActive() { return g_fantasize.load(); }
void BandOf(uint32_t w, uint32_t h, uint32_t& x0, uint32_t& y0, uint32_t& bw, uint32_t& bh) { Band(w, h, x0, y0, bw, bh); }
void SetTallPanel(bool on) {
    if (g_tallPanel.exchange(on) != on) LOG("[ui] tall panel %s (width %.2f m)", on ? "on (minigame)" : "off", PanelWidth());
    xr::SetUiPanelWidth(PanelWidth());
    xr::SetUiPanelDrop(-PanelYOffset());
}
// Minigames: the panel's elevation (degrees, negative = below eye level). The panel only covers about +-37 deg around
// its centre and the icons sit all over her body, so it follows the gaze up / down: still while the gaze stays within
// minigame_follow_deadzone of its centre, then gliding (~0.5 s) to centre on it (0.9.27: a fixed drop only moved
// the cut-off from the bottom to the top).
std::atomic<float> g_panelElevDeg{0.f};
void UpdatePanelFollow(float pitch) {
    const auto& c = config::Get();
    static bool s_was = false;
    static ULONGLONG s_last = 0;
    static bool s_moving = false;
    const ULONGLONG now = GetTickCount64();
    const float dt = s_last ? std::fmin(0.1f, float(now - s_last) / 1000.f) : 0.f;
    s_last = now;
    const bool tall = g_tallPanel.load();
    if (!tall || !c.vrAePanelFollow) {
        s_was = false;
        g_panelElevDeg = tall ? -c.vrAePanelDrop : 0.f;
        return;
    }
    pitch = std::fmax(-60.f, std::fmin(35.f, pitch));
    float e = g_panelElevDeg.load();
    if (g_aeResultUp.load()) {  // results screen: glide back to straight ahead and stay (0.9.59: it followed the head)
        const float rest = -c.vrAePanelDrop;
        e += (rest - e) * std::fmin(1.f, dt * 6.f);
        g_panelElevDeg = e;
        s_was = true, s_moving = false;
        return;
    }
    if (!s_was) e = -c.vrAePanelDrop, s_was = true, s_moving = false;
    // minigame_follow_deadzone 0 (default): it follows the gaze all the time, smoothed - the flat game's view follows
    // the mouse continuously (0.9.46: the dead zone felt like "you must be past this angle to move it")
    const float dz = c.vrAeFollowDeadzone;
    if (dz <= 0.f) {
        e += (pitch - e) * std::fmin(1.f, dt * 8.f);
        g_panelElevDeg = e;
        return;
    }
    if (std::fabs(pitch - e) > dz) s_moving = true;          // out of the dead zone: recentre on the gaze
    if (s_moving) {
        e += (pitch - e) * std::fmin(1.f, dt * 6.f);
        if (std::fabs(pitch - e) < 1.f) s_moving = false;
    }
    g_panelElevDeg = e;
}
float PanelYOffset() {
    if (!g_tallPanel.load()) return 0.f;
    return config::Get().vrUiDistance * std::tan(g_panelElevDeg.load() * 3.14159265f / 180.f);
}
float PanelWidth() {
    const auto& c = config::Get();
    return c.vrUiWidth * (g_tallPanel.load() && c.vrAePanelScale > 0.f ? c.vrAePanelScale : 1.f);
}
// the rows the panel image has (tall minigame panel, or hud_extend_rows in stages): 0 when it has none
float PanelExtension() { return g_panelExtFrac.load(); }
void SetEffectRestShift(float stageUnits) { g_eoRest = stageUnits; }
void SetStageRows(bool on) {
    if (g_stageRows.exchange(on) != on) LOG("[ui] stage HUD rows above / below the panel %s", on ? "on" : "off");
}
void SetHudToggleContext(bool gameplay) { g_hudToggleCtx = gameplay; }
bool HudToggleContext() { return g_hudToggleCtx.load(); }
void ToggleHud() {
    const bool h = !g_hudHidden.load();
    g_hudHidden = h;  // applied after the category cache (no cache reset: the render thread reads it)
    LOG("[ui] gameplay HUD %s (toggle button)", h ? "hidden" : "shown");
}
void ShowHud(const char* why) {
    if (g_hudHidden.exchange(false)) LOG("[ui] gameplay HUD shown again (%s)", why);
}
void TraceAs(uint32_t ms) { g_asTraceUntil = GetTickCount64() + ms; }
std::atomic<bool> g_forceBg{false};
void DumpLayers() { g_dumpLayers = 15, g_forceBg = true; }
void ToggleRoomDebug() { g_roomDebug = !g_roomDebug.load(); }
void SetMenuLayers(bool on) {
    g_layersOff = !on;
    LOG("[ui] layered menus %s (dev)", on ? "on" : "off");
}
bool ShopOpen() { return g_shopVoid.load(); }
void SetLayerLag(int frames) {
    g_layerLag = frames;
    LOG("[ui] layered menu lag = %d", frames);
}
bool FlickRequestUp() { return g_flickSerials.load() != 0; }
bool AeRequestRevealed() { return g_aeRequestRevealed.load(); }
void DokiTick() {
    static uintptr_t s_player = 0;
    static ULONGLONG s_find = 0;
    if (GetTickCount64() >= s_find) {  // the movie exists in stages only: looked up twice a second
        s_find = GetTickCount64() + 500;
        if (!s_player || !ue3::IsObject(s_player) || MovieClass(s_player) != "GG2HudDokiLightMovie") s_player = gfx::FindPlayer("GG2HudDokiLightMovie");
    }
    bool vis = false;
    if (s_player && ue3::IsObject(s_player)) gfx::GetBool(s_player, "_root.effWPeace._visible", vis);
    if (g_dokiPeaceFull.exchange(vis) != vis) LOG("[ui] Doki Doki Double Peace effect %s", vis ? "shown: full screen" : "gone: panel");
}
// Reveal mode 1 (0.9.25): like the hidden icons, a request only shows - and only takes touches - while the
// pointer's reveal circle covers it, so you search her body with the pointer. The request layer (_root.requestLayer,
// unmasked in the game's movie) is shown / hidden as a whole; there is normally one request at a time.
void AeRevealTick() {
    static bool s_hidden = false;
    static uintptr_t s_player = 0;
    const auto& c = config::Get();
    float px = 0, py = 0;
    const bool on = c.vrAeMinigameFix && c.vrAeRevealMode == 1 && vr::pointer::TouchModeActive();
    const bool active = on && g_lastAeIconT[1] && GetTickCount64() - g_lastAeIconT[1] < 300;
    bool inside = true;
    if (active) {
        const float r = 278.f * g_aeMaskScale.load() / 100.f * (c.vrAeRevealAura > 0.f ? c.vrAeRevealAura : 1.f);
        inside = vr::pointer::PanelStage(px, py) && std::hypot(g_lastAeIcon[1][0] - px, g_lastAeIcon[1][1] - py) <= r;
    }
    g_aeRequestRevealed = inside;
    if (!c.vrAeMinigameFix || !vr::pointer::TouchModeActive() || !g_aeUnmasked.load()) return;
    if (!s_player || !ue3::IsObject(s_player) || MovieClass(s_player).find("GG2HudActionEventMovie") == std::string::npos)
        s_player = gfx::FindPlayer("GG2HudActionEventMovie");
    if (!s_player) return;
    {
        static uintptr_t s_maskHiddenFor = 0;
        if (s_maskHiddenFor != s_player && gfx::SetBool(s_player, "_root.target.maskSight._visible", false)) s_maskHiddenFor = s_player;
    }
    // hidden icons: shown while the pointer's circle covers them (mode 1), always (mode 0)
    {
        float qx = 0, qy = 0;
        const bool have = vr::pointer::PanelStage(qx, qy);
        const float rr = 278.f * g_aeMaskScale.load() / 100.f * (c.vrAeRevealAura > 0.f ? c.vrAeRevealAura : 1.f);
        const ULONGLONG now = GetTickCount64();
        for (int i = 0; i < 32; ++i) {
            auto& ic = g_aeIcons[i];
            if (!ic.t || now - ic.t > 300) continue;  // not alive (not positioned lately): leave it to the game
            const int want = c.vrAeRevealMode != 1 || (have && std::hypot(ic.x - qx, ic.y - qy) <= rr) ? 1 : 0;
            if (want == ic.shown) continue;
            if (gfx::SetBool(s_player, "_root.target.targetLayer.targetAll" + std::to_string(i) + "._visible", want != 0)) ic.shown = want;
        }
    }
    const bool hide = active && !inside;
    if (hide == s_hidden) return;
    if (gfx::SetBool(s_player, "_root.requestLayer._visible", !hide)) s_hidden = hide;
    static int s_logs = 0;
    if (s_logs++ < 10) LOG("[ae] request layer %s (reveal circle)", hide ? "hidden" : "shown");
}
bool GesturePhase() { return g_gesturePhase.load(); }
bool LastAeIcon(int kind, float& x, float& y) {
    // hidden icons move every frame; a request icon is placed once and stays (up to ~8 s)
    if (kind < 0 || kind > 3 || !g_lastAeIconT[kind] || GetTickCount64() - g_lastAeIconT[kind] > (kind == 0 || kind == 3 ? 500u : 15000u)) return false;
    x = g_lastAeIcon[kind][0], y = g_lastAeIcon[kind][1];
    return true;
}
bool ContinuePromptUp() { return g_continueUp.load() && MovieOpen("GG2HudContinueMovie"); }
bool StageResultUp() { return MovieOpen("GG2HudResultStageMovie"); }
bool MamakitaUp() { return MovieOpen("GG2HudMamakitaMovie"); }
bool TakeAeNewIcon() { return g_aeNewIcon.exchange(false); }
void SetTagTest(bool on, float x, float y) {
    g_tagTestX = x, g_tagTestY = y, g_tagTestOn = on;
    LOG("[ui] dev: name tag test %s (%.0f,%.0f)", on ? "on" : "off", x, y);
}
void ResetContinuePrompt() { g_continueUp = false; }
bool MovieOpen(const char* movieClass) {
    for (auto& dp : g_drawnPlayers) {
        const uintptr_t q = dp.load();
        uint32_t bits = 0;
        if (!q || !ue3::IsObject(q) || MovieClass(q) != movieClass || !game::Read(q + 0x118, bits) || !(bits & 1)) continue;
        bool vis = false;  // open all along (the continue prompt's movie lives through the whole stage, its root hidden)
        if (gfx::GetBool(q, "_root._visible", vis) && vis) return true;
    }
    return false;
}
void UpdateMenuLayersGT(uint64_t frameId, bool menuScene) {
    {  // Academy Store showing: its movie's root is visible (the hub's is hidden meanwhile); test: both flip on leaving
        bool shown = false;
        if (menuScene && config::Get().vrMenuLayers)
            for (auto& dp : g_drawnPlayers) {
                const uintptr_t q = dp.load();
                if (!q || !ue3::IsObject(q) || MovieClass(q) != "GG2HudUIShopMovie") continue;
                bool vis = false;
                gfx::GetBool(q, "_root._visible", vis);
                shown |= vis;
            }
        g_shopVoid = shown;
    }
    // Captures are rare: a burst of one frame per layer shortly after the movie appears and then every few seconds.
    // In between, every frame shows the panel: the layer clips (decorative) are kept off it with _visible, which
    // leaves the movie's timeline animations alone; the movie's other clips are hidden (_visible) only during the burst.
    struct Clip { std::string path; int layer; bool wantVis = true, setVis = true, ours = false; };
    static std::vector<Clip> clips;
    static uintptr_t player = 0;
    static int specIdx = -1;
    static ULONGLONG foundAt = 0, nextBurst = 0;
    static int burst = -1;  // index in the burst (-1 = no burst running)
    static bool captured = false;  // a first burst has completed: until then the movie stays whole on the panel
    auto drop = [&]() {
        for (auto& c : clips) {  // give the movie back its own state
            if (!player || !ue3::IsObject(player)) break;
            if (c.layer >= 0 && c.ours) gfx::SetBool(player, c.path + "._visible", c.wantVis);
            if (c.layer < 0 && c.ours) gfx::SetBool(player, c.path + "._visible", true);
        }
        player = 0, clips.clear(), burst = -1, captured = false;
        g_bgCropU = -1.f;
    };
    uint32_t openBits = 0;
    if (player && (!menuScene || !ue3::IsObject(player) || MovieClass(player) != kLayerSpecs[specIdx].movie ||
                   !game::Read(player + 0x118, openBits) || !(openBits & 1)))
        drop();
    if (menuScene && g_enabled && config::Get().vrMenuLayers && !g_layersOff.load()) {
        // the movies being drawn (published by the render thread): a spec listed earlier wins (Options over the title)
        for (int k = 0; k < kNumLayerSpecs && (!player || k < specIdx); ++k) {
            uintptr_t pl = 0;
            for (auto& d : g_drawnPlayers) {
                const uintptr_t q = d.load();
                if (q && ue3::IsObject(q) && MovieClass(q) == kLayerSpecs[k].movie && game::Read(q + 0x118, openBits) && (openBits & 1)) pl = q;
            }
            if (!pl || pl == player) continue;
            if (player) drop();
            player = pl, specIdx = k, foundAt = GetTickCount64(), nextBurst = foundAt + 100;
            g_resetSettle = true;
            const auto& spec = kLayerSpecs[k];
            std::vector<std::string> unnamed;  // unnamed root clips: Scaleform names them instanceN
            const ULONGLONG tp = GetTickCount64();
            for (int i = 0; i < 500; ++i) {  // seen: up to 55 (title), 175 (Options), 290 (Score Attack)
                const std::string nm = "instance" + std::to_string(i);
                std::string v;
                gfx::GetString(pl, "_root." + nm + "._name", v);
                if (v != nm) continue;
                bool listed = false;
                for (int l = 0; l < 4; ++l)
                    for (auto& n : SplitNames(spec.layers[l])) listed |= n == nm;
                if (!listed) unnamed.push_back(nm);
            }
            for (int l = 0; l < 4; ++l)
                for (auto& n : SplitNames(spec.layers[l])) {
                    if (n == "@first") {
                        if (unnamed.empty()) continue;
                        clips.push_back({"_root." + unnamed.front(), LayerOfClip(spec, l)});
                        unnamed.erase(unnamed.begin());
                    } else if (n == "@largest") {  // the biggest unnamed root clip (a full-screen picture)
                        size_t best = unnamed.size();
                        float bestA = 0;
                        for (size_t u = 0; u < unnamed.size(); ++u) {
                            float w = 0, h = 0;
                            gfx::GetNumber(pl, "_root." + unnamed[u] + "._width", w), gfx::GetNumber(pl, "_root." + unnamed[u] + "._height", h);
                            if (w * h > bestA) bestA = w * h, best = u;
                        }
                        if (best == unnamed.size()) continue;
                        clips.push_back({"_root." + unnamed[best], LayerOfClip(spec, l)});
                        unnamed.erase(unnamed.begin() + best);
                    } else {
                        clips.push_back({"_root." + n, LayerOfClip(spec, l)});
                    }
                }
            for (auto& n : SplitNames(spec.others)) clips.push_back({"_root." + n, -1});
            std::string un;
            for (auto& n : unnamed) {
                float x = 0, y = 0;
                gfx::GetNumber(pl, "_root." + n + "._x", x), gfx::GetNumber(pl, "_root." + n + "._y", y);
                clips.push_back({"_root." + n, LayerOfClip(spec, spec.unnamedLayer)}), un += " " + n + "(" + std::to_string(int(x)) + "," + std::to_string(int(y)) + ")";
            }
            LOG("[ui] layered menu: %s found (%zu clips; unnamed (layer %d):%s; probe %llu ms)", spec.movie, clips.size(), spec.unnamedLayer,
                un.c_str(), GetTickCount64() - tp);
            break;
        }
    } else if (player) {
        drop();
    }
    g_layerPlayer = player;
    const uint64_t target = frameId + uint64_t(g_layerLag.load());
    if (!player) {
        g_phaseRing[target % 16] = -1;
        g_cutoutsOn = false, g_cutMask = 0;
        return;
    }
    g_activeSpec = specIdx;
    const auto& spec = kLayerSpecs[specIdx];
    // The movie's decorative clips (girls, logo) are shown as cut-outs at their depths. A cut-out layer is "live"
    // while it changes - its clips move (position, alpha, scale, own frame, visibility) or its captured picture
    // differs from the previous capture (animations inside the clips: the logo's sparkles - 0.9.7) - and settled
    // once both have been still. menu_layer_animation=1: live layers stay cut-outs and are re-captured every other
    // frame (alternating with the panel), so the intros play at their depths; 0: while anything is live the clips play
    // on the panel, and the cut-outs come back once everything has settled. Only one layer (or the panel) can be
    // rendered per frame: Scaleform renders the snapshot of its last advance, and _visible changes show one frame on.
    const bool depthMode = config::Get().vrMenuLayerAnimation != 0;
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG s_stillSince[4]{}, s_lastIssue[4]{}, s_imgLiveSince[4]{};
    static std::vector<float> s_sig[4];
    static uint32_t s_issued[4]{}, s_capCount[4]{};
    static ULONGLONG s_settleRef[4]{};  // a capture taken at or after this must have been compared (the timed refresh)
    static bool s_cutOn = false;  // panel mode: the cut-outs are shown
    static ULONGLONG s_nextVerify = 0;
    static std::vector<int8_t> plan;
    static size_t planPos = 0;
    static int s_rr = 0;
    static bool s_lastCapture = false;
    if (g_resetSettle) {
        g_resetSettle = false;
        for (int l = 0; l < 4; ++l)
            s_stillSince[l] = now, s_lastIssue[l] = 0, s_imgLiveSince[l] = 0, s_sig[l].clear(), s_issued[l] = g_layMeasured[l].load(),
            s_capCount[l] = 0, s_settleRef[l] = now;
        s_cutOn = false, s_nextVerify = 0, plan.clear(), planPos = 0, burst = -1, s_lastCapture = false;
    }
    bool hasLayer[4]{};
    for (int l = 1; l < 4; ++l) hasLayer[l] = *spec.layers[l] != 0 && LayerOfClip(spec, l) == l;
    extern std::atomic<bool> g_forceBg;
    static bool s_forced = false;
    if (g_forceBg.exchange(false)) s_forced = true, nextBurst = now;
    // while the Academy Store is up the hub's movie is hidden: captures then are empty (the hub came back as a void
    // with cursor trails until the next refresh). No captures meanwhile; a fresh one right after the store closes.
    static bool s_wasShop = false;
    const bool shopNow = g_shopVoid.load();
    if (s_wasShop && !shopNow) nextBurst = now + 150;
    s_wasShop = shopNow;
    if (shopNow) nextBurst = std::max<ULONGLONG>(nextBurst, now + 150);
    if (captured && !g_layValid[0] && nextBurst > now + 400) nextBurst = now + 400;
    // per cut-out layer: are its clips moving?
    constexpr size_t kPer = 6;
    for (int l = 1; l < 4; ++l) {
        if (!hasLayer[l]) continue;
        std::vector<float> sig;
        for (auto& c : clips) {
            if (c.layer != l) continue;
            float v[kPer] = {0, 0, 0, 0, 0, 0};
            if (c.wantVis) {  // an invisible clip's movement doesn't show (intro pieces are moved into place first)
                gfx::GetNumber(player, c.path + "._alpha", v[2]);
                if (v[2] > 0.f) {
                    gfx::GetNumber(player, c.path + "._x", v[0]), gfx::GetNumber(player, c.path + "._y", v[1]);
                    gfx::GetNumber(player, c.path + "._xscale", v[3]), gfx::GetNumber(player, c.path + "._currentframe", v[4]);
                }
                v[5] = 1.f;
            }
            sig.insert(sig.end(), v, v + kPer);
        }
        bool moved = sig.size() != s_sig[l].size();
        for (size_t i = 0; !moved && i < sig.size(); ++i) moved = std::fabs(sig[i] - s_sig[l][i]) > 0.05f;
        s_sig[l].swap(sig);
        if (moved) s_stillSince[l] = now;
    }
    // per cut-out layer: live or settled
    bool live[4]{}, anyLive = false, anyMoving = false, allSettled = true;
    for (int l = 1; l < 4; ++l) {
        if (!hasLayer[l]) continue;
        const bool still = now - s_stillSince[l] > 400;
        // the latest compared capture was taken after the clips went still, and matched the one before it
        const bool imgSame = s_capCount[l] >= 2 && g_layMeasT[l].load() > s_stillSince[l] && g_layMeasT[l].load() >= s_settleRef[l] && g_layDiff[l].load() < 10.f;
        live[l] = !s_capCount[l] || !still || !imgSame;
        if (!still || !s_capCount[l]) s_imgLiveSince[l] = 0;
        else if (live[l] && !s_imgLiveSince[l]) s_imgLiveSince[l] = now;
        else if (!live[l]) s_imgLiveSince[l] = 0;
        anyLive |= live[l], anyMoving |= !still, allSettled &= !live[l];
    }
    // the next phase to show
    int8_t phase = 0;
    // no capture frame (its other clips hidden) while a click is going on: it would miss its button
    const bool clicking = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || now - pointer::LastButtonTick() < 250;
    if (clicking) {
        // the panel; the plan resumes afterwards
    } else if (plan.size() > planPos) {
        phase = plan[planPos++];
    } else {
        plan.clear(), planPos = 0;
        const bool timed = now >= nextBurst;
        if (timed) {  // the background (static pictures refreshed; the scrolling one captured once) + a refresh of all
            const bool needBg = spec.bgMode == 2 ? (!g_layValid[0] || s_forced) : true;
            s_forced = false;
            for (int i = 0; i < 120; ++i) {  // unnamed root clips created since (intro animation pieces: up to 55);
                                              // each miss is an engine log line (DevGFxUI), so not the full 500
                const std::string nm = "instance" + std::to_string(i), path = "_root." + nm;
                bool known = false;
                for (auto& c : clips) known |= c.path == path;
                if (known) continue;
                std::string v;
                gfx::GetString(player, path + "._name", v);
                if (v == nm) clips.push_back({path, LayerOfClip(spec, spec.unnamedLayer)}), LOG("[ui] layered menu: new unnamed clip %s (layer %d)", nm.c_str(), spec.unnamedLayer);
            }
            if (needBg) plan.push_back(1);
            for (int l = 1; l < 4; ++l)
                if (hasLayer[l] && (depthMode || s_cutOn)) plan.push_back(int8_t(l + 1));
            // re-checked often while the menu settles: pieces of a half-reset intro (returning from the Dressing Room)
            // can show for a moment and vanish without any clip property changing (0.9.7: stars left on the logo)
            nextBurst = now + (now - foundAt < 10000 ? 300 : 6000);
        } else if (depthMode && anyLive && s_lastCapture) {
            // a panel frame between captures
        } else if (depthMode && anyLive) {
            // the next live layer; a layer that only changes inside (a looping effect) every 4th frame
            for (int k = 0; k < 3 && !plan.size(); ++k) {
                const int l = 1 + (s_rr + k) % 3;
                if (!live[l]) continue;
                const bool slow = s_imgLiveSince[l] && now - s_imgLiveSince[l] > 3000;
                static int s_slowTick = 0;
                if (slow && (++s_slowTick & 1)) continue;
                plan.push_back(int8_t(l + 1)), s_rr = l % 3;
            }
        } else if (!depthMode) {
            if (s_cutOn && anyMoving) {
                s_cutOn = false;
                static int s_logs = 0;
                if (s_logs++ < 20) LOG("[ui] layered menu: animating - girls/logo back on the panel");
            }
            // everything still: verify with captures (until two in a row match) before showing the cut-outs
            if (!s_cutOn && !anyMoving && now >= s_nextVerify) {
                for (int l = 1; l < 4; ++l)
                    if (hasLayer[l]) plan.push_back(int8_t(l + 1));
                s_nextVerify = now + 300;
            }
            if (!s_cutOn && allSettled) {
                s_cutOn = true;
                static int s_logs = 0;
                if (s_logs++ < 20) LOG("[ui] layered menu: settled %.1f s after it appeared - girls/logo as cut-outs", (now - foundAt) / 1000.0);
            }
        }
        if (plan.size()) phase = plan[planPos++];
    }
    if (phase >= 1) {
        const int l = phase - 1;
        ++s_issued[l], ++s_capCount[l], s_lastIssue[l] = now;
        if (l == 0) captured = true;
    }
    s_lastCapture = phase >= 2;
    // cut-outs shown (composite) and their clips off the panel
    uint32_t cutMask = 0;
    for (int l = 1; l < 4; ++l)
        if (hasLayer[l] && s_capCount[l] && (depthMode || s_cutOn)) cutMask |= 1u << l;
    g_cutoutsOn = cutMask != 0;
    g_cutMask = cutMask;
    {
        static bool s_wasLive = false;
        static ULONGLONG s_liveT = 0;
        if (anyLive != s_wasLive) {
            if (anyLive) s_liveT = now;
            static int s_logs = 0;
            if (s_logs++ < 40)
                LOG("[ui] layered menu: %s at %.1f s%s", anyLive ? "animating" : "settled", (now - foundAt) / 1000.0,
                    anyLive ? "" : (" (live for " + std::to_string((now - s_liveT) / 100 / 10.0).substr(0, 4) + " s)").c_str());
            s_wasLive = anyLive;
        }
    }
    for (auto& c : clips) {
        if (c.layer >= 0) {  // layer clips: _visible
            bool cur = true;
            gfx::GetBool(player, c.path + "._visible", cur);
            if (!c.ours || cur != c.setVis) c.wantVis = cur;  // the movie set it itself
            bool v;
            if (phase > 0) v = phase == c.layer + 1 && c.wantVis;                // a capture frame: that layer alone
            else if (c.layer == 0) v = captured && g_layValid[0] ? false : c.wantVis;  // background: off once captured
            else v = (cutMask >> c.layer) & 1 ? false : c.wantVis;                // girls / logo: off while cut-outs show
            if (v != cur) gfx::SetBool(player, c.path + "._visible", v);
            c.setVis = v, c.ours = true;
        } else if (phase > 0 || c.ours) {  // other clips: hidden only during a capture, the visible ones
            bool cur = false;
            gfx::GetBool(player, c.path + "._visible", cur);
            if (phase > 0 && cur) gfx::SetBool(player, c.path + "._visible", false), c.ours = true;
            else if (phase == 0) {
                if (!cur) gfx::SetBool(player, c.path + "._visible", true);  // as we left it: shown again
                c.ours = false;
            }
        }
    }
    g_phaseRing[target % 16] = phase;
}
void KeepScopeViewRT(ID3D11Texture2D* bb, uint64_t frameId) {
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC cur{};
    if (g_scv.tex) g_scv.tex->GetDesc(&cur);
    if (!g_scv.tex || cur.Width != d.Width || cur.Height != d.Height || cur.Format != d.Format) {
        const bool ok = g_scv.Create(dev, d.Width, d.Height, d.Format, false);
        LOG("[ui] zoom scope render target %ux%u fmt %u %s", d.Width, d.Height, d.Format, ok ? "created" : "FAILED");
        if (!ok) g_scv.w = g_scv.h = 0;
    }
    if (g_scv.tex) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        ctx->CopyResource(g_scv.tex, bb);
        ctx->Release();
        g_scvFrame = frameId;
        static int s_logs = 0;
        if (s_logs++ < 3) LOG("[ui] zoom scope render kept (frame %llu)", frameId);
    }
    dev->Release();
}
}  // namespace ggdpvr::vr::ui
