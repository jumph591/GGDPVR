// GGDPVR Setup - the settings screen: pages of options on the left and in the middle, a small animated demonstration of the
// selected option on the right. Included by setup_main.cpp (its helpers: Text, RoundRect, SetIniValue...). A change is
// written to ggdpvr.ini at once, only that value (SetIniValue); the game reads the file when it starts.
constexpr float kSW = 980, kSH = 660;
const RectF kListArea(204, 76, 396, 532), kCard(616, 76, 344, 232);
constexpr float kRowH = 56;
const Color kInk(255, 74, 30, 66), kSoft(255, 128, 92, 120), kPink(255, 232, 62, 140), kEdge(255, 243, 182, 214);

enum OptType { kOptToggle, kOptChoice, kOptSlider, kOptBind, kOptInfo, kOptGuide, kOptLink };  // kOptBind: a [controls] line; kOptInfo, kOptGuide (a mode's controls), kOptLink (to a page): only show
enum Demo {
    kDTurn, kDVignette, kDJump, kDShake, kDBob, kDAuto, kDSnapAngle, kDFade, kDGrab, kDLevel, kDMiniFade, kDMiniPanel, kDFlick, kDHud, kDScreen,
    kDFullFov, kDSight, kDWeak, kDMarks, kDKiss, kDStomp, kDInd, kDIndRing, kDControllers, kDHand, kDHaptics, kDAimOrigin, kDAimSmooth,
    kDRender, kDWorld, kDShadow, kDMirror, kDFrameHold, kDAudio, kDBind, kDStickTurn, kDStickOther, kDGuide
};
struct Choice {
    const wchar_t* label;
    const char* value;
};
struct Opt {
    const char* key;
    const wchar_t* label;
    OptType type;
    const char* def;
    bool advanced;  // its section when the player's file doesn't have the line yet
    Demo demo;
    const wchar_t* help;
    float lo = 0, hi = 1, step = 0.1f;
    const wchar_t* unit = L"";  // L"%": shown as a percentage
    std::vector<Choice> choices = {};
    std::vector<std::pair<const wchar_t*, const wchar_t*>> guide = {};  // kOptGuide: what -> how ({key} = that action's buttons)
    int target = 0;  // kOptLink: the page
    const char* needKey = nullptr;  // greyed out unless this option has...
    const char* needVal = nullptr;  // ...this value
    const wchar_t* needText = nullptr;
};
struct Page {
    const wchar_t* name;
    std::vector<Opt> opts;
};
constexpr int kMapPage = 6, kGuidePage = 8;  // Button mapping; the controls guide (no place in the side list: opened from Button mapping)
bool Editable(const Opt& o) { return o.type == kOptToggle || o.type == kOptChoice || o.type == kOptSlider || o.type == kOptBind; }
Opt Toggle(const char* k, const wchar_t* l, const char* d, bool adv, Demo demo, const wchar_t* h) { return {k, l, kOptToggle, d, adv, demo, h}; }
Opt Pick(const char* k, const wchar_t* l, const char* d, bool adv, Demo demo, const wchar_t* h, std::vector<Choice> c) {
    Opt o{k, l, kOptChoice, d, adv, demo, h};
    o.choices = std::move(c);
    return o;
}
Opt Slide(const char* k, const wchar_t* l, const char* d, bool adv, Demo demo, const wchar_t* h, float lo, float hi, float step, const wchar_t* unit) {
    Opt o{k, l, kOptSlider, d, adv, demo, h};
    o.lo = lo, o.hi = hi, o.step = step, o.unit = unit;
    return o;
}
Opt Bind(const char* k, const wchar_t* l, const char* d, const wchar_t* h) { return {k, l, kOptBind, d, false, kDBind, h}; }
Opt Info(const char* k, const wchar_t* l, Demo demo, const wchar_t* h) { return {k, l, kOptInfo, "", false, demo, h}; }
Opt Guide(const wchar_t* l, std::vector<std::pair<const wchar_t*, const wchar_t*>> g) {
    Opt o{"_guide", l, kOptGuide, "", false, kDGuide, L""};
    o.guide = std::move(g);
    return o;
}
Opt Link(const wchar_t* l, int target, const wchar_t* h) {
    Opt o{"_link", l, kOptLink, "", false, kDGuide, h};
    o.target = target;
    return o;
}
Opt Needs(Opt o, const char* key, const char* val, const wchar_t* text) {
    o.needKey = key, o.needVal = val, o.needText = text;
    return o;
}
const std::vector<Page>& Pages() {
    static const wchar_t* const kSnapOnly = L"Used with Snap turning (Comfort page).";
    static const wchar_t* const kStickHelp =
        L"With Snap turning, in stages: push your off-hand stick left or right to turn by the Stick turn size. Pull it back (down) to face "
        L"where the game's camera is looking right now.\nDuring grabs, Zako Rush, menus and route choices the stick does its usual job instead.";
    static const std::vector<Page> pages = {
        {L"Comfort",
         {Pick("turning", L"Turning", "snap", false, kDTurn,
               L"Game: the game turns your view along its route, as on a monitor.\nSnap: your view stays level and only turns with a quick "
               L"black blink, using your off-hand stick. Boss moves become blinks too.",
               {{L"Game", "game"}, {L"Snap", "snap"}}),
          Slide("vignette", L"Tunnel vision", "0.6", false, kDVignette,
                L"The edges of your view darken while the camera moves you, stronger the faster it goes. Used everywhere the camera moves "
                L"you; never while you're playing a minigame's shooting or touch part.", 0, 1, 0.05f, L"%"),
          Toggle("jump_blink", L"Comfortable jumps", "1", false, kDJump,
                 L"Big drops go dark from take-off to landing, and small hops go in a straight line. Off shows the game's own arcs."),
          Toggle("remove_hit_shake", L"Remove hit shake", "1", false, kDShake,
                 L"Takes out the camera shake when you're hit and when a heroine's rockets explode, so your view stays steady."),
          Toggle("remove_walk_bob", L"Remove walking bob", "1", false, kDBob,
                 L"Takes out the camera's up-and-down bob as the route walks you along.")}},
        {L"Snap turning",
         {Info("_stick_snap", L"How to turn", kDStickTurn, kStickHelp),
          Needs(Pick("auto_snap", L"Turn me when a walk starts", "travel", true, kDAuto,
                     L"On: when a longer walk starts and you're facing well away from it, a blink turns you to face it.\nOff: only you turn.",
                     {{L"On", "travel"}, {L"Off", "off"}}), "turning", "snap", kSnapOnly),
          Needs(Slide("snap_angle", L"Stick turn size", "90", true, kDSnapAngle, L"How far one push of the stick turns you.", 15, 180, 15, L"°"),
                "turning", "snap", kSnapOnly),
          Needs(Slide("snap_fade_ms", L"Blink length", "90", true, kDFade,
                      L"How long the black fade out, and back in, takes. 0 turns you at once, without a fade.", 0, 300, 10, L" ms"),
                "turning", "snap", kSnapOnly),
          Needs(Toggle("grab_snap", L"Blink when grabbed", "1", true, kDGrab,
                       L"A girl grabbing or shoving you: on, a blink turns you to her (and back when you're free). Off, the game's own quick turn."),
                "turning", "snap", kSnapOnly)}},
        {L"Minigames",
         {Toggle("minigame_level", L"Keep the camera straight", "1", true, kDLevel,
                 L"You stay upright and the horizon stays level. You never have to look more than 25° up or down."),
          Toggle("minigame_fade", L"Fade violent camera moves", "1", true, kDMiniFade,
                 L"When the camera lurches between scenes (being knocked down, for example) the view fades out and back in, and the Sisters' back-and-forth swoop is taken out."),
          Slide("minigame_panel_scale", L"Panel size", "1.3", true, kDMiniPanel,
                L"How much bigger the panel is in minigames, so request icons further out on her body stay on it.", 1, 1.6f, 0.05f, L"%"),
          Slide("minigame_flick_gain", L"Swipe strength", "1.5", true, kDFlick,
                L"Swipe requests: your hand movement counts this many times over (the game expects a fast, long swipe).", 1, 3, 0.1f, L"×")}},
        {L"Panel and screens",
         {Slide("ui_distance", L"HUD panel distance", "1.6", false, kDHud, L"How far away the panel with the game's HUD and menus floats.", 0.8f, 3, 0.1f, L" m"),
          Slide("ui_width", L"HUD panel width", "1.8", false, kDHud, L"How wide the panel with the game's HUD and menus is.", 0.8f, 3.2f, 0.1f, L" m"),
          Slide("screen_distance", L"Flat screen distance", "2.0", false, kDScreen, L"How far away the flat screen for logos, story CGs and movies is.", 1, 4, 0.1f, L" m"),
          Slide("screen_width", L"Flat screen width", "2.6", false, kDScreen, L"How wide the flat screen for logos, story CGs and movies is.", 1.2f, 5, 0.1f, L" m"),
          Slide("ui_fullscreen_fov", L"Full-screen effects", "0.9", false, kDFullFov,
                L"How much of your view fades, flashes and the pink damage edge cover.", 0.5f, 1, 0.05f, L"%")}},
        {L"Sight and marks",
         {Slide("sight_center_scale", L"Sight centre size", "0.75", false, kDSight, L"The small spinning ring in the middle of your sight. Smaller makes far-away weak spots easier to aim at.", 0.3f, 1, 0.05f, L"%"),
          Slide("weak_spot_size", L"Weak-spot pop-ups", "1", false, kDWeak, L"The pop-ups when your sight is over a weak spot, compared to the girls.", 0.3f, 1.5f, 0.05f, L"%"),
          Toggle("screen_marks", L"Show screen marks", "1", false, kDMarks, L"The kiss marks and shoe prints that attacks leave on the screen."),
          Needs(Slide("kiss_mark_size", L"Kiss mark size", "1", false, kDKiss, L"The size of the kiss marks. 100% is the size the game draws them.", 0.2f, 1.5f, 0.05f, L"%"), "screen_marks", "1", L"Used with Show screen marks."),
          Needs(Slide("stomp_mark_size", L"Stomp mark size", "0.33", false, kDStomp, L"The size of the shoe prints when a girl stomps on you. 100% is the size the game draws them.", 0.15f, 1, 0.01f, L"%"), "screen_marks", "1", L"Used with Show screen marks."),
          Toggle("indicators", L"Off-screen arrows", "1", true, kDInd, L"A small pink arrow points to each girl out of view, and a faint blue ring points where the game is looking."),
          Needs(Slide("indicator_ring", L"Arrow distance", "26", true, kDIndRing, L"How far from the middle of your view the arrows sit.", 10, 45, 1, L"°"), "indicators", "1", L"Used with Off-screen arrows."),
          Needs(Slide("indicator_size", L"Arrow size", "2.6", true, kDIndRing, L"How big the arrows are.", 1, 6, 0.2f, L"°"), "indicators", "1", L"Used with Off-screen arrows.")}},
        {L"Controls",
         {Toggle("controllers", L"VR controllers", "1", false, kDControllers, L"Aim by pointing, with the buttons working the game. Off: play with a gamepad or the mouse and keyboard, still in VR."),
          Pick("dominant_hand", L"Aiming hand", "right", false, kDHand, L"The hand you aim and shoot with. Switching it also swaps the trigger and grip controls between your hands.", {{L"Left", "left"}, {L"Right", "right"}}),
          Toggle("haptics", L"Vibration", "1", false, kDHaptics, L"Controller vibration when you shoot, get hit, and click in menus."),
          Pick("aim_origin", L"Shots come from", "head", true, kDAimOrigin, L"Head: between your eyes toward where your controller points, so sight and shots line up (recommended).\nHand: straight out of the controller, like a laser pointer.", {{L"Head", "head"}, {L"Hand", "hand"}}),
          Slide("aim_smoothing", L"Aim steadying", "1.0", true, kDAimSmooth, L"Steadies the aim against small hand tremors, without slowing fast movements.", 0, 2, 0.1f, L"")}},
        {L"Button mapping",
         {Link(L"Full controls guide", kGuidePage, L"What every button does in every part of the game: stages, battles, Doki-Doki Mode, minigames, menus and more. It always shows your own buttons."),
          Info("_stick_snap2", L"Off-hand stick", kDStickTurn, kStickHelp),
          Info("_stick_main", L"Aiming-hand stick", kDStickOther,
               L"Scrolls long lists in menus, and turns the camera in the Collection, the Dressing Room and the story game over. You aim by pointing, so it isn't needed for aiming."),
          Bind("shoot", L"Shoot", "main.trigger", L"Shoots the girls; hold it to charge your shot. In menus it clicks what you point at."),
          Bind("zoom", L"Zoom", "main.grip", L"Look through obstacles. In minigames it also touches her."),
          Bind("sight_speed", L"Sight speed", "off.grip", L"The game's sight speed button. It has no effect when you aim with a controller."),
          Bind("special_requests", L"Touch", "off.trigger", L"The game's touch button: the minigames' hands-on part and the Collection's Fantasize."),
          Bind("doki_doki_mode", L"Doki-Doki Mode", "left.button_upper", L"Starts Doki-Doki Mode when at least one Heart Gauge is full."),
          Bind("mom_is_coming", L"Mom is coming!", "left.button_lower", L"The game's quick-hide screen."),
          Bind("confirm", L"Confirm", "right.button_lower", L"Confirm in menus."),
          Bind("cancel", L"Cancel", "right.button_upper", L"Cancel or go back in menus."),
          Bind("pause", L"Pause", "left.menu, right.menu", L"Pauses the game. Meta Quest controllers only have a menu button on the left."),
          Bind("r3", L"R3", "main.stick_click", L"The gamepad's right stick press, used by a few screens (the Collection's glasses)."),
          Bind("recenter", L"Re-center", "off.stick_click", L"Makes the direction you're facing \"straight ahead\" again."),
          Bind("hud_toggle", L"Hide or show the HUD", "right.button_upper", L"In stages and boss fights only, to see a girl up close. Elsewhere this button does its other job.")}},
        {L"Picture and sound",
         {Slide("render_scale", L"Image sharpness", "1.0", true, kDRender, L"Higher is sharper but slower, lower is faster but blurrier.", 0.6f, 1.6f, 0.05f, L"×"),
          Slide("world_scale", L"World size", "85.0", true, kDWorld, L"How big the world feels around you. Higher makes the world and the girls feel smaller.", 60, 120, 1, L""),
          Toggle("dynamic_shadows", L"Moving shadows", "0", false, kDShadow, L"The game's moving shadows. Off makes VR much smoother and is hard to notice."),
          Toggle("desktop_mirror", L"Show on PC monitor", "0", false, kDMirror, L"Also keeps showing the game on your monitor while you play in VR (costs some frame rate)."),
          Slide("frame_hold_slots", L"Steady frame rate", "0", true, kDFrameHold, L"Shows every frame for the same number of headset refreshes: a steady, lower frame rate if your PC can't keep up. Most people should leave this off.", 0, 6, 1, L""),
          Toggle("headset_audio", L"Sound through headset", "1", false, kDAudio, L"Plays the game's sound through your headset. Off uses your PC's normal sound output.")}},
        {L"Controls guide",
         {Link(L"\u2039 Back to Button mapping", kMapPage, L""),
          Guide(L"Everywhere", {{L"Pause", L"{pause}"}, {L"Re-center the view", L"{recenter}"}, {L"Turn (Snap turning, in stages)", L"Off-hand stick left / right"},
                                {L"Face where the game is looking", L"Pull the off-hand stick back (Snap turning, in stages)"}}),
          Guide(L"Stages and boss battles", {{L"Aim", L"Point your aiming hand"}, {L"Pheromone shot", L"{shoot} (hold to charge)"}, {L"Zoom, see through obstacles", L"{zoom} (hold)"},
                                             {L"Start Doki-Doki Mode", L"{doki_doki_mode} (needs a full Heart Gauge)"}, {L"Mom is coming!", L"{mom_is_coming}"}, {L"Choose your route at arrows", L"Off-hand stick"},
                                             {L"Hide or show the HUD", L"{hud_toggle}"}, {L"A boss's \"!\" markers", L"Shoot them like everything else"}}),
          Guide(L"Doki-Doki Mode", {{L"Choose a girl", L"Point at her marker, {shoot}"}, {L"Touch her gently", L"Tap {shoot}"}, {L"Rub", L"Hold {shoot}"},
                                    {L"Change the camera angle", L"{sight_speed} / {zoom}"}, {L"Euphoria", L"{doki_doki_mode}"}}),
          Guide(L"Final Doki-Doki Mode", {{L"Aim at a spot", L"Point your aiming hand"}, {L"Zoom in and shoot", L"Hold {zoom} to zoom in, let go to shoot"}, {L"Quick shot", L"{shoot}"}, {L"Euphoria (her gauge full)", L"{doki_doki_mode}"}}),
          Guide(L"Minigames", {{L"Hidden targets", L"Find them inside your sight's circle, {shoot}"}, {L"Special requests", L"Point at the icon on her body, then {zoom}: tap = poke, hold and move = rub, hold still = hold, hold and swipe = flick"},
                               {L"Hands-on commands", L"Hold {zoom}, {shoot} or {special_requests} and move your hand the way the game shows"}, {L"...or with the sticks", L"Circles and scratching with the aiming-hand stick; two-handed drags with either stick"},
                               {L"Looking around", L"The panel follows your gaze up and down"}}),
          Guide(L"Menus and story scenes", {{L"Click what you point at", L"{shoot}"}, {L"Confirm", L"{confirm}"}, {L"Back", L"{cancel}"}, {L"Move through a menu", L"Off-hand stick"}, {L"Scroll long lists", L"Aiming-hand stick"},
                                            {L"Switch tabs", L"{sight_speed} / {zoom}"}, {L"Story: next line", L"{shoot} or {confirm}"}}),
          Guide(L"Collection and Dressing Room", {{L"Turn the camera", L"Aiming-hand stick (also on the story game-over screen)"}}),
          Guide(L"Fantasize (Collection)", {{L"Move the camera", L"Off-hand stick"}, {L"Tilt", L"Aiming-hand stick"}, {L"Glasses", L"{r3}"}, {L"Pose", L"{confirm}"}, {L"Doki-Doki", L"{doki_doki_mode}"},
                                            {L"Zoom in and out", L"{sight_speed} / {zoom}"}, {L"Reset the zoom", L"{sight_speed} and {zoom} together"}, {L"Touch", L"{special_requests}"}, {L"Back", L"{cancel}"}})}},
    };
    return pages;
}

// ---- values ------------------------------------------------------------------------------------------------------
std::map<std::string, std::string> g_vals;
int g_page = 0, g_sel = 0, g_hoverRow = -1, g_hoverPage = -1, g_hoverLink = -1, g_drag = -1;
float g_listScroll = 0;
std::wstring g_sStatus;
bool g_sStatusBad = false, g_sDirty = true;
std::unique_ptr<Bitmap> g_sStatic;
ULONGLONG g_sT0 = 0;
float g_shotT = -1;  // --settings-shot: the demo time

std::wstring IniPath() { return Join(Join(g_game, L"Binaries\\Win64"), L"GGDPVR\\ggdpvr.ini"); }
std::string ReadOpt(const std::wstring& ini, const Opt& o) {  // as the mod finds it (src\core\config.cpp Sec)
    const std::wstring key = Widen(o.key);
    if (o.type == kOptBind) {
        wchar_t buf[128]{};
        GetPrivateProfileStringW(L"controls", key.c_str(), Widen(o.def).c_str(), buf, 128, ini.c_str());
        return Narrow(buf);
    }
    for (const wchar_t* sec : {L"advanced", L"vr", L"debug"}) {
        wchar_t buf[128]{};
        GetPrivateProfileStringW(sec, key.c_str(), L"\x01", buf, 128, ini.c_str());
        if (buf[0] != 1) return Narrow(buf);
    }
    return o.def;
}
void LoadSettings() {
    const std::wstring ini = IniPath();
    for (const Page& p : Pages())
        for (const Opt& o : p.opts)
            if (Editable(o)) g_vals[o.key] = ReadOpt(ini, o);
    g_sDirty = true;
}
float Num(const char* k) { return float(atof(g_vals[k].c_str())); }
bool Is(const char* k, const char* v) { return g_vals[k] == v; }
bool On(const char* k) { return Num(k) != 0.f; }
bool Usable(const Opt& o) { return !o.needKey || Is(o.needKey, o.needVal); }
std::string FormatNum(float v) {
    char b[32];
    sprintf_s(b, "%.2f", v);
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}
std::wstring ValueText(const Opt& o) {
    const float v = Num(o.key);
    if (!strcmp(o.key, "vignette") && v <= 0.f) return L"Off";
    if (!strcmp(o.key, "snap_fade_ms") && v <= 0.f) return L"No fade";
    if (!strcmp(o.key, "frame_hold_slots")) return v < 2.f ? L"Off" : L"1/" + std::to_wstring(int(v)) + L" rate";
    if (!wcscmp(o.unit, L"%")) return std::to_wstring(int(std::lround(v * 100.f))) + L"%";
    return Widen(FormatNum(v)) + o.unit;
}
void SaveOpt(const Opt& o) {
    const DWORD e = SetIniValue(IniPath(), o.key, g_vals[o.key], o.type == kOptBind ? "controls" : o.advanced ? "advanced" : "vr");
    g_sStatusBad = e != 0;
    g_sStatus = e == ERROR_ACCESS_DENIED ? L"Windows didn't allow changing the settings file. Run GGDPVR Setup as administrator and try again."
                : e ? L"Could not change the settings file (Windows error " + std::to_wstring(e) + L")."
                    : !GamePids().empty() ? L"Saved. Restart the game for this to apply."
                                          : L"Saved. Applies the next time you start the game.";
    g_sDirty = true;
}
void SetValue(const Opt& o, const std::string& v, bool save) {
    g_vals[o.key] = v;
    g_sDirty = true;
    if (save) SaveOpt(o);
}
void ResetPage() {
    if (!AskYesNo(g_wnd, std::wstring(L"Put everything on the ") + Pages()[size_t(g_page)].name + L" page back to its default?", false)) return;
    for (const Opt& o : Pages()[size_t(g_page)].opts)
        if (Editable(o)) SetValue(o, o.def, true);
}

// ---- button mapping: a [controls] line is "hand.button, hand.button" ------------------------------------------------
const char* const kHands[4] = {"main", "off", "left", "right"};
const wchar_t* const kHandNames[4] = {L"Aiming", L"Other", L"Left", L"Right"};
const char* const kBtns[6] = {"trigger", "grip", "button_lower", "button_upper", "stick_click", "menu"};
const wchar_t* const kBtnNames[6] = {L"Trigger", L"Grip", L"A / X", L"B / Y", L"Stick click", L"Menu"};
std::vector<std::string> BindTokens(std::string s) {
    for (char& c : s) c = c == ',' || c == ';' || c == '+' || c == '\t' ? ' ' : char(tolower(static_cast<unsigned char>(c)));
    std::vector<std::string> out;
    for (size_t a = 0; a < s.size();) {
        size_t b = s.find(' ', a);
        if (b == std::string::npos) b = s.size();
        if (b > a && s.find('.', a) < b) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}
std::string Physical(const std::string& token) {  // "main.trigger" -> "right.trigger" (dominant_hand)
    const size_t dot = token.find('.');
    std::string hand = token.substr(0, dot);
    const bool leftHanded = g_vals["dominant_hand"] == "left";
    if (hand == "main") hand = leftHanded ? "left" : "right";
    else if (hand == "off") hand = leftHanded ? "right" : "left";
    return hand + token.substr(dot);
}
std::wstring BindSummary(const std::string& v) {
    std::wstring out;
    for (const std::string& t : BindTokens(v)) {
        const size_t dot = t.find('.');
        std::wstring part;
        if (t == "left.button_upper" || t == "left.button_lower" || t == "right.button_upper" || t == "right.button_lower") {  // the letter says it all
            out += (out.empty() ? L"" : L" + ") + std::wstring(t == "left.button_upper" ? L"Y" : t == "left.button_lower" ? L"X" : t == "right.button_upper" ? L"B" : L"A");
            continue;
        }
        for (int h = 0; h < 4; ++h)
            if (t.compare(0, dot, kHands[h]) == 0) part = kHandNames[h];
        for (int b = 0; b < 6; ++b)
            if (t.compare(dot + 1, std::string::npos, kBtns[b]) == 0) part += std::wstring(L" ") + kBtnNames[b];
        out += (out.empty() ? L"" : L" + ") + part;
    }
    return out.empty() ? L"Not set" : out;
}
void ToggleBind(const Opt& o, const std::string& token) {
    auto tokens = BindTokens(g_vals[o.key]);
    const auto it = std::find(tokens.begin(), tokens.end(), token);
    if (it != tokens.end()) tokens.erase(it);
    else tokens.push_back(token);
    std::string v;
    for (const std::string& t : tokens) v += (v.empty() ? "" : ", ") + t;
    SetValue(o, v.empty() ? "none" : v, true);
}
std::wstring BindClash(const Opt& o) {  // the other actions on the same physical buttons
    std::wstring out;
    const auto mine = BindTokens(g_vals[o.key]);
    for (const Opt& other : Pages()[size_t(g_page)].opts) {
        if (other.type != kOptBind || !strcmp(other.key, o.key)) continue;
        bool shared = false;
        for (const std::string& a : mine)
            for (const std::string& b : BindTokens(g_vals[other.key])) shared |= Physical(a) == Physical(b);
        if (shared) out += (out.empty() ? L"" : L", ") + std::wstring(other.label);
    }
    return out;
}
const float kPickY = 392, kPickCol = 62, kPickRow = 24, kPickLabels = 96;
RectF PickCell(int hand, int btn) { return RectF(kCard.X + kPickLabels + hand * kPickCol, kPickY + 20 + btn * kPickRow, kPickCol, kPickRow); }
int g_hoverCell = -1;

// ---- demonstration drawing ------------------------------------------------------------------------------------------
float Clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
float Smooth(float a, float b, float x) {
    x = Clamp01((x - a) / (b - a));
    return x * x * (3.f - 2.f * x);
}
const float kDegF = 3.14159265f / 180.f;
void Disc(Graphics& g, float x, float y, float rad, Color c) {
    SolidBrush b(c);
    g.FillEllipse(&b, x - rad, y - rad, rad * 2, rad * 2);
}
void Line(Graphics& g, float x0, float y0, float x1, float y1, Color c, float w = 2.f) {
    Pen p(c, w);
    p.SetStartCap(LineCapRound), p.SetEndCap(LineCapRound);
    g.DrawLine(&p, x0, y0, x1, y1);
}
void Ring(Graphics& g, float x, float y, float rad, Color c, float w = 2.f) {
    Pen p(c, w);
    g.DrawEllipse(&p, x - rad, y - rad, rad * 2, rad * 2);
}
void Black(Graphics& g, const RectF& r, float a) {
    if (a <= 0.f) return;
    SolidBrush b(Color(BYTE(255 * Clamp01(a)), 0, 0, 0));
    g.FillRectangle(&b, r);
}
// the settings screen's pictures (res\: girl = Tsubomi Haruno, kiss and shoe prints)
std::unique_ptr<Bitmap> g_artGirl, g_artKiss, g_artShoe;
void LoadSettingsArt() {
    if (!g_artGirl) g_artGirl.reset(LoadPng(IDR_GIRL)), g_artKiss.reset(LoadPng(IDR_KISS)), g_artShoe.reset(LoadPng(IDR_SHOE));
}
void DrawArt(Graphics& g, Bitmap* b, const RectF& r) {
    if (!b) return;
    const InterpolationMode m = g.GetInterpolationMode();
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(b, r);
    g.SetInterpolationMode(m);
}
void Girl(Graphics& g, float x, float foot, float h) {  // her full height h, standing at (x, foot)
    if (!g_artGirl) return;
    const float w = h * float(g_artGirl->GetWidth()) / float(g_artGirl->GetHeight());
    DrawArt(g, g_artGirl.get(), RectF(x - w / 2, foot - h, w, h));
}
void Scene(Graphics& g, const RectF& r, float scroll = 0, float tilt = 0, float dx = 0, float dy = 0, float girl = 0.62f, float horizon = 0.5f) {
    GraphicsState st = g.Save();
    const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, m = r.Width;
    g.TranslateTransform(cx + dx, cy + dy), g.RotateTransform(tilt), g.TranslateTransform(-cx, -cy);
    const float hy = r.Y + r.Height * horizon;
    LinearGradientBrush sky(PointF(0, hy - m), PointF(0, hy + 1), Color(255, 120, 165, 235), Color(255, 255, 216, 190));
    g.FillRectangle(&sky, RectF(r.X - m, hy - m, r.Width + 2 * m, m + 1));
    LinearGradientBrush floor(PointF(0, hy), PointF(0, hy + r.Height), Color(255, 214, 196, 200), Color(255, 150, 130, 150));
    g.FillRectangle(&floor, RectF(r.X - m, hy, r.Width + 2 * m, m));
    for (int i = -7; i <= 7; ++i) Line(g, cx + i * 8.f, hy, cx + i * 90.f, hy + r.Height, Color(90, 255, 255, 255), 1.5f);
    for (int k = 0; k < 7; ++k) {
        const float z = float(std::fmod(k / 7.f + scroll, 1.f)), y = hy + r.Height * z * z * 0.7f;
        Line(g, r.X - m, y, r.X + r.Width + m, y, Color(90, 255, 255, 255), 1.5f);
    }
    if (girl > 0) Girl(g, cx, hy + r.Height * 0.3f, r.Height * girl);
    g.Restore(st);
}
void VignetteOver(Graphics& g, const RectF& r, float v) {  // the mod's shader in small: the aperture closes from 60 to 30 degrees
    if (v <= 0.001f) return;
    const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, R = std::hypot(r.Width, r.Height) * 0.56f;
    const float tanHalf = std::tan(56.f * kDegF), ap = 60.f - 30.f * v;
    const float inner = std::min(R * 0.98f, std::tan(ap * kDegF) / tanHalf * r.Width * 0.5f), outer = std::min(R * 0.995f, std::tan((ap + 12.f) * kDegF) / tanHalf * r.Width * 0.5f);
    GraphicsPath p;
    p.AddEllipse(cx - R, cy - R, R * 2, R * 2);
    PathGradientBrush b(&p);
    const BYTE a = BYTE(247.f * Clamp01(v * 1.5f));
    Color cols[4] = {Color(a, 0, 0, 0), Color(a, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0)};
    REAL pos[4] = {0.f, 1.f - outer / R, 1.f - inner / R, 1.f};
    b.SetInterpolationColors(cols, pos, 4);
    g.FillRectangle(&b, r);
}
void TopBg(Graphics& g, const RectF& r) {
    SolidBrush b(Color(255, 250, 240, 248));
    g.FillRectangle(&b, r);
    for (float x = r.X; x < r.GetRight(); x += 28) Line(g, x, r.Y, x, r.GetBottom(), Color(255, 240, 224, 236), 1.f);
    for (float y = r.Y; y < r.GetBottom(); y += 28) Line(g, r.X, y, r.GetRight(), y, Color(255, 240, 224, 236), 1.f);
}
void HeadTop(Graphics& g, float x, float y, float yaw, Color c = kPink, float cone = 56.f) {  // yaw clockwise from up
    SolidBrush b(Color(90, c.GetR(), c.GetG(), c.GetB()));
    g.FillPie(&b, RectF(x - cone, y - cone, cone * 2, cone * 2), yaw - 90 - 38, 76);
    Disc(g, x, y, 11, c);
    Disc(g, x + std::sin(yaw * kDegF) * 11, y - std::cos(yaw * kDegF) * 11, 3.5f, Color(255, 255, 255, 255));
}
void Arrow(Graphics& g, float x0, float y0, float x1, float y1, Color c, float w = 3.f) {
    Pen p(c, w);
    AdjustableArrowCap cap(4.f, 4.f);
    p.SetCustomEndCap(&cap);
    g.DrawLine(&p, x0, y0, x1, y1);
}
void Caption(Graphics& g, const RectF& r, const std::wstring& t, bool dark = true) {
    RectF box(r.X, r.GetBottom() - 30, r.Width, 30);
    SolidBrush b(dark ? Color(150, 40, 16, 40) : Color(200, 255, 255, 255));
    g.FillRectangle(&b, box);
    Text(g, t, box, 13, FontStyleBold, dark ? Color(255, 255, 255, 255) : kInk, StringAlignmentCenter, StringAlignmentCenter);
}
void Chevron(Graphics& g, float x, float y, float ang, float s, Color c) {  // pointing along ang (degrees, clockwise from right)
    const float a = ang * kDegF, ca = std::cos(a), sa = std::sin(a);
    auto P = [&](float u, float v) { return PointF(x + u * ca - v * sa, y + u * sa + v * ca); };
    const PointF p0 = P(-0.28f * s, -0.42f * s), p1 = P(0.32f * s, 0), p2 = P(-0.28f * s, 0.42f * s);
    Pen pen(c, std::max(2.f, s * 0.26f));
    pen.SetStartCap(LineCapRound), pen.SetEndCap(LineCapRound), pen.SetLineJoin(LineJoinRound);
    const PointF pts[3] = {p0, p1, p2};
    g.DrawLines(&pen, pts, 3);
}
void Print(Graphics& g, float x, float y, float s) {  // a shoe print, s = its height
    if (!g_artShoe) return;
    const float w = s * float(g_artShoe->GetWidth()) / float(g_artShoe->GetHeight());
    DrawArt(g, g_artShoe.get(), RectF(x - w / 2, y - s / 2, w, s));
}
void Kiss(Graphics& g, float x, float y, float s) {  // a kiss print, s = its width
    if (!g_artKiss) return;
    const float h = s * float(g_artKiss->GetHeight()) / float(g_artKiss->GetWidth());
    DrawArt(g, g_artKiss.get(), RectF(x - s / 2, y - h / 2, s, h));
}
void Controller(Graphics& g, float x, float y, Color c, float rot = 0) {
    GraphicsState st = g.Save();
    g.TranslateTransform(x, y), g.RotateTransform(rot);
    Ring(g, 0, -14, 15, c, 4.f);
    Line(g, 0, 0, 0, 34, c, 11.f);
    g.Restore(st);
}
void Flat(Graphics& g, const RectF& area, float dist, float width, float maxD, float maxW, const wchar_t* tag) {  // seen from above
    TopBg(g, area);
    const float hx = area.X + area.Width / 2, hy = area.GetBottom() - 34, unit = std::min((area.Height - 74) / maxD, area.Width * 0.92f / maxW);
    const float py = hy - dist * unit, half = width * unit * 0.5f;
    SolidBrush cone(Color(50, 232, 62, 140));
    const PointF tri[3] = {PointF(hx, hy), PointF(hx - half, py), PointF(hx + half, py)};
    g.FillPolygon(&cone, tri, 3);
    Line(g, hx - half, py, hx + half, py, Color(255, 122, 31, 92), 6.f);
    HeadTop(g, hx, hy, 0, kPink, 0.1f);
    Text(g, tag, RectF(area.X + 8, area.Y + 6, area.Width - 16, 20), 13, FontStyleBold, kSoft, StringAlignmentNear, StringAlignmentNear);
    Text(g, Widen(FormatNum(dist)) + L" m away, " + Widen(FormatNum(width)) + L" m wide", RectF(area.X + 8, area.Y + 24, area.Width - 16, 20), 13, FontStyleRegular, kInk,
         StringAlignmentNear, StringAlignmentNear);
}

std::wstring RenderSizeText();
// a controller's face, the buttons this action uses in pink
void ControllerFace(Graphics& g, float x, float y, bool left, const Opt& o) {
    const std::string side = left ? "left." : "right.";
    auto bound = [&](const char* b) {
        for (const std::string& t : BindTokens(g_vals[o.key]))
            if (Physical(t) == side + b) return true;
        return false;
    };
    auto col = [&](const char* b) { return bound(b) ? kPink : Color(255, 118, 96, 128); };
    const float out = left ? -1.f : 1.f;
    { SolidBrush b(col("trigger")); g.FillRectangle(&b, RectF(x - 16, y - 70, 32, 12)); }
    { SolidBrush b(col("grip")); g.FillRectangle(&b, RectF(out < 0 ? x - 43 : x + 36, y - 22, 7, 62)); }
    GraphicsPath body;
    RoundRect(body, RectF(x - 36, y - 60, 72, 118), 20);
    SolidBrush bodyB(Color(255, 58, 42, 70));
    g.FillPath(&bodyB, &body);
    const float sx = x - out * 12, sy = y - 32, bx = x + out * 14;
    Disc(g, sx, sy, 12, col("stick_click"));
    Disc(g, sx, sy, 6, Color(255, 40, 28, 50));
    Disc(g, bx, y - 6, 9, col("button_upper"));
    Disc(g, bx - out * 16, y + 12, 9, col("button_lower"));
    Text(g, left ? L"Y" : L"B", RectF(bx - 9, y - 15, 18, 18), 11, FontStyleBold, Color(255, 255, 255, 255), StringAlignmentCenter, StringAlignmentCenter);
    Text(g, left ? L"X" : L"A", RectF(bx - out * 16 - 9, y + 3, 18, 18), 11, FontStyleBold, Color(255, 255, 255, 255), StringAlignmentCenter, StringAlignmentCenter);
    if (left) Disc(g, x + 12, y + 38, 6, col("menu"));
    const bool aims = (g_vals["dominant_hand"] == "left") == left;
    Text(g, std::wstring(left ? L"Left" : L"Right") + (aims ? L" (aiming)" : L""), RectF(x - 70, y + 62, 140, 18), 12.5f, FontStyleBold, kSoft, StringAlignmentCenter, StringAlignmentNear);
}
void DrawDemo(Graphics& g, const RectF& r, const Opt& o, float t) {
    const float p5 = float(std::fmod(t, 5.f)), v = Editable(o) ? Num(o.key) : 0.f, cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    const bool on = v != 0.f;
    switch (o.demo) {
        case kDGuide: {
            TopBg(g, r);
            const wchar_t* const modes[6] = {L"Stages and boss battles", L"Doki-Doki Mode", L"Minigames", L"Menus and story scenes", L"Collection", L"Fantasize"};
            for (int i = 0; i < 6; ++i) {
                const RectF line(r.X + 40, r.Y + 18 + i * 32.f, r.Width - 80, 26);
                GraphicsPath p;
                RoundRect(p, line, 7);
                SolidBrush b(i % 2 ? Color(255, 255, 236, 245) : Color(255, 250, 214, 232));
                g.FillPath(&b, &p);
                Text(g, modes[i], line, 13, FontStyleBold, kInk, StringAlignmentCenter, StringAlignmentCenter);
            }
            break;
        }
        case kDBind:
            TopBg(g, r);
            ControllerFace(g, cx - 82, cy - 10, true, o);
            ControllerFace(g, cx + 82, cy - 10, false, o);
            Caption(g, r, BindSummary(g_vals[o.key]), false);
            break;
        case kDStickTurn: {
            TopBg(g, r);
            const float p6 = float(std::fmod(t, 6.f)), a = g_vals["snap_angle"].empty() ? 90.f : Num("snap_angle");
            const float px = r.X + 74, py = cy - 8, hx = r.GetRight() - 106, hy = cy - 4, game = -55.f;
            const int phase = p6 < 2.f ? 0 : p6 < 4.f ? 1 : 2;
            const float push = Smooth(0.35f, 0.5f, std::fmod(p6, 2.f)) * (1.f - Smooth(0.9f, 1.1f, std::fmod(p6, 2.f)));
            const float yaw = phase == 0 ? (p6 >= 0.5f ? a : 0.f) : phase == 1 ? (p6 >= 2.5f ? 0.f : a) : (p6 >= 4.5f ? game : 0.f);
            Disc(g, px, py, 42, Color(255, 232, 220, 232));
            Ring(g, px, py, 42, Color(255, 200, 176, 196), 2.f);
            const float kx = px + (phase == 0 ? 26.f : phase == 1 ? -26.f : 0.f) * push, ky = py + (phase == 2 ? 26.f : 0.f) * push;
            Disc(g, kx, ky, 18, Color(255, 58, 42, 70));
            Disc(g, kx, ky, 9, kPink);
            Arrow(g, hx, hy, hx + std::sin(game * kDegF) * 78, hy - std::cos(game * kDegF) * 78, Color(255, 120, 170, 240), 2.5f);
            HeadTop(g, hx, hy, yaw, kPink, 70);
            for (float b : {0.5f, 2.5f, 4.5f}) Black(g, RectF(r.X + r.Width * 0.45f, r.Y, r.Width * 0.55f, r.Height - 30), 1.f - std::fabs(p6 - b) / 0.1f);
            Caption(g, r, phase == 0 ? L"Push right: turn right" : phase == 1 ? L"Push left: turn left" : L"Pull back: face where the game is looking", false);
            break;
        }
        case kDStickOther: {
            TopBg(g, r);
            RectF list(cx - 110, r.Y + 22, 220, r.Height - 66);
            const float sc = float(std::fmod(t * 26.f, 34.f));
            GraphicsState st = g.Save();
            g.SetClip(list, CombineModeIntersect);
            for (int i = -1; i < 7; ++i) {
                SolidBrush b(i % 2 ? Color(255, 250, 214, 232) : Color(255, 255, 232, 243));
                g.FillRectangle(&b, RectF(list.X, list.Y + i * 34.f - sc, list.Width, 32));
            }
            g.Restore(st);
            Pen e(kEdge, 2.f);
            g.DrawRectangle(&e, list);
            Caption(g, r, L"Scrolls lists; turns some cameras", false);
            break;
        }
        case kDTurn: {
            TopBg(g, r);
            const bool snap = Is("turning", "snap");
            const float route = Smooth(1.f, 2.f, p5) * 100.f - Smooth(3.5f, 4.5f, p5) * 100.f;
            const float yaw = snap ? (p5 >= 2.1f && p5 < 4.6f ? 100.f : 0.f) : route;
            HeadTop(g, cx, cy + 6, yaw, kPink, 74);
            const float ra = route * kDegF;
            Arrow(g, cx, cy + 6, cx + std::sin(ra) * 92, cy + 6 - std::cos(ra) * 92, Color(255, 150, 130, 160), 2.5f);
            if (snap) Black(g, r, std::max(1.f - std::fabs(p5 - 2.1f) / 0.14f, 1.f - std::fabs(p5 - 4.6f) / 0.14f));
            Caption(g, r, snap ? L"You stay put; a quick blink turns you" : L"The game turns your view with the route");
            break;
        }
        case kDVignette: {
            const float moving = Smooth(0.3f, 1.f, p5) * (1.f - Smooth(3.2f, 4.2f, p5));
            Scene(g, r, float(std::fmod(t * 0.5f, 1.f)) * (moving > 0.05f ? 1.f : 0.f), 0, 0, 0, 0.5f);
            VignetteOver(g, r, v * moving);
            Caption(g, r, moving > 0.5f ? L"The camera moves you" : L"Still: no tunnel vision");
            break;
        }
        case kDJump: {
            const float air = Clamp01((p5 - 1.2f) / 1.4f), arc = std::sin(air * 3.14159f);
            const bool flying = p5 > 1.2f && p5 < 2.6f;
            if (on) {
                Scene(g, r);
                Black(g, r, std::min(Smooth(1.05f, 1.25f, p5), 1.f - Smooth(2.55f, 2.75f, p5)));
                Caption(g, r, flying ? L"Dark until you land" : L"Big drops are hidden");
            } else {
                Scene(g, r, 0, 0, 0, arc * 54.f);
                Caption(g, r, L"You see the game's arc");
            }
            break;
        }
        case kDShake: {
            const float hit = float(std::fmod(t, 2.5f)), amt = on ? 0.f : (1.f - Smooth(0.f, 0.7f, hit)) * 14.f;
            Scene(g, r, 0, 0, std::sin(t * 61.f) * amt, std::cos(t * 47.f) * amt);
            Caption(g, r, on ? L"A hit: your view stays steady" : L"A hit: the view shakes");
            break;
        }
        case kDBob:
            Scene(g, r, float(std::fmod(t * 0.45f, 1.f)), 0, 0, on ? 0.f : std::sin(t * 9.f) * 8.f);
            Caption(g, r, on ? L"Walking: smooth" : L"Walking: the view bobs");
            break;
        case kDAuto: {
            TopBg(g, r);
            const bool travel = Is("auto_snap", "travel");
            const float show = p5 > 1.f && p5 < 4.4f ? 1.f : 0.f, yaw = travel && p5 >= 1.6f && p5 < 4.4f ? 120.f : 0.f;
            HeadTop(g, cx, cy + 6, yaw, kPink, 74);
            if (show > 0) Arrow(g, cx, cy + 6, cx + std::sin(120.f * kDegF) * 94, cy + 6 - std::cos(120.f * kDegF) * 94, Color(255, 96, 150, 96), 3.f);
            if (travel) Black(g, r, std::max(1.f - std::fabs(p5 - 1.6f) / 0.14f, 1.f - std::fabs(p5 - 4.4f) / 0.14f));
            Caption(g, r, travel ? L"A walk starts: a blink turns you to it" : L"A walk starts: you stay facing where you were");
            break;
        }
        case kDSnapAngle: {
            TopBg(g, r);
            const float step = std::floor(t / 1.2f), yaw = step * v, ph = float(std::fmod(t, 1.2f));
            for (float a = 0; a < 360.f; a += std::max(1.f, v)) {
                const float ar = a * kDegF;
                Line(g, cx + std::sin(ar) * 80, cy + 4 - std::cos(ar) * 80, cx + std::sin(ar) * 90, cy + 4 - std::cos(ar) * 90, kSoft, 2.f);
            }
            HeadTop(g, cx, cy + 4, yaw, kPink, 74);
            Black(g, r, 1.f - std::min(ph, 1.2f - ph) / 0.1f);
            Caption(g, r, std::to_wstring(int(v)) + L"° for each push of the stick");
            break;
        }
        case kDFade: {
            Scene(g, r);
            const float ph = float(std::fmod(t, 2.2f)) * 1000.f, ms = v, amt = ms <= 0.f ? (ph < 40.f ? 1.f : 0.f) : 1.f - std::fabs(ph - ms) / ms;
            Black(g, r, amt);
            Caption(g, r, ms <= 0.f ? L"A cut: no fade" : L"Fades out and back in, " + std::to_wstring(int(ms)) + L" ms each");
            break;
        }
        case kDGrab: {
            TopBg(g, r);
            const float tx = cx + 86, ty = cy - 4, turn = Smooth(1.2f, 1.6f, p5) - Smooth(3.8f, 4.2f, p5);
            const float yaw = on ? (p5 >= 1.4f && p5 < 4.f ? 90.f : 0.f) : turn * 90.f;
            Disc(g, tx, ty, 12, Color(255, 170, 80, 150));
            HeadTop(g, cx - 18, cy - 4, yaw, kPink, 70);
            if (on) Black(g, r, std::max(1.f - std::fabs(p5 - 1.4f) / 0.14f, 1.f - std::fabs(p5 - 4.f) / 0.14f));
            Caption(g, r, on ? L"Grabbed: a blink turns you to her" : L"Grabbed: the game swings you round");
            break;
        }
        case kDLevel: {
            const float w = std::sin(t * 0.9f);
            Scene(g, r, 0, on ? 0.f : w * 22.f, 0, 0, 0.62f, on ? 0.5f : 0.5f + w * 0.16f);
            Caption(g, r, on ? L"The horizon stays level" : L"The camera tilts up, down and sideways");
            break;
        }
        case kDMiniFade: {
            const float lurch = Smooth(1.f, 1.2f, p5) * (1.f - Smooth(2.2f, 2.5f, p5));
            Scene(g, r, 0, 0, on ? 0.f : std::sin(t * 11.f) * 44.f * lurch, on ? 0.f : std::cos(t * 9.f) * 26.f * lurch);
            if (on) Black(g, r, lurch);
            Caption(g, r, lurch > 0.5f ? (on ? L"The camera lurches: dark until it settles" : L"The camera lurches") : L"Between scenes");
            break;
        }
        case kDMiniPanel: {
            Scene(g, r, 0, 0, 0, 0, 0.74f);
            const float w = r.Width * 0.46f * v, h = w * 9.f / 16.f;
            RectF pan(cx - w / 2, cy - h / 2 + 8, w, h);
            SolidBrush f(Color(40, 255, 255, 255));
            g.FillRectangle(&f, pan);
            Pen edge(Color(255, 255, 255, 255), 2.f);
            g.DrawRectangle(&edge, pan);
            Disc(g, cx - w * 0.36f, cy + 22, 7, kPink), Disc(g, cx + w * 0.38f, cy - 16, 7, kPink), Disc(g, cx + 8, cy + 8, 7, kPink);
            Caption(g, r, L"The icons on her body stay on the panel");
            break;
        }
        case kDFlick: {
            TopBg(g, r);
            const float ph = Smooth(0.f, 0.5f, float(std::fmod(t, 1.8f))), len = 34.f * v * ph;
            Controller(g, cx - 96 + 24 * ph, cy - 10, kInk, 8.f);
            Arrow(g, cx - 40, cy + 14, cx - 40 + 34.f, cy + 14, Color(255, 150, 130, 160), 3.f);
            Arrow(g, cx - 40, cy - 34, cx - 40 + std::max(4.f, len * 2.f), cy - 34, kPink, 5.f);
            Text(g, L"counts as", RectF(cx - 40, cy - 22, 120, 20), 12.5f, FontStyleRegular, kSoft, StringAlignmentNear, StringAlignmentNear);
            Caption(g, r, L"Your swipe, counted " + Widen(FormatNum(v)) + L" times over", false);
            break;
        }
        case kDHud: Flat(g, r, Num("ui_distance"), Num("ui_width"), 3.2f, 3.4f, L"HUD panel, from above"); break;
        case kDScreen: Flat(g, r, Num("screen_distance"), Num("screen_width"), 4.2f, 5.2f, L"Flat screen, from above"); break;
        case kDFullFov: {
            Scene(g, r);
            const float a = 0.5f + 0.5f * std::sin(t * 3.f), w = r.Width * v, h = r.Height * v;
            SolidBrush f(Color(BYTE(50 + 110 * a), 255, 150, 200));
            g.FillRectangle(&f, RectF(cx - w / 2, cy - h / 2, w, h));
            Caption(g, r, std::to_wstring(int(std::lround(v * 100))) + L"% of your view");
            break;
        }
        case kDSight: {
            Scene(g, r, 0, 0, 0, 0, 0.74f);
            const float a = t * 90.f;
            Ring(g, cx, cy, 36, Color(220, 255, 255, 255), 2.f);
            const float rs = 15.f * v;
            GraphicsState st = g.Save();
            g.TranslateTransform(cx, cy), g.RotateTransform(a);
            Pen p(Color(255, 255, 90, 170), 3.f);
            g.DrawArc(&p, -rs, -rs, rs * 2, rs * 2, 0, 120), g.DrawArc(&p, -rs, -rs, rs * 2, rs * 2, 180, 120);
            g.Restore(st);
            Caption(g, r, L"The spinning centre of your sight");
            break;
        }
        case kDWeak: {
            Scene(g, r, 0, 0, 0, 0, 0.74f);
            const float pop = 26.f * v * (1.f + 0.05f * std::sin(t * 7.f));
            Disc(g, cx, cy + 20, pop, Color(235, 255, 80, 160));
            Ring(g, cx, cy + 20, pop, Color(255, 255, 255, 255), 2.f);
            Text(g, L"!", RectF(cx - pop, cy + 20 - pop, pop * 2, pop * 2), pop * 1.25f, FontStyleBold, Color(255, 255, 255, 255), StringAlignmentCenter, StringAlignmentCenter);
            Caption(g, r, L"A weak-spot pop-up, compared to her");
            break;
        }
        case kDMarks:
            Scene(g, r, 0, 0, 0, 0, 0.62f);
            if (on) Kiss(g, cx - 92, cy - 34, 92), Print(g, cx + 96, cy - 8, 110);
            Caption(g, r, on ? L"Marks stay on your view for a moment" : L"No marks");
            break;
        case kDKiss:
            Scene(g, r, 0, 0, 0, 0, 0.62f);
            Kiss(g, cx, cy - 8, r.Width * 0.27f * v);
            Caption(g, r, std::to_wstring(int(std::lround(v * 100))) + L"% of the game's size");
            break;
        case kDStomp:
            Scene(g, r, 0, 0, 0, 0, 0.62f);
            Print(g, cx, cy - 6, r.Height * 1.1f * v);
            Caption(g, r, std::to_wstring(int(std::lround(v * 100))) + L"% of the game's size");
            break;
        case kDInd: {
            Scene(g, r, 0, 0, 0, 0, 0.5f);
            if (on) {
                const float a1 = 200.f + std::sin(t) * 14.f, ringR = 60.f;
                Chevron(g, cx + std::cos(a1 * kDegF) * ringR, cy + std::sin(a1 * kDegF) * ringR, a1, 18, Color(255, 255, 130, 190));
                const float a2 = -24.f + std::cos(t * 0.8f) * 10.f;
                Chevron(g, cx + std::cos(a2 * kDegF) * ringR, cy + std::sin(a2 * kDegF) * ringR, a2, 18, Color(255, 255, 130, 190));
                Ring(g, cx + 12, cy + 56, 8, Color(190, 140, 200, 255), 2.f);
            }
            Caption(g, r, on ? L"Arrows point to girls out of view" : L"No arrows");
            break;
        }
        case kDIndRing: {
            Scene(g, r, 0, 0, 0, 0, 0.5f);
            const float ringR = Num("indicator_ring") / 56.f * r.Width * 0.5f, s = Num("indicator_size") * 6.f;
            Ring(g, cx, cy, ringR, Color(90, 255, 255, 255), 1.f);
            for (float a : {200.f, -24.f, 96.f}) Chevron(g, cx + std::cos(a * kDegF) * ringR, cy + std::sin(a * kDegF) * ringR, a, s, Color(255, 255, 130, 190));
            Caption(g, r, L"Arrows around the middle of your view");
            break;
        }
        case kDControllers: {
            TopBg(g, r);
            if (on) {
                Controller(g, cx - 56, cy - 6, kInk, -6.f), Controller(g, cx + 56, cy - 6, kPink, 6.f);
                Arrow(g, cx + 58, cy - 24, cx + 92, cy - 76, kPink, 2.5f);
            } else {
                GraphicsPath pad;
                RoundRect(pad, RectF(cx - 62, cy - 34, 124, 62), 26);
                SolidBrush b(kInk);
                g.FillPath(&b, &pad);
                Disc(g, cx - 32, cy - 8, 9, Color(255, 240, 200, 225)), Disc(g, cx + 32, cy - 8, 9, Color(255, 240, 200, 225));
            }
            Caption(g, r, on ? L"Aim by pointing" : L"Gamepad or mouse and keyboard", false);
            break;
        }
        case kDHand: {
            Scene(g, r, 0, 0, 0, 0, 0.6f);
            const bool right = Is("dominant_hand", "right");
            const float hx = cx + (right ? 94.f : -94.f), wob = std::sin(t * 1.3f) * 8.f;
            Controller(g, hx, cy + 48, kPink, right ? -18.f : 18.f);
            Controller(g, cx + (right ? -94.f : 94.f), cy + 48, Color(170, 74, 30, 66), right ? 18.f : -18.f);
            Line(g, hx, cy + 34, cx + wob, cy - 8, Color(220, 255, 255, 255), 2.f);
            Ring(g, cx + wob, cy - 8, 8, Color(255, 255, 90, 170), 2.5f);
            Caption(g, r, right ? L"You aim with your right hand" : L"You aim with your left hand");
            break;
        }
        case kDHaptics: {
            TopBg(g, r);
            const float pulse = on ? (1.f - Smooth(0.f, 0.35f, float(std::fmod(t, 1.2f)))) : 0.f;
            Controller(g, cx + std::sin(t * 70.f) * 3.f * pulse, cy - 4, kInk);
            for (int i = 1; i <= 3; ++i) if (pulse > 0.f) Ring(g, cx, cy - 4, 40.f + i * 14.f * (1.f - pulse), Color(BYTE(170 * pulse), 232, 62, 140), 2.f);
            Caption(g, r, on ? L"It buzzes when you shoot or get hit" : L"No vibration", false);
            break;
        }
        case kDAimOrigin: {
            TopBg(g, r);
            const bool head = Is("aim_origin", "head");
            const float hx = r.X + 60, hy = cy - 42, hdx = r.X + 76, hdy = cy + 44, tx = r.GetRight() - 56, ty = cy - 30;
            Disc(g, hx, hy, 16, kPink);
            Controller(g, hdx, hdy, kInk, 75.f);
            Line(g, head ? hx : hdx, head ? hy : hdy - 8, tx, ty, kPink, 2.5f);
            Ring(g, tx, ty, 13, kInk, 2.5f), Disc(g, tx, ty, 3, kInk);
            if (!head) {
                Ring(g, tx, ty - 22, 13, Color(120, 128, 92, 120), 2.f);
                Line(g, hx, hy, tx, ty - 22, Color(90, 128, 92, 120), 1.5f);
            }
            Caption(g, r, head ? L"Sight and shots line up" : L"Shots can land a little off the sight", false);
            break;
        }
        case kDAimSmooth: {
            Scene(g, r, 0, 0, 0, 0, 0.74f);
            const float a = (1.f - v * 0.45f) * 7.f + 0.5f;
            const float x = cx + std::sin(t * 23.f) * a + std::sin(t * 41.f) * a * 0.6f + std::sin(t) * 22.f, y = cy + 6 + std::cos(t * 29.f) * a;
            Ring(g, x, y, 12, Color(255, 255, 90, 170), 3.f), Disc(g, x, y, 2.5f, Color(255, 255, 255, 255));
            Caption(g, r, v <= 0.f ? L"Every tremor shows" : L"Small tremors are steadied");
            break;
        }
        case kDRender: {
            const float res = std::min(1.f, std::max(0.2f, v * 0.62f));  // (blurrier below 100%, sharper above it)
            const int w = std::max(16, int(r.Width * res)), h = std::max(10, int(r.Height * res));
            Bitmap low(w, h, PixelFormat32bppPARGB);
            Graphics sg(&low);
            sg.SetSmoothingMode(SmoothingModeAntiAlias);
            sg.ScaleTransform(float(w) / r.Width, float(h) / r.Height);
            Scene(sg, RectF(0, 0, r.Width, r.Height), 0, 0, 0, 0, 0.74f);
            const InterpolationMode m = g.GetInterpolationMode();
            g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
            g.DrawImage(&low, r);
            g.SetInterpolationMode(m);
            const std::wstring size = RenderSizeText();
            Caption(g, r, !size.empty() ? size : v > 1.05f ? L"Sharper, slower" : v < 0.95f ? L"Softer, faster" : L"The headset's own resolution");
            break;
        }
        case kDWorld: {
            TopBg(g, r);
            const float floor = r.GetBottom() - 36, you = 130.f, girl = you * 0.96f * 85.f / v;
            Line(g, r.X, floor, r.GetRight(), floor, kSoft, 2.f);
            Disc(g, cx - 70, floor - you + 12, 12, kPink), Line(g, cx - 70, floor - you + 24, cx - 70, floor, kPink, 8.f);
            Girl(g, cx + 54, floor, std::min(190.f, girl));
            Caption(g, r, v > 90.f ? L"The world feels smaller" : v < 80.f ? L"The world feels bigger" : L"The world at its normal size", false);
            break;
        }
        case kDShadow: {
            TopBg(g, r);
            const float floor = r.GetBottom() - 44, x = cx + std::sin(t) * 50.f;
            if (on) {
                SolidBrush s(Color(90, 30, 20, 50));
                g.FillEllipse(&s, RectF(x - 44, floor - 8, 88, 18));
            }
            Girl(g, x, floor, 130);
            Caption(g, r, on ? L"Real moving shadows, at a cost" : L"Simple shadows: smoother VR", false);
            break;
        }
        case kDMirror: {
            TopBg(g, r);
            RectF scr(cx - 66, cy - 66, 132, 80);
            if (on) Scene(g, scr, 0, 0, 0, 0, 0.62f);
            else Black(g, scr, 0.85f);
            Pen edge(kInk, 4.f);
            g.DrawRectangle(&edge, scr);
            Line(g, cx, cy + 14, cx, cy + 30, kInk, 6.f), Line(g, cx - 28, cy + 32, cx + 28, cy + 32, kInk, 5.f);
            Caption(g, r, on ? L"Your monitor shows the game too" : L"Only the headset shows the game", false);
            break;
        }
        case kDFrameHold: {
            TopBg(g, r);
            const int hold = int(v) < 2 ? 0 : int(v), n = 14;
            const float x0 = r.X + 20, w = (r.Width - 40) / n, y = cy - 26;
            for (int i = 0; i < n; ++i) {
                const int group = hold ? i / hold : (i * 3 / 4 + i / 7);  // (off: an uneven pattern)
                const Color c = group % 2 ? Color(255, 232, 62, 140) : Color(255, 255, 170, 205);
                SolidBrush b(c);
                g.FillRectangle(&b, RectF(x0 + i * w + 1, y, w - 2, 46));
            }
            Text(g, L"headset refreshes, coloured by frame", RectF(r.X, y + 52, r.Width, 22), 12.5f, FontStyleRegular, kSoft, StringAlignmentCenter, StringAlignmentNear);
            Caption(g, r, hold ? L"Every frame shown " + std::to_wstring(hold) + L" times: steady" : L"Frames shown as soon as they're ready", false);
            break;
        }
        case kDAudio: {
            TopBg(g, r);
            const float x = on ? cx - 30 : cx + 40, y = cy - 8, ph = float(std::fmod(t, 0.9f)) / 0.9f;
            if (on) {
                GraphicsPath hmd;
                RoundRect(hmd, RectF(cx - 74, cy - 34, 88, 50), 16);
                SolidBrush b(kInk);
                g.FillPath(&b, &hmd);
            } else {
                GraphicsPath spk;
                RoundRect(spk, RectF(cx + 12, cy - 46, 56, 80), 10);
                SolidBrush b(kInk);
                g.FillPath(&b, &spk);
                Disc(g, cx + 40, cy + 8, 15, Color(255, 240, 200, 225));
            }
            for (int i = 0; i < 3; ++i) Ring(g, x, y, 40.f + (i + ph) * 22.f, Color(BYTE(170 - i * 50), 232, 62, 140), 2.f);
            Caption(g, r, on ? L"Sound comes from the headset" : L"Sound comes from your PC's output", false);
            break;
        }
    }
}

// ---- layout + drawing ------------------------------------------------------------------------------------------------
const std::vector<Opt>& CurOpts() { return Pages()[size_t(g_page)].opts; }
RectF RowRect(int i) { return RectF(kListArea.X, kListArea.Y + i * kRowH - g_listScroll, kListArea.Width, kRowH - 4); }
RectF ControlRect(const RectF& row) { return RectF(row.X + 196, row.Y + 8, 188, row.Height - 16); }
RectF PageRect(int i) { return RectF(18, 76 + i * 46.f, 172, 40); }
const RectF kBack(18, 18, 92, 36), kOpenFile(18, 540, 172, 30), kReset(18, 574, 172, 30);

// what the game renders per eye at the current Image sharpness (the mod saves this headset's size at 100%); L"" until it has
std::wstring RenderSizeText() {
    std::string s;
    int w = 0, h = 0;
    if (!ReadAll(Join(LocalDir(), L"render_size.txt"), s) || sscanf_s(s.c_str(), "%d %d", &w, &h) != 2 || w <= 0 || h <= 0) return L"";
    const float k = Num("render_scale");
    return std::to_wstring((int(float(w) * k) + 1) & ~1) + L" \u00D7 " + std::to_wstring((int(float(h) * k) + 1) & ~1) + L" per eye";
}
void DrawOptRow(Graphics& g, const Opt& o, int i) {
    const RectF row = RowRect(i), ctl = ControlRect(row);
    const bool sel = i == g_sel, hover = i == g_hoverRow, use = Usable(o);
    GraphicsPath bg;
    RoundRect(bg, row, 10);
    SolidBrush fill(sel ? Color(255, 255, 234, 245) : hover ? Color(240, 255, 245, 250) : Color(210, 255, 255, 255));
    g.FillPath(&fill, &bg);
    Pen edge(sel ? Color(255, 236, 110, 170) : kEdge, sel ? 2.f : 1.f);
    g.DrawPath(&edge, &bg);
    const Color ink = use ? kInk : Color(255, 190, 170, 184), pink = use ? kPink : Color(255, 216, 196, 208);
    Text(g, o.label, RectF(row.X + 14, row.Y, 178, row.Height), 14, FontStyleBold, ink, StringAlignmentNear, StringAlignmentCenter, false, true);
    if (o.type == kOptToggle) {
        const bool on = On(o.key);
        const RectF pill(ctl.GetRight() - 56, ctl.Y + (ctl.Height - 28) / 2, 56, 28);
        GraphicsPath p;
        RoundRect(p, pill, 14);
        SolidBrush b(on ? pink : Color(255, 224, 212, 222));
        g.FillPath(&b, &p);
        Disc(g, on ? pill.GetRight() - 14 : pill.X + 14, pill.Y + 14, 11, Color(255, 255, 255, 255));
        Text(g, on ? L"On" : L"Off", RectF(ctl.X, ctl.Y, ctl.Width - 66, ctl.Height), 13, FontStyleRegular, use ? kSoft : ink, StringAlignmentFar, StringAlignmentCenter);
    } else if (o.type == kOptChoice) {
        const float n = float(o.choices.size()), w = ctl.Width / n;
        GraphicsPath p;
        RoundRect(p, ctl, 10);
        SolidBrush b(Color(255, 245, 232, 241));
        g.FillPath(&b, &p);
        for (size_t c = 0; c < o.choices.size(); ++c) {
            const bool cur = g_vals[o.key] == o.choices[c].value;
            const RectF cell(ctl.X + c * w, ctl.Y, w, ctl.Height);
            if (cur) {
                GraphicsPath q;
                RectF in = cell;
                in.Inflate(-2, -2);
                RoundRect(q, in, 8);
                SolidBrush s(pink);
                g.FillPath(&s, &q);
            }
            Text(g, o.choices[c].label, cell, 13.5f, FontStyleBold, cur ? Color(255, 255, 255, 255) : ink, StringAlignmentCenter, StringAlignmentCenter);
        }
    } else if (o.type == kOptBind) {
        Text(g, BindSummary(g_vals[o.key]), ctl, 13, FontStyleBold, sel ? kPink : kInk, StringAlignmentFar, StringAlignmentCenter);
    } else if (o.type == kOptLink) {
        Text(g, o.target == kGuidePage ? L"Open \u203A" : L"", ctl, 13, FontStyleBold, kPink, StringAlignmentFar, StringAlignmentCenter);
    } else if (o.type == kOptGuide) {
        Text(g, L"\u203A", ctl, 15, FontStyleBold, kSoft, StringAlignmentFar, StringAlignmentCenter);
    } else if (o.type == kOptInfo) {
        Text(g, L"Always on", ctl, 13, FontStyleRegular, kSoft, StringAlignmentFar, StringAlignmentCenter);
    } else {
        const float x0 = ctl.X + 2, x1 = ctl.GetRight() - 66, y = row.Y + row.Height / 2, k = Clamp01((Num(o.key) - o.lo) / (o.hi - o.lo));
        Line(g, x0, y, x1, y, Color(255, 232, 216, 228), 6.f);
        Line(g, x0, y, x0 + (x1 - x0) * k, y, pink, 6.f);
        Disc(g, x0 + (x1 - x0) * k, y, 9.f, Color(255, 255, 255, 255));
        Ring(g, x0 + (x1 - x0) * k, y, 9.f, pink, 2.5f);
        Text(g, ValueText(o), RectF(x1 + 8, ctl.Y, 58, ctl.Height), 13, FontStyleBold, ink, StringAlignmentFar, StringAlignmentCenter);
    }
}
void DrawSettingsStatic(Graphics& g) {
    LinearGradientBrush bg(PointF(0, 0), PointF(0, kSH), Color(255, 255, 228, 242), Color(255, 236, 222, 255));
    g.FillRectangle(&bg, RectF(0, 0, kSW + 2, kSH + 2));
    // header
    {
        GraphicsPath p;
        RoundRect(p, kBack, 10);
        SolidBrush f(g_hoverLink == 0 ? Color(255, 255, 238, 247) : Color(235, 255, 255, 255));
        g.FillPath(&f, &p);
        Pen e(Color(255, 240, 139, 190), 1.5f);
        g.DrawPath(&e, &p);
        Text(g, L"‹ Back", kBack, 14, FontStyleBold, Color(255, 122, 31, 92), StringAlignmentCenter, StringAlignmentCenter);
    }
    Text(g, L"Settings", RectF(126, 14, 300, 42), 26, FontStyleBold, kInk, StringAlignmentNear, StringAlignmentCenter);
    Text(g, L"Changes apply the next time the game starts.", RectF(kCard.X, 14, kCard.Width, 42), 13, FontStyleRegular, kSoft, StringAlignmentFar, StringAlignmentCenter);
    // pages
    const auto& pages = Pages();
    for (int i = 0; i < kGuidePage; ++i) {
        const RectF r = PageRect(i);
        const bool cur = i == (g_page == kGuidePage ? kMapPage : g_page);
        GraphicsPath p;
        RoundRect(p, r, 10);
        SolidBrush f(cur ? Color(255, 232, 62, 140) : i == g_hoverPage ? Color(255, 255, 238, 247) : Color(190, 255, 255, 255));
        g.FillPath(&f, &p);
        Text(g, pages[size_t(i)].name, RectF(r.X + 14, r.Y, r.Width - 18, r.Height), 14.5f, FontStyleBold, cur ? Color(255, 255, 255, 255) : kInk, StringAlignmentNear, StringAlignmentCenter);
    }
    Text(g, L"Open settings file", kOpenFile, 13.5f, g_hoverLink == 1 ? FontStyleUnderline : FontStyleRegular, Color(255, 140, 60, 115), StringAlignmentCenter, StringAlignmentCenter);
    Text(g, L"Reset this page", kReset, 13.5f, g_hoverLink == 2 ? FontStyleUnderline : FontStyleRegular, Color(255, 140, 60, 115), StringAlignmentCenter, StringAlignmentCenter);
    // options
    {
        GraphicsState st = g.Save();
        g.SetClip(kListArea);
        const auto& opts = CurOpts();
        for (int i = 0; i < int(opts.size()); ++i) DrawOptRow(g, opts[size_t(i)], i);
        g.Restore(st);
    }
    // what the selected option does
    const Opt& o = CurOpts()[size_t(g_sel)];
    if (o.type == kOptGuide) {  // a mode's controls: the whole column, with the player's own buttons
        Text(g, o.label, RectF(kCard.X, kCard.Y, kCard.Width, 30), 20, FontStyleBold, kInk, StringAlignmentNear, StringAlignmentNear);
        FontFamily ff(L"Segoe UI");
        Font f(&ff, 13, FontStyleRegular, UnitPixel), fb(&ff, 13, FontStyleBold, UnitPixel);
        float y = kCard.Y + 40;
        for (const auto& line : o.guide) {
            std::wstring how = line.second;
            for (size_t a; (a = how.find(L'{')) != std::wstring::npos;) {
                const size_t b = how.find(L'}', a);
                how.replace(a, b - a + 1, BindSummary(g_vals[Narrow(how.substr(a + 1, b - a - 1))]));
            }
            const float wa = 146, wh = kCard.Width - wa - 8;
            RectF ma, mh;
            g.MeasureString(line.first, -1, &fb, RectF(0, 0, wa, 400), &ma);
            g.MeasureString(how.c_str(), -1, &f, RectF(0, 0, wh, 400), &mh);
            const float h = std::max(ma.Height, mh.Height) + 8;
            Text(g, line.first, RectF(kCard.X, y, wa, h), 13, FontStyleBold, kInk, StringAlignmentNear, StringAlignmentNear, false, true);
            Text(g, how, RectF(kCard.X + wa + 8, y, wh, h), 13, FontStyleRegular, kInk, StringAlignmentNear, StringAlignmentNear, false, true);
            y += h;
            Line(g, kCard.X, y - 3, kCard.GetRight(), y - 3, Color(255, 243, 214, 230), 1.f);
        }
        Text(g, g_sStatus, RectF(204, 618, 756, 26), 13, FontStyleRegular, g_sStatusBad ? Color(255, 196, 40, 70) : kSoft, StringAlignmentNear, StringAlignmentCenter);
        return;
    }
    Text(g, o.label, RectF(kCard.X, kCard.GetBottom() + 12, kCard.Width, 28), 19, FontStyleBold, kInk, StringAlignmentNear, StringAlignmentNear);
    const std::wstring def = o.type == kOptBind ? BindSummary(o.def) : !Editable(o) ? L"" : o.type == kOptToggle ? (atof(o.def) != 0 ? L"On" : L"Off") : o.type == kOptChoice ? [&] {
        for (const Choice& c : o.choices)
            if (!strcmp(c.value, o.def)) return std::wstring(c.label);
        return std::wstring();
    }() : [&] {
        const std::string keep = g_vals[o.key];
        g_vals[o.key] = o.def;
        const std::wstring t = ValueText(o);
        g_vals[o.key] = keep;
        return t;
    }();
    Text(g, o.help, RectF(kCard.X, kCard.GetBottom() + 44, kCard.Width, o.type == kOptBind ? 40.f : 186.f), o.type == kOptBind ? 13.f : 14.f, FontStyleRegular, kInk, StringAlignmentNear, StringAlignmentNear, false, true);
    if (o.type == kOptBind) {
        for (int h = 0; h < 4; ++h)
            Text(g, kHandNames[h], RectF(kCard.X + kPickLabels + h * kPickCol, kPickY, kPickCol, 20), 12, FontStyleBold, kSoft, StringAlignmentCenter, StringAlignmentCenter);
        const auto tokens = BindTokens(g_vals[o.key]);
        for (int b = 0; b < 6; ++b) {
            Text(g, kBtnNames[b], RectF(kCard.X, kPickY + 20 + b * kPickRow, kPickLabels, kPickRow), 13, FontStyleRegular, kInk, StringAlignmentNear, StringAlignmentCenter);
            for (int h = 0; h < 4; ++h) {
                const RectF c = PickCell(h, b);
                const bool on = std::find(tokens.begin(), tokens.end(), std::string(kHands[h]) + "." + kBtns[b]) != tokens.end();
                if (g_hoverCell == h * 6 + b) {
                    SolidBrush f(Color(255, 255, 228, 241));
                    g.FillRectangle(&f, c);
                }
                Disc(g, c.X + c.Width / 2, c.Y + c.Height / 2, 8, on ? kPink : Color(255, 255, 255, 255));
                Ring(g, c.X + c.Width / 2, c.Y + c.Height / 2, 8, on ? kPink : kEdge, 1.5f);
            }
        }
        const std::wstring clash = BindClash(o);
        if (!clash.empty()) Text(g, L"This button also does: " + clash, RectF(kCard.X, 578, kCard.Width, 38), 13, FontStyleBold, Color(255, 176, 96, 40), StringAlignmentNear, StringAlignmentNear, false, true);
    }
    if (Editable(o)) Text(g, L"Default: " + def, RectF(kCard.X, 556, kCard.Width, 22), 13, FontStyleRegular, kSoft, StringAlignmentNear, StringAlignmentNear);
    if (!strcmp(o.key, "render_scale")) {
        const std::wstring size = RenderSizeText();
        Text(g, size.empty() ? L"Start the game once in VR to see the resolution here." : L"Rendering at " + size, RectF(kCard.X, 578, kCard.Width, 38), 13,
             FontStyleBold, kInk, StringAlignmentNear, StringAlignmentNear, false, true);
    }
    if (!Usable(o)) Text(g, o.needText, RectF(kCard.X, 578, kCard.Width, 22), 13, FontStyleBold, Color(255, 196, 90, 40), StringAlignmentNear, StringAlignmentNear);
    Text(g, g_sStatus, RectF(204, 618, 756, 26), 13, FontStyleRegular, g_sStatusBad ? Color(255, 196, 40, 70) : kSoft, StringAlignmentNear, StringAlignmentCenter);
}
void DrawCard(Graphics& g, float t) {
    if (CurOpts()[size_t(g_sel)].type == kOptGuide) return;  // (its text fills the column)
    GraphicsPath p;
    RoundRect(p, kCard, 14);
    g.SetClip(&p);
    DrawDemo(g, kCard, CurOpts()[size_t(g_sel)], t);
    g.ResetClip();
    Pen e(kEdge, 2.f);
    g.DrawPath(&e, &p);
}
void Quality(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
}
float DemoTime() { return g_shotT >= 0.f ? g_shotT : float(GetTickCount64() - g_sT0) / 1000.f; }
void PaintSettings(HDC hdc, const RECT& dirty) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    if (g_sDirty || !g_sStatic || int(g_sStatic->GetWidth()) != w || int(g_sStatic->GetHeight()) != h) {
        g_sStatic = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        Graphics g(g_sStatic.get());
        Quality(g);
        g.ScaleTransform(g_scale, g_scale);
        DrawSettingsStatic(g);
        g_sDirty = false;
    }
    Graphics screen(hdc);
    const int cx = int(kCard.X * g_scale), cy = int(kCard.Y * g_scale), cw = int(kCard.Width * g_scale) + 3, ch = int(kCard.Height * g_scale) + 3;
    if (dirty.left < cx || dirty.top < cy || dirty.right > cx + cw || dirty.bottom > cy + ch) screen.DrawImage(g_sStatic.get(), 0, 0, w, h);
    Bitmap card(cw, ch, PixelFormat32bppPARGB);
    Graphics g(&card);
    Quality(g);
    g.TranslateTransform(-float(cx), -float(cy));
    g.DrawImage(g_sStatic.get(), 0, 0, w, h);  // (the page behind the card's rounded corners)
    g.ScaleTransform(g_scale, g_scale);
    DrawCard(g, DemoTime());
    screen.DrawImage(&card, cx, cy, cw, ch);
}

// ---- input --------------------------------------------------------------------------------------------------------
void InvalidateAll() { InvalidateRect(g_wnd, nullptr, FALSE); }
float MaxScroll() { return std::max(0.f, CurOpts().size() * kRowH - kListArea.Height); }
void ClampScroll() { g_listScroll = std::min(MaxScroll(), std::max(0.f, g_listScroll)); }
void Select(int i) {
    if (i == g_sel) return;
    g_sel = i;
    g_sT0 = GetTickCount64();  // the demonstration starts from its beginning
    g_sDirty = true;
}
void ShowRow(int i) {  // keyboard: the selected row in view
    const float top = i * kRowH, bottom = top + kRowH;
    if (top < g_listScroll) g_listScroll = top;
    else if (bottom > g_listScroll + kListArea.Height) g_listScroll = bottom - kListArea.Height;
    ClampScroll();
    g_sDirty = true;
}
void SliderTo(const Opt& o, const RectF& row, float x, bool save) {
    const RectF ctl = ControlRect(row);
    const float x0 = ctl.X + 2, x1 = ctl.GetRight() - 66, k = Clamp01((x - x0) / (x1 - x0));
    const float v = o.lo + std::round((o.hi - o.lo) * k / o.step) * o.step;
    SetValue(o, FormatNum(std::min(o.hi, std::max(o.lo, v))), save);
}
void Nudge(const Opt& o, int dir) {
    if (!Usable(o) || !Editable(o) || o.type == kOptBind) return;
    if (o.type == kOptToggle) return SetValue(o, On(o.key) ? "0" : "1", true);
    if (o.type == kOptChoice) {
        int cur = 0;
        for (size_t c = 0; c < o.choices.size(); ++c)
            if (g_vals[o.key] == o.choices[c].value) cur = int(c);
        const int n = int(o.choices.size());
        return SetValue(o, o.choices[size_t((cur + dir + n) % n)].value, true);
    }
    SetValue(o, FormatNum(std::min(o.hi, std::max(o.lo, Num(o.key) + dir * o.step))), true);
}
void CloseSettings() {
    KillTimer(g_wnd, 1);
    g_settings = false;
    g_drag = -1;
    g_sStatic.reset();
    SizeWindow(g_wnd);
    Refresh();
}
void ReleaseSettingsArt() { g_artGirl.reset(), g_artKiss.reset(), g_artShoe.reset(), g_sStatic.reset(); }
void OpenSettings() {
    LoadSettingsArt();
    LoadSettings();
    g_settings = true, g_page = 0, g_sel = 0, g_listScroll = 0, g_sStatus.clear(), g_sStatusBad = false, g_sT0 = GetTickCount64();
    SizeWindow(g_wnd);
    SetTimer(g_wnd, 1, 33, nullptr);
    InvalidateAll();
}
bool SettingsMessage(UINT msg, WPARAM wp, LPARAM lp) {
    const float mx = float(short(LOWORD(lp))) / g_scale, my = float(short(HIWORD(lp))) / g_scale;
    auto rowAt = [&](float x, float y) {
        if (!kListArea.Contains(x, y)) return -1;
        const int i = int((y - kListArea.Y + g_listScroll) / kRowH);
        return i < int(CurOpts().size()) ? i : -1;
    };
    switch (msg) {
        case WM_TIMER: {
            RECT r{LONG(kCard.X * g_scale), LONG(kCard.Y * g_scale), LONG(kCard.GetRight() * g_scale) + 3, LONG(kCard.GetBottom() * g_scale) + 3};
            InvalidateRect(g_wnd, &r, FALSE);
            return true;
        }
        case WM_MOUSEMOVE: {
            if (g_drag >= 0) {
                SliderTo(CurOpts()[size_t(g_drag)], RowRect(g_drag), mx, false);
                InvalidateAll();
                return true;
            }
            int page = -1, link = kBack.Contains(mx, my) ? 0 : kOpenFile.Contains(mx, my) ? 1 : kReset.Contains(mx, my) ? 2 : -1;
            for (int i = 0; i < kGuidePage; ++i)
                if (PageRect(i).Contains(mx, my)) page = i;
            const int row = rowAt(mx, my);
            int cell = -1;
            if (CurOpts()[size_t(g_sel)].type == kOptBind)
                for (int h = 0; h < 4; ++h)
                    for (int b = 0; b < 6; ++b)
                        if (PickCell(h, b).Contains(mx, my)) cell = h * 6 + b;
            if (page != g_hoverPage || link != g_hoverLink || row != g_hoverRow || cell != g_hoverCell) {
                g_hoverPage = page, g_hoverLink = link, g_hoverRow = row, g_hoverCell = cell, g_sDirty = true;
                if (row >= 0 && CurOpts()[size_t(row)].type != kOptBind) Select(row);  // (a mapping row needs a click: the mouse crosses others on its way to the picker)
                InvalidateAll();
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, g_wnd, 0};
                TrackMouseEvent(&t);
            }
            return true;
        }
        case WM_MOUSELEAVE:
            g_hoverPage = g_hoverLink = g_hoverRow = g_hoverCell = -1, g_sDirty = true;
            InvalidateAll();
            return true;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (g_hoverPage >= 0 || g_hoverLink >= 0 || g_hoverRow >= 0 || g_hoverCell >= 0)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return true;
            }
            return false;
        case WM_MOUSEWHEEL:
            g_listScroll -= float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * kRowH;
            ClampScroll();
            g_sDirty = true;
            InvalidateAll();
            return true;
        case WM_LBUTTONDOWN: {
            SetCapture(g_wnd);
            if (kBack.Contains(mx, my)) {
                ReleaseCapture();
                CloseSettings();
                return true;
            }
            if (kOpenFile.Contains(mx, my)) {
                ReleaseCapture();
                const std::wstring arg = L"\"" + IniPath() + L"\"";
                ShellExecuteW(g_wnd, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                return true;
            }
            if (kReset.Contains(mx, my)) {
                ReleaseCapture();
                ResetPage();
                InvalidateAll();
                return true;
            }
            for (int i = 0; i < kGuidePage; ++i)
                if (PageRect(i).Contains(mx, my)) {
                    g_page = i, g_sel = 0, g_listScroll = 0, g_sT0 = GetTickCount64(), g_sDirty = true;
                    InvalidateAll();
                    return true;
                }
            if (CurOpts()[size_t(g_sel)].type == kOptBind)
                for (int h = 0; h < 4; ++h)
                    for (int b = 0; b < 6; ++b)
                        if (PickCell(h, b).Contains(mx, my)) {
                            ToggleBind(CurOpts()[size_t(g_sel)], std::string(kHands[h]) + "." + kBtns[b]);
                            InvalidateAll();
                            return true;
                        }
            const int row = rowAt(mx, my);
            if (row >= 0) {
                Select(row);
                const Opt& o = CurOpts()[size_t(row)];
                if (o.type == kOptLink) {
                    g_page = o.target, g_sel = o.target == kGuidePage ? 1 : 0, g_listScroll = 0, g_sT0 = GetTickCount64(), g_sDirty = true;
                    InvalidateAll();
                    return true;
                }
                const RectF rr = RowRect(row), ctl = ControlRect(rr);
                if (Usable(o) && ctl.Contains(mx, my)) {
                    if (o.type == kOptToggle) SetValue(o, On(o.key) ? "0" : "1", true);
                    else if (o.type == kOptChoice) SetValue(o, o.choices[std::min(o.choices.size() - 1, size_t((mx - ctl.X) / (ctl.Width / float(o.choices.size()))))].value, true);
                    else if (o.type == kOptSlider) g_drag = row, SliderTo(o, rr, mx, false);
                }
                InvalidateAll();
            }
            return true;
        }
        case WM_LBUTTONUP:
            ReleaseCapture();
            if (g_drag >= 0) {
                SaveOpt(CurOpts()[size_t(g_drag)]);
                g_drag = -1;
                InvalidateAll();
            }
            return true;
        case WM_KEYDOWN: {
            const int n = int(CurOpts().size());
            if (wp == VK_ESCAPE) CloseSettings();
            else if (wp == VK_UP) Select((g_sel + n - 1) % n), ShowRow(g_sel), InvalidateAll();
            else if (wp == VK_DOWN) Select((g_sel + 1) % n), ShowRow(g_sel), InvalidateAll();
            else if (wp == VK_LEFT) Nudge(CurOpts()[size_t(g_sel)], -1), InvalidateAll();
            else if (wp == VK_RIGHT || wp == VK_SPACE || wp == VK_RETURN) Nudge(CurOpts()[size_t(g_sel)], 1), InvalidateAll();
            else if (wp == VK_TAB) {
                const int pc = kGuidePage;
                g_page = (std::min(g_page, kMapPage + 1) + (GetKeyState(VK_SHIFT) < 0 ? pc - 1 : 1)) % pc, g_sel = 0, g_listScroll = 0, g_sT0 = GetTickCount64(), g_sDirty = true;
                InvalidateAll();
            } else return false;
            return true;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && g_drag < 0) LoadSettings(), InvalidateAll();  // the file may have been edited meanwhile
            return false;
        default:
            return false;
    }
}

// dev (--settings-shot): every page, and each option's demonstration at a moment showing it, without a window or any change
void SettingsShots(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    CLSID png;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    LoadSettingsArt();
    LoadSettings();
    g_scale = 1.f;
    int index = 0;
    for (int page = 0; page < int(Pages().size()); ++page) {
        g_page = page;
        for (g_sel = 0; g_sel < int(CurOpts().size()); ++g_sel) {
            Bitmap b(int(kSW), int(kSH), PixelFormat32bppPARGB);
            Graphics g(&b);
            Quality(g);
            g_listScroll = std::min(MaxScroll(), std::max(0.f, g_sel * kRowH - kListArea.Height + kRowH));
            DrawSettingsStatic(g);
            DrawCard(g, 1.45f);
            const std::wstring name = Join(dir, std::to_wstring(page) + L"_" + std::to_wstring(g_sel) + L"_" + Widen(CurOpts()[size_t(g_sel)].key) + L".png");
            b.Save(name.c_str(), &png, nullptr);
            ++index;
        }
    }
}
