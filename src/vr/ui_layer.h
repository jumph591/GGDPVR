#pragma once
#include <cstdint>

struct ID3D11Texture2D;
struct ID3D11Device;
namespace ggdpvr::xr { struct FrameRecord; }

// Stage 5: UI layer separation.
// Scaleform (all of GG2's HUD and menus) is drawn by one UE3 render command, FGFxRenderUI, at the end of
// each viewport draw. In stereo frames its Execute is wrapped on the render thread:
//  - left eye: skipped (the eye image stays UI-free);
//  - right eye: the eye image is saved, the UI is drawn twice onto the cleared backbuffer (black, then
//    white) and both results are kept, then the eye image is put back.
// From the black/white pair the UI's colour and coverage are recovered exactly:
//    alpha = 1 - (white - black), premultiplied colour = black.
namespace ggdpvr::vr::ui {
bool Install();
void SetEnabled(bool on);  // debug/config switch
bool Enabled();
bool FantasizeActive();  // the Fantasize (wild fancy) screen is up
// The 16:9 HUD band (the UI's "show all" area) inside a w x h view; the UI layer image is this band.
void UpdatePanelFollow(float headPitchDeg);  // game thread, per frame (minigames: the panel follows the gaze)
float PanelYOffset();        // the panel centre's height in the recentered frame (metres; minigames: lowered)
float PanelWidth();          // the panel's width now (metres): ui_width, x minigame_panel_scale in minigames
void SetTallPanel(bool on);  // minigames: the panel also shows the canvas above / below the HUD band
void SetHudToggleContext(bool gameplay);  // stages / boss battles in play (not paused, no prompt): the HUD toggle works
bool HudToggleContext();
void ToggleHud();
void ShowHud(const char* why);             // un-hides it (stage over / left / lost)                         // hides / shows the gameplay HUD (HP, score, gauges, boss HP, score pop-ups)
void SetStageRows(bool on);
void SetEffectRestShift(float stageUnits);  // game thread, per frame: the HUD panel depth's disparity (effect_rest_depth)  // stages: the panel also shows the canvas above / below the band (hud_extend_rows)
float PanelExtension();      // ...those rows (each side) as a fraction of the band height; 0 = normal panel
void BandOf(uint32_t w, uint32_t h, uint32_t& x0, uint32_t& y0, uint32_t& bw, uint32_t& bh);
void TraceAs(uint32_t ms);  // dev: log every ActionScript call for ms milliseconds
// Game thread, once per frame before the draws: layered menu movies (title screen) - sets which part of the movie
// the coming frames show (see ui_layer.cpp kLayerSpecs).
void UpdateMenuLayersGT(uint64_t frameId, bool menuScene);
void SetLayerLag(int frames);  // dev
bool ShopOpen();  // the Academy Store screen is up (GG2HudUIShopMovie)
bool ContinuePromptUp();
bool StageResultUp();   // the end-of-stage score screen (stages, boss battles) is showing
bool MamakitaUp();      // "Mom is coming" (GG2HudMamakitaMovie) covers the screen
bool TakeAeNewIcon();   // minigames: the HUD placed a new hidden icon since the last call (resets)
void SetTagTest(bool on, float x, float y);  // dev: !tagtest - name tag slot 0 pinned at an eye-view position
void DokiTick();        // game thread, per frame: Doki Doki Double Peace effect -> full screen
void AeRevealTick();    // game thread, per frame: minigame reveal mode - the request layer shown only inside the circle
bool AeRequestRevealed();  // minigames: no request showing, or the pointer's reveal circle covers it (touches allowed)
bool FlickRequestUp();  // minigames: a flick (sweep) request is showing
bool GesturePhase();    // minigames: the timed gesture ("skinship") phase is on (StartResistGauge .. EndResistGauge)
bool LastAeIcon(int kind, float& x, float& y);  // dev: the last hidden (0) / request (1) icon / gesture guide (2) on the panel                 // game thread: the Continue / Give Up prompt is up (StartContinueWipe .. EndContinue)
void ResetContinuePrompt();              // game mode changed (Score Attack's prompt ends in a reload, without EndContinue)
bool MovieOpen(const char* movieClass);  // game thread: a movie of that class is being drawn, open, its root visible
void DumpLayers();
void SetMenuLayers(bool on);  // dev: !menulayers 0|1
void ToggleRoomDebug();       // dev: !roomdebug             // dev: write each layer texture once (dump_layerN.bmp)
// Render thread, at the Present of the zoom scope draw (FrameRecord eye 2): keep its image for the eyes' scope circle.
void KeepScopeViewRT(ID3D11Texture2D* bb, uint64_t frameId);
// Render thread, desktop_mirror=1: the HUD panel drawn into the eye image going to the desktop window (call only
// after that image was copied to the headset).
void MirrorPanelRT(ID3D11Device* dev, ID3D11Texture2D* bb, const xr::FrameRecord& rec);
}  // namespace ggdpvr::vr::ui
