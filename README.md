# GTA2 60 FPS
A `ddraw.dll` proxy that adds frame interpolation to *Grand Theft Auto 2*, smoothing the game out to 60 FPS without the infamous speed-up weirdness.
Objects and the camera are interpolated between game ticks, so movement looks smooth while the underlying simulation runs exactly as it always did.

Compatible GTA2 version:
- 27/04/2004 3:29PM 9.6.0.0

## Installation
2. Copy your **real** 32-bit `ddraw.dll` from `C:\Windows\SysWOW64\ddraw.dll` into the GTA 2 installation folder and rename it to `real_ddraw.dll`.
1. Drop custom  `ddraw.dll` from this project, next to `gta2.exe` (GTA 2 installation folder).
3. Drop `gta2_60fps.ini` also in same folder as `gta2.exe`.
4. Launch the game.

That's it! 🖤

## Configuration
You may adjust things in ini file.

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
