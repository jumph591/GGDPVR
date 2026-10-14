#include "config.h"

#include <windows.h>
#include <cctype>
#include <sstream>

namespace ggdpvr::config {
namespace {
Values g_values;
std::wstring g_iniPath;

bool Bool(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(sec, key, def ? 1 : 0, ini.c_str()) != 0;
}
std::string Str(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[4096]{};
    GetPrivateProfileStringW(sec, key, def, buf, 4096, ini.c_str());
    char out[4096]{};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return out;
}
// 0.9.104: a key is looked up in [advanced], then [vr] (then [debug], older inis) - the player file has the everyday
// options under [vr] and the fine tuning under [advanced]. Retired options read from a section that never exists
// (kFixed), so their built-in value always applies.
const wchar_t* const kFixed = L"__fixed__";
const wchar_t* Sec(const wchar_t* key) {
    for (const wchar_t* s : {L"advanced", L"vr", L"debug"}) {
        wchar_t buf[8]{};
        GetPrivateProfileStringW(s, key, L"\x01", buf, 8, g_iniPath.c_str());
        if (buf[0] != 1) return s;
    }
    return L"vr";
}
// [controls]: "main.trigger, left.menu" -> inputs; "none" / empty -> no input. Unknown entries are collected.
const char* const kControlKeys[kControlActions] = {"shoot", "zoom", "sight_speed", "special_requests",
                                                   "doki_doki_mode", "mom_is_coming", "confirm", "cancel",
                                                   "pause", "r3", "recenter", "hud_toggle"};
const char* const kControlDefaults[kControlActions] = {
    "main.trigger", "main.grip", "off.grip", "off.trigger", "left.button_upper", "left.button_lower",
    "right.button_lower", "right.button_upper", "left.menu, right.menu", "main.stick_click", "off.stick_click",
    "right.button_upper"};
std::vector<unsigned char> ParseControls(std::string s, std::string& invalid) {
    std::vector<unsigned char> out;
    for (auto& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    for (auto& c : s)
        if (c == ',' || c == ';' || c == '\t' || c == '+') c = ' ';
    std::stringstream ss(s);
    std::string t;
    while (ss >> t) {
        if (t == "none" || t == "off" || t == "-") continue;
        const size_t dot = t.find('.');
        const std::string h = dot == std::string::npos ? "" : t.substr(0, dot), b = dot == std::string::npos ? t : t.substr(dot + 1);
        int hand = h == "main" || h == "aim" || h == "dominant" ? kMain
                 : h == "off" || h == "other" ? kOff
                 : h == "left" ? kLeft
                 : h == "right" ? kRight : -1;
        int btn = b == "trigger" ? kTrigger
                : b == "grip" || b == "squeeze" ? kGrip
                : b == "stick_click" || b == "thumbstick_click" || b == "trackpad_click" ? kStickClick
                : b == "button_lower" || b == "lower" ? kButtonLower
                : b == "button_upper" || b == "upper" ? kButtonUpper
                : b == "menu" ? kMenu : -1;
        if (hand < 0 || btn < 0) {
            invalid += (invalid.empty() ? "" : ", ") + t;
            continue;
        }
        out.push_back(static_cast<unsigned char>(hand * 8 + btn));
    }
    return out;
}
std::wstring Str16(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[256]{};
    GetPrivateProfileStringW(sec, key, def, buf, 256, ini.c_str());
    return buf;
}
}  // namespace

void Load(const std::wstring& modDir) {
    const std::wstring ini = modDir + L"\\ggdpvr.ini";
    g_iniPath = ini;
    Values& v = g_values;
    v.rrProbe = Bool(ini, L"diag", L"rr_probe", v.rrProbe);
    v.rrTraceLifecycle = Bool(ini, L"diag", L"rr_trace_lifecycle", v.rrTraceLifecycle);
    v.fixDeferredCleanupFlush = Bool(ini, kFixed, L"deferred_cleanup_flush", v.fixDeferredCleanupFlush);
    v.fixShaderNanClamp = Bool(ini, kFixed, L"shader_nan_clamp", v.fixShaderNanClamp);
    v.d3d11DisableMotionBlur = Bool(ini, kFixed, L"d3d11_disable_motionblur", v.d3d11DisableMotionBlur);
    v.d3d11MsaaSamples = GetPrivateProfileIntW(kFixed, L"d3d11_msaa_samples", v.d3d11MsaaSamples, ini.c_str());
    v.vrEnabled = Bool(ini, Sec(L"enabled"), L"enabled", v.vrEnabled);
    v.vrScreenDistance = float(_wtof(Str16(ini, Sec(L"screen_distance"), L"screen_distance", L"2.0").c_str()));
    v.vrScreenWidth = float(_wtof(Str16(ini, Sec(L"screen_width"), L"screen_width", L"2.6").c_str()));
    v.vrUiLayer = Bool(ini, kFixed, L"ui_layer", true);
    v.vrUiDistance = float(_wtof(Str16(ini, Sec(L"ui_distance"), L"ui_distance", L"1.6").c_str()));
    v.vrUiWidth = float(_wtof(Str16(ini, Sec(L"ui_width"), L"ui_width", L"1.8").c_str()));
    v.vrMode = GetPrivateProfileIntW(Sec(L"mode"), L"mode", v.vrMode, ini.c_str());
    v.vrWorldScale = float(_wtof(Str16(ini, Sec(L"world_scale"), L"world_scale", L"85.0").c_str()));
    v.vrFovMarginDeg = float(_wtof(Str16(ini, kFixed, L"fov_margin_deg", L"2").c_str()));
    v.vrDebugFakeHead = GetPrivateProfileIntW(Sec(L"debug_fake_head"), L"debug_fake_head", 0, ini.c_str());
    {
        std::stringstream im(Str(ini, kFixed, L"immersive_games", L"GG2ShootingGame,GG2ActionEvent,GG2Bs,GG2DokiSp,GG2GameOverGameInfo,GG2CollectionGameInfo,GG2LockerRoomGameInfo,GG2TitleGame,GG2BaseGame,GG2DemoGame,GG2ScoreAttackGameInfo"));
        std::string it;
        while (std::getline(im, it, ','))
            if (!it.empty()) v.vrImmersiveGames.push_back(it);
    }
    {
        std::stringstream pg(Str(ini, kFixed, L"pointer_games", L"GG2GameOverGameInfo,GG2CollectionGameInfo,GG2LockerRoomGameInfo,GG2TitleGame,GG2BaseGame,GG2DemoGame,GG2ScoreAttackGameInfo,GG2ActionEvent"));
        std::string it;
        while (std::getline(pg, it, ','))
            if (!it.empty()) v.vrPointerGames.push_back(it);
    }
    auto list = [&](const wchar_t* key, const wchar_t* def, std::vector<std::string>& out) {
        std::stringstream ss(Str(ini, kFixed, key, def));  // (retired: the lists below are fixed)
        std::string it;
        while (std::getline(ss, it, ','))
            if (!it.empty()) out.push_back(it);
    };
    list(L"ui_fullscreen_movies", L"GG2ScreenFadeMovie,GG2HudShootingDamageEffectMovie,GG2HudDokiFadeMovie", v.vrUiFullscreenMovies);
    list(L"ui_hidden_movies", L"", v.vrUiHiddenMovies);
    list(L"ui_tint_movies", L"GG2HudContinueMovie,GG2HudWindowPauseMovie,GG2HudShootingScopeMovie", v.vrUiTintMovies);
    v.vrUiTintFeather = float(_wtof(Str16(ini, kFixed, L"ui_tint_feather", L"0.06").c_str()));
    if (v.vrUiTintFeather < 0.005f) v.vrUiTintFeather = 0.005f;
    v.vrScreenMarks = Bool(ini, Sec(L"screen_marks"), L"screen_marks", true);
    v.vrUiFullscreenFov = float(_wtof(Str16(ini, Sec(L"ui_fullscreen_fov"), L"ui_fullscreen_fov", L"0.9").c_str()));
    v.vrPauseOnScreen = Bool(ini, kFixed, L"pause_on_screen", false);
    v.vrCropToEyeFov = Bool(ini, kFixed, L"crop_to_eye_fov", true);
    v.vrResEnabled = Bool(ini, kFixed, L"manage_resolution", true);
    v.vrRenderScale = float(_wtof(Str16(ini, Sec(L"render_scale"), L"render_scale", L"1.0").c_str()));
    v.vrScreenResW = GetPrivateProfileIntW(kFixed, L"screen_res_width", 1920, ini.c_str());
    v.vrScreenResH = GetPrivateProfileIntW(kFixed, L"screen_res_height", 1080, ini.c_str());
    v.vrRemapCursor = Bool(ini, kFixed, L"remap_cursor", true);
    v.vrFixShoutVisibility = Bool(ini, kFixed, L"fix_shout_visibility", true);
    v.vrHeadFacingTests = Bool(ini, Sec(L"head_facing_tests"), L"head_facing_tests", true);
    v.vrNameTags3d = Bool(ini, Sec(L"name_tags_3d"), L"name_tags_3d", true);
    v.vrFrameSmoothing = Bool(ini, Sec(L"frame_smoothing"), L"frame_smoothing", false);
    v.vrZoomInPanel = Bool(ini, kFixed, L"zoom_in_panel", true);
    v.vrZoomScope = Bool(ini, kFixed, L"zoom_scope", true);
    v.vrHaptics = Bool(ini, Sec(L"haptics"), L"haptics", true);
    v.vrWeakSpotAtReticle = Bool(ini, kFixed, L"weak_spot_at_reticle", true);
    v.vrCollectionBackdrop = Bool(ini, kFixed, L"collection_backdrop", true);
    v.vrDressingBackdrop = Bool(ini, kFixed, L"dressing_backdrop", true);
    v.vrMenuLayers = Bool(ini, kFixed, L"menu_layers", true);
    v.vrMenuLayerAnimation = GetPrivateProfileIntW(kFixed, L"menu_layer_animation", 1, ini.c_str());
    v.vrShopVoidWhite = GetPrivateProfileIntW(kFixed, L"shop_void_white", 1, ini.c_str());
    v.vrFreezeWatch = GetPrivateProfileIntW(kFixed, L"freeze_watch", 1, ini.c_str());
    v.vrHeadsetAudio = GetPrivateProfileIntW(Sec(L"headset_audio"), L"headset_audio", 1, ini.c_str());
    v.vrReleaseIdleSwapchains = GetPrivateProfileIntW(kFixed, L"release_idle_swapchains", 0, ini.c_str());
    v.vrAeMinigameFix = GetPrivateProfileIntW(kFixed, L"minigame_pointer", 1, ini.c_str());
    v.vrAeControllerGuide = GetPrivateProfileIntW(kFixed, L"minigame_controller_guide", 1, ini.c_str());
    v.vrAeRevealMode = GetPrivateProfileIntW(kFixed, L"minigame_reveal_mode", 1, ini.c_str());
    v.vrAeRevealAura = float(_wtof(Str16(ini, kFixed, L"minigame_reveal_aura", L"1").c_str()));
    v.vrAeRevealScale = float(_wtof(Str16(ini, kFixed, L"minigame_reveal_scale", L"12").c_str()));
    v.vrAeFlickGain = float(_wtof(Str16(ini, Sec(L"minigame_flick_gain"), L"minigame_flick_gain", L"1.5").c_str()));
    v.vrAeRequestRadius = float(_wtof(Str16(ini, Sec(L"minigame_request_radius"), L"minigame_request_radius", L"0").c_str()));
    v.vrAePanelDrop = float(_wtof(Str16(ini, Sec(L"minigame_panel_drop"), L"minigame_panel_drop", L"0").c_str()));
    v.vrAePanelFollow = GetPrivateProfileIntW(kFixed, L"minigame_panel_follow", 1, ini.c_str());
    v.vrAeFollowDeadzone = float(_wtof(Str16(ini, Sec(L"minigame_follow_deadzone"), L"minigame_follow_deadzone", L"0").c_str()));
    v.vrUiFullscreenFovDoki = float(_wtof(Str16(ini, Sec(L"ui_fullscreen_fov_doki"), L"ui_fullscreen_fov_doki", L"0.8").c_str()));
    v.vrUiDokiFlowerBand = float(_wtof(Str16(ini, Sec(L"ui_doki_flower_band"), L"ui_doki_flower_band", L"0").c_str()));
    v.developer = GetPrivateProfileIntW(Sec(L"developer"), L"developer", 0, ini.c_str());
    v.logging = GetPrivateProfileIntW(Sec(L"log"), L"log", 1, ini.c_str()) != 0 || v.developer;  // 1.0.1: on by default
    v.vrPerfLog = v.logging;  // 0.9.110: the log always includes the frame-time lines (perf_log retired)
    v.vrTitleLogoOnPanel = GetPrivateProfileIntW(kFixed, L"title_logo_on_panel", 1, ini.c_str());
    v.vrEffectStereo = GetPrivateProfileIntW(kFixed, L"effect_stereo", 1, ini.c_str());
    v.vrHudExtendRows = GetPrivateProfileIntW(kFixed, L"hud_extend_rows", 1, ini.c_str());
    v.vrEffectFullView = GetPrivateProfileIntW(kFixed, L"effect_full_view", 1, ini.c_str());
    v.vrWeakSpotDistScale = GetPrivateProfileIntW(kFixed, L"weak_spot_distance_scale", 1, ini.c_str());
    v.vrLoadingSkipStale = GetPrivateProfileIntW(kFixed, L"loading_skip_stale", 1, ini.c_str());
    v.vrXrCleanExit = GetPrivateProfileIntW(kFixed, L"xr_clean_exit", 1, ini.c_str());
    v.vrFollowSystemRecenter = GetPrivateProfileIntW(kFixed, L"follow_system_recenter", 1, ini.c_str());
    v.vrRemoveHitShake = GetPrivateProfileIntW(Sec(L"remove_hit_shake"), L"remove_hit_shake", 1, ini.c_str());
    v.vrEffectRestDepth = GetPrivateProfileIntW(kFixed, L"effect_rest_depth", 1, ini.c_str());
    v.vrReticleStyle = GetPrivateProfileIntW(kFixed, L"reticle_style", 1, ini.c_str());
    v.vrFddmDokiOffsets = GetPrivateProfileIntW(kFixed, L"fddm_doki_offsets", 1, ini.c_str());
    v.vrTitleFacePlayer = float(_wtof(Str16(ini, Sec(L"title_face_player"), L"title_face_player", L"1.3").c_str()));
    v.vrSightCenterScale = float(_wtof(Str16(ini, Sec(L"sight_center_scale"), L"sight_center_scale", L"0.75").c_str()));
    v.vrRemoveWalkBob = GetPrivateProfileIntW(Sec(L"remove_walk_bob"), L"remove_walk_bob", 1, ini.c_str());
    v.vrQuitWithRuntime = GetPrivateProfileIntW(kFixed, L"quit_with_runtime", 1, ini.c_str());
    v.vrWeakSpotFullSizeM = float(_wtof(Str16(ini, Sec(L"weak_spot_full_size_m"), L"weak_spot_full_size_m", L"1.5").c_str()));
    v.vrWeakSpotMinSize = float(_wtof(Str16(ini, Sec(L"weak_spot_min_size"), L"weak_spot_min_size", L"0.3").c_str()));
    v.vrDynamicShadows = GetPrivateProfileIntW(Sec(L"dynamic_shadows"), L"dynamic_shadows", 0, ini.c_str());
    v.vrTitleGroundGirls = GetPrivateProfileIntW(kFixed, L"title_ground_girls", 1, ini.c_str());
    v.vrTitleFrontPush = float(_wtof(Str16(ini, Sec(L"title_front_push"), L"title_front_push", L"0.75").c_str()));
    v.vrTitleFrontRight = float(_wtof(Str16(ini, Sec(L"title_front_right"), L"title_front_right", L"0.1").c_str()));
    v.vrEarlyWait = GetPrivateProfileIntW(kFixed, L"early_wait", 1, ini.c_str());
    v.vrPredictSlots = GetPrivateProfileIntW(kFixed, L"predict_frame_slots", 1, ini.c_str());
    v.vrFrameLockHz = float(_wtof(Str16(ini, kFixed, L"frame_lock_hz", L"0").c_str()));
    v.vrFrameHoldSlots = GetPrivateProfileIntW(Sec(L"frame_hold_slots"), L"frame_hold_slots", 0, ini.c_str());
    v.vrDesktopMirror = GetPrivateProfileIntW(Sec(L"desktop_mirror"), L"desktop_mirror", 0, ini.c_str());
    v.vrRenderCant = GetPrivateProfileIntW(kFixed, L"render_tilt", 0, ini.c_str());
    v.vrImmersiveLoading = GetPrivateProfileIntW(kFixed, L"immersive_loading", 1, ini.c_str());
    // [controls] (0.9.106). An ini from before it may still have hud_toggle_button (A / B / R3 / none): used when
    // [controls] has no hud_toggle.
    for (int a = 0; a < kControlActions; ++a) {
        wchar_t key[64]{};
        MultiByteToWideChar(CP_UTF8, 0, kControlKeys[a], -1, key, 64);
        std::string val = Str(ini, L"controls", key, L"\x01");
        if (val == "\x01") {
            val = kControlDefaults[a];
            if (a == kHudToggle) {
                const std::wstring b = Str16(ini, Sec(L"hud_toggle_button"), L"hud_toggle_button", L"");
                if (b == L"A" || b == L"a") val = "right.button_lower";
                else if (b == L"R3" || b == L"r3" || b == L"stick") val = "main.stick_click";
                else if (b == L"none" || b == L"0") val = "none";
            }
        }
        v.vrControls[a] = ParseControls(val, v.vrControlsInvalid);
    }
    v.vrAePanelScale = float(_wtof(Str16(ini, Sec(L"minigame_panel_scale"), L"minigame_panel_scale", L"1.3").c_str()));
    v.vrHeadsetAudioDevice = Str16(ini, Sec(L"headset_audio_device"), L"headset_audio_device", L"");
    v.vrGameOverHeight = float(_wtof(Str16(ini, Sec(L"gameover_height"), L"gameover_height", L"130").c_str()));
    v.vrGameOverDistance = float(_wtof(Str16(ini, Sec(L"gameover_distance"), L"gameover_distance", L"130").c_str()));
    v.vrGameOverGazeOffset = float(_wtof(Str16(ini, Sec(L"gameover_gaze_offset"), L"gameover_gaze_offset", L"-15").c_str()));
    v.vrDressingDistance = float(_wtof(Str16(ini, Sec(L"dressing_distance"), L"dressing_distance", L"4.5").c_str()));
    v.vrZoomScopeDeg = float(_wtof(Str16(ini, kFixed, L"zoom_scope_deg", L"0").c_str()));
    v.vrZoomScopeRender = Bool(ini, Sec(L"zoom_scope_render"), L"zoom_scope_render", true);
    v.vrWeakSpotSize = float(_wtof(Str16(ini, Sec(L"weak_spot_size"), L"weak_spot_size", L"1.0").c_str()));
    v.vrFantasizeDolly = Bool(ini, kFixed, L"fantasize_dolly", true);
    v.vrControllers = Bool(ini, Sec(L"controllers"), L"controllers", true);
    v.vrDominantHand = Str(ini, Sec(L"dominant_hand"), L"dominant_hand", L"right") == "left" ? 0 : 1;
    v.vrAimFromHead = Str(ini, Sec(L"aim_origin"), L"aim_origin", L"head") != "hand";
    v.vrReticleDistance = float(_wtof(Str16(ini, Sec(L"reticle_distance"), L"reticle_distance", L"60.0").c_str()));
    v.vrReticleSizeDeg = float(_wtof(Str16(ini, Sec(L"reticle_size"), L"reticle_size", L"1.4").c_str()));
    v.vrRevealRing = Bool(ini, kFixed, L"reveal_ring", true);
    v.vrAimSmoothing = float(_wtof(Str16(ini, Sec(L"aim_smoothing"), L"aim_smoothing", L"1.0").c_str()));
    v.vrRuntime = Str16(ini, Sec(L"runtime"), L"runtime", L"system");
    v.ue3Bootstrap = Bool(ini, L"ue3", L"bootstrap", v.ue3Bootstrap);
    v.ue3DumpReflection = Bool(ini, L"ue3", L"dump_reflection", v.ue3DumpReflection);
    v.processEventHook = Bool(ini, L"ue3", L"processevent_hook", v.processEventHook);
    // once-a-second cursor lines and a 15 s script summary: developer logs; a player's log gets the summary each minute
    v.cursorProbe = Bool(ini, L"ue3", L"cursor_probe", v.developer != 0);
    v.peLogFirstN = GetPrivateProfileIntW(L"ue3", L"pe_log_first_n", v.peLogFirstN, ini.c_str());
    v.peSummarySeconds = float(GetPrivateProfileIntW(L"ue3", L"pe_summary_seconds", v.developer ? 15 : 60, ini.c_str()));
    std::stringstream ss(Str(ini, L"ue3", L"pe_log_filters",
                             L"GG2Camera,Camera.UpdateCamera,GetBaseFov,GetGameFov,SetCameraBaseRotation,"
                             L"CalcUpdatePlayerPose,UpdateRotation,PostRender,GFxMoviePlayer.Start,"
                             L"GG2Hud,ShotNormal,ControlCursor,LoadMap,GameInfo.InitGame,PostBeginPlay"));
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) v.peLogFilters.push_back(item);
}

const Values& Get() { return g_values; }
void SetShopVoidWhite(int white) {
    g_values.vrShopVoidWhite = white;  // not saved: an F-key hotkey for it also reached the game (F11 changed the resolution - 0.9.10)
}
void SetWorldScale(float uuPerMetre) {
    g_values.vrWorldScale = uuPerMetre;
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", uuPerMetre);
    if (!g_iniPath.empty()) WritePrivateProfileStringW(Sec(L"world_scale"), L"world_scale", buf, g_iniPath.c_str());  // kept for next time
}
}  // namespace ggdpvr::config
