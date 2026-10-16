// gta2_compat.cpp — detect the GTA2 Widescreen Fix and defer conflicting features.
// The WS fix (Ultimate ASI Loader as dinput.dll + GTA2.WidescreenFix.asi) owns the
// video-mode / resolution / frontend / zoom / camera-smoothing paths. We own the
// 60fps pump + object interp, which it completely lacks. This module reads which
// side should hold each contested knob and force-disables ours accordingly.
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <string>
#include "gta2_ws_compat.h"
#pragma comment(lib,"psapi.lib")
extern void Dbg(const char* f, ...);   // async ring logger in gta2_60fps.cpp
static bool FileExistsW(const wchar_t* p) {
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool GameDirW(std::wstring& out) {
	wchar_t p[MAX_PATH];
	DWORD n = GetModuleFileNameW(GetModuleHandleW(nullptr), p, MAX_PATH);
	if (!n || n >= MAX_PATH) return false;
	wchar_t* s = wcsrchr(p, L'\\');
	if (!s) return false;
	*s = 0; out = p; return true;
}
// Module check: exact name first, then any loaded module with "widescreen" in it
// (catches renamed folders / repacks). File check is primary — it's load-order proof.
static bool ModuleLoaded(const char*& how) {
	if (GetModuleHandleA("GTA2.WidescreenFix.asi")) { how = "GTA2.WidescreenFix.asi"; return true; }
	HMODULE mods[1024]; DWORD need = 0;
	if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
		int cnt = (int)(need / sizeof(HMODULE));
		for (int i = 0; i < cnt; i++) {
			char nm[MAX_PATH];
			if (GetModuleFileNameA(mods[i], nm, MAX_PATH)) {
				for (char* q = nm; *q; ++q) *q = (char)tolower((unsigned char)*q);
				if (strstr(nm, "widescreen")) { how = "*widescreen*"; return true; }
			}
		}
	}
	return false;
}
// Lenient bool: 1/true/yes/on -> true; 0/false/no/off -> false; else default.
// (Matches CIniReader::ReadBoolean's tolerance; also eats inline junk via first token.)
static bool ReadWsBool(const std::wstring& iniW, const wchar_t* sec, const wchar_t* key, bool def) {
	wchar_t buf[64] = { 0 };
	GetPrivateProfileStringW(sec, key, def ? L"1" : L"0", buf, 63, iniW.c_str());
	const wchar_t* p = buf;
	while (*p == L' ' || *p == L'\t') ++p;
	switch (towlower(*p)) {
	case L'1': case L't': case L'y': return true;
	case L'o': return towlower(p[1]) == L'n';   // "on" true, "off" false
	case L'0': case L'f': case L'n': return false;
	default:   return def;
	}
}
static void SetB(bool* ptr, bool val, const char* name) {
	if (!ptr || *ptr == val) return;
	Dbg("[COMPAT]   %-14s %d -> %d\n", name, (int)*ptr, (int)val);
	*ptr = val;
}
static void SetI(int32_t* ptr, int32_t val, const char* name) {
	if (!ptr || *ptr == val) return;
	Dbg("[COMPAT]   %-14s %d -> %d\n", name, (int)*ptr, (int)val);
	*ptr = val;
}
bool Compat_Run(const ModFlags& f, int mode) {
	std::wstring dir, iniW, asiW;
	bool haveDir = GameDirW(dir);
	if (haveDir) {
		iniW = dir + L"\\scripts\\GTA2.WidescreenFix.ini";
		asiW = dir + L"\\scripts\\GTA2.WidescreenFix.asi";
	}
	bool fileHit = haveDir && FileExistsW(asiW.c_str());
	const char* modHow = "-";
	bool modHit = ModuleLoaded(modHow);
	bool present = (mode == 2) || fileHit || modHit;
	Dbg("[COMPAT] WidescreenFix: file=%d module=%d(%s) forced=%d => %s\n",
		(int)fileHit, (int)modHit, modHit ? modHow : "-", (int)(mode == 2),
		present ? "PRESENT" : "absent");
	if (!present) { Dbg("[COMPAT] standalone; no changes.\n"); return false; }
	if (mode == 0) {
		Dbg("[COMPAT] *** detected but ws_compat=0: NOT adjusting. Expect clashes in\n");
		Dbg("[COMPAT]     video-mode bytes / menu decode / resolution / zoom keys / MapLayers (if profiling).\n");
		return true;
	}
	// Only setting that changes our decision; defaults match the WS fix's own code.
	bool wsSmoothFollow = fileHit ? ReadWsBool(iniW, L"CAMERA", L"SmoothFollow", true) : true;
	bool wsRotate = fileHit ? ReadWsBool(iniW, L"CAMERA", L"RotateWithPlayer", true) : true;
	Dbg("[COMPAT] deferring (ws SmoothFollow=%d Rotate=%d):\n", (int)wsSmoothFollow, (int)wsRotate);
	// Hard: the WS fix owns the whole video / frontend / resolution path (it always
	// patches the Vid_SetFullscreen bpp site + res; its menu decode fixes the tint).
	// Us patching the PRIMARY push site on top = the green-menu it deliberately avoids.
	SetB(f.apply32bpp, false, "32bit_mode");
	SetB(f.menuScale, false, "menu_scale_fix");
	SetB(f.skipIntro, false, "skip_intro");      // theirs: SkipMovie
	SetB(f.unifyRes, false, "unifyres");        // theirs rewrites Vid_ModeGate
	SetI(f.resWidth, 0, "res_width");       // theirs: ResX/ResY
	SetI(f.resHeight, 0, "res_height");
	SetB(f.zoom, false, "zoom");            // theirs zoom (same -/= keys by default)
	// Diagnostics that would put a 2nd trampoline on Render_MapLayers (0x472110)
	// against their DrawLayers inline. Never let these co-exist.
	SetB(f.childProfile, false, "childprofile");
	SetB(f.mapProfile, false, "mapprofile");
	// Camera: exactly ONE writer of cam[38..40]. (Objects stay ours either way —
	// they don't touch objects, and tiles+objects project through the same cam.)
	if (wsSmoothFollow) {
		SetB(f.doCamera, false, "camera");
		Dbg("[COMPAT]   camera -> WS (SmoothFollow on); our interp + cull_follow idle.\n");
	}
	else {
		Dbg("[COMPAT]   camera -> OURS (SmoothFollow off); interp + cull_follow active.\n");
	}
	Dbg("[COMPAT] kept: pump, objects, tilelist_opt, audio-warm, pre/forcedirty, arrows, banners.\n");
	return true;
}