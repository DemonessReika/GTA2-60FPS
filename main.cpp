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
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <tlhelp32.h>
#include "MinHook.h"
#include <ddraw.h>   // near the top with the other includes

#include "gta2_ws_compat.h"

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
static bool g_DumpOnCrash = false; // create dump file on crash

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
static bool g_debugLogic = false;

static int g_wsCompat = 1;   // ws_compat: 0 off / 1 auto / 2 force

// ---- adaptive pump (present-cost-gated) ----
static bool     g_adaptivePump = true;
static double   g_debt = 0.0;            // DIAGNOSTIC only now (not a gate)
static DWORD    g_prevTickWall = 0;
static uint32_t g_pumpSkips = 0;
static double   g_lastDrawCPU = 16.0;    // EMA of Render_DrawScene ms (scene BUILD only; cheap, NOT the gate)
static double   g_lastPresentCPU = 16.0; // EMA of FULL present (Hook_Render incl Flip/BitBlt); THIS gates the pump
static double   g_pumpBudgetMs = 42.0;   // legacy knob (no longer gates; kept for ini compat + logging)

// ---- fix car/ped/object flicker on spawn ----
static uint32_t g_spawnObjSuppressRenderForTicks = 3;   // "spawn_object_rendertick_fix_warmup_tick_time": ticks to suppress interp after first appearance
static uint32_t g_spawnObjSkips = 0;	// debug stuff?
static int32_t g_maxObjMove = 4 << 14;   // "maxmove" tiles: lerp deltas bigger than this snap (relocate/recycle), never swoop

// ---- map-layer / tile-dedup profiler (diagnostic; flag: mapprofile, DEFAULT OFF) ----
static bool     g_mapProfile = false;
static LARGE_INTEGER g_qpf = { 0 };
static double   g_tscPerMs = 0.0;
static uint32_t g_mapCalls = 0;
static uint64_t g_dedupTsc = 0;      static uint32_t g_dedupCalls = 0;
static uint64_t g_dedupScan = 0;     static uint32_t g_dedupMax = 0;

// ---- gang / mission HUD arrow interp (screen-space marker slots) ---------
static bool     g_doArrows = true;       // "arrows"
static int32_t  g_arrowSnap = 120 << 14;   // "arrowsnap" px(Q14): per-tick delta above this => snap
static uint32_t g_arrowWarn = 0;
struct ArrowState { int32_t px = 0, py = 0, cx = 0, cy = 0; uint32_t tick = 0xFFFFFFFFu; bool wasEn = false; };
static std::unordered_map<int32_t*, ArrowState> g_arrow;
typedef int(__fastcall* fn_slots_t)(void*, void*);
static fn_slots_t oSlots = nullptr;

//

static bool g_bApply32bpp = true;



/// BONUS CONTENT 



static bool g_tileListOptimization = true;   // ini "tilelist_opt"

// ---- manual zoom: drive HEIGHT target [6]; engine propagates to [40]->[30..33]->[8..11] ----
static bool     g_zoom = true;     // "zoom"
static int32_t  g_zoomOffset = 0;         // persistent user offset (Q14), additive on speed-zoom base
static int32_t  g_zoomStep = 2048;      // "zoomstep"  per-tick while held (0.125/tick)
static int32_t  g_zoomMax = 73728;     // "zoommax"   +5.0  -> total height ~6.0
static int32_t  g_zoomMin = 0;         // "zoommin"   0 = never dip below speed-zoom baseline
static int      g_zoomKeys = 1;         // "zoomkeys"  0=numpad +/- , 1=main-row -/=
static const int32_t Z_ENGINE_MAX = 14 << 14;   // 229376, straight from Camera_ClampZoom (5E3D54)


// HUD Fix

static bool g_menuScale = true;                // menu_scale_fix
static bool g_skipIntro = false;	// skip_intro

// ---- HUD banner shrink (area-name + vehicle-name plaques) ----
static bool    g_bannerShrink = false;   // ini "banner_shrink"
static int     g_bannerPct = 75;     // ini "banner_scale" (percent; 100 = original)
static double  g_bannerK = 0.75;
static bool    g_inBanner = false;  // true only while a banner fn runs (main thread, sync)
static bool    g_bannerUiPatched = false;
static int32_t g_bannerSavedUi = 0;
static int32_t g_bannerOffX = 0, g_bannerOffY = 0;   // Q14 virtual, added to x/y in shared draw fns

// ---- banner dedup: suppress repeat area/car banners until map reload ----
static bool g_bannerDedup = false;   // ini "banner_dedup"
static bool g_bannerDedupCar = false;   // ini "banner_dedup_car"

struct BannerDedupState
{
	std::unordered_set<std::wstring> seen;   // strings shown >=1x this level
	std::wstring curEpisode;                 // string we've already decided on
	bool curSuppressed = false;              // the decision for curEpisode
};

static BannerDedupState g_dedupArea, g_dedupCar;
typedef char* (__fastcall* fn_levelstart_t)(void*, void*);
typedef char* (__cdecl* fn_sessionteardown_t)();
static fn_levelstart_t oLevelStart = nullptr;
static fn_sessionteardown_t oSessionTeardown = nullptr;


typedef void(__fastcall* fn_banner_t)(void*, void*);
static fn_banner_t oAreaBanner = nullptr, oVehBanner = nullptr;
typedef int(__stdcall* fn_drawicon_t)(int, int, int, int, int, int, int, int, int);
typedef void(__stdcall* fn_drawsprite_t)(int, int, int, int, int, int, int, int);
typedef void(__stdcall* fn_drawscaledstr_t)(int, int, int, int);
static fn_drawicon_t      oDrawIcon = nullptr;
static fn_drawsprite_t    oDrawSprite = nullptr;
static fn_drawscaledstr_t oDrawScaledStr = nullptr;
static int  g_resWidth = 0, g_resHeight = 0;   // res_width / res_height (0 = use registry)


static bool g_audioWarmUp = true;

/// BONUS CONTENT ^

typedef int(__fastcall* fn_maplayers_t)(void*, void*);
static fn_maplayers_t oMapLayers = nullptr;
typedef int(__fastcall* fn_tileadd_t)(void*, void*, void*, void*);
static fn_tileadd_t oTileAdd = nullptr;


struct {
	uintptr_t tick, render, logic, pos, pSession, pNextDue, pCarHead, pPedHead, dtor, scene,
		maplayers, tileadd, surf, spr2, ui, modegate, slots, camcommit,
		areaBanner, vehBanner, drawIcon, drawSprite, drawScaledStr,
		levelStart, sessionTeardown;
} g;


static void UseHardcoded()
{
	g.tick = 0x462A30;			// Game_FrameGate
	g.render = 0x461960;		// Game_RenderPresent
	g.logic = 0x461930;			// Game_LogicTick
	g.pos = 0x420600;			// Disp::SetPos
	g.pSession = 0x5EB4FC;		// g_session
	g.pNextDue = 0x662748;		// g_nextTickDue
	g.pCarHead = 0x5E4CA0;		// g_carPool
	g.pPedHead = 0x5E5BBC;		// g_pedPool
	g.dtor = 0x4BDC40;			// ~Disp:: (Disp object destructor)
	g.scene = 0x45A5A0;			// Render_DrawScene (parent; we time its whole cost as drawCPU)
	g.maplayers = 0x472110;		// Render_MapLayers: 8-layer iso compositor (child; measured tiny)
	g.tileadd = 0x46BB90;		// TileList_AddUnique: O(n^2) dedup — real, scales with zoom-out
	g.surf = 0x4CAEC0;			// Render_SetupSurface (Vid_GetSurface + MakeScreenTable + gbh_SetWindow)
	g.spr2 = 0x4B98B0;			// Render_StackSprites: stacked/tall-sprite draw (object-bound)
	g.ui = 0x4CA440;			// Hud_DrawAll: (ENTIRE HUD pass)
	g.modegate = 0x4CB290;		// Vid_ModeGate: frontend/gameplay res decision (unifyres hook target)
	g.slots = 0x4C84C0;			// Marker_DrawAll: Mission arrow stuff.
	g.camcommit = 0x41F2F0;		// Camera::Commit
	// --- v102: banner shrink ---
	g.areaBanner = 0x4C9890;  // Hud_DrawAreaNameBanner    (Hud_DrawAll call #8)
	g.vehBanner = 0x4C94F0;  // Hud_DrawVehicleNameBanner (Hud_DrawAll call #9)
	g.drawIcon = 0x4C71B0;  // Hud_DrawIcon          (virtual Q14 -> *uiScale -> screen; plaques)
	g.drawSprite = 0x4C7280;  // Hud_DrawSprite        (area-banner text)
	g.drawScaledStr = 0x4BA2C0;  // Hud_DrawScaledString  (vehicle-banner text)
	//
	g.levelStart = 0x45B5F0;  // Level_SpawnAndStart (clears dedup sets on map change/restart)
	g.sessionTeardown = 0x462060;	// Session_Teardown: Game exit or entering back main menu
}

static BYTE* const g_pFrontendFlag = (BYTE*)0x595018;  // g_frontendActive: 1 while frontend/menu active

static uintptr_t g_d3dBase = 0;

// ---- logging ----------------------------------------------------------
// ---- async logging: lock-free MPSC ring + dedicated flush thread -------------------
// Producers (game-loop hooks, Watchdog, VEH on ANY thread) only CLAIM->WRITE->PUBLISH a
// slot. They never touch the file, never block, never lock. One flush thread drains to disk.
static FILE* g_log = nullptr;
static const uint32_t   LOG_SLOTS = 8192;        // ring depth (MUST be power of two)
static const uint32_t   LOG_MASK = LOG_SLOTS - 1;
static const uint32_t   LOG_PAYLOAD = 512;         // text bytes/slot (matches old b[512])
struct LogSlot
{
	std::atomic<uint32_t> seq;      // Vyukov publish sequence
	uint32_t              stamp;    // timeGetTime() at enqueue (real event time)
	uint32_t              len;
	char                  text[LOG_PAYLOAD];
};
static LogSlot* g_logRing = nullptr;
static std::atomic<uint32_t> g_logEnq{ 0 };   // producers claim tickets here (CAS)
static uint32_t              g_logDeq = 0;     // consumer cursor (guarded by g_logDrainCs)
static std::atomic<uint32_t> g_logDrops{ 0 };
static uint32_t              g_logDropsReported = 0;
static CRITICAL_SECTION      g_logDrainCs;     // serialises the CONSUMER side only (flush thd vs crash drain)
static HANDLE                g_logThread = nullptr;
static volatile bool         g_logRun = true;
static bool                  g_logMirrorODS = false;  // optional: flush thread also does OutputDebugStringA
static void LogOpen()
{

	char path[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);
	char* s = strrchr(path, '\\');
	if (s) strcpy(s + 1, "gta2_60fps.log"); else strcpy(path, "gta2_60fps.log");
	g_log = fopen(path, "w");
}
// ---- producer: hot path, lock-free, reentrancy-safe --------------------
void Dbg(const char* f, ...)
{
	if (!g_logEnabled) return;                      // master switch: ~free when off
	char b[LOG_PAYLOAD];
	va_list a; va_start(a, f);
	int n = vsnprintf(b, sizeof b, f, a);           // MUST format here: varargs die after return
	va_end(a);
	if (n < 0) n = 0;
	if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;   // bytes actually in b
	if (!g_logRing) return;                         // ring not up yet (very early boot)
	uint32_t pos = g_logEnq.load(std::memory_order_relaxed);
	for (;;)
	{
		LogSlot& s = g_logRing[pos & LOG_MASK];
		uint32_t seq = s.seq.load(std::memory_order_acquire);
		int32_t  dif = (int32_t)(seq - pos);
		if (dif == 0)
		{
			if (g_logEnq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
			{
				s.stamp = timeGetTime();
				s.len = (uint32_t)n;
				memcpy(s.text, b, n);
				s.seq.store(pos + 1, std::memory_order_release);   // publish
				return;
			}
			// CAS failed: 'pos' reloaded by ref -> loop
		}
		else if (dif < 0)
		{
			g_logDrops.fetch_add(1, std::memory_order_relaxed);    // ring full -> drop newest
			return;
		}
		else
		{
			pos = g_logEnq.load(std::memory_order_relaxed);        // another producer won; retry
		}
	}
}
// ---- consumer: single-at-a-time (flush thread OR crash drain) ----------
static bool LogPopLocked(LogSlot& out)   // caller holds g_logDrainCs
{
	uint32_t pos = g_logDeq;
	LogSlot& s = g_logRing[pos & LOG_MASK];
	uint32_t seq = s.seq.load(std::memory_order_acquire);
	int32_t  dif = (int32_t)(seq - (pos + 1));
	if (dif != 0) return false;                     // empty (dif<0)
	out.stamp = s.stamp;
	out.len = s.len > LOG_PAYLOAD ? LOG_PAYLOAD : s.len;
	memcpy(out.text, s.text, out.len);
	s.seq.store(pos + LOG_MASK + 1, std::memory_order_release);   // free slot for reuse
	g_logDeq = pos + 1;
	return true;
}
static void LogDrain(bool doFlush)
{
	if (!g_logRing) return;
	EnterCriticalSection(&g_logDrainCs);
	LogSlot tmp; bool any = false;
	while (LogPopLocked(tmp))
	{
		any = true;
		if (g_log) fprintf(g_log, "[%u] %.*s", tmp.stamp, (int)tmp.len, tmp.text);
		if (g_logMirrorODS)
		{
			char line[LOG_PAYLOAD + 1];
			int m = (int)(tmp.len > LOG_PAYLOAD ? LOG_PAYLOAD : tmp.len);
			memcpy(line, tmp.text, m); line[m] = 0;
			OutputDebugStringA(line);
		}
	}
	uint32_t drops = g_logDrops.load(std::memory_order_relaxed);
	if (drops != g_logDropsReported && g_log)
	{
		fprintf(g_log, "[%u] [LOG] *** %u msgs dropped (ring full) ***\n",
			timeGetTime(), drops - g_logDropsReported);
		g_logDropsReported = drops; any = true;
	}
	if (any && doFlush && g_log) fflush(g_log);
	LeaveCriticalSection(&g_logDrainCs);
}
static DWORD WINAPI LogFlushThread(LPVOID)
{
	while (g_logRun) { LogDrain(true); Sleep(2); }  // simple poll; cheap
	LogDrain(true);                                 // final drain on stop
	return 0;
}
static void LogInitRing()
{
	InitializeCriticalSection(&g_logDrainCs);
	g_logRing = new LogSlot[LOG_SLOTS];
	for (uint32_t i = 0; i < LOG_SLOTS; i++)
		g_logRing[i].seq.store(i, std::memory_order_relaxed);   // Vyukov init
	g_logEnq.store(0, std::memory_order_relaxed);
	g_logDeq = 0; g_logRun = true;
	g_logThread = CreateThread(nullptr, 0, LogFlushThread, nullptr, 0, nullptr);
}
// best-effort drain for the fatal path: never hang death on a wedged flush thread
static void LogDrainForCrash()
{
	if (!g_logRing) return;
	g_logRun = false;                               // ask flush thread to stop looping
	DWORD deadline = timeGetTime() + 300;
	while (!TryEnterCriticalSection(&g_logDrainCs))
	{
		if ((int)(timeGetTime() - deadline) >= 0) return;   // give up; thread holds it (it's draining anyway)
		Sleep(1);
	}
	LogSlot tmp;
	while (LogPopLocked(tmp))
		if (g_log) fprintf(g_log, "[%u] %.*s", tmp.stamp, (int)tmp.len, tmp.text);
	if (g_log) fflush(g_log);
	LeaveCriticalSection(&g_logDrainCs);
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
		int w = *(int*)0x673584 + 1;   // width-1  (set by Render_SetupSurface aka 0x4CAEC0)
		int h = *(int*)0x673474 + 1;   // height-1 (set by Render_SetupSurface aka 0x4CAEC0)
		if (w != lw || h != lh)
		{
			lw = w; lh = h;
			const char* r = GetModuleHandleA("d3ddll.dll") ? "d3ddll(D3D)" :
				GetModuleHandleA("3dfx.dll") ? "3dfx" :
				GetModuleHandleA("dmaglide.dll") ? "dmaglide(Glide)" : "?";
			Dbg("[VID] backbuffer=%dx%d renderer=%s\n", w, h, r);
			__try
			{
				uintptr_t base = *(uintptr_t*)0x673D20;               // g_surface, engine surface struct
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
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
//

static void FrontendScale(float& s, int& ox, int& oy)
{
	int W = 640, H = 480;
	__try {
		uintptr_t surf = *(uintptr_t*)0x673D20;                 // g_surface
		if (surf > 0x10000 && surf < 0x7FFFFFFF) {
			int w = *(int*)(surf + 72), h = *(int*)(surf + 76);
			if (w >= 320 && w <= 7680 && h >= 240 && h <= 4320) { W = w; H = h; }
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	float sx = (float)W / 640.0f, sy = (float)H / 480.0f;
	s = sx < sy ? sx : sy;
	ox = (int)llround((W - 640.0 * s) / 2.0);
	oy = (int)llround((H - 480.0 * s) / 2.0);
}
// content: scale the 4 verts (x@+0, y@+4, stride 8 floats). frontend-only, in place (scratch).
typedef void(__stdcall* fn_gbhquad_t)(int, int, int, int);
static fn_gbhquad_t o_gbhDrawQuad = nullptr;
static void __stdcall Hook_gbhDrawQuad(int a1, int a2, int a3, int a4)
{
	if (g_menuScale && *g_pFrontendFlag && a3) {
		__try {
			float s; int ox, oy; FrontendScale(s, ox, oy);
			if (s != 1.0f || ox || oy) {
				float* v = (float*)a3;
				for (int i = 0; i < 4; i++) {
					v[i * 8] = v[i * 8] * s + ox;
					v[i * 8 + 1] = v[i * 8 + 1] * s + oy;
				}
				// Gbh_DrawQuad_Internal (0xE02CC0): with flag 0x10000 it rebuilds
				// verts 1..3 from TEXTURE w/h (tex+14/+16) reading ONLY vert0.xy, so
				// our scaled SIZE is discarded and only POSITION survives => glyphs
				// land at scaled pen positions but draw at native size ("spaced + thin",
				// city number drifts out of its circle). Clear 0x10000 so all 4 scaled
				// verts are honoured; keep the 0x20000 the engine force-sets alongside
				// it (texture-address render state) so addressing is unchanged.
				if (a1 & 0x10000) a1 = (a1 | 0x20000) & ~0x10000;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	o_gbhDrawQuad(a1, a2, a3, a4);
}
// background: reimplement the Blt with a scaled (stretched) dest rect. frontend-only.
typedef int8_t(__stdcall* fn_gbhblit_t)(int, int, int, int, int, int, int);
static fn_gbhblit_t o_gbhBlitImage = nullptr;
static int8_t __stdcall Hook_gbhBlitImage(int slot, int sl, int st, int sr, int sb, int dx, int dy)
{
	if (!g_menuScale || !*g_pFrontendFlag)
		return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);
	__try {
		int count = *(int*)(g_d3dBase + 0x13898);                 // dword_E13898
		if (slot < 0 || slot >= count) return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);
		unsigned char* table = *(unsigned char**)(g_d3dBase + 0x13894);  // dword_E13894
		if (!table) return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);
		unsigned char* e = table + 16 * slot;
		if (!*(int*)e) return -1;
		IDirectDrawSurface7* img = *(IDirectDrawSurface7**)(e + 12);
		if (!img || img->IsLost() != DD_OK)                       // lost -> let original restore (returns -10)
			return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);
		IDirectDrawSurface7* target = *(IDirectDrawSurface7**)(*(uintptr_t*)(g_d3dBase + 0x13DC8) + 312);
		if (!target) return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);
		float s; int ox, oy; FrontendScale(s, ox, oy);
		int rL = dx, rT = dy, rR = dx - sl + sr, rB = dy - st + sb;               // original's 1:1 dest
		RECT dst = { (LONG)(llround(rL * s) + ox), (LONG)(llround(rT * s) + oy),
					 (LONG)(llround(rR * s) + ox), (LONG)(llround(rB * s) + oy) };
		RECT src = { sl, st, sr, sb };
		HRESULT hr = target->Blt(&dst, img, &src, DDBLT_WAIT, nullptr);
		if (hr == DD_OK) return 0;
		return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);            // blt failed -> fall back
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return o_gbhBlitImage(slot, sl, st, sr, sb, dx, dy);
	}
}
//

typedef int(__stdcall* fn_gbhloadimg_t)(unsigned char*);
static fn_gbhloadimg_t o_gbhLoadImage = nullptr;
static inline int Mask_Shift(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; ++s; } return s; }
static inline int Mask_Size(DWORD m) { while (m && !(m & 1)) m >>= 1; int n = 0; while (m & 1) { m >>= 1; ++n; } return n; }
static bool g_menuFmtLogged = false;
static void FixMenuImage32(const unsigned char* a1, int slot)
{
	if (!g_d3dBase || slot < 0) return;
	__try {
		int count = *(int*)(g_d3dBase + 0x13898);                       // dword_E13898
		if (slot >= count) return;
		unsigned char* table = *(unsigned char**)(g_d3dBase + 0x13894); // dword_E13894 (ptr var)
		if (!table) return;
		IDirectDrawSurface7* surf = *(IDirectDrawSurface7**)(table + 16 * slot + 12);
		if (!surf) return;
		DDSURFACEDESC2 d; memset(&d, 0, sizeof d); d.dwSize = sizeof d;
		if (FAILED(surf->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
		if (!g_menuFmtLogged) {
			g_menuFmtLogged = true;
			Dbg("[MENU32] slot=%d bpp=%u pitch=%d W=%u H=%u R=%08X G=%08X B=%08X\n",
				slot, d.ddpfPixelFormat.dwRGBBitCount, (int)d.lPitch,
				*(const uint16_t*)(a1 + 12), *(const uint16_t*)(a1 + 14),
				d.ddpfPixelFormat.dwRBitMask, d.ddpfPixelFormat.dwGBitMask, d.ddpfPixelFormat.dwBBitMask);
		}
		if (d.ddpfPixelFormat.dwRGBBitCount == 32 && d.lpSurface) {
			const int W = *(const uint16_t*)(a1 + 12);
			const int H = *(const uint16_t*)(a1 + 14);
			int rL = Mask_Shift(d.ddpfPixelFormat.dwRBitMask) + Mask_Size(d.ddpfPixelFormat.dwRBitMask) - 5;
			int gL = Mask_Shift(d.ddpfPixelFormat.dwGBitMask) + Mask_Size(d.ddpfPixelFormat.dwGBitMask) - 5;
			int bL = Mask_Shift(d.ddpfPixelFormat.dwBBitMask) + Mask_Size(d.ddpfPixelFormat.dwBBitMask) - 5;
			if (rL < 0) rL = 0; if (gL < 0) gL = 0; if (bL < 0) bL = 0;
			const unsigned char* src = a1 + a1[0] + 18;    // matches v9 = &a1[*a1 + 18]
			unsigned char* base = (unsigned char*)d.lpSurface;
			for (int y = 0; y < H; ++y) {
				uint32_t* row = (uint32_t*)(base + (intptr_t)d.lPitch * (H - 1 - y)); // src top -> dst bottom
				for (int x = 0; x < W; ++x) {
					uint16_t w = (uint16_t)(src[0] | (src[1] << 8)); src += 2;
					row[x] = (((uint32_t)(w & 0x1F)) << bL)    // src lo  -> B   (exact
						| (((uint32_t)((w >> 5) & 0x1F)) << gL)    // src mid -> G    replica of
						| (((uint32_t)((w >> 10) & 0x1F)) << rL);   // src hi  -> R    the original)
				}
			}
		}
		surf->Unlock(nullptr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int __stdcall Hook_gbhLoadImage(unsigned char* a1)
{
	int slot = o_gbhLoadImage(a1);
	if (slot >= 0 && g_bApply32bpp) FixMenuImage32(a1, slot);
	return slot;
}

// Install lazily: Frontend_LoadMenuBgImages (0x452E20) runs at frontend-enter,
// long after our Init() and after d3ddll is loaded -> no boot race.
typedef void(__cdecl* fn_loadbg_t)();
static fn_loadbg_t o_FrontendLoadMenuBgImages = nullptr;
static void __cdecl Hook_FrontendLoadMenuBgImages()
{
	static bool once = false;
	if (!once) {
		once = true;
		HMODULE h = GetModuleHandleA("d3ddll.dll");
		if (h) {
			g_d3dBase = (uintptr_t)h;
			if (g_bApply32bpp) {
				void* p = (void*)GetProcAddress(h, "gbh_LoadImage");
				if (p && MH_CreateHook(p, (void*)&Hook_gbhLoadImage, (void**)&o_gbhLoadImage) == MH_OK) MH_EnableHook(p);
				Dbg("[MENU32] LoadImage %s\n", o_gbhLoadImage ? "ok" : "FAIL");
			}
			if (g_menuScale) {
				void* q = (void*)GetProcAddress(h, "gbh_DrawQuad");
				if (q && MH_CreateHook(q, (void*)&Hook_gbhDrawQuad, (void**)&o_gbhDrawQuad) == MH_OK) MH_EnableHook(q);
				void* b = (void*)GetProcAddress(h, "gbh_BlitImage");
				if (b && MH_CreateHook(b, (void*)&Hook_gbhBlitImage, (void**)&o_gbhBlitImage) == MH_OK) MH_EnableHook(b);
				Dbg("[MENU32] scale quad=%s blit=%s\n", o_gbhDrawQuad ? "ok" : "FAIL", o_gbhBlitImage ? "ok" : "FAIL");
			}
		}
		else Dbg("[MENU32] d3ddll not loaded\n");
	}
	o_FrontendLoadMenuBgImages();
}
// -----------------------------------------------------------------------
// Audio file-cache warmer
// Fixes stutter caused by loading sounds during active gameplay (Especially on HDDs)

static uintptr_t g_pMilesDriver = 0x66C950;
static bool LooksLikeAsciiPath(const char* s)
{
	if (!s) return false;
	int n = 0;
	for (; n < 120; ++n) {
		char c = s[n]; if (!c) break;
		if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
	}
	return n >= 2 && n < 120;
}
static bool ReadRadioPrefix(uintptr_t pMiles, char* out, size_t cap)
{
	if (!pMiles) return false;
	__try {
		const char* a = (const char*)(pMiles + 5);                 // form A: struct base, inline string
		if (LooksLikeAsciiPath(a)) { lstrcpynA(out, a, (int)cap); return true; }
		uintptr_t s = *(uintptr_t*)pMiles;                          // form B: pointer var -> deref
		if (s > 0x10000 && s < 0x7FFFFFFF) {
			const char* b = (const char*)(s + 5);
			if (LooksLikeAsciiPath(b)) { lstrcpynA(out, b, (int)cap); return true; }
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return false;
}


static volatile LONG g_audioWarmStarted = 0;
static volatile LONG g_audioWarmFinished = 0;
static HANDLE g_audioWarmThread = nullptr;
struct WarmFileStats
{
	uint32_t files = 0;
	uint64_t bytes = 0;
	uint32_t failures = 0;
};
static bool GetGameDirectoryW(std::wstring& out)
{
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(
		GetModuleHandleW(nullptr),
		path,
		ARRAYSIZE(path));
	if (!n || n >= ARRAYSIZE(path))
		return false;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash)
		return false;
	*slash = L'\0';
	out.assign(path);
	return true;
}
static void WarmOneFileW(const std::wstring& path, WarmFileStats& stats, uint64_t maxBytes /*0 = whole*/)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (h == INVALID_HANDLE_VALUE) { ++stats.failures; return; }
	constexpr DWORD CHUNK = 64 * 1024;
	std::vector<BYTE> buffer(CHUNK);
	uint64_t total = 0;
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(h, buffer.data(), CHUNK, &got, nullptr)) { ++stats.failures; break; }
		stats.bytes += got; total += got;
		if (got == 0 || got < CHUNK) break;
		if (maxBytes && total >= maxBytes) break;   // partial warm (AIL streams anyway)
	}
	CloseHandle(h); ++stats.files;
}
static void WarmDirectoryPatternW(const std::wstring& directory, const wchar_t* pattern,
	WarmFileStats& stats, uint64_t maxBytes = 0)
{
	std::wstring search = directory; search += L"\\"; search += pattern;
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW(search.c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		std::wstring full = directory; full += L"\\"; full += fd.cFileName;
		WarmOneFileW(full, stats, maxBytes);
	} while (FindNextFileW(find, &fd));
	FindClose(find);
}
static DWORD WINAPI WarmAudioThread(LPVOID)
{
	// This thread should never compete aggressively with the game thread.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	SetThreadPriorityBoost(GetCurrentThread(), TRUE);
	std::wstring gameDir;
	if (!GetGameDirectoryW(gameDir))
	{
		Dbg("[AUDIOWARM] unable to determine GTA2 directory\n");
		InterlockedExchange(&g_audioWarmFinished, 1);
		return 0;
	}
	const std::wstring audioDir = gameDir + L"\\data\\Audio";
	const std::wstring vocalsDir = audioDir + L"\\Vocals";
	const std::wstring gtaudioDir = gameDir + L"\\GTAudio";
	WarmFileStats bankStats;
	WarmFileStats vocalStats;
	// Small banks first. These are likely to be used by the engine during
	// level startup and should remain preferentially cached.
	WarmDirectoryPatternW(audioDir, L"*.RAW", bankStats);
	WarmDirectoryPatternW(audioDir, L"*.SDT", bankStats);
	// Voice clips are the primary suspected source of the 200-250 ms hitch.
	WarmDirectoryPatternW(vocalsDir, L"*.WAV", vocalStats);
	// Radio files are large and can evict useful cache pages, so do these last.
	// Radio: derive the real dir from g_milesDriver+5 instead of the bogus GTAudio glob.
	WarmFileStats radioStats;
	char prefix[MAX_PATH] = { 0 };
	if (ReadRadioPrefix(g_pMilesDriver, prefix, sizeof prefix)) {
		std::string p(prefix);
		size_t slash = p.find_last_of("\\/");
		std::string dirA = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
		std::wstring dirW(dirA.begin(), dirA.end());
		if (dirW.size() < 2 || dirW[1] != L':') dirW = gameDir + L"\\" + dirW;   // relative -> anchor
		Dbg("[AUDIOWARM] radio prefix=\"%s\" dir=\"%ls\"\n", prefix, dirW.c_str());
		WarmDirectoryPatternW(dirW, L"*.WAV", radioStats, 1536u * 1024u);        // first ~1.5MB each
	}
	else {
		Dbg("[AUDIOWARM] radio prefix UNAVAILABLE (set g_pMilesDriver = g_milesDriver address)\n");
	}
	Dbg(
		"[AUDIOWARM] finished "
		"banks=%u files/%llu bytes/%u failures, "
		"vocals=%u files/%llu bytes/%u failures, "
		"radio=%u files/%llu bytes/%u failures\n",
		bankStats.files,
		(unsigned long long)bankStats.bytes,
		bankStats.failures,
		vocalStats.files,
		(unsigned long long)vocalStats.bytes,
		vocalStats.failures,
		radioStats.files,
		(unsigned long long)radioStats.bytes,
		radioStats.failures);
	InterlockedExchange(&g_audioWarmFinished, 1);
	return 0;
}
static void StartAudioWarm()
{
	// Only start one worker per process. The Windows cache survives level
	// restarts, so repeating this is normally unnecessary.
	if (InterlockedCompareExchange(&g_audioWarmStarted, 1, 0) != 0)
		return;
	g_audioWarmFinished = 0;
	g_audioWarmThread = CreateThread(
		nullptr,
		0,
		WarmAudioThread,
		nullptr,
		0,
		nullptr);
	if (!g_audioWarmThread)
	{
		InterlockedExchange(&g_audioWarmStarted, 0);
		Dbg("[AUDIOWARM] CreateThread failed, error=%lu\n", GetLastError());
		return;
	}
	Dbg("[AUDIOWARM] worker started\n");
}




// ---- perf: scene-draw timing (isolates the SCENE BUILD cost = drawCPU; NOT the present) ----
static double   g_drawSum = 0;
static DWORD g_drawMax = 0;
static uint32_t g_drawN = 0;

typedef void(__fastcall* fn_scene_t)(void*, void*);

static fn_scene_t oSceneDraw = nullptr;

void Crash_ReinstallSEHFilter();

static double g_presentQPCema = 16.5, g_sceneQPCema = 13.5;

static void __fastcall Hook_SceneDraw(void* thisp, void* edx)
{
	DWORD t = timeGetTime();
	LARGE_INTEGER pa; QueryPerformanceCounter(&pa);
	oSceneDraw(thisp, edx);
	LARGE_INTEGER pb; QueryPerformanceCounter(&pb);
	double pres = (double)(pb.QuadPart - pa.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
	g_sceneQPCema = g_sceneQPCema * 0.9 + pres * 0.1;
	Crash_ReinstallSEHFilter();   // Render_DrawScene tail just ran __CxxRestore... — re-arm NOW
	DWORD d = timeGetTime() - t;
	g_drawSum += d;

	if (d > g_drawMax)
		g_drawMax = d;
	++g_drawN;
	++g_sceneN;                          // PERF3 divisor (scene-draws per heartbeat)

	g_lastDrawCPU = g_lastDrawCPU * 0.6 + (double)d * 0.4;
}
static bool SessionIsPaused()
{
	__try {
		uintptr_t s = *(uintptr_t*)g.pSession;
		return s && *(int*)s == 2;          // *session==2 == paused 
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
typedef int(__fastcall* fn_camcommit_t)(void*, void*);
static fn_camcommit_t oCamCommit = nullptr;
static int __fastcall Hook_CamCommit(void* thisp, void* edx)
{
	if (g_zoom && g_zoomOffset && !SessionIsPaused())
		__try {
		int32_t* cam = (int32_t*)thisp;
		int32_t z = cam[6] + g_zoomOffset;          // [6] = staged height target
		if (z > Z_ENGINE_MAX) z = Z_ENGINE_MAX;     // respect engine's own ceiling
		if (z < 0) z = 0;
		cam[6] = z;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return oCamCommit(thisp, edx);
}

// log stuff

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

// ---- Small performance bonus. Can cut up to 2 to 4 ms sometimes, especially with zoom out
// Turns O(n^2) into O(n)
struct TileXY { int32_t x, y; };
static std::vector<TileXY> g_tiles;
static const int TS_OFF = 256, TS_DIM = 768;          // covers tile coords -256..511
static uint16_t g_tileStamp[TS_DIM][TS_DIM];           // ~1.1 MB, zero-init
static uint16_t g_tileGen = 1;
typedef void(__fastcall* fn_tiledraw_t)(void*, void*, int*, int*);
static const fn_tiledraw_t Tile_DrawAt = (fn_tiledraw_t)0x471F20;
typedef char(__fastcall* fn_tiledrain_t)(void*, void*);
static fn_tiledrain_t oTileDrain = nullptr;
static int __fastcall Hook_TileAddUnique(void* t, void* e, int* px, int* py)   // TileList_DrawUnique 0x46BB90
{
	int x = *px, y = *py;
	unsigned ux = (unsigned)(x + TS_OFF), uy = (unsigned)(y + TS_OFF);
	if (ux < (unsigned)TS_DIM && uy < (unsigned)TS_DIM) {
		uint16_t& st = g_tileStamp[uy][ux];
		if (st == g_tileGen) return 0;             // already listed this layer
		st = g_tileGen;
	}
	else {
		for (size_t i = 0; i < g_tiles.size(); i++)  // way off-map: rare slow path
			if (g_tiles[i].x == x && g_tiles[i].y == y) return 0;
	}
	g_tiles.push_back({ x, y });
	return 1;
}
static char __fastcall Hook_TileDrain(void* t, void* e)                        // TileList_Drain 0x4720E0
{
	for (size_t i = g_tiles.size(); i-- > 0; )        // engine draws newest -> oldest
		Tile_DrawAt(t, e, &g_tiles[i].x, &g_tiles[i].y);
	if (g_tiles.size() > g_dedupMax) g_dedupMax = (uint32_t)g_tiles.size();
	g_tiles.clear();
	if (++g_tileGen == 0) { memset(g_tileStamp, 0, sizeof g_tileStamp); g_tileGen = 1; }
	return 0;
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

static bool g_useDeadSet = true;
static bool g_preDirty = true;   // force-dirty +72 BEFORE draw (fixes stale-cache ghost — doc §2)

static double g_capFps = 60.0;

//

static bool    g_cullFollow = true;        // ini "cull_follow"
static int32_t g_savedRect[4];
static bool    g_savedRectValid = false;

// -----------------------------------------------------------------------
static bool g_camZ = true;         // "cameraz": also interpolate camera HEIGHT [40] (ramps/elevation)
static bool g_unifyRes = true;    // "unifyres": menu resolves to config res => no menu<->game blink

static std::unordered_set<int32_t*> g_dead;

static uint32_t g_dtorHits = 0;
static uint32_t g_extraInterp = 0;


typedef int(__fastcall* fn_dtor_t)(void*, void*);
static fn_dtor_t oDtor = nullptr;


static bool g_Watchdog = false;

static void LoadConfig()
{
	char path[MAX_PATH]; GetModuleFileNameA(GetModuleHandleA(nullptr), path, MAX_PATH);
	char* s = strrchr(path, '\\');
	if (s)
		strcpy(s + 1, "gta2_60fps.ini");
	else
		strcpy(path, "gta2_60fps.ini");
	//
	g_doPump = GetPrivateProfileIntA("60fps", "pump", 1, path) != 0;
	g_adaptivePump = GetPrivateProfileIntA("60fps", "adaptivepump", 1, path) != 0;
	g_pumpBudgetMs = (double)GetPrivateProfileIntA("60fps", "pumpbudget", 42, path);
	if (g_pumpBudgetMs < 20)
		g_pumpBudgetMs = 42.0;
	
	g_doObjs = GetPrivateProfileIntA("60fps", "objects", 1, path) != 0;
	g_doCamera = GetPrivateProfileIntA("60fps", "camera", 1, path) != 0;
	g_camZ = GetPrivateProfileIntA("60fps", "cameraz", 1, path) != 0;
	g_forceDirty = GetPrivateProfileIntA("60fps", "forcedirty", 1, path) != 0;
	g_useDeadSet = GetPrivateProfileIntA("60fps", "deadset", 1, path) != 0;
	g_preDirty = GetPrivateProfileIntA("60fps", "predirty", 1, path) != 0;


	{
		int m = GetPrivateProfileIntA("60fps", "maxmove", 4, path);
		if (m < 1)
			m = 4;
		g_maxObjMove = m << 14;
	}

	g_doArrows = GetPrivateProfileIntA("60fps", "arrows", 1, path) != 0;
	{
		int a = GetPrivateProfileIntA("60fps", "arrowsnap", 120, path);
		if (a < 1)
			a = 120;
		g_arrowSnap = a << 14;
	}

	g_capFps = (double)GetPrivateProfileIntA("60fps", "capfps", 60, path);
	if (g_capFps < 1)
		g_capFps = 60.0;

	// tiles
	g_tileListOptimization = GetPrivateProfileIntA("60fps", "tilelist_opt", 1, path) != 0;
	

	g_spawnObjSuppressRenderForTicks = (uint32_t)GetPrivateProfileIntA("60fps", "spawn_object_rendertick_fix_warmup_tick_time", 3, path);
	g_unifyRes = GetPrivateProfileIntA("60fps", "unifyres", 1, path) != 0;

	g_audioWarmUp = GetPrivateProfileIntA("60fps", "audiocache_warmup", 1, path) != 0;

	//
	g_logEnabled = GetPrivateProfileIntA("60fps", "log", 1, path) != 0;
	g_debugLogic = GetPrivateProfileIntA("60fps", "debug_logic", 0, path) != 0;
	g_childProfile = GetPrivateProfileIntA("60fps", "childprofile", 0, path) != 0;
	g_mapProfile = GetPrivateProfileIntA("60fps", "mapprofile", 0, path) != 0;


	g_DumpOnCrash = GetPrivateProfileIntA("60fps", "dump_on_crash", 0, path) != 0;
	g_Watchdog = GetPrivateProfileIntA("60fps", "stall_watchdog", 0, path) != 0;

	//



	g_zoom = GetPrivateProfileIntA("60fps", "zoom", 1, path) != 0;
	g_zoomOffset = GetPrivateProfileIntA("60fps", "zoom_offset", 0, path);
	g_zoomStep = GetPrivateProfileIntA("60fps", "zoom_step", 2048, path);
	g_zoomMax = GetPrivateProfileIntA("60fps", "zoom_max", 73728, path);
	if (g_zoomMax > 73728)
		g_zoomMax = 73728;
	g_zoomMin = GetPrivateProfileIntA("60fps", "zoom_min", 0, path);
	g_zoomKeys = GetPrivateProfileIntA("60fps", "zoom_keys", 1, path);

	g_bannerDedup = GetPrivateProfileIntA("60fps", "banner_dedup", 0, path) != 0;
	g_bannerDedupCar = GetPrivateProfileIntA("60fps", "banner_dedup_car", 0, path) != 0;

	g_bannerShrink = GetPrivateProfileIntA("60fps", "banner_shrink", 0, path) != 0;
	g_bannerPct = GetPrivateProfileIntA("60fps", "banner_scale", 75, path);
	if (g_bannerPct < 5)
		g_bannerPct = 5;
	if (g_bannerPct > 400)
		g_bannerPct = 400;

	g_bApply32bpp = GetPrivateProfileIntA("60fps", "32bit_mode", 1, path) != 0;
	g_menuScale = GetPrivateProfileIntA("60fps", "menu_scale_fix", 1, path) != 0;
	g_skipIntro = GetPrivateProfileIntA("60fps", "skip_intro", 0, path) != 0;

	g_resWidth = GetPrivateProfileIntA("60fps", "res_width", 0, path);
	g_resHeight = GetPrivateProfileIntA("60fps", "res_height", 0, path);
	g_cullFollow = GetPrivateProfileIntA("60fps", "cull_follow", 1, path) != 0;

	g_wsCompat = GetPrivateProfileIntA("60fps", "ws_compat", 1, path);
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
	uint32_t bornTick = 0xFFFFFFFFu;   // first tick we saw this slot (spawn-warmup anchor)
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
		if (g_debugLogic)
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
		if (g_debugLogic)
			Dbg("[CAL] ped LATCHED kind=%d hits=%d\n", best, bestHits);
	}
	else if (++g_pedTries >= MAX_TRIES)
	{
		g_pedKind = -2;
		if (g_debugLogic)
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



static uintptr_t g_imgBase = 0x400000;
static uintptr_t g_imgTop = 0;
static uintptr_t g_selfBase = 0, g_selfTop = 0;   // our proxy ddraw module range
static volatile LONG g_proxyFaultDumped = 0;      // one-shot deep dump of first in-proxy fault
// Dedup: each unique faulting EIP logs ONCE (+ rare count). VEH fires on ANY thread, so keep it
// lock-light & alloc-free (the insert race is benign). Turns 64MB of identical first-chance AVs
// into a few lines that say WHERE, WHICH MODULE, and HOW MANY.
static struct { uintptr_t eip; volatile LONG count; } g_faultSites[256];
static volatile LONG g_faultSiteN = 0;
static LONG WINAPI Veh(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;
	if (g_expectingFaults) { ++g_suppressedFaults; return EXCEPTION_CONTINUE_SEARCH; }
	uintptr_t eip = ep->ContextRecord->Eip;
	bool inGame = (eip >= g_imgBase && eip < g_imgTop);
	bool inProxy = (g_selfBase && eip >= g_selfBase && eip < g_selfTop);
	if (inProxy && InterlockedExchange(&g_proxyFaultDumped, 1) == 0)
	{
		if (g_debugLogic)
			Dbg("[VEH] *** FIRST in-PROXY fault eip=%08X RVA=0x%X  <-- disASM this rva in ddraw.dll\n",
				(unsigned)eip, (unsigned)(eip - g_selfBase));
		if (g_debugLogic)
			Dbg("[VEH]   eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X\n",
				ep->ContextRecord->Eax, ep->ContextRecord->Ebx, ep->ContextRecord->Ecx,
				ep->ContextRecord->Edx, ep->ContextRecord->Esi, ep->ContextRecord->Edi,
				ep->ContextRecord->Ebp, ep->ContextRecord->Esp);
		uintptr_t sp = ep->ContextRecord->Esp;
		for (int i = 0; i < 96; i++)
		{
			uintptr_t p = sp + (uintptr_t)i * 4, v = 0; int ok = 1;
			__try { v = *(volatile uintptr_t*)p; }
			__except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
			if (!ok) break;
			bool g2 = (v >= g_imgBase && v < g_imgTop);
			bool px = (v >= g_selfBase && v < g_selfTop);
			if (g2 || px)
			{
				if (g_debugLogic)
					Dbg("[VEH]   stk+%02X = %08X  %s\n", i * 4, (unsigned)v, g2 ? "gta2.exe" : "proxy");
			}
		}
	}
	LONG n = g_faultSiteN, idx = -1;
	for (LONG i = 0; i < n && i < 256; i++)
		if (g_faultSites[i].eip == eip) { idx = i; break; }
	LONG c;
	if (idx < 0) {
		idx = InterlockedIncrement(&g_faultSiteN) - 1;
		if (idx < 256) { g_faultSites[idx].eip = eip; g_faultSites[idx].count = 1; c = 1; }
		else c = 2;                      // table full -> never "first" -> stop logging new sites
	}
	else c = InterlockedIncrement(&g_faultSites[idx].count);
	// first hit of each site, then rarely; ALWAYS log anything inside gta2.exe (that's the bug class we care about)
	bool doLog = inGame || (c == 1) || (c % 100000 == 0);
	if (doLog) {
		char mod[64] = "?"; uintptr_t mb = 0; HMODULE hm = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)eip, &hm) && hm) {
			char p[MAX_PATH]; GetModuleFileNameA(hm, p, MAX_PATH);
			char* b = strrchr(p, '\\'); lstrcpynA(mod, b ? b + 1 : p, sizeof mod);
			mb = (uintptr_t)hm;
		}
		ULONG_PTR* xi = ep->ExceptionRecord->ExceptionInformation;
		if (g_debugLogic)
			Dbg("[VEH] AV %s target=%p eip=%08X (%s+0x%X) %s #%ld | cp=%ld tick=%u inRender=%d\n",
				xi[0] ? "WRITE" : "READ", (void*)xi[1], (unsigned)eip, mod,
				(unsigned)(eip - mb), inGame ? "**GTA2**" : "sys/handled", c,
				g_cp, g_tick, (int)g_inRender);
		if (c == 1)
		{
			if (g_debugLogic)
				Dbg("[VEH]   eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
					ep->ContextRecord->Eax, ep->ContextRecord->Ebx, ep->ContextRecord->Ecx,
					ep->ContextRecord->Edx, ep->ContextRecord->Esi, ep->ContextRecord->Edi);
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
// ---- fatal-crash post-mortem ------------------------------------------

typedef BOOL(WINAPI* fn_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
	PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static fn_MiniDumpWriteDump p_MiniDumpWriteDump = nullptr;

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
	if (!g_DumpOnCrash)
		return;

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
		LogDrainForCrash();   // <-- flush whatever's still in the ring, or the crash lines vanish
	}
	ChangeDisplaySettingsA(nullptr, 0);
	return EXCEPTION_EXECUTE_HANDLER;
}
void Crash_ReinstallSEHFilter() { SetUnhandledExceptionFilter(Uef); }
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
//

//
// read the wide banner string at thisp+2; false if gate byte==0 or empty
static bool BannerReadString(void* thisp, std::wstring& out)
{
	wchar_t buf[200]; int n = 0; bool ok = false;
	__try {
		if (*(const uint8_t*)thisp != 0) {
			const wchar_t* s = (const wchar_t*)((const char*)thisp + 2);
			for (; n < 199; n++) { wchar_t c = s[n]; if (!c) break; buf[n] = c; }
			buf[n] = 0; ok = (n > 0);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; n = 0; }
	out.assign(buf, n);
	return ok;
}
// returns true => SUPPRESS this draw (repeat banner)
static bool BannerDedupCheck(BannerDedupState& st, void* thisp)
{
	std::wstring s;
	if (!BannerReadString(thisp, s)) { st.curEpisode.clear(); return false; } // gone
	if (s == st.curEpisode) return st.curSuppressed;                          // same episode
	st.curEpisode = s;                                                        // NEW episode
	bool suppress = (st.seen.find(s) != st.seen.end());
	if (!suppress) st.seen.insert(s);
	st.curSuppressed = suppress;
	if (g_debugLogic)
		Dbg("[BANNER] %s \"%ls\" (seen=%zu)\n", suppress ? "SUPPRESS" : "show", s.c_str(), st.seen.size());

	return suppress;
}
static void BannerDedupReset()
{
	g_dedupArea.seen.clear(); g_dedupArea.curEpisode.clear(); g_dedupArea.curSuppressed = false;
	g_dedupCar.seen.clear();  g_dedupCar.curEpisode.clear();  g_dedupCar.curSuppressed = false;
	if(g_debugLogic)
		Dbg("[60] banner dedup: sets cleared (level start)\n");
}
static char* __fastcall Hook_LevelStart(void* thisp, void* edx)
{
	Dbg("[Level] Level Start\n");
	if(g_bannerDedup || g_bannerDedupCar)
		BannerDedupReset();
	// Start after the level-start hook is entered so the warmer does not
	// delay level initialization on the main thread.

	if(g_audioWarmUp)
		StartAudioWarm();
	return oLevelStart(thisp, edx);
}

static char* __cdecl Hook_SessionTeardown()
{
	Dbg("[Level] Level End/Game Quit?\n");
	return oSessionTeardown();
}
// ---------- goal 1: banner shrink (scale-about-center) ----------
static void BannerBegin(int cyInt)
{
	g_bannerK = (double)g_bannerPct / 100.0;
	if (g_bannerK < 0.05) g_bannerK = 0.05;
	if (g_bannerK > 4.0)  g_bannerK = 4.0;
	double inv = 1.0 / g_bannerK - 1.0;
	g_bannerOffX = (int32_t)llround((double)((int64_t)320 << 14) * inv);   // horiz center = 320
	g_bannerOffY = (int32_t)llround((double)((int64_t)cyInt << 14) * inv);   // vert anchor per-banner
	g_bannerUiPatched = false;
	int32_t* cam = CameraPhys();
	if (cam)
		__try {
		g_bannerSavedUi = cam[42];
		cam[42] = (int32_t)llround((double)g_bannerSavedUi * g_bannerK);
		g_bannerUiPatched = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { g_bannerUiPatched = false; }
	g_inBanner = true;
}
static void BannerEnd()
{
	g_inBanner = false;
	if (g_bannerUiPatched) {
		int32_t* cam = CameraPhys();
		if (cam) __try { cam[42] = g_bannerSavedUi; }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		g_bannerUiPatched = false;
	}
}
static void __fastcall Hook_AreaBanner(void* thisp, void* edx)
{
	if (g_bannerDedup && BannerDedupCheck(g_dedupArea, thisp)) return;   // repeat -> skip
	if (!g_bannerShrink) { oAreaBanner(thisp, edx); return; }
	BannerBegin(27);
	__try { oAreaBanner(thisp, edx); }
	__finally { BannerEnd(); }
}
static void __fastcall Hook_VehBanner(void* thisp, void* edx)
{
	if (g_bannerDedupCar && BannerDedupCheck(g_dedupCar, thisp)) return;   // repeat -> skip
	if (!g_bannerShrink) { oVehBanner(thisp, edx); return; }
	int cy = 10;
	__try { cy = ((int*)thisp)[18]; }
	__except (EXCEPTION_EXECUTE_HANDLER) { cy = 10; }
	BannerBegin(cy);
	__try { oVehBanner(thisp, edx); }
	__finally { BannerEnd(); }
}
// the 3 shared primitives: add the center-pin offset ONLY while a banner is drawing
static int __stdcall Hook_DrawIcon(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9)
{
	if (g_inBanner) { a3 += g_bannerOffX; a4 += g_bannerOffY; }   // a3=x, a4=y (Q14 virtual)
	return oDrawIcon(a1, a2, a3, a4, a5, a6, a7, a8, a9);
}
static void __stdcall Hook_DrawSprite(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8)
{
	if (g_inBanner) { a2 += g_bannerOffX; a3 += g_bannerOffY; }   // a2=x, a3=y
	oDrawSprite(a1, a2, a3, a4, a5, a6, a7, a8);
}
static void __stdcall Hook_DrawScaledStr(int a1, int a2, int a3, int a4)
{
	if (g_inBanner) { a2 += g_bannerOffX; a3 += g_bannerOffY; }   // a2=x, a3=y
	oDrawScaledStr(a1, a2, a3, a4);
}


// -----------------------
static float Alpha()
{
	DWORD due = *(DWORD*)g.pNextDue;
	DWORD ft = g_frametime ? g_frametime : 33;
	double a = (double)(int)(timeGetTime() - (due - ft)) / (double)ft;
	return a < 0 ? 0.f : (a > 1 ? 1.f : (float)a);
}
static int __fastcall Hook_Slots(void* thisp, void* edx)
{
	if (!g_doArrows) return oSlots(thisp, edx);
	if (BadPtr((uintptr_t)thisp)) return oSlots(thisp, edx);   // never probe a junk base
	struct { int32_t* s; int32_t x, y; } touched[17];
	int   nTouched = 0;
	float al = Alpha();
	for (int i = 0; i < 17; i++)
	{
		int32_t* s = (int32_t*)((char*)thisp + (size_t)124 * i);
		if (BadPtr((uintptr_t)s)) continue;                   // guard BEFORE the read, not just __except
		uint8_t  en; int32_t liveX, liveY;
		__try { en = *((uint8_t*)s + 45); liveX = s[0]; liveY = s[1]; }
		__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
		if (!en) {                                    // disabled: arm snap for the re-enable edge
			auto it = g_arrow.find(s);
			if (it != g_arrow.end()) it->second.wasEn = false;
			continue;
		}
		ArrowState& a = g_arrow[s];
		if (a.tick != g_tick) {                       // first render of a new tick: shift cur->prev
			a.px = a.cx; a.py = a.cy;
			a.cx = liveX; a.cy = liveY;
			a.tick = g_tick;
		}
		else {                                      // pumped render, same tick: value must match
			if ((liveX != a.cx || liveY != a.cy) && g_arrowWarn < 20) {
				++g_arrowWarn;
				if(g_debugLogic)
					Dbg("[ARW] per-render write?! i=%d tick=%u wasX=%d nowX=%d\n", i, g_tick, a.cx, liveX);
			}
			a.cx = liveX; a.cy = liveY;
		}
		if (!a.wasEn) { a.px = liveX; a.py = liveY; }  // enable edge: snap (prev is stale)
		if (llabs((int64_t)a.cx - a.px) > g_arrowSnap ||
			llabs((int64_t)a.cy - a.py) > g_arrowSnap) {
			a.px = a.cx; a.py = a.cy;
		}  // retarget/wrap: snap
		a.wasEn = true;
		int32_t lx = a.px + (int32_t)llround((double)(a.cx - a.px) * al);
		int32_t ly = a.py + (int32_t)llround((double)(a.cy - a.py) * al);
		__try {
			s[0] = lx; s[1] = ly;
			touched[nTouched].s = s; touched[nTouched].x = liveX; touched[nTouched].y = liveY; ++nTouched;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	int r = oSlots(thisp, edx);
	for (int i = 0; i < nTouched; i++)
		__try { touched[i].s[0] = touched[i].x; touched[i].s[1] = touched[i].y; }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
}
// ---- menu/gameplay resolution unifier (kills the mode-switch blink) ----
// Vid_ModeGate (0x4CB290) forces 640x480 whenever g_frontendActive (frontend) is set, so menu(640) vs
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
			if(g_debugLogic)
				Dbg("[60] !! setter DURING render this=%p ra=%p\n", thisp, _ReturnAddress());
		}
		return oPos(thisp, edx, x, y, z);
	}
	if (g_doObjs)
	{
		int32_t* d = (int32_t*)thisp;
		g_dead.erase(d);
		Obj& s = g_obj[d];
		if (s.bornTick == 0xFFFFFFFFu) s.bornTick = g_tick;   // anchor the warmup window
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
	if (g_zoom) {
		int vkOut = g_zoomKeys ? VK_OEM_MINUS : VK_SUBTRACT;  // minus = zoom OUT = raise cam
		int vkIn = g_zoomKeys ? VK_OEM_PLUS : VK_ADD;       // plus  = zoom IN
		if (GetAsyncKeyState(vkOut) & 0x8000) g_zoomOffset += g_zoomStep;
		if (GetAsyncKeyState(vkIn) & 0x8000) g_zoomOffset -= g_zoomStep;
		if (g_zoomOffset > g_zoomMax) g_zoomOffset = g_zoomMax;
		if (g_zoomOffset < g_zoomMin) g_zoomOffset = g_zoomMin;
	}

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
		if (g_debugLogic)
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
		if (g_debugLogic)
			Dbg("[HIST] setter kind:seen/move/lerp(dead)  %s\n", hb[0] ? hb : "(none)");
		DWORD now = timeGetTime();
		DWORD w = g_perfT0 ? now - g_perfT0 : 0;
		if (g_debugLogic)
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
		if (g_debugLogic)
		{
			Dbg("[PERF2] drawCPU avg=%.2f max=%u (n=%u) drawEMA=%.1f presentEMA=%.1f | pumpBudget=%.0f debt=%.0f skips=%u | mode673E2C=%d fskip673598=%d\n",
				g_drawN ? g_drawSum / g_drawN : 0.0, g_drawMax, g_drawN, g_lastDrawCPU, g_lastPresentCPU, g_pumpBudgetMs, g_debt, g_pumpSkips,
				*(int*)0x673E2C, *(BYTE*)0x673598);	// 0x673E2C = g_bMultiplayer, 0x673598 = g_frameSkipEnable
			Dbg("[PERF4] g_presentQPCema=%.3f g_sceneQPCema=%.3f diff=%.3f\n", g_presentQPCema, g_sceneQPCema, g_presentQPCema - g_sceneQPCema);
		}
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
				uint32_t daNow = *(uint32_t*)0x5E8B78, ddNow = *(uint32_t*)0x5E8B7C;	// timer_displayAdd, timer_displayDraw
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
		if(g_debugLogic)
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
		g_savedRectValid = false;

		// to-do check whether cull follow is working or whether it does harm
		if (g_cullFollow) __try {
			int32_t dx = g_camPtr[38] - g_savedXY[0];   // interp - committed (0 if teleport-skipped)
			int32_t dy = g_camPtr[39] - g_savedXY[1];
			g_savedRect[0] = g_camPtr[30]; g_savedRect[1] = g_camPtr[31];
			g_savedRect[2] = g_camPtr[32]; g_savedRect[3] = g_camPtr[33];
			g_camPtr[30] += dx; g_camPtr[31] += dx;         // x0/x1
			g_camPtr[32] += dy; g_camPtr[33] += dy;         // y0/y1
			g_savedRectValid = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { g_savedRectValid = false; }
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
					{
						if(g_debugLogic)
							Dbg("[MISS] persistent mover NEVER live d=%p moves=%u cur=(%d,%d)\n", d, s.moves, s.cx, s.cy);
					}
				}
				continue;
			}

			if (g_useDeadSet && g_live.count(d) == 0)
				++g_extraInterp;

			if (s.px == s.cx && s.py == s.cy)
				continue;

			int64_t dxv = llabs((int64_t)s.cx - s.px);
			int64_t dyv = llabs((int64_t)s.cy - s.py);
			if (dxv > TELE || dyv > TELE)
				continue;                                    // warp / level jump
			if (dxv > g_maxObjMove || dyv > g_maxObjMove)          // relocate / recycled slot: snap, never swoop
			{
				if (g_debugLogic)
					Dbg("[SNAP] d=%p age=%u k=%d dx=%.1f dy=%.1f (tiles) px=(%d,%d) cx=(%d,%d) cam=(%d,%d)\n",
						d, (unsigned)(g_tick - s.bornTick), SafeKind(d),
						dxv / 16384.0, dyv / 16384.0, s.px, s.py, s.cx, s.cy, g_savedXY[0], g_savedXY[1]);
				continue;
			}
			if ((uint32_t)(g_tick - s.bornTick) < g_spawnObjSuppressRenderForTicks)   // fresh spawn settle (dtor'd slots)
			{
				++g_spawnObjSkips;
				continue;
			}
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
					{
						if (g_debugLogic)
							Dbg("[DUALW] d=%p tracked=(%d,%d) actual=(%d,%d)\n", d, s.cx, s.cy, ox, oy);
					}
				}
			}
			else
			{
				static int n = 0;
				if (n++ < 50)
				{
					if(g_debugLogic)
						Dbg("[SEH] dead obj on WRITE d=%p tick=%u\n", d, g_tick);
				}
			}
		}
	}

	CP(24);
	CP(25);
	LARGE_INTEGER pa; QueryPerformanceCounter(&pa);
	char r = oRender();
	LARGE_INTEGER pb; QueryPerformanceCounter(&pb);
	double pres = (double)(pb.QuadPart - pa.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
	g_presentQPCema = g_presentQPCema * 0.9 + pres * 0.1;
	CP(26);

	for (auto& s : g_savedObj)
		SafeRestore(s.d, s.x, s.y, g_forceDirty);

	if (g_doCamera && g_camPtr)
	{
		g_camPtr[38] = g_savedXY[0];
		g_camPtr[39] = g_savedXY[1];
		if (g_camZ)
			g_camPtr[40] = g_savedXY[2];
		if (g_savedRectValid) {
			g_camPtr[30] = g_savedRect[0]; g_camPtr[31] = g_savedRect[1];
			g_camPtr[32] = g_savedRect[2]; g_camPtr[33] = g_savedRect[3];
		}
	}
	CP(27);
	g_inRender = false;
	DWORD rd = timeGetTime() - rt0;
	g_renderDurSum += rd;
	if (rd > g_renderDurMax)
		g_renderDurMax = rd;
	++g_renderN;
	g_lastPresentCPU = g_lastPresentCPU * 0.6 + (double)rd * 0.4;   // <-- feeds the pump gate (FULL present cost, incl BitBlt/flip)
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
	// Gate on the REAL present cost (g_lastPresentCPU = EMA of the whole Game_RenderPresent incl
	// Flip/BitBlt), NOT scene-build (g_lastDrawCPU, which is cheap and never sees the blit). Only
	// fire a 2nd present if a full one fits before the next logic tick is due. On the 16-bit shim
	// backend this correctly REFUSES most pumps (present is expensive); fix the backend and present
	// collapses, need drops, pumps fire freely. Same knob, honest number.
	int timeLeft = (int)(due - now);                              // handles DWORD wrap for small deltas
	int need = g_adaptivePump ? (int)(g_lastPresentCPU + 3.0) : 6;
	if (need < 6) need = 6;                                       // keep the old 6ms floor
	if (g_doPump && CameraPhys() && timeLeft > need)
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
	else if (g_doPump)
	{
		++g_pumpSkips;
	}
	CP(3);
	return r;
}

// Force 32bpp fullscreen: Vid_SetFullscreen's two `push 10h` -> `push 20h`
static void Patch32bppMode() {
	// Vid_SetFullscreen (0x4CB570): two REAL bpp args to Vid_CheckMode.
	//   push 10h @ 0x4CB583  (operand byte @ +1 = 0x4CB584) -> primary mode check
	//   push 10h @ 0x4CB5A5  (operand byte @ +1 = 0x4CB5A6) -> 640x480 fallback
	// A 3rd push 10h @ 0x4CB5DC is only the bpp *reported* in Game_Assert's
	// varargs — cosmetic, DO NOT touch.
	const uintptr_t sites[2] = { 0x4CB584, 0x4CB5A6 };
	int done = 0;
	for (uintptr_t p : sites) {
		uint8_t* b = (uint8_t*)p;
		if (b[-1] != 0x6A || b[0] != 0x10) {            // confirm it's 'push 10h'
			Dbg("[32BIT] %08X not push 10h (got %02X %02X) SKIP\n", (unsigned)p, b[-1], b[0]);
			continue;
		}
		DWORD o; VirtualProtect(b, 1, PAGE_EXECUTE_READWRITE, &o);
		*b = 0x20;
		VirtualProtect(b, 1, o, &o);
		++done;
		Dbg("[32BIT] patched %08X -> push 20h\n", (unsigned)p);
	}
	Dbg("[32BIT] %d/2 patched%s\n", done, done == 2 ? "" : "  *** INCOMPLETE — half-16/half-32! ***");
}


typedef int(__stdcall* fn_reggetint_t)(const char*, int);
static fn_reggetint_t oRegGetInt = nullptr;
static int __stdcall Hook_RegGetInt(const char* name, int def)
{
	if (name) {
		if (g_resWidth > 0 && g_resHeight > 0) {
			if (!_stricmp(name, "full_width") || !_stricmp(name, "window_width"))  return g_resWidth;
			if (!_stricmp(name, "full_height") || !_stricmp(name, "window_height")) return g_resHeight;
		}
		if (g_skipIntro && !_stricmp(name, "do_play_movie")) return 0;  // WinMain: !=1 => v12=0 => straight to menu
	}
	return oRegGetInt(name, def);
}
static void Init()
{
	LoadConfig();                       // read ini FIRST (sets g_logEnabled, ...)
	if (g_logEnabled) { LogOpen(); LogInitRing(); }

// broken currently
//	{
//		ModFlags mf{ &g_bApply32bpp, &g_menuScale, &g_skipIntro, &g_unifyRes, &g_zoom,
//					 &g_doCamera, &g_childProfile, &g_mapProfile, &g_resWidth, &g_resHeight };
//		Compat_Run(mf, g_wsCompat);
//	}

	if(g_bApply32bpp)
		Patch32bppMode();

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

	// log any stall that occurs
	if(g_Watchdog)
		CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);

	if (MH_Initialize() != MH_OK)
	{
		Dbg("[60] MH_Initialize FAILED\n");
		return;
	}

	{
		HMODULE self = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&UseHardcoded, &self) && self)
		{
			MODULEINFO smi;
			if (GetModuleInformation(GetCurrentProcess(), self, &smi, sizeof smi))
			{
				g_selfBase = (uintptr_t)smi.lpBaseOfDll;
				g_selfTop = g_selfBase + smi.SizeOfImage;
				Dbg("[60] proxy base=%08X top=%08X (fault RVA = eip-base)\n",
					(unsigned)g_selfBase, (unsigned)g_selfTop);
			}
		}
	}
	if (g_bApply32bpp || g_menuScale) {
		if (MH_CreateHook((void*)0x452E20, (void*)&Hook_FrontendLoadMenuBgImages,
			(void**)&o_FrontendLoadMenuBgImages) != MH_OK)
			Dbg("[60] hook FrontendLoadMenuBgImages FAILED\n");
	}
	if (g_skipIntro || (g_resWidth > 0 && g_resHeight > 0))
		if (MH_CreateHook((void*)0x4B5660, (void*)&Hook_RegGetInt, (void**)&oRegGetInt) != MH_OK)
			Dbg("[60] hook Registry_GetInt FAILED\n");
	if (MH_CreateHook((void*)g.tick, (void*)&Hook_Tick, (void**)&oTick) != MH_OK) Dbg("[60] hook tick FAILED\n");
	if (MH_CreateHook((void*)g.render, (void*)&Hook_Render, (void**)&oRender) != MH_OK) Dbg("[60] hook render FAILED\n");
	if (MH_CreateHook((void*)g.logic, (void*)&Hook_Logic, (void**)&oLogic) != MH_OK) Dbg("[60] hook logic FAILED\n");
	if (MH_CreateHook((void*)g.pos, (void*)&Hook_Pos, (void**)&oPos) != MH_OK) Dbg("[60] hook pos FAILED\n");
	if (MH_CreateHook((void*)g.dtor, (void*)&Hook_Dtor, (void**)&oDtor) != MH_OK) Dbg("[60] hook dtor FAILED\n");
	if (MH_CreateHook((void*)g.scene, (void*)&Hook_SceneDraw, (void**)&oSceneDraw) != MH_OK) Dbg("[60] hook scene FAILED\n");

	if (g_tileListOptimization)
	{
		g_tiles.reserve(16384);
		if (MH_CreateHook((void*)g.tileadd, (void*)&Hook_TileAddUnique, (void**)&oTileAdd) != MH_OK) Dbg("[60] hook tileadd FAILED\n");
		if (MH_CreateHook((void*)0x4720E0, (void*)&Hook_TileDrain, (void**)&oTileDrain) != MH_OK) Dbg("[60] hook tiledrain FAILED\n");
	}

	if (g_unifyRes)
	{
		if (MH_CreateHook((void*)g.modegate, (void*)&Hook_ModeGate, (void**)&oModeGate) != MH_OK)
			Dbg("[60] hook modegate FAILED\n");
	}
	if (g_childProfile || g_mapProfile)
	{
		if (MH_CreateHook((void*)g.maplayers, (void*)&Hook_MapLayers, (void**)&oMapLayers) != MH_OK) Dbg("[60] hook maplayers FAILED\n");
	}
	if (g_childProfile)
	{
		if (MH_CreateHook((void*)g.surf, (void*)&Hook_Surf, (void**)&oSurf) != MH_OK) Dbg("[60] hook surf FAILED\n");
		if (MH_CreateHook((void*)g.spr2, (void*)&Hook_Spr2, (void**)&oSpr2) != MH_OK) Dbg("[60] hook spr2 FAILED\n");
		if (MH_CreateHook((void*)g.ui, (void*)&Hook_Ui, (void**)&oUi) != MH_OK) Dbg("[60] hook ui FAILED\n");
	}

	if (g_doArrows)
	{
		if (MH_CreateHook((void*)g.slots, (void*)&Hook_Slots, (void**)&oSlots) != MH_OK)
			Dbg("[60] hook slots(arrows) FAILED\n");
	}
	if (g_zoom)
	{
		if (MH_CreateHook((void*)g.camcommit, (void*)&Hook_CamCommit, (void**)&oCamCommit) != MH_OK)
			Dbg("[60] hook camcommit(zoom) FAILED\n");
	}
	if (g_bannerShrink || g_bannerDedup || g_bannerDedupCar)   // banner fns needed by shrink OR dedup
	{
		if (MH_CreateHook((void*)g.areaBanner, (void*)&Hook_AreaBanner, (void**)&oAreaBanner) != MH_OK) Dbg("[60] hook areaBanner FAILED\n");
		if (MH_CreateHook((void*)g.vehBanner, (void*)&Hook_VehBanner, (void**)&oVehBanner) != MH_OK) Dbg("[60] hook vehBanner FAILED\n");
	}
	if (g_bannerShrink)                    // shrink-only primitives
	{
		if (MH_CreateHook((void*)g.drawIcon, (void*)&Hook_DrawIcon, (void**)&oDrawIcon) != MH_OK) Dbg("[60] hook drawIcon FAILED\n");
		if (MH_CreateHook((void*)g.drawSprite, (void*)&Hook_DrawSprite, (void**)&oDrawSprite) != MH_OK) Dbg("[60] hook drawSprite FAILED\n");
		if (MH_CreateHook((void*)g.drawScaledStr, (void*)&Hook_DrawScaledStr, (void**)&oDrawScaledStr) != MH_OK) Dbg("[60] hook drawScaledStr FAILED\n");
	}
	
	if (MH_CreateHook((void*)g.levelStart, (void*)&Hook_LevelStart, (void**)&oLevelStart) != MH_OK) Dbg("[60] hook levelStart FAILED\n");


	if (MH_CreateHook((void*)g.sessionTeardown, (void*)&Hook_SessionTeardown, (void**)&oSessionTeardown) != MH_OK) Dbg("[60] hook sessionTeardown FAILED\n");

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