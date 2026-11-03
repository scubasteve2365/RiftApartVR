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

The launcher window is skipped. Splash and menus show on a flat screen in front of you. The world is a solid 3D place around you. Cutscenes play on a stereo screen fixed in the room.

Click and hold both thumbsticks for half a second to open the VR menu. The game ignores the pad while the menu is open (Xbox style pads, tested).

| Setting | What it does |
|---|---|
| Recenter view | Makes where you are facing now the front |
| World scale | How much of a miniature the world looks |
| Decoupled pitch | On: the world stays level, only the camera's turn moves you. Off: the camera's up and down tilt moves the world too |
| HUD distance | How far away the HUD panel sits |
| Resolution | Percent of what your headset asks for. Next launch |
| DLSS scene size | How large each eye is drawn before DLSS: 50 Performance, 58 Balanced, 67 Quality, 77 Ultra Quality, 100 DLAA. Next launch |
| Skip the launcher window | Next launch |
| Cutscenes on a screen | Off leaves cutscenes in the 3D view |
| Aim dot | Shows where you aim, as a ring in the world |

Settings save to `RiftApartVR.ini` in the game's folder.

DLSS is used when DLSS is chosen in the game's graphics menu. With any other choice the eyes draw at full size.

## Licence

MIT. Uses MinHook (BSD 2-Clause) and the OpenXR loader (Apache 2.0). DLSS files are the game's own.
