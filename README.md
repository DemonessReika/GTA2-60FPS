# GTA2 60 FPS
A `ddraw.dll` proxy that adds frame interpolation to *Grand Theft Auto 2*, smoothing the game out to 60 FPS without the infamous speed-up weirdness  (Not DLSS related. No input lag!).
Objects and the camera are interpolated between game ticks, so movement looks smooth while the underlying simulation runs exactly as it always did.

Compatible GTA2 version:
- 27/04/2004 3:29PM 9.6.0.0 (Tested with 9.6.0.0 and 1280x960 resolution)
- Untested on newer versions.
# Changelog

## v1.07 & v1.06
- Added .ini toggle that lets you disable the blinking purple arrow leading to the level end when you reach required money (Arrow still appears if you are in the area where the level end zone is, so that you don't get lost!) 
(Set in ini hide_endlevel_arrow_if_far=1)
- Resolved stutter that rarely occured at high movement speed (We now trust DirectX directly to tell us information about vblanks)
- Fixed cull distance of sprites not being sufficient with max zoom-out leading to occassional sprite pop-in.
- Fixed collectible counter not counting properly the "reward" special tokens (such as token awarded for completing tutorial in downtown).
- Fixed ddraw.dll being loaded by GTA 2 map editor (due to being in same directory!) which wasn't likely to cause any issues, but better safe than sorry!
- Fixed crash caused by 60 fps fix when you start new save/load save multiple times.
- Fixed vanilla GTA2's crash related to bus/train array counter never being reset (happened when you restart game by going to main menu then back in-game 10+ times.)
- Fixed sprite render limit being exceeded by handling it gracefully, (should solve rare crash when too much renders at once)
- Structured ini config to be more digestable to read, (may add mod settings to in-game in future as alternative)
- [Experimental] Added .ini toggle for windowed/borderless mode. 

## v1.05
- Added "collectible_counter" .ini toggle, when set to 1 will let you see the amount of special tokens you collected so far on each map (special token = one of 50 gta 2 logo pickups)
- In-game now runs under DirectX11 reducing majority of cpu/gpu related issues.
- Camera is now fully 60-fps smooth, the desynchronization with higher speeds due to vblank miscalculations is solved.
- Removed legacy throttling of the vsync frames.
- Project is kinda feature complete aside from fixing widescreen compat, adding windowed mode and above 60 fps support such as 120 or 240.

## v1.03.1 
- Disabled Auto-Compatibility with widescreen fix, feature broken currently. It might work with widescreen fix assuming you manually tinker with .ini properties.

## v1.03
- Added automatic compatibility with "GTA 2 - Widescreen Fix" (untested but should be good to go).
- Added enabled-by-default 32-bit rendering mode instead of regular 16-bit (ini toggle 32bit_mode), should reduce risk of performance issues from 16-to-32 compatibility layer that Windows OS does.
- Fixed main menu being scaled down on resolutions larger than 640x480.
- Skipping intro is now togglable using .ini variable.
- Pedestrian/car spawning logic is now properly taking into account the zoom-out/zoom-in.
- Fixed rare glitch where ped/car can spawn and disappear shortly after due to recycler at camera boundary edges.
- Bunch of other GTA 2 performance fixes.

## v1.02
- Added experimental fix to GTA2's stutter/lag whenever a new sound is played (such as weapon pick-up sound or radio station music).
- Added optional .ini settings to only display banners "name of town area" and "name of car" once per gameplay (resets on playing new game or reloading save).
- Added optional .ini settings to reduce size of banners.
- Fixed flickering/ghosting when some vehicles spawned such as police cars with sirens.
- Improved performance of vanilla GTA2 tileset rendering.
- Fixed VEH spam.
- Logging to .log file now is detached from main thread.
- Dumping crash data to .dmp file is now optional and .ini toggle.
- Made .ini file easier to read.
- And probably other things i forgot...


# v1.01
- Fixed mission markers stuttering.
- Added zoom-in/zoom-out with keyboard "-" and "+" buttons, can be disabled using .ini or swapped to numpad "-" and "+" keys instead.

# v1.00
- Initial Release.


## Installation
1. Drop `ddraw.dll` next to `gta2.exe` (GTA 2 installation folder).
2. Copy your **real** 32-bit `ddraw.dll` from `C:\Windows\SysWOW64\ddraw.dll` into the same folder and rename it to `real_ddraw.dll`.
3. Drop `gta2_60fps.ini` also in same folder as `gta2.exe`.
4. Launch the game.

That's it! 🖤

Also, It's recommended to update .ini file too if you updated from older version to newer one of GTA 2 60 FPS!

## Configuration
You may adjust things in ini file.
I finished all three cities on 1280x960 resolution with 60 fps-fix, your mileage may vary.

## Building
- Visual Studio 2022, **x86** target, built as `ddraw.dll`.
- Requires [MinHook](https://github.com/TsudaKageyu/minhook).

## Notes
- If you enable `log=1`, the resulting `.log` file contains info about **your system** (paths, running modules, etc). Don't blindly paste it into bug reports without a look.
- `real_ddraw.dll` is a Windows system file and is **not** included — copy your own.

## License
[GPLv3](LICENSE) — free software; if you distribute it or a modified version,
you must share the source under the same license.
Uses [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause) — see [licenses/](licenses/).