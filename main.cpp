// gta2_60fps.cpp — x86 DDRAW.DLL PROXY build
// Build as ddraw.dll, drop next to gta2.exe.
// Copy the REAL 32-bit ddraw (SysWOW64) next to gta2.exe as real_ddraw.dll.
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <intrin.h>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <tlhelp32.h>
#include "MinHook.h"

#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"libMinHook.x86.lib")

// ---- ddraw.dll export forwarders -> real_ddraw.dll --------------------
#pragma comment(linker, "/export:DirectDrawCreate=real_ddraw.DirectDrawCreate")
#pragma comment(linker, "/export:DirectDrawCreateEx=real_ddraw.DirectDrawCreateEx")
#pragma comment(linker, "/export:DirectDrawCreateClipper=real_ddraw.DirectDrawCreateClipper")
#pragma comment(linker, "/export:DirectDrawEnumerateA=real_ddraw.DirectDrawEnumerateA")
#pragma comment(linker, "/export:DirectDrawEnumerateW=real_ddraw.DirectDrawEnumerateW")
#pragma comment(linker, "/export:DirectDrawEnumerateExA=real_ddraw.DirectDrawEnumerateExA")
#pragma comment(linker, "/export:DirectDrawEnumerateExW=real_ddraw.DirectDrawEnumerateExW")
#pragma comment(linker, "/export:DllCanUnloadNow=real_ddraw.DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/export:DllGetClassObject=real_ddraw.DllGetClassObject,PRIVATE")
#pragma comment(linker, "/export:GetSurfaceFromDC=real_ddraw.GetSurfaceFromDC")
#pragma comment(linker, "/export:D3DParseUnknownCommand=real_ddraw.D3DParseUnknownCommand")
#pragma comment(linker, "/export:AcquireDDThreadLock=real_ddraw.AcquireDDThreadLock")
#pragma comment(linker, "/export:ReleaseDDThreadLock=real_ddraw.ReleaseDDThreadLock")
#pragma comment(linker, "/export:CompleteCreateSysmemSurface=real_ddraw.CompleteCreateSysmemSurface")
#pragma comment(linker, "/export:DDInternalLock=real_ddraw.DDInternalLock")
#pragma comment(linker, "/export:DDInternalUnlock=real_ddraw.DDInternalUnlock")
#pragma comment(linker, "/export:GetDDSurfaceLocal=real_ddraw.GetDDSurfaceLocal")
#pragma comment(linker, "/export:GetOLEThunkData=real_ddraw.GetOLEThunkData")
#pragma comment(linker, "/export:RegisterSpecialCase=real_ddraw.RegisterSpecialCase")
#pragma comment(linker, "/export:SetAppCompatData=real_ddraw.SetAppCompatData")

// ---- master toggles (ini: [60fps]) ------------------------------------
static bool g_logEnabled = true;   // "log"  master log switch. 0 => no file, Dbg() is a no-op.

// ---- [PERF3] 45A5A0 child decomposition (flag: childprofile, DEFAULT OFF) ----
static bool     g_childProfile = false;
static int64_t  g_qcSurf = 0, g_qcMap = 0, g_qcSpr2 = 0, g_qcUi = 0;
static uint32_t g_sceneN = 0, g_daPrev = 0, g_ddPrev = 0;
typedef int(__cdecl* fn_surf_t)();
typedef int(__fastcall* fn_spr2_t)(void*, void*);
typedef char(__fastcall* fn_ui_t)(void*, void*);
static fn_surf_t oSurf = nullptr; static fn_spr2_t oSpr2 = nullptr; static fn_ui_t oUi = nullptr;

// ---- perf probe ----
static DWORD    g_perfT0 = 0, g_lastTickMs = 0, g_tickDtMax = 0, g_tickDtMin = 0xFFFFFFFF;
static uint32_t g_renderN = 0, g_pumpN = 0;
static double   g_renderDurSum = 0; static DWORD g_renderDurMax = 0;

// ---- adaptive pump (drawCPU-gated) ----
static bool     g_adaptivePump = true;
static double   g_debt = 0.0;            // DIAGNOSTIC only now (not a gate)
static DWORD    g_prevTickWall = 0;
static uint32_t g_pumpSkips = 0;
static double   g_lastDrawCPU = 16.0;    // EMA of scene-draw ms (updated in Hook_SceneDraw); gates the pump
static double   g_pumpBudgetMs = 42.0;   // pumped-tick budget (ms); skip 2nd render if 2*drawCPU+4 exceeds it

// ---- map-layer / tile-dedup profiler (diagnostic; flag: mapprofile, DEFAULT OFF) ----
static bool     g_mapProfile = false;
static LARGE_INTEGER g_qpf = { 0 };
static double   g_tscPerMs = 0.0;
static uint32_t g_mapCalls = 0;
static uint64_t g_dedupTsc = 0;      static uint32_t g_dedupCalls = 0;
static uint64_t g_dedupScan = 0;     static uint32_t g_dedupMax = 0;
typedef int(__fastcall* fn_maplayers_t)(void*, void*);
static fn_maplayers_t oMapLayers = nullptr;
typedef int(__fastcall* fn_tileadd_t)(void*, void*, void*, void*);
static fn_tileadd_t oTileAdd = nullptr;
struct { uintptr_t tick, render, logic, pos, pSession, pNextDue, pCarHead, pPedHead, dtor, scene, maplayers, tileadd, surf, spr2, ui, modegate; } g;

static void UseHardcoded()
{
	g.tick = 0x462A30; g.render = 0x461960; g.logic = 0x461930;
	g.pos = 0x420600;
	g.pSession = 0x5EB4FC; g.pNextDue = 0x662748;
	g.pCarHead = 0x5E4CA0; g.pPedHead = 0x5E5BBC;
	g.dtor = 0x4BDC40;
	g.scene = 0x45A5A0;      // Render_DrawScene (parent; we time its whole cost as drawCPU)
	g.maplayers = 0x472110;  // Render_MapLayers: 8-layer iso compositor (child; measured tiny)
	g.tileadd = 0x46BB90;    // TileList_AddUnique: O(n^2) dedup — real but ~0.1ms, IRRELEVANT
	g.surf = 0x4CAEC0;       // Render_SetupSurface (Vid_GetSurface + MakeScreenTable + gbh_SetWindow)
	g.spr2 = 0x4B98B0;       // stacked/tall-sprite draw (object-bound)
	g.ui = 0x4CA440;         // quit-menu overlay (~0 in gameplay)
	g.modegate = 0x4CB290;   // sub_4CB290: frontend/gameplay res decision (unifyres hook target)
}

// ---- logging ----------------------------------------------------------

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static void LogOpen()
{
	InitializeCriticalSection(&g_logCs);
	char path[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);
	char* s = strrchr(path, '\\');
	if (s) strcpy(s + 1, "gta2_60fps.log"); else strcpy(path, "gta2_60fps.log");
	g_log = fopen(path, "w");
}
static void Dbg(const char* f, ...)
{
	if (!g_logEnabled) return;                 // master switch: log=0 => fully silent, ~zero cost
	char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a);
	OutputDebugStringA(b);
	if (g_log)
	{
		EnterCriticalSection(&g_logCs);
		fprintf(g_log, "[%u] %s", timeGetTime(), b);
		fflush(g_log);
		LeaveCriticalSection(&g_logCs);
	}
}
static const char* PrioName(DWORD c)
{
	switch (c)
	{
		case IDLE_PRIORITY_CLASS:	return "IDLE";
		case BELOW_NORMAL_PRIORITY_CLASS:	return "BELOW_NORMAL";
		case NORMAL_PRIORITY_CLASS:	return "NORMAL";
		case ABOVE_NORMAL_PRIORITY_CLASS:	return "ABOVE_NORMAL";
		case HIGH_PRIORITY_CLASS:	return "HIGH";
		case REALTIME_PRIORITY_CLASS:	return "REALTIME";
		default:	return "?";
	}
}
static void LogParent()
{
	DWORD myPid = GetCurrentProcessId(), pPid = 0; WCHAR pName[MAX_PATH] = L"?";
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (s != INVALID_HANDLE_VALUE)
	{
		PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
		if (Process32FirstW(s, &pe))
			do
			{
				if (pe.th32ProcessID == myPid)
				{
					pPid = pe.th32ParentProcessID;
					break;
				}
			} while (Process32NextW(s, &pe));

		if (pPid)
		{
			pe.dwSize = sizeof pe;
			if (Process32FirstW(s, &pe))
				do
				{
					if (pe.th32ProcessID == pPid)
					{
						wcsncpy(pName, pe.szExeFile, MAX_PATH - 1);
						break;
					}
				} while (Process32NextW(s, &pe));
		}
		CloseHandle(s);
	}
	Dbg("[ENV] parentPid=%lu parent=%ls\n", pPid, pName);
}
static void LogModules()
{
	HMODULE m[512]; DWORD need = 0;
	if (EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need))
	{
		int n = need / sizeof(HMODULE);
		for (int i = 0; i < n; i++)
		{
			char nm[MAX_PATH];
			if (GetModuleFileNameA(m[i], nm, MAX_PATH))
			{
				char* b = strrchr(nm, '\\');
				Dbg("[ENV] mod: %s\n", b ? b + 1 : nm);
			}
		}
	}
}
static BOOL CALLBACK EnumWndProc(HWND h, LPARAM)
{
	DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(h))
	{
		LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE); RECT r; GetWindowRect(h, &r);
		Dbg("[ENV] hwnd=%p style=0x%08lX ex=0x%08lX rect=(%ld,%ld)-(%ld,%ld) %ldx%ld\n",
			h, st, ex, r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
	}
	return TRUE;
}
static void LogLaunchEnv()
{
	if (!g_logEnabled)
		return;
	Dbg("[ENV] cmdline: %s\n", GetCommandLineA());

	char cwd[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, cwd); Dbg("[ENV] cwd: %s\n", cwd);

	DWORD pc = GetPriorityClass(GetCurrentProcess());
	Dbg("[ENV] priority=%s(0x%lX) dpiAware=%d\n", PrioName(pc), pc, (int)IsProcessDPIAware());
	DWORD_PTR pm = 0, sm = 0;
	
	GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
	Dbg("[ENV] affinity proc=0x%zX sys=0x%zX\n", (size_t)pm, (size_t)sm);   // read-only diagnostic (confirms raw launch = full affinity)
	
	LogParent();
	LogModules();
}
static void LogVideoMode()
{
	if (!g_logEnabled) return;
	static int lw = -1, lh = -1;
	__try
	{
		int w = *(int*)0x673584 + 1;   // width-1  (set by sub_4CAEC0)
		int h = *(int*)0x673474 + 1;   // height-1 (set by sub_4CAEC0)
		if (w != lw || h != lh)
		{
			lw = w; lh = h;
			const char* r = GetModuleHandleA("d3ddll.dll") ? "d3ddll(D3D)" :
				GetModuleHandleA("3dfx.dll") ? "3dfx" :
				GetModuleHandleA("dmaglide.dll") ? "dmaglide(Glide)" : "?";
			Dbg("[VID] backbuffer=%dx%d renderer=%s\n", w, h, r);
			__try
			{
				uintptr_t base = *(uintptr_t*)0x673D20;               // engine surface struct
				if (base > 0x10000 && base < 0x7FFFFFFF)
				{
					int w = *(int*)(base + 72), pitch = *(int*)(base + 84);
					Dbg("[VID2] base=%08X width=%d pitch=%d bpp~%d\n", (unsigned)base, w, pitch, w ? pitch * 8 / w : 0);
					int md = *(int*)(base + 64), fsflag = *(int*)(base + 128);   // +64: -2=windowed ; +128: 1=exclusive-FS
					Dbg("[VID3] mode=%d fsflag=%d => %s\n", md, fsflag,
						(md == -2 || fsflag == 0) ? "WINDOWED(Blt) - no mode switch"
						: "EXCLUSIVE-FULLSCREEN(Flip) - SetDisplayMode active");
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER){}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- perf: scene-draw timing (isolates the render cost = drawCPU) ----
static double   g_drawSum = 0;
static DWORD g_drawMax = 0;
static uint32_t g_drawN = 0;

typedef void(__fastcall* fn_scene_t)(void*, void*);

static fn_scene_t oSceneDraw = nullptr;

static void __fastcall Hook_SceneDraw(void* thisp, void* edx)
{
	DWORD t = timeGetTime();
	oSceneDraw(thisp, edx);

	DWORD d = timeGetTime() - t;
	g_drawSum += d;

	if (d > g_drawMax)
		g_drawMax = d;
	++g_drawN;
	++g_sceneN;                          // PERF3 divisor (scene-draws per heartbeat)

	g_lastDrawCPU = g_lastDrawCPU * 0.6 + (double)d * 0.4;
}

// ---- map-layer profiler hooks (only armed when childprofile/mapprofile=1) ----
static int __fastcall Hook_MapLayers(void* thisp, void* edx)
{
	LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
	int r = oMapLayers(thisp, edx); QueryPerformanceCounter(&b);
	g_qcMap += (b.QuadPart - a.QuadPart); ++g_mapCalls; return r;
}
static int  __cdecl    Hook_Surf()
{
	LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
	int r = oSurf(); QueryPerformanceCounter(&b); g_qcSurf += (b.QuadPart - a.QuadPart); return r;
}
static int  __fastcall Hook_Spr2(void* t, void* e)
{
	LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
	int r = oSpr2(t, e); QueryPerformanceCounter(&b); g_qcSpr2 += (b.QuadPart - a.QuadPart); return r;
}
static char __fastcall Hook_Ui(void* t, void* e)
{
	LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
	char r = oUi(t, e); QueryPerformanceCounter(&b); g_qcUi += (b.QuadPart - a.QuadPart); return r;
}
static int __fastcall Hook_TileAddUnique(void* thisp, void* edx, void* a2, void* a3)
{
	uint32_t cnt = 0;
	__try
	{
		cnt = ((volatile uint32_t*)thisp)[3007];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		cnt = 0;
	}
	if (cnt > g_dedupMax)
		g_dedupMax = cnt;
	g_dedupScan += cnt;
	++g_dedupCalls;

	uint64_t t0 = __rdtsc();
	int r = oTileAdd(thisp, edx, a2, a3);
	g_dedupTsc += (__rdtsc() - t0);

	return r;
}
// ---- freeze localizer -------------------------------------------------
static volatile LONG g_cp = 0;
static volatile LONG g_progress = 0;
static void* g_lastObj = nullptr;
static volatile bool g_inRender = false;
static int  g_reentryLog = 0;

#define CP(n) do { g_cp = (LONG)(n);g_progress++; } while(0)

// ---- runtime bisect flags ---------------------------------------------
static bool g_doPump = true;
static bool g_doObjs = true;
static bool g_doCamera = true;
static bool g_forceDirty = true;

static bool g_preDirty = true;   // force-dirty +72 BEFORE draw (fixes stale-cache ghost — doc §2)

static double g_capFps = 60.0;

// -----------------------------------------------------------------------
static bool g_camZ = true;         // "cameraz": also interpolate camera HEIGHT [40] (ramps/elevation)
static bool g_unifyRes = false;    // "unifyres": menu resolves to config res => no menu<->game blink

static BYTE* const g_pFrontendFlag = (BYTE*)0x595018;  // byte_595018: 1 while frontend/menu active

static std::unordered_set<int32_t*> g_dead;

static uint32_t g_dtorHits = 0;
static uint32_t g_extraInterp = 0;

static bool g_useDeadSet = true;
typedef int(__fastcall* fn_dtor_t)(void*, void*);
static fn_dtor_t oDtor = nullptr;

static void LoadConfig()
{
	char path[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);
	char* s = strrchr(path, '\\');
	if (s)
		strcpy(s + 1, "gta2_60fps.ini");
	else
		strcpy(path, "gta2_60fps.ini");

	g_logEnabled = GetPrivateProfileIntA("60fps", "log", 1, path) != 0;
	g_doPump = GetPrivateProfileIntA("60fps", "pump", 1, path) != 0;
	g_doObjs = GetPrivateProfileIntA("60fps", "objects", 1, path) != 0;
	g_doCamera = GetPrivateProfileIntA("60fps", "camera", 1, path) != 0;
	g_camZ = GetPrivateProfileIntA("60fps", "cameraz", 1, path) != 0;
	g_forceDirty = GetPrivateProfileIntA("60fps", "forcedirty", 1, path) != 0;
	g_useDeadSet = GetPrivateProfileIntA("60fps", "deadset", 1, path) != 0;
	g_preDirty = GetPrivateProfileIntA("60fps", "predirty", 1, path) != 0;

	g_adaptivePump = GetPrivateProfileIntA("60fps", "adaptivepump", 1, path) != 0;
	g_pumpBudgetMs = (double)GetPrivateProfileIntA("60fps", "pumpbudget", 42, path);
	if (g_pumpBudgetMs < 20)
		g_pumpBudgetMs = 42.0;

	g_mapProfile = GetPrivateProfileIntA("60fps", "mapprofile", 0, path) != 0;
	g_childProfile = GetPrivateProfileIntA("60fps", "childprofile", 0, path) != 0;  // DEFAULT OFF

	g_capFps = (double)GetPrivateProfileIntA("60fps", "capfps", 60, path);
	if (g_capFps < 1)
		g_capFps = 60.0;
	g_unifyRes = GetPrivateProfileIntA("60fps", "unifyres", 0, path) != 0;
}
static void ForceDirtyNow(int32_t* d)
{   // clear +72 on both caches NOW, before the draw
	__try
	{
		uintptr_t c1 = (uint32_t)d[1]; if (c1 >= 0x10000 && c1 < 0x7FFFFFFF && !(c1 & 3)) *(uint8_t*)(c1 + 72) = 0;
		uintptr_t c3 = (uint32_t)d[3]; if (c3 >= 0x10000 && c3 < 0x7FFFFFFF && !(c3 & 3)) *(uint8_t*)(c3 + 72) = 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
static DWORD  g_prevDue = 0;
static DWORD g_frametime = 33;
static DWORD g_lastPump = 0;

static int32_t g_savedXY[3];
static int32_t* g_camPtr = nullptr;   // v8: [0]=x [1]=y [2]=z

static const int32_t TELE = 32 << 14;
struct Obj
{
	int32_t px = 0, py = 0, cx = 0, cy = 0; uint32_t tick = 0xFFFFFFFFu;
	uint32_t moves = 0; uint32_t lastLive = 0;
};

static std::unordered_map<int32_t*, Obj> g_obj;
static int __fastcall Hook_Dtor(void* thisp, void* edx)
{
	int32_t* d = (int32_t*)thisp;
	g_dead.insert(d);
	g_obj.erase(d);
	++g_dtorHits;
	return oDtor(thisp, edx);
}

static std::vector<int32_t*> g_moved;
struct SavedObj
{
	int32_t* d;
	int32_t x;
	int32_t y;
};
static std::vector<SavedObj> g_savedObj;

static uint32_t g_tick = 0;
static uint32_t g_frame = 0;

// ---- diag ----
static inline bool BadPtr(uintptr_t p)
{
	return p < 0x00010000 || p > 0x7FFFFFFF || (p & 3);
}

static int SafeKind(int32_t* d)
{
	__try
	{
		return d[12];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return -1;
	}
}  // +48

static uint32_t g_kindSeen[16] = { 0 };
static uint32_t g_kindMove[16] = { 0 };
static uint32_t g_kindLerp[16] = { 0 };
static uint32_t g_kindMiss[16] = { 0 };
static uint32_t g_kindDead[16] = { 0 };
static uint32_t g_carMiss = 0;

// ---- liveness (deadset shipped; pool-walk = fallback + diagnostics) ----

static std::unordered_set<int32_t*> g_live;
static std::vector<int32_t*> g_liveVec;
static int g_liveCar = 0, g_livePed = 0;

static uint32_t g_deadSkips = 0;
static uint32_t g_missCount = 0;
static uint32_t g_dualCount = 0;

static int g_carKind = 2;
static int g_pedKind = -1;
static int g_pedTries = 0;

static const int MAX_TRIES = 12;
static volatile LONG g_expectingFaults = 0;
static uint32_t g_suppressedFaults = 0;
static uintptr_t DeriveHead(uintptr_t base, int kind)
{
	__try
	{
		switch (kind)
		{
			case 0: return *(uintptr_t*)base;
			case 1: return *(uintptr_t*)(base + 4);
			case 2:
			{
				uintptr_t m = *(uintptr_t*)base;
				return BadPtr(m) ? 0 : *(uintptr_t*)(m + 4);
			}
			case 3:
			{
				uintptr_t m = *(uintptr_t*)base;
				return BadPtr(m) ? 0 : *(uintptr_t*)m;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return 0;
}
static void WalkCar(uintptr_t head, int nextOff, int dispOff, int* outValid, int* outHits, bool collect)
{
	int valid = 0, hits = 0;
	__try
	{
		uintptr_t e = head; int guard = 0;
		while (e && !BadPtr(e) && guard++ < 65536)
		{
			int32_t* disp = *(int32_t**)(e + dispOff);
			if (disp && !BadPtr((uintptr_t)disp))
			{
				++valid;
				if (g_obj.find(disp) != g_obj.end())
					++hits;
				if (collect)
					g_liveVec.push_back(disp);
			}
			e = *(uintptr_t*)(e + nextOff);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	*outValid = valid; *outHits = hits;
}
static void WalkPed(uintptr_t head, int nextOff, int midOff, int dispOff, int* outValid, int* outHits, bool collect)
{
	int valid = 0, hits = 0;
	__try
	{
		uintptr_t e = head; int guard = 0;
		while (e && !BadPtr(e) && guard++ < 65536)
		{
			uintptr_t mid = *(uintptr_t*)(e + midOff);
			if (mid && !BadPtr(mid))
			{
				int32_t* disp = *(int32_t**)(mid + dispOff);
				if (disp && !BadPtr((uintptr_t)disp))
				{
					++valid;
					if (g_obj.find(disp) != g_obj.end())
						++hits;
					if (collect)
						g_liveVec.push_back(disp);
				}
			}
			e = *(uintptr_t*)(e + nextOff);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	*outValid = valid; *outHits = hits;
}
static void CalibratePed()
{
	int best = -1, bestHits = 0;
	for (int k = 0; k < 4; k++)
	{
		uintptr_t h = DeriveHead(g.pPedHead, k);
		int valid = 0, hits = 0;

		if (h)
			WalkPed(h, 352, 360, 128, &valid, &hits, false);

		Dbg("[CAL] ped kind=%d head=%08X valid=%d hits=%d\n", k, (unsigned)h, valid, hits);
		if (hits > bestHits)
		{
			bestHits = hits;
			best = k;
		}
	}
	if (best >= 0 && bestHits >= 3)
	{
		g_pedKind = best;
		Dbg("[CAL] ped LATCHED kind=%d hits=%d\n", best, bestHits);
	}
	else if (++g_pedTries >= MAX_TRIES)
	{
		g_pedKind = -2;
		Dbg("[CAL] ped DISABLED after %d tries\n", g_pedTries);
	}
}
static void CollectLive()
{
	if (!g_useDeadSet && g_pedKind == -1 && g_obj.size() >= 64)
	{
		InterlockedIncrement(&g_expectingFaults);
		CalibratePed();
		InterlockedDecrement(&g_expectingFaults);
	}
	g_liveVec.clear();
	g_liveCar = g_livePed = 0;

	uintptr_t h = DeriveHead(g.pCarHead, g_carKind);

	if (h)
	{
		int v, hh;
		size_t b = g_liveVec.size();
		WalkCar(h, 76, 80, &v, &hh, true);
		g_liveCar = (int)(g_liveVec.size() - b);
	}
	if (!g_useDeadSet && g_pedKind >= 0)
	{
		uintptr_t hp = DeriveHead(g.pPedHead, g_pedKind);
		if (hp)
		{
			int v, hh;
			size_t b = g_liveVec.size();
			WalkPed(hp, 352, 360, 128, &v, &hh, true);
			g_livePed = (int)(g_liveVec.size() - b);
		}
	}
	g_live.clear();

	for (int32_t* d : g_liveVec)
		g_live.insert(d);

	g_carMiss = 0;
	memset(g_kindMiss, 0, sizeof g_kindMiss);
	for (int32_t* d : g_liveVec)
	{
		if (BadPtr((uintptr_t)d))
			continue;

		if (g_obj.find(d) != g_obj.end())
			continue;

		++g_carMiss;

		int k = SafeKind(d);
		if (k < 0 || k > 15)
			k = 15;
		g_kindMiss[k]++;
	}
}
static bool SafeSaveWrite(int32_t* d, int32_t nx, int32_t ny, int32_t* oldx, int32_t* oldy)
{
	__try
	{
		*oldx = d[5];
		*oldy = d[6];
		d[5] = nx;
		d[6] = ny;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}
static bool SafeRestore(int32_t* d, int32_t ox, int32_t oy, bool doDirty)
{
	__try
	{
		d[5] = ox;
		d[6] = oy;
		if (doDirty)
		{
			uintptr_t c1 = (uint32_t)d[1];
			if (c1 >= 0x00010000 && c1 < 0x7FFFFFFF && !(c1 & 3))
				*(uint8_t*)(c1 + 72) = 0;
			uintptr_t c3 = (uint32_t)d[3];
			if (c3 >= 0x00010000 && c3 < 0x7FFFFFFF && !(c3 & 3))
				*(uint8_t*)(c3 + 72) = 0;
		}
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}
static LONG WINAPI Veh(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
	{
		if (g_expectingFaults)
		{
			++g_suppressedFaults;
			return EXCEPTION_CONTINUE_SEARCH;
		}
		ULONG_PTR* xi = ep->ExceptionRecord->ExceptionInformation;

		Dbg("[VEH] AV %s target=%p eip=%08X | cp=%ld tick=%u lastObj=%p inRender=%d\n",
			xi[0] ? "WRITE" : "READ", (void*)xi[1], ep->ContextRecord->Eip, g_cp, g_tick, g_lastObj, (int)g_inRender);
		Dbg("[VEH] eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
			ep->ContextRecord->Eax, ep->ContextRecord->Ebx, ep->ContextRecord->Ecx,
			ep->ContextRecord->Edx, ep->ContextRecord->Esi, ep->ContextRecord->Edi);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
// ---- fatal-crash post-mortem ------------------------------------------

typedef BOOL(WINAPI* fn_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
	PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static fn_MiniDumpWriteDump p_MiniDumpWriteDump = nullptr;
static uintptr_t g_imgBase = 0x400000;
static uintptr_t g_imgTop = 0;

static void DumpStack(EXCEPTION_POINTERS* ep)
{
	uintptr_t sp = ep->ContextRecord->Esp;
	Dbg("[STK] esp=%08X ebp=%08X (scanning for return addrs into gta2.exe)\n",
		ep->ContextRecord->Esp, ep->ContextRecord->Ebp);
	for (int i = 0; i < 160; i++)
	{
		uintptr_t p = sp + (uintptr_t)i * 4;
		uintptr_t v = 0;
		int ok;

		__try
		{
			v = *(volatile uintptr_t*)p;
			ok = 1;
		}
		
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ok = 0;
		}
		
		if (!ok)
			break;

		if (v >= g_imgBase && v < g_imgTop)
			Dbg("[STK] +%03X = %08X\n", i * 4, v);
	}
}
static void WriteDump(EXCEPTION_POINTERS* ep)
{
	if (!p_MiniDumpWriteDump)
		return;

	char path[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);

	char* s = strrchr(path, '\\');
	char name[64];

	snprintf(name, sizeof name, "gta2_60fps_%u.dmp", timeGetTime());
	if (s)
		strcpy(s + 1, name);
	else
		strcpy(path, name);

	HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
	{
		Dbg("[UEF] dump CreateFile failed\n");
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION mei;
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = ep;
	mei.ClientPointers = FALSE;

	BOOL ok = p_MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
		(MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo),
		&mei, nullptr, nullptr);

	CloseHandle(h);
	Dbg("[UEF] minidump %s -> %s\n", path, ok ? "OK" : "FAILED");
}
static LONG WINAPI Uef(EXCEPTION_POINTERS* ep)
{
	static LONG once = 0;
	if (InterlockedExchange(&once, 1) == 0)
	{
		Dbg("[UEF] ***FATAL*** exc=%08X eip=%08X cp=%ld tick=%u lastObj=%p inRender=%d\n",
			ep->ExceptionRecord->ExceptionCode, ep->ContextRecord->Eip, g_cp, g_tick, g_lastObj, (int)g_inRender);
		DumpStack(ep);
		WriteDump(ep);
	}
	ChangeDisplaySettingsA(nullptr, 0);
	return EXCEPTION_EXECUTE_HANDLER;
}
// ---- watchdog ---------------------------------------------------------
static DWORD WINAPI Watchdog(LPVOID)
{
	LONG lastProg = -1; int same = 0;
	for (;;)
	{
		Sleep(250);
		LONG p = g_progress;
		if (p == lastProg)
		{
			same++;
			if (same == 4 || (same % 8) == 0)
				Dbg("[WD] STALL cp=%ld prog=%ld lastObj=%p tick=%u moved=%zu objs=%zu inRender=%d\n",
					g_cp, g_progress, g_lastObj, g_tick, g_moved.size(), g_obj.size(), (int)g_inRender);
		}
		else
		{
			same = 0;
			lastProg = p;
		}
	}
}
static int32_t* CameraPhys()
{
	uintptr_t s = *(uintptr_t*)g.pSession;
	if (!s)
		return nullptr;

	uintptr_t e = *(uintptr_t*)(s + 28);
	if (!e)
		return nullptr;
	return (int32_t*)(e + 332);
}
static float Alpha()
{
	DWORD due = *(DWORD*)g.pNextDue;
	DWORD ft = g_frametime ? g_frametime : 33;
	double a = (double)(int)(timeGetTime() - (due - ft)) / (double)ft;

	return a < 0 ? 0.f : (a > 1 ? 1.f : (float)a);
}

// ---- menu/gameplay resolution unifier (kills the mode-switch blink) ----
// sub_4CB290 forces 640x480 whenever byte_595018 (frontend) is set, so menu(640) vs
// gameplay(config) always differ across a transition -> Vid_CloseScreen + Vid_SetMode churn
// = the ~5s monitor resync. Clearing the flag ONLY across this call makes the menu resolve to
// config res too -> menu==gameplay -> no reconcile -> no blink. Restore immediately; the rest of
// WinMain's frontend logic is untouched. Main-thread only => no locking needed.

typedef char(__cdecl* fn_modegate_t)();
static fn_modegate_t oModeGate = nullptr;
static char __cdecl Hook_ModeGate()
{
	if (!g_unifyRes)
		return oModeGate();

	BYTE saved = *g_pFrontendFlag;
	*g_pFrontendFlag = 0;              // menu resolves to config res too (kills the menu<->game blink)
	char r = oModeGate();
	*g_pFrontendFlag = saved;

	return r;
}

typedef char(__cdecl* fn_t)();
static fn_t oTick = nullptr;
static fn_t oRender = nullptr;
static fn_t oLogic = nullptr;

typedef int(__fastcall* fn_pos_t)(void*, void*, int, int, int);

static fn_pos_t oPos = nullptr;

static int __fastcall Hook_Pos(void* thisp, void* edx, int x, int y, int z)
{
	if (g_inRender)
	{
		if (g_reentryLog < 50)
		{
			g_reentryLog++;
			Dbg("[60] !! setter DURING render this=%p ra=%p\n", thisp, _ReturnAddress());
		}
		return oPos(thisp, edx, x, y, z);
	}
	if (g_doObjs)
	{
		int32_t* d = (int32_t*)thisp;
		g_dead.erase(d);
		Obj& s = g_obj[d];
		if (s.tick != g_tick)
		{
			s.px = s.cx;
			s.py = s.cy;
			s.tick = g_tick;
			g_moved.push_back(d);
		}

		if (x != s.cx || y != s.cy)
			s.moves++;
		s.cx = x;
		s.cy = y;
	}
	return oPos(thisp, edx, x, y, z);
}
static char __cdecl Hook_Logic()
{
	CP(10);
	++g_tick;

	{
		DWORD t = timeGetTime();
		if (g_lastTickMs)
		{
			DWORD dt = t - g_lastTickMs;
			if (dt > g_tickDtMax)
				g_tickDtMax = dt;
			if (dt < g_tickDtMin)
				g_tickDtMin = dt;
		}

		g_lastTickMs = t;
		if (g_prevTickWall)
		{
			int actual = (int)(t - g_prevTickWall);
			int ideal = (int)(g_frametime ? g_frametime : 33);
			g_debt += (double)(actual - ideal);
			if (g_debt < 0)
				g_debt = 0;
			if (g_debt > 300)
				g_debt = 300;
		}
		g_prevTickWall = t;
	}

	g_moved.clear();
	CP(11);
	char r = oLogic();
	CP(12);
	if (g_doObjs && (g_tick % 300) == 0)
	{
		for (auto it = g_obj.begin(); it != g_obj.end(); )
		{
			if ((uint32_t)(g_tick - it->second.tick) > 300)
				it = g_obj.erase(it);
			else
				++it;
		}
	}
	CP(13);
	if ((g_tick % 30) == 0)
	{
		Dbg("[60] tick#%u moved=%zu objs=%zu carLive=%d carMiss=%u dtor=%u deadN=%zu dualw=%u a=%.2f\n",
			g_tick, g_moved.size(), g_obj.size(), g_liveCar, g_carMiss,
			g_dtorHits, g_dead.size(), g_dualCount, Alpha());
		char hb[512];
		int off = 0;

		hb[0] = 0;

		for (int k = 0; k < 16 && off < (int)sizeof(hb) - 1; k++)
		{
			if (g_kindSeen[k])
			{
				int w = snprintf(hb + off, sizeof(hb) - off, "k%d:%u/%u/%u(d%u) ", k, g_kindSeen[k], g_kindMove[k], g_kindLerp[k], g_kindDead[k]);
				if (w > 0)
					off += w;
			}
		}

		Dbg("[HIST] setter kind:seen/move/lerp(dead)  %s\n", hb[0] ? hb : "(none)");

		DWORD now = timeGetTime();
		DWORD w = g_perfT0 ? now - g_perfT0 : 0;

		Dbg("[PERF] ticks/s=%.1f fps=%.1f pumps=%u | tickDt min=%u max=%u | rDur avg=%.2f max=%u ms | ftMeas=%u\n",
			w ? 30000.0 / w : 0, w ? g_renderN * 1000.0 / w : 0, g_pumpN,
			g_tickDtMin == 0xFFFFFFFF ? 0 : g_tickDtMin, g_tickDtMax,
			g_renderN ? g_renderDurSum / g_renderN : 0, g_renderDurMax, g_frametime);

		g_perfT0 = now;
		g_tickDtMax = 0;
		g_tickDtMin = 0xFFFFFFFF;
		g_renderN = 0;
		g_pumpN = 0;
		g_renderDurSum = 0;
		g_renderDurMax = 0;

		Dbg("[PERF2] drawCPU avg=%.2f max=%u (n=%u) ema=%.1f budget=%.0f | debt=%.0f skips=%u | mode673E2C=%d fskip673598=%d\n",
			g_drawN ? g_drawSum / g_drawN : 0.0, g_drawMax, g_drawN, g_lastDrawCPU, g_pumpBudgetMs, g_debt, g_pumpSkips,
			*(int*)0x673E2C, *(BYTE*)0x673598);

		g_drawSum = 0;
		g_drawMax = 0;
		g_drawN = 0;
		g_pumpSkips = 0;

		if (g_mapProfile || g_childProfile)
		{
			double invF = g_qpf.QuadPart ? 1000.0 / (double)g_qpf.QuadPart : 0.0;
			uint32_t sn = g_sceneN ? g_sceneN : 1;
			double surfMs = (double)g_qcSurf * invF / sn, mapMs = (double)g_qcMap * invF / sn;
			double spr2Ms = (double)g_qcSpr2 * invF / sn, uiMs = (double)g_qcUi * invF / sn;
			if (g_childProfile)
			{
				uint32_t daNow = *(uint32_t*)0x5E8B78, ddNow = *(uint32_t*)0x5E8B7C;
				uint32_t da = daNow - g_daPrev;
				uint32_t dd = ddNow - g_ddPrev;
				
				g_daPrev = daNow; g_ddPrev = ddNow;
				
				double child = surfMs + mapMs + spr2Ms + uiMs;
				
				Dbg("[PERF3] surf=%.2f map=%.2f spr2=%.2f ui=%.2f | child=%.2f remain=%.2f | drawEMA=%.1f engAdd=%u engDraw=%u (n=%u)\n",
					surfMs, mapMs, spr2Ms, uiMs, child, g_lastDrawCPU - child, g_lastDrawCPU, da, dd, sn);
			}

			if (g_mapProfile)
			{
				double dedupMs = (g_mapCalls && g_tscPerMs > 0) ? ((double)g_dedupTsc / g_tscPerMs) / g_mapCalls : 0;
				double ratio = g_dedupCalls ? (double)g_dedupScan / g_dedupCalls : 0;

				Dbg("[MAP] map=%.2f | dedup=%.2fms/f ratio=%.0f maxN=%u\n", mapMs, dedupMs, ratio, g_dedupMax);
			}
			g_qcSurf = g_qcMap = g_qcSpr2 = g_qcUi = 0;
			g_sceneN = 0;

			g_mapCalls = g_dedupCalls = g_dedupMax = 0;
			g_dedupTsc = g_dedupScan = 0;
		}
	}
	return r;
}
static char __cdecl Hook_Render()
{
	CP(20);
	DWORD rt0 = timeGetTime();
	g_inRender = true;
	++g_frame;
	if (g_logEnabled && g_frame == 5)
	{   // fire early so a quick quit still captures env
		Dbg("[ENV] screen=%dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
		EnumWindows(EnumWndProc, 0);
	}

	LogVideoMode();

	g_camPtr = CameraPhys();
	float a = Alpha();
	if (g_doCamera && g_camPtr)
	{
		int n = g_camZ ? 3 : 2;           // [34/35/36]=prev x/y/z ; [38/39/40]=cur x/y/z
		for (int i = 0; i < n; i++)
		{
			int32_t p = g_camPtr[34 + i];
			int32_t c = g_camPtr[38 + i];
			g_savedXY[i] = c;
			if (llabs((int64_t)c - p) <= TELE)   // snap (don't swoop) on map-warps / level jumps
			{
				g_camPtr[38 + i] = p + (int32_t)llround((double)(c - p) * a);
			}
		}
	}
	CP(21);
	g_savedObj.clear();
	if (g_doObjs)
	{
		g_extraInterp = 0;
		memset(g_kindSeen, 0, sizeof g_kindSeen);
		memset(g_kindMove, 0, sizeof g_kindMove);
		memset(g_kindLerp, 0, sizeof g_kindLerp);
		memset(g_kindDead, 0, sizeof g_kindDead);

		if (!g_useDeadSet || (g_frame % 30) == 0)
			CollectLive();

		for (int32_t* d : g_moved)
		{
			g_lastObj = d; CP(23);

			auto it = g_obj.find(d);
			
			if (it == g_obj.end())
				continue;

			Obj& s = it->second;

			int kk = SafeKind(d);
			
			if (kk < 0 || kk > 15)
				kk = 15;

			g_kindSeen[kk]++;

			if (s.px != s.cx || s.py != s.cy)
				g_kindMove[kk]++;

			bool alive;

			if (g_useDeadSet)
			{
				alive = (g_dead.count(d) == 0);
			}
			else
			{
				alive = (g_live.count(d) != 0);
				if (alive)
					s.lastLive = g_tick;
			}
			if (!alive)
			{
				++g_deadSkips;
				g_kindDead[kk]++;
				if (!g_useDeadSet && s.moves > 90 && s.lastLive == 0)
				{
					++g_missCount;
					if ((g_missCount % 400) == 1)
						Dbg("[MISS] persistent mover NEVER live d=%p moves=%u cur=(%d,%d)\n", d, s.moves, s.cx, s.cy);
				}
				continue;
			}

			if (g_useDeadSet && g_live.count(d) == 0)
				++g_extraInterp;

			if (s.px == s.cx && s.py == s.cy)
				continue;

			if (llabs((int64_t)s.cx - s.px) > TELE || llabs((int64_t)s.cy - s.py) > TELE)
				continue;

			int32_t lx = s.px + (int32_t)llround((double)(s.cx - s.px) * a);
			int32_t ly = s.py + (int32_t)llround((double)(s.cy - s.py) * a);

			int32_t ox, oy;

			if (SafeSaveWrite(d, lx, ly, &ox, &oy))
			{
				if (g_preDirty)
					ForceDirtyNow(d);   // <-- ghost fix: dirty before draw, not just on restore
				g_savedObj.push_back({ d, ox, oy });
				g_kindLerp[kk]++;

				if (ox != s.cx || oy != s.cy)
				{
					++g_dualCount;
					if ((g_dualCount % 400) == 1)
						Dbg("[DUALW] d=%p tracked=(%d,%d) actual=(%d,%d)\n", d, s.cx, s.cy, ox, oy);
				}
			}
			else
			{
				static int n = 0;
				if (n++ < 50)
					Dbg("[SEH] dead obj on WRITE d=%p tick=%u\n", d, g_tick);
			}
		}
	}

	CP(24);
	CP(25);
	char r = oRender();
	CP(26);

	for (auto& s : g_savedObj)
		SafeRestore(s.d, s.x, s.y, g_forceDirty);

	if (g_doCamera && g_camPtr)
	{
		g_camPtr[38] = g_savedXY[0];
		g_camPtr[39] = g_savedXY[1];
		if (g_camZ)
			g_camPtr[40] = g_savedXY[2];
	}
	CP(27);
	g_inRender = false;
	DWORD rd = timeGetTime() - rt0;
	g_renderDurSum += rd;
	if (rd > g_renderDurMax)
		g_renderDurMax = rd;
	++g_renderN;

	return r;
}
static char __cdecl Hook_Tick()
{
	CP(1);
	char r = oTick();
	DWORD due = *(DWORD*)g.pNextDue;
	DWORD now = timeGetTime();
	if (due != g_prevDue)
	{
		DWORD ft = due - g_prevDue;
		if (g_prevDue && ft > 0 && ft < 500)
			g_frametime = ft;
		g_prevDue = due;
	}

	bool canAfford = !g_adaptivePump || (g_lastDrawCPU * 2.0 + 4.0) <= g_pumpBudgetMs;
	if (g_doPump && canAfford && CameraPhys() && (int)(due - now) > 6)
	{
		DWORD cap = (DWORD)(1000.0 / g_capFps);
		if ((now - g_lastPump) >= cap)
		{
			g_lastPump = now;
			++g_pumpN;
			CP(2);
			((fn_t)g.render)(); 
		}
	}
	else if (g_doPump && !canAfford)
	{
		++g_pumpSkips;
	}
	CP(3);
	return r;
}
static void Init()
{
	LoadConfig();                       // read ini FIRST (sets g_logEnabled, ...)
	
	if (g_logEnabled)
		LogOpen();

	AddVectoredExceptionHandler(1, Veh);
	SetUnhandledExceptionFilter(Uef);

	MODULEINFO mi;
	if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof mi))
	{
		g_imgBase = (uintptr_t)mi.lpBaseOfDll;
		g_imgTop = g_imgBase + mi.SizeOfImage;
	}

	if (HMODULE dh = LoadLibraryA("dbghelp.dll"))
		p_MiniDumpWriteDump = (fn_MiniDumpWriteDump)GetProcAddress(dh, "MiniDumpWriteDump");

	Dbg("[60] ==== ddraw proxy start ==== pump=%d(adapt=%d,budget=%.0f) objs=%d cam=%d camz=%d fdirty=%d predirty=%d unifyres=%d deadset=%d log=%d child=%d map=%d cap=%.0f\n",
		g_doPump, g_adaptivePump, g_pumpBudgetMs, g_doObjs, g_doCamera, g_camZ, g_forceDirty, g_preDirty, g_unifyRes, g_useDeadSet, g_logEnabled, g_childProfile, g_mapProfile, g_capFps);
	
	timeBeginPeriod(1);
	QueryPerformanceFrequency(&g_qpf);

	if (g_mapProfile)
	{
		LARGE_INTEGER q0, q1;
		uint64_t t0, t1;

		QueryPerformanceCounter(&q0);
		t0 = __rdtsc();

		Sleep(50);
		t1 = __rdtsc();
		QueryPerformanceCounter(&q1);

		double qms = g_qpf.QuadPart ? (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart : 1.0;

		g_tscPerMs = qms > 0.0 ? (double)(t1 - t0) / qms : 0.0;

		Dbg("[MAP] tsc calib: %.0f tsc/ms (qpf=%lld)\n", g_tscPerMs, (long long)g_qpf.QuadPart);
	}

	UseHardcoded();

	g_obj.reserve(4096);
	g_moved.reserve(1024);
	g_savedObj.reserve(1024);
	g_live.reserve(4096);
	g_liveVec.reserve(4096);
	g_dead.reserve(4096);

	CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);

	if (MH_Initialize() != MH_OK)
	{
		Dbg("[60] MH_Initialize FAILED\n");
		return;
	}

	if (MH_CreateHook((void*)g.tick, (void*)&Hook_Tick, (void**)&oTick) != MH_OK) Dbg("[60] hook tick FAILED\n");
	if (MH_CreateHook((void*)g.render, (void*)&Hook_Render, (void**)&oRender) != MH_OK) Dbg("[60] hook render FAILED\n");
	if (MH_CreateHook((void*)g.logic, (void*)&Hook_Logic, (void**)&oLogic) != MH_OK) Dbg("[60] hook logic FAILED\n");
	if (MH_CreateHook((void*)g.pos, (void*)&Hook_Pos, (void**)&oPos) != MH_OK) Dbg("[60] hook pos FAILED\n");
	if (MH_CreateHook((void*)g.dtor, (void*)&Hook_Dtor, (void**)&oDtor) != MH_OK) Dbg("[60] hook dtor FAILED\n");
	if (MH_CreateHook((void*)g.scene, (void*)&Hook_SceneDraw, (void**)&oSceneDraw) != MH_OK) Dbg("[60] hook scene FAILED\n");

	if (g_unifyRes)
	{
		if (MH_CreateHook((void*)g.modegate, (void*)&Hook_ModeGate, (void**)&oModeGate) != MH_OK)
			Dbg("[60] hook modegate FAILED\n");
	}

	if (g_childProfile || g_mapProfile)
	{
		if (MH_CreateHook((void*)g.maplayers, (void*)&Hook_MapLayers, (void**)&oMapLayers) != MH_OK) Dbg("[60] hook maplayers FAILED\n");
	}
	if (g_mapProfile)
	{
		if (MH_CreateHook((void*)g.tileadd, (void*)&Hook_TileAddUnique, (void**)&oTileAdd) != MH_OK) Dbg("[60] hook tileadd FAILED\n");
	}

	if (g_childProfile)
	{
		if (MH_CreateHook((void*)g.surf, (void*)&Hook_Surf, (void**)&oSurf) != MH_OK) Dbg("[60] hook surf FAILED\n");
		if (MH_CreateHook((void*)g.spr2, (void*)&Hook_Spr2, (void**)&oSpr2) != MH_OK) Dbg("[60] hook spr2 FAILED\n");
		if (MH_CreateHook((void*)g.ui, (void*)&Hook_Ui, (void**)&oUi) != MH_OK) Dbg("[60] hook ui FAILED\n");
	}

	if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
		Dbg("[60] enable FAILED\n");
	Dbg("[60] hooks armed\n");

	LogLaunchEnv();
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID)
{
	if (r == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(h);
		CreateThread(nullptr, 0, [](LPVOID)->DWORD { Init(); return 0; }, nullptr, 0, nullptr);
	}
	return TRUE;
}