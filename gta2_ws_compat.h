#pragma once
#include <cstdint>
// Addresses of the mod's config globals that may need force-disabling when the
// GTA2 Widescreen Fix is present. main.cpp fills this and passes it to Compat_Run.
struct ModFlags {
	bool* apply32bpp;    // 32bit_mode
	bool* menuScale;     // menu_scale_fix
	bool* skipIntro;     // skip_intro
	bool* unifyRes;      // unifyres
	bool* zoom;          // zoom
	bool* doCamera;      // camera (our interp; also gates cull_follow)
	bool* childProfile;  // childprofile
	bool* mapProfile;    // mapprofile
	int32_t* resWidth;      // res_width
	int32_t* resHeight;     // res_height
};
// mode: 0 = detect + warn only (don't change config)
//       1 = auto: detect, and if present defer conflicting features
//       2 = force: treat as present (for testing without the fix installed)
// Returns true if the Widescreen Fix was detected (or forced).
bool Compat_Run(const ModFlags& f, int mode);