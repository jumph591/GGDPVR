#pragma once
#include <string>
#include <vector>

namespace ggdpvr::config {
// Reads Binaries\Win64\GGDPVR\ggdpvr.ini (next to GGDPVR.dll). Missing keys use the defaults below.
void Load(const std::wstring& modDir);

// [controls] (0.9.106): game buttons the VR controllers can press, and the controller inputs
enum ControlAction { kShoot, kZoom, kSightSpeed, kSpecialRequests, kDokiDokiMode, kMomIsComing, kConfirm, kCancel,
                     kPause, kR3, kRecenter, kHudToggle, kControlActions };
enum ControlHand { kMain, kOff, kLeft, kRight };
enum ControlButton { kTrigger, kGrip, kStickClick, kButtonLower, kButtonUpper, kMenu };

struct Values {
    // [diag]
    bool rrProbe = true;            // E1b: log still-initialised render resources at destruction
    bool rrTraceLifecycle = true;   // E1b: trace Init/ReleaseResource for suspect vtables
    // [fix]
    bool fixDeferredCleanupFlush = true;   // E1b fix (see fix/fix_deferred_cleanup.h)
    bool fixShaderNanClamp = true;         // devil-aura green blob fix (see fix/fix_shader_nan.h)
    // [render]
    bool d3d11DisableMotionBlur = true;  // UE3 motion blur flickers (blue haze every other frame) under D3D11
    int d3d11MsaaSamples = 1;            // 4x MSAA breaks Scaleform stencil masks under D3D11 (MSAA DSV + non-MSAA backbuffer)
    // [vr]
    bool vrEnabled = true;          // Stage 2: OpenXR virtual screen
    float vrScreenDistance = 2.0f;  // metres in front of the head at recenter
    float vrScreenWidth = 2.6f;     // metres
    bool vrUiLayer = true;          // Stage 5: HUD/menus on a floating panel in immersive scenes
    float vrUiDistance = 1.6f;      // metres in front of the head at recenter
    float vrUiWidth = 1.8f;         // metres (16:9)
    std::vector<std::string> vrUiFullscreenMovies;  // Scaleform movie classes drawn over the whole 3D view instead of the panel
    std::vector<std::string> vrUiHiddenMovies;      // Scaleform movie classes not drawn at all in immersive scenes
    std::vector<std::string> vrUiTintMovies;        // movies on the panel whose full-stage background tint also fills the view
    bool vrScreenMarks = true;      // kiss marks / shoe prints left by attacks: false = suppressed in VR
    float vrUiTintFeather = 0.06f;  // width of the panel-edge tint handover (fraction of the panel half-size)
    float vrUiFullscreenFov = 0.9f; // full-screen effects span this fraction of the headset FOV (tangent scale)
    int vrMode = 2;                 // 0 = virtual screen, 1 = head-tracked mono, 2 = stereo (Stage 4)
    float vrWorldScale = 85.0f;      // Unreal units per metre (girls' head bone ~124-135 uu above the feet)
    bool vrMirrorLeftEyeSkip = true;   // stereo: do not present the left eye to the desktop window
    bool vrCropToEyeFov = true;        // submit exact per-eye FOVs (crop the symmetric render); see xr_screen.cpp FillView
    bool vrRemapCursor = true;         // map game-screen cursor pixels into non-1920x1080 views (VR resolution)
    bool vrFixShoutVisibility = true;  // shout projectiles: on-screen test assumes 1920x1080
    bool vrHeadFacingTests = true;     // 0.9.113: weak-spot pop-up / Doki Doki tag "in front" tests use the eye's facing, not the game camera's
    bool vrNameTags3d = true;          // 0.9.113: the targeted girl's name tag drawn in 3D when the panel can't show it
    bool vrFrameSmoothing = false;     // 0.9.114: false = the engine's frame smoothing (62 fps cap) is turned off while VR runs
    bool vrZoomInPanel = true;         // right-click zoom shown in VR (scope, or the panel window without controllers)
    bool vrZoomScope = true;           // with controller aim: magnified circle around the reticle
    float vrZoomScopeDeg = 0.f;        // scope radius in degrees; 0 = the game's own scope size
    bool vrZoomScopeRender = true;     // the scope circle shows its own narrow-FOV render (sharp) instead of a stretched eye image
    float vrWeakSpotSize = 1.0f;           // 0.9.104: weak-spot pop-up size, 1 = the same size relative to the scene as on a monitor
    float vrWeakSpotScale = 0.47f;         // size of the weak-spot pop-ups relative to the game's
    bool vrFantasizeDolly = true;      // Fantasize zoom moves the camera toward the girl instead of narrowing the FOV
    bool vrControllers = true;      // Stage 7: VR controllers drive a virtual gamepad + aim
    bool vrHaptics = true;
    bool vrWeakSpotAtReticle = true;    // weak-spot pop-ups drawn in the 3D view at the reticle, not on the panel
    bool vrMenuLayers = true;          // title screen: background on a sphere, girls and logo as cut-outs at depth
    bool vrDressingBackdrop = true;
    int vrMenuLayerAnimation = 1;     // title: 1 = intro animations play at their depths, 0 = on the panel until settled
    int vrAeMinigameFix = 1;           // minigames: icons over the body on the panel, hit tests / touches where the pointer is
    int vrAeControllerGuide = 1;       // minigames: the controller versions of the timed-phase guides; thumbsticks work there too
    int vrAeRevealMode = 1;            // minigames: 1 = the reveal circle follows the pointer (vanilla size) and requests show /
                                       // work only inside it; 0 = everything always visible (reveal circle x minigame_reveal_scale)
    float vrAeRevealAura = 1.f;        // minigames, reveal mode 1: the circle this many times the game's size
    float vrAeRevealScale = 12.f;      // minigames: the hidden-icon reveal circle is this many times larger (1 = the game's)
    float vrAeFlickGain = 1.5f;        // minigames: flick requests - the pointer's motion on the panel counts this many times
    float vrAeRequestRadius = 0.f;     // minigames: requests count within this distance of the icon (0 = anywhere, like the game)
    float vrAePanelDrop = 0.f;         // minigames: the panel this many degrees lower (follow off: fixed; on: where it starts)
    int vrAePanelFollow = 1;           // minigames: the panel follows the gaze up / down (outside a dead zone)
    float vrAeFollowDeadzone = 0.f;    // ...degrees the gaze may wander from the panel centre before it follows
    float vrUiFullscreenFovDoki = 0.8f;
    float vrUiDokiFlowerBand = 0.f;     // ...its bottom part (the flowers) kept in its true shape (0 = stretched to the view) // Doki Doki's Double Peace border: its share of the headset FOV (closer in)
    int vrRenderCant = 0;              // tilt both eye renders toward what the eyes see (fewer pixels); 0 = straight ahead
    int vrTitleLogoOnPanel = 1;        // title screen: the logo on the panel (full-rate animation) instead of a cut-out
    int vrEffectStereo = 1;            // 0.9.70 A: the per-eye effect layer (weak-spot pop-ups, boss marks) shifted per eye by the game's own disparity
    int vrHudExtendRows = 1;           // 0.9.70 B: stages: the panel also shows the HUD rows above / below the band (name tags)
    float vrTitleFacePlayer = 1.3f;    // 0.9.94: the title girls' cut-outs turned toward the start position (0..1 of the turn)
    int vrFddmDokiOffsets = 1;         // 0.9.92: FDDM zoom-in dokis: only their body point converted, their fly-out offset kept
    float vrSightCenterScale = 0.75f;   // 0.9.89: the game sight's spinning centre scaled (aura untouched); 1 = as the game draws it
    int vrReticleStyle = 1;            // 0.9.86: 0 = the mod's ring reticle, 1 = the game's own sight (aura) at the reticle
    int vrEffectRestDepth = 1;         // 0.9.84: with no weak-spot pop-up / boss mark up, the effect layer (kiss marks) at the HUD's depth
    int vrRemoveHitShake = 1;          // 0.9.83: the game's camera shake when you're hit taken out of the view
    int vrRemoveWalkBob = 1;           // 0.9.83: the game's walking bob (move shake) taken out of the view
    int vrFollowSystemRecenter = 1;    // 0.9.79: the headset's own recenter (LOCAL space change) re-centres the game too
    int vrXrCleanExit = 1;             // 0.9.77: the OpenXR session / instance closed before the process exits (exit crash)
    int vrQuitWithRuntime = 1;         // 0.9.77: the runtime's "Quit app" (session EXITING) closes the game
    int vrLoadingSkipStale = 1;        // 0.9.75: a half-drawn loading frame (just after the resize) shows no panel
    int vrWeakSpotDistScale = 1;           // 0.9.71: weak-spot pop-ups sized by their measured distance (full size up to weak_spot_full_size_m)
    float vrWeakSpotFullSizeM = 1.5f;      // ...distance (m) up to which a weak-spot pop-up keeps its full size
    float vrWeakSpotMinSize = 0.3f;        // ...the smallest a far weak-spot pop-up gets (fraction of full size)
    int vrEffectFullView = 1;          // 0.9.70 C: the per-eye effect layer covers the whole eye view (weak-spot pop-ups low in the view)
    int vrDynamicShadows = 0;          // 0 = the game's dynamic shadows off in VR (faster; restored on exit); 1 = the game's setting
    int vrTitleGroundGirls = 1;
    float vrTitleFrontPush = 0.75f;
    float vrTitleFrontRight = 0.1f;    // title screen: the front girls this many metres to the right (negative = left)    // title screen: the front girls this many metres further back (same size: smaller)        // title screen: the girl cut-outs stand on the floor at their cut-off edge
    int vrEarlyWait = 1;               // stereo: wait for the headset before eye 0 renders (not after it), with a flush
    int vrPredictSlots = 1;            // predict the head pose for the display slot the frame will really reach
    int vrFrameHoldSlots = 0;          // 0.9.81: hold every frame this many display slots, any refresh (2 = half rate; 0 = off)
    float vrFrameLockHz = 0.f;         // even cadence: hold every frame for the same number of display slots (0 = off)
    int vrDesktopMirror = 0;           // 1: the right eye still goes to the desktop window (0 = no desktop presents in VR)
    int logging = 0;                   // 0.9.104: log=1 - write the log (developer=1 also logs)
    int developer = 0;                 // 0.9.101: [debug] developer=1 - developer console, F-key hotkeys, dumps (development only)
    int vrPerfLog = 0;                 // [perf] lines every 5 s: frame rate, CPU / GPU time per eye
    int vrImmersiveLoading = 1;        // loading screens in VR scenes: their background colour all around, the rest on the panel
    // 0.9.106 [controls]: the VR controller inputs of each game button, in ControlAction order. An input is
    // hand * 8 + button: hands kMain / kOff / kLeft / kRight, buttons kTrigger ... kMenu (below).
    std::vector<unsigned char> vrControls[12];
    std::string vrControlsInvalid;     // entries that could not be read (logged at startup)
    float vrAePanelScale = 1.3f;       // minigames: the HUD panel this much larger, so icons further out on her body show
    int vrReleaseIdleSwapchains = 0;   // free XR swapchains unused for 10 s (VRAM); breaks capture layers (OBS Mirror)
    int vrHeadsetAudio = 1;            // the game's sound to the headset's audio device, if one is connected at start
    std::wstring vrHeadsetAudioDevice;  // part of the audio device's name to use instead (empty = automatic)
    int vrFreezeWatch = 1;             // diag: log frame stalls with video memory use, and a frozen eye pose (xr_screen)
    int vrShopVoidWhite = 1;           // Academy Store: the void around the booth (and its fades) 1 = white (0.9.10), 0 = black
    float vrGameOverHeight = 130.f;    // story game over: camera this many uu above the girl's feet (0 = the game's ~100)
    float vrGameOverDistance = 130.f;  // ...and this many uu from her (0 = the game's 150)
    float vrGameOverGazeOffset = -15.f;  // ...and she looks this many uu above (+) / below (-) the moved camera
    float vrDressingDistance = 4.5f;   // Dressing Room: metres from the girl the viewpoint moves to (0 = the game's own)    // Dressing Room: an endless floor and a bubble-wallpaper dome instead of the black void
    bool vrCollectionBackdrop = true;  // Collection menus: their background fills the view (no stale 3D image)          // controller vibration: game rumble, shots, hits, menu clicks
    int vrDominantHand = 1;         // 1 right, 0 left (trigger/grip/stick roles mirror)
    bool vrAimFromHead = true;        // aim ray from the head through the controller's far aim point (no hand-eye parallax)
    float vrReticleDistance = 60.0f; // max metres along the aim ray (closer when the shot ray hits something)
    float vrAimSmoothing = 1.0f;    // controller jitter filter strength (0 = off)
    bool vrRevealRing = true;       // outer ring showing the sight's reveal area (mini demons, Mr. Happiness)
    float vrReticleSizeDeg = 1.4f;  // angular radius
    bool vrResEnabled = true;        // manage the render resolution (setres) per scene
    float vrRenderScale = 1.0f;      // immersive render density relative to the headset's recommended resolution
    int vrScreenResW = 1920, vrScreenResH = 1080;  // resolution for virtual-screen scenes
    bool vrPauseOnScreen = false;    // show the in-stage pause menu on the virtual screen instead of immersive
    std::vector<std::string> vrImmersiveGames;  // GameInfo class-name substrings shown immersively; others on the virtual screen
    std::vector<std::string> vrPointerGames;    // immersive menu scenes where the controller points/clicks instead of shooting
    int vrDebugFakeHead = 0;        // debug: synthetic head pose without a headset (see head_tracking.cpp)
    float vrFovMarginDeg = 2.f;     // extra render FOV beyond the headset's eye FOV (reprojection margin)
    std::wstring vrRuntime = L"system";  // system | oculus (Quest Link) | steamvr | virtualdesktop | <path to runtime json>
    // [ue3]
    bool ue3Bootstrap = true;       // E3: find GNames/GObjects, derive reflection offsets
    bool ue3DumpReflection = true;  // E3: write reflection_dump.txt
    bool processEventHook = true;   // E3: hook UObject::ProcessEvent
    std::vector<std::string> peLogFilters;  // substrings of function full names to log
    int peLogFirstN = 3;            // log the first N calls of every matching function
    float peSummarySeconds = 15.f;  // periodic per-function call-count summary
    bool cursorProbe = true;        // log the sight cursor fields once a second
};
const Values& Get();
void SetWorldScale(float uuPerMetre);  // live (the !worldscale command); also saved to ggdpvr.ini
void SetShopVoidWhite(int white);     // live, not saved (test: !shopvoid); the user sets it in ggdpvr.ini
}  // namespace ggdpvr::config
