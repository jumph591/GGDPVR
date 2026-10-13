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


CONTROLS (VR controllers; you can change all of them, see SETTINGS)
--------------------------------------------------------------------
The controls below are for a right-handed player on Meta Quest controllers. Left-handed: set dominant_hand=left,
and the trigger and grip controls swap hands.
  Aim ............................ point your right controller
  Pheromone shot (X) ............. right trigger (hold to charge). In menus it clicks what you point at.
  Zoom (RB) ...................... right grip
  Sight speed (LB) ............... left grip
  Special requests (RT) .......... left trigger
  Doki-Doki Mode (Y) ............. Y
  Mom is coming! (BACK) .......... X
  Confirm (A) / Cancel (B) ....... A / B
  Pause (START) .................. left menu button
  Hide / show the HUD ............ B, during stages and boss fights
  Menus and route choice ......... left thumbstick (D-pad); right thumbstick scrolls lists
  Re-center the view ............. press the left thumbstick down (your headset's own re-center works too)
Other controllers (Valve Index, Windows Mixed Reality, HTC Vive, HP Reverb G2) work too - the settings file explains
where their buttons are. To play with a gamepad or mouse and keyboard while still seeing the game in VR, set
controllers=0 in the settings file.

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
