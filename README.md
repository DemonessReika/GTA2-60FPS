# GTA2 60 FPS
A `ddraw.dll` proxy that adds frame interpolation to *Grand Theft Auto 2*, smoothing the game out to 60 FPS without the infamous speed-up weirdness  (Not DLSS related. No input lag!).
Objects and the camera are interpolated between game ticks, so movement looks smooth while the underlying simulation runs exactly as it always did.

Compatible GTA2 version:
- 27/04/2004 3:29PM 9.6.0.0 (Tested with 9.6.0.0 and 1280x960 resolution)
- Untested on newer versions.
# Changelog

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