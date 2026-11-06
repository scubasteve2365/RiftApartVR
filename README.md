# Rift Apart VR

Ratchet & Clank: Rift Apart in VR. Native stereo: both eyes from the same game tick. Third person, gamepad, OpenXR, DLSS in both eyes.

Work in progress. Not released yet.

## Install

1. Keep these three files together in any folder: `Install Rift Apart VR.bat`, `winhttp.dll`, `openxr_loader.dll`.
2. Double-click the bat file. It finds the game through Steam and copies the two dll files into the game's folder.
3. Start your headset's OpenXR runtime, then start the game from Steam.

If it cannot find the game, or to remove the mod, run the bat file from a command prompt:

```
"Install Rift Apart VR.bat" "E:\SteamLibrary\steamapps\common\Ratchet & Clank - Rift Apart"
"Install Rift Apart VR.bat" uninstall
```

## Playing

The launcher window is skipped. Splash and menus show on a flat screen in front of you. The world is a solid 3D place around you. Cutscenes play in the same 3D world, apart from very zoomed in shots.

Click and hold both thumbsticks for half a second to open the VR menu. The game does not see the pad while the menu is open, or until you let go of every button after closing it.

| Control | In the menu |
|---|---|
| LB / RB or LT / RT | Change section |
| D-pad up / down | Choose a setting |
| D-pad left / right, or A | Change it |
| B, or both thumbsticks | Close |

Each setting has a short explanation at the bottom of the menu.

| Section | Setting | What it does |
|---|---|---|
| General | Recenter view | Makes where you are facing now the front. SteamVR's own recenter does the same |
| General | World scale | How much of a miniature the world looks |
| General | Decoupled pitch | On: the world stays level, only the camera's turn moves you. Off: the camera's tilt moves the world too. Auto: level, except on gravity walls |
| General | HUD distance | How far away the HUD panel sits |
| General | Skip the launcher window | Next launch |
| Performance | Frame generation | Off, 2X or 3X. Off by default. Uses OFXR-BRIDGE from the `ofxr` folder, which loads at launch once a mode is chosen. Close the OFXR tray program first |
| Performance | Frame generation type | Optical flow, DLSS-G, or Hybrid (next launch). FSR 3.1 is listed but makes no frames yet |
| Performance | Optical flow quality | Fast, Medium or Slow |
| Performance | Optical flow detail | 25 to 100 percent |
| Performance | Stereo | Native draws both eyes each frame and is the default. Alternate draws one eye per frame, faster, and turns frame generation off. Depth rebuild makes the other eye from the drawn one and needs DLSS on |
| Options | Vendor screen up close | Shop screens fill your view when the game zooms onto one |
| Options | Aim dot | Shows where you aim, as a ring in the world |
| Options | Cutscenes on a screen | Zoomed shots: very zoomed in cutscene shots show on a screen, the rest in 3D. Needed for a GPU hang at one scripted ride |
| Options | Menu sound | The menu's click: Off, Low, Medium or High |
| Developer | Snapshot in 2 seconds | Saves pictures of what the mod shows, for reporting a problem |
| Developer | Frame generation pacing | Framework or Director. Director can lower latency but has frozen the game on one test PC. Every launch starts on Framework |
| Developer | Hand frames to SteamVR from | Render thread (default) or XR thread, a test |

Render resolution comes from SteamVR (its resolution setting). `Resolution=` in the ini still scales it, as a percent.

Settings save to `RiftApartVR.ini` in the game's folder.

In VR the game runs in a 2560x1440 window, whatever your settings say, and your monitor shows the left eye. Your saved settings are not changed by the mod, unless you save settings in the game's own menu while it runs.

DLSS is used when DLSS is chosen in the game's graphics menu. With any other choice the eyes draw at full size.

The mod tells the game VSync is off while in VR, so the headset sets the frame rate. `GameVSyncOff=0` in the ini keeps the game's own VSync.

## Performance tips

- Texture Quality: Medium or High, not Ultra. Ultra makes the game load textures in and out as you move, and in VR that costs the CPU several milliseconds every frame, even with plenty of video memory. On an RTX 5090 test PC, Medium took the busy areas from about 60 to a steady 72. Texture Filtering 16x is fine.
- In SteamVR, set this game's Throttling Behavior to Limit at your headset's refresh rate (SteamVR Settings, Video, Per-Application Video Settings). On Auto, SteamVR can drop the game to half rate and keep it there.
- If lowering Resolution does not raise the frame rate, the CPU is the limit, not the graphics card.

## Licence

MIT. Uses MinHook (BSD 2-Clause) and the OpenXR loader (Apache 2.0). DLSS files are the game's own.

The `ofxr` folder is a separate program, OFXR-BRIDGE by tig3rmast3r and contributors, under GPL-3.0, not under this mod's licence. Its licence and source link are in that folder.
