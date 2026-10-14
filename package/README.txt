GGDPVR - play Gal*Gun: Double Peace in VR
==========================================
Version {VERSION}

GGDPVR is a mod for the PC version of Gal*Gun: Double Peace. It shows the game in your VR headset in full 3D:
you look around with your head and aim by pointing a VR controller.


WHAT YOU NEED
-------------
- Gal*Gun: Double Peace for Windows. GGDPVR is made and tested with the Steam version; another PC copy of the same
  game version should work too.
- A PC VR headset and the software that connects it to your PC, for example Meta Quest Link / Air Link, SteamVR,
  Virtual Desktop, Windows Mixed Reality or Pimax Play. Any headset that works with OpenXR games works.
- Nothing else to install. You don't need to change the game's launch options either: the mod starts the game in
  DirectX 11 mode by itself.


INSTALL - THE EASY WAY
----------------------
1. Unpack this zip anywhere (for example your Downloads folder).
2. Run GGDPVR-Setup.exe.
   It finds a Steam copy of the game by itself. Not from Steam, or installed somewhere unusual? Click "Change..."
   and choose your game folder - the one with the Binaries folder in it.
   If Windows shows a "Windows protected your PC" warning: click "More info", then "Run anyway".
3. Click Install.
4. Start your VR software, then start the game (Steam, or the setup's "Start game" button).

GGDPVR-Setup.exe is also how you repair (for example if a file went missing), switch the mod off and on, open your
settings and logs, and uninstall. It always keeps your settings.


UPDATING
--------
GGDPVR never checks for updates by itself - it has no network code. When a new version comes out, download its zip
from the page you got this one from and run the new GGDPVR-Setup.exe: it updates the installed mod and keeps your
settings.


INSTALL - BY HAND
-----------------
1. Open the game folder. Steam: right-click Gal*Gun: Double Peace > Manage > Browse local files. Other copies:
   the folder you installed the game to (it has a Binaries folder in it).
2. Copy the "Binaries" folder from this zip into that game folder. Windows asks whether to merge the folders: yes.
   That adds exactly these files (no game file is replaced):
       Binaries\Win64\dinput8.dll          the loader that starts the mod with the game
       Binaries\Win64\GGDPVR\GGDPVR.dll    the mod
       Binaries\Win64\GGDPVR\ggdpvr.ini    your settings
   When updating by hand, don't copy ggdpvr.ini over your own, or your settings go back to the defaults.
3. Start your VR software, then start the game from Steam as usual.


CONTROLS
--------
What each button does in each part of the game. These are the defaults for a right-handed player on Meta Quest
controllers. Left-handed (dominant_hand=left): the triggers, grips and thumbsticks swap hands; A, B, X and Y stay
where they are. Every button can be changed in the settings file's [controls] section (see SETTINGS).

Everywhere
  Pause .......................... left menu button
  Re-center the view ............. press the left thumbstick down (your headset's own re-center works too)

Stages and boss battles
  Aim ............................ point your right controller
  Pheromone shot ................. right trigger; hold it to charge a stronger shot
  Zoom (see through obstacles) ... right grip, hold
  Start Doki-Doki Mode ........... Y (needs at least one full Heart Gauge)
  Mom is coming! ................. X
  Choose your route at arrows .... left thumbstick
  Hide / show the HUD ............ B
  Sight speed .................... left grip (the game's setting for moving the sight with a stick: no effect
                                   when you aim with the controller)
  In boss battles, the "!" markers of a boss's special attack are shot like everything else.

Doki-Doki Mode (in a stage)
  Choose a girl .................. point at the marker on her, right trigger
  Touch her gently ............... tap the right trigger
  Rub ............................ hold the right trigger down
  Change the camera angle ........ left / right grip (more cameras are sold in the Academy Store)
  Euphoria ....................... Y

Final Doki-Doki Mode (end of a route)
  Aim at a spot (head, chest...) . point your right controller
  Zoom in and shoot .............. hold the right grip to zoom in, let go to shoot
  Quick shot, without zooming .... right trigger
  Euphoria (her gauge full) ...... Y

Minigames (the HUD panel moves up and down with your gaze)
  Targets ........................ shoot the hidden targets all over her before the time runs out - they show
                                   inside your sight's circle: right trigger
  Special requests ............... heart icons under the Satisfaction Gauge mean she wants something: follow her
                                   voice to the request icon on her body. Point at it so it shows inside your
                                   sight's circle, then use the right grip: tap = poke, hold and move your
                                   hand = rub, hold still = hold, hold and swipe = flick
  Hands-on ....................... when the Satisfaction Gauge is full, repeat the on-screen commands as fast
                                   as you can: hold the right grip or either trigger and move your hand the way
                                   the game shows (drag up / down, circle, scratch)
                                   ...or with the thumbsticks: circles and scratching with the right stick,
                                   the two-handed up / down drags with either stick

Menus, the hub and story scenes (the pointer stays on a menu while it is up, wherever you point)
  Click what you point at ........ right trigger
  Confirm / back ................. A / B
  Move through a menu ............ left thumbstick
  Scroll long lists .............. right thumbstick
                                   (pause menu, Continue and results screens: moves through the items)
  Switch tabs .................... left / right grip, in menus that have them
  Story scenes: next line ........ right trigger or A

Collection, Dressing Room and the story game-over screen
  Turn the camera ................ right thumbstick
Collection - Fantasize
  Move the camera ................ left thumbstick
  Tilt ........................... right thumbstick
  Glasses ........................ press the right thumbstick down
  Pose ........................... A
  Doki-Doki ...................... Y
  Zoom in and out ................ left / right grip (you move closer to her or back)
  Reset the zoom ................. both grips
  Touch .......................... left trigger
  Back ........................... B

Other controllers (Valve Index, Windows Mixed Reality, HTC Vive, HP Reverb G2, Steam Frame) work too - the settings
file explains where their buttons are. To play with a gamepad or mouse and keyboard while still seeing the game in VR,
set controllers=0 in the settings file.

Quest / Rift controllers through SteamVR (Steam Link, Virtual Desktop in SteamVR mode, Link + SteamVR): SteamVR
uses the left menu button for its own menu, so Pause needs another button. In the settings file's [controls]
section set, for example, pause=main.stick_click - or rebind it in SteamVR's controller settings for the game.
(Valve Index, HTC Vive, Windows Mixed Reality and Steam Frame controllers aren't affected.)


SETTINGS
--------
All settings are in one file: Binaries\Win64\GGDPVR\ggdpvr.ini in the game folder. Open it with Notepad. Every
option is explained there and shows its default. Save the file, and the changes apply the next time you start the
game. The everyday options come first: the HUD panel's size and distance, weak-spot pop-up size, comfort options,
performance, then the controller buttons ([controls]), then [advanced] for fine-tuning.
To go back to the default settings: delete ggdpvr.ini, then click Repair in GGDPVR-Setup.exe.


PLAYING WITHOUT VR
------------------
Click "Turn mod off" in GGDPVR-Setup.exe: the game starts as if the mod wasn't installed. "Turn mod on" brings VR
back. (By hand: create an empty file called "disabled" in the GGDPVR folder, and delete it again for VR. Or set
enabled=0 in ggdpvr.ini.)


UNINSTALL
---------
Easy way: run GGDPVR-Setup.exe and click Uninstall (bottom right). It removes everything the mod added.

By hand: delete these from the game's Binaries\Win64 folder:
    dinput8.dll
    the GGDPVR folder
and, if you like, the mod's log folder %LOCALAPPDATA%\GGDPVR (paste that into the Explorer address bar).

While VR runs, the mod temporarily changes a few of the game's graphics settings (for example motion blur and
anti-aliasing) and puts your own values back when the game closes. If the game crashed during VR, those settings
are put back the next time the game starts with the mod. So, before uninstalling by hand after a crash, start the
game once and quit it normally - or use GGDPVR-Setup.exe's Uninstall, which puts them back for you.

The mod doesn't change any of the game's own files, and never touches your save data.


PROBLEMS?
---------
- Nothing appears in the headset: start your VR software before the game, and make sure it's the active OpenXR
  runtime (SteamVR: Settings > OpenXR; Meta Quest Link: Settings > General > OpenXR Runtime).
- The game is still on the monitor only: check that enabled=1 in ggdpvr.ini and that there is no "disabled" file in
  the GGDPVR folder, then click Repair in GGDPVR-Setup.exe.
- Reporting a problem: set log=1 in ggdpvr.ini, play until the problem happens, quit, and send the file
  %LOCALAPPDATA%\GGDPVR\logs\ggdpvr.log with your report (GGDPVR-Setup.exe's "Log folder" button opens it).


PRIVACY
-------
GGDPVR has no network code: it never connects to the internet and never sends anything anywhere. It writes no log
unless you turn log=1 on. The log stays on your PC, and it never contains your Windows user name or device names.
