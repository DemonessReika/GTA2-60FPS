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
#include <ddraw.h>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dcomp.lib")

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

static bool g_tileListOptimization = true;   // ini "tilelist_opt"


// ---- manual zoom: drive HEIGHT target [6]; engine propagates to [40]->[30..33]->[8..11] ----
static bool     g_zoom = true;      // ini "zoom"          master on/off
static int      g_zoomKeys = 1;         // ini "zoom_keys"     0 = numpad +/- , 1 = main-row -/=
static int32_t  g_zoomOffset = 0;         // ini "zoom_offset"   persistent zoom-out amount (Q14)
static int32_t  g_zoomStep = 2048;      // ini "zoom_step"     per-tick while held
static int32_t  g_zoomMax = 73728;     // ini "zoom_max"
static int32_t  g_zoomMin = 0;         // ini "zoom_min"      0 = never below speed-zoom baseline
static int      g_zoomPersp = 0;         // ini "zoom_perspective"     0..100 (0 = original/flat)

static bool g_zoomPerspAllowTogglingWithNumKeys = false;

static int      g_zoomPerspAlt = 25;       // ini "zoom_perspective_alt" what num2 flips to
// --- fixed internals (tuning done; no ini clutter — edit here only if you must) ---
static const int32_t Z_ENGINE_MAX = 14 << 14; // Camera_ClampZoom ceiling (229376)
static const int32_t Z_CULL_BIAS = 16384;    // kCamHeightBiasQ14 = 1 tile (sprite-cull bias)
static const int32_t g_zoomPerspBias = 131072;   // 8 tiles = Cam_ProjectVertex literal depth (framing-exact)
static int      g_zoomPerspKey = 0x62;    // numpad 2 : toggle persp <-> persp_alt
static int      g_zoomPerspDnKey = 0x61;    // numpad 1 : persp -
static int      g_zoomPerspUpKey = 0x63;    // numpad 3 : persp +
static int      g_zoomPerspStep = 10;      // nudge amount
static bool     g_zoomPerspSnap = true;    // instant flip on toggle (no warp)
static bool     g_zoomCullFix = true;    // widen sprite cull toward B (no edge pop)
static uint32_t g_zoomSnapTick = 0xFFFFFFFFu;


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

static int g_flipLatency = 2;   // ini "flip_latency": 1 = min latency (stutters), 2 = smooth

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

static bool g_flipPresent = true;
static bool g_flipVsync = true;
static bool g_flipSuppressEngine = true;   // "flip_suppress_engine": stub engine flip -> single presenter (the tear fix)
static bool g_flipLog = false;

/// BONUS CONTENT ^

typedef int(__fastcall* fn_maplayers_t)(void*, void*);
static fn_maplayers_t oMapLayers = nullptr;
typedef int(__fastcall* fn_tileadd_t)(void*, void*, void*, void*);
static fn_tileadd_t oTileAdd = nullptr;


struct {
	uintptr_t tick, render, logic, pos, pSession, pNextDue, pCarHead, pPedHead, dtor, scene,
		maplayers, tileadd, surf, spr2, ui, modegate, slots, slotsOne, camcommit,
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
	g.slotsOne = 0x4C82C0;		// Marker_DrawOne: Mission arrow stuff, we use this one for making post-game better.
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

static uint32_t g_tick = 0;

static int  g_hideEndMarkerWhenFurtherThan = 12;
// end level arrow grace timer (keep exit arrow visible x sec after score hits max))
static int      g_endArrowGraceSeconds = 25;    // ini "endlevel_arrow_grace_seconds" (0 = off = old behaviour)
static bool     g_endArrowReached = false; // score currently >= winning score this run
static uint32_t g_endArrowReachTick = 0;     // g_tick at the latest score>=max rising edge

typedef void(__fastcall* fn_markerDrawOne)(void*, void*);
static fn_markerDrawOne oDrawOne = nullptr;

typedef bool(__fastcall* fn_Mission_HasArrowOverride)(void*, void*);
static const fn_Mission_HasArrowOverride Mission_HasArrowOverrideFn = (fn_Mission_HasArrowOverride)0x4C7350;

static int GetPlayerScore()
{
	__try {
		uintptr_t s = *(uintptr_t*)g.pSession;
		if (s < 0x10000 || s > 0x7FFFFFFF) return false;
		uintptr_t player = *(uintptr_t*)(s + 28);          // session[7] = local player
		if (player < 0x10000 || player > 0x7FFFFFFF) return false;
		int score = *(int*)(player + 724);       // player+724 = score, may need extra deref, 

		return score;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static uintptr_t       g_pHud = 0x672F40;        // <<< g_hud (pointer var) — NEED THIS

typedef void(__thiscall* fn_getctrlxyz_t)(void* player, int* x, int* y, int* z);
static const fn_getctrlxyz_t Player_GetControlledWorldXYZ_fn = (fn_getctrlxyz_t)0x4A6610;
// local player's position (ped on foot, or the car it's driving)
static bool GetControlledPos(int32_t& x, int32_t& y, int32_t& z)
{
	__try {
		uintptr_t s = *(uintptr_t*)g.pSession;
		if (s < 0x10000 || s > 0x7FFFFFFF) return false;
		uintptr_t player = *(uintptr_t*)(s + 56);          // session[14] = local player (engine uses this)
		if (player < 0x10000 || player > 0x7FFFFFFF) return false;
		int px = 0, py = 0, pz = 0;
		Player_GetControlledWorldXYZ_fn((void*)player, &px, &py, &pz);
		x = px; y = py; z = pz;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// marker's active part world XYZ (Marker+120 = active MarkerPart; part+20/24/28 = resolved world pos)
static bool GetMarkerWorldPos(void* markerBase, int32_t& x, int32_t& y, int32_t& z)
{
	__try {
		uintptr_t m = (uintptr_t)markerBase;
		uintptr_t part = *(uintptr_t*)(m + 120);           // active MarkerPart ptr
		if (part < 0x10000 || part > 0x7FFFFFFF) return false;
		if (*(int32_t*)(part + 16) == 0) return false;     // part type 0 = free/unresolved aka can't gate
		x = *(int32_t*)(part + 20);
		y = *(int32_t*)(part + 24);
		z = *(int32_t*)(part + 28);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// 2D (top-down) range test, worldDist in whole world units (for examplee 128 tiles)
static bool __forceinline IsBeyond2D(int64_t dx, int64_t dy, int64_t worldDist)
{
	const int64_t q14 = worldDist * 16384;
	return (dx * dx + dy * dy) > (q14 * q14);
}

static const uintptr_t ADDR_pMissionState = 0x6644BC;
static int GetScoreMax()
{
	__try {
		uintptr_t ms = *(uintptr_t*)ADDR_pMissionState;
		if (ms < 0x10000 || ms > 0x7FFFFFFF) return 0;
		return ((int*)ms)[196];      // mission[196] = $$$ needed until won map on single player, not sure about multiplayer though.
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool __forceinline IsBeyondQ14DistSq(int64_t dx, int64_t dy, int64_t dz, int64_t worldDist)
{
	// worldDist is in whole world units (64). Convert to Q14 once.
	const int64_t q14 = worldDist * 16384;
	return (dx * dx + dy * dy + dz * dz) > (q14 * q14);
}
static bool g_bhideEndLevelArrowIf_Far = false;
static void __fastcall Hook_SlotsOne(void* thisp, void* edx)
{
	if (g_hideEndMarkerWhenFurtherThan == 0 || !g_bhideEndLevelArrowIf_Far) { oDrawOne(thisp, edx); return; }
	int iArrowKind = *(int*)((char*)thisp + 32);           // Marker+32 = kind (5 = gang/exit arrow)
	if (iArrowKind == 5 && (!Mission_HasArrowOverrideFn(*(void**)ADDR_pMissionState, 0)))
	{
		bool inGrace = false;
		if (g_endArrowGraceSeconds > 0 && g_endArrowReached)
		{
			uint32_t elapsed = g_tick - g_endArrowReachTick;
			uint32_t graceTicks = (uint32_t)g_endArrowGraceSeconds * 30u;
			inGrace = (elapsed < graceTicks);
		}
		if (!inGrace)
		{
			int32_t tx, ty, tz, px, py, pz;
			if (GetMarkerWorldPos(thisp, tx, ty, tz) && GetControlledPos(px, py, pz))
			{
				int64_t dx = (int64_t)tx - px;
				int64_t dy = (int64_t)ty - py;
				if (IsBeyond2D(dx, dy, g_hideEndMarkerWhenFurtherThan))
					return;                                 // beyond range we suppress
			}
			// couldn't resolve a position we draw normally (fail visible)
		}
	}
	oDrawOne(thisp, edx);
}
//

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

static bool g_collectibleCounter = false;   // ini "collectible_counter"
static bool g_collectibleShowTotal = false;   // ini "collectible_show_total" (1="X OF Y", 0="X")
static int  g_collectibleMsgTicks = 90;     // ini "collectible_msg_ticks" (HUD timer, 1..255)

static const uintptr_t g_pFxMgr = 0x665788;          // g_fxMgr (pointer var) CONFIRMED

typedef char(__fastcall* fn_trycollect_t)(void*, void*, int);
static fn_trycollect_t oTryCollect = nullptr;
typedef char(__fastcall* fn_islocal_t)(void*, void*);
typedef void(__fastcall* fn_layout1_t)(void*, void*);
static const fn_islocal_t Player_IsLocal_fn = (fn_islocal_t)0x41DC70;
static const fn_layout1_t Hud_LayoutCenteredText1_fn = (fn_layout1_t)0x4C69D0;

static const uintptr_t g_gameSetup = 0x5EC070;               // the struct, not pointer to struct!
static const uintptr_t kProgressCounterOff = 349 * 4;        // 1396 / 0x574
static inline int GameSetup_GetProgressCounter()
{
	// g_gameSetup is static data. we just do a fixed address read.
	return *(int*)(g_gameSetup + kProgressCounterOff);
}

static void ShowCollectibleMessage()
{
	__try {
		int collected = GameSetup_GetProgressCounter();       // actual running total
		if (collected < 0) collected = 0;
		int total = 0;
		if (g_collectibleShowTotal)                           // only need total if i print it
		{
			uintptr_t fx = *(uintptr_t*)g_pFxMgr;
			if (fx >= 0x10000 && fx <= 0x7FFFFFFF)
			{
				total = *(int*)(fx + 84);
				if (total < 0) total = 0; if (total > 50) total = 50;
			}
		}
		if (!g_pHud) return;
		uintptr_t hud = *(uintptr_t*)g_pHud;
		if (hud < 0x10000 || hud > 0x7FFFFFFF) return;
		uint8_t* slot = (uint8_t*)(hud + 10168);              // CenteredText1
		wchar_t text[64];
		if (g_collectibleShowTotal)
			_snwprintf(text, 63, L"%d OF %d TOKENS COLLECTED", collected, total);
		else if (collected == 1)
			_snwprintf(text, 63, L"%d TOKEN COLLECTED", collected);
		else
			_snwprintf(text, 63, L"%d TOKENS COLLECTED", collected);
		text[63] = 0;
		int t = g_collectibleMsgTicks; if (t < 1) t = 1; if (t > 255) t = 255;
		slot[0] = (uint8_t)t;
		lstrcpynW((wchar_t*)(slot + 2), text, 60);
		Hud_LayoutCenteredText1_fn(slot, nullptr);
		if (g_debugLogic) Dbg("[COLLECT] total=%d (fxtotal=%d)\n", collected, total);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
static char __fastcall Hook_TryCollectPickup(void* thisp, void* edx, int a2)
{
	int type = 0;
	if (g_collectibleCounter)
		__try { type = *(int*)((char*)a2 + 24); }
	__except (EXCEPTION_EXECUTE_HANDLER) { type = 0; }
	char r = oTryCollect(thisp, edx, a2);                     // let the game collect it first
	if (g_collectibleCounter && r && type == 266) {           // collectible, successfully grabbed
		__try {
			void* player = *(void**)((char*)thisp + 348);     // ped+348 = owning Player*
			if (player && Player_IsLocal_fn(player, nullptr)) // mirror the game's local-only rule
				ShowCollectibleMessage();
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	return r;
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
// ===== surf claw-back (§33) — drop the redundant pre-draw backbuffer lock =====
static bool g_surfClawback = true;          // ini "surf_clawback"
static uintptr_t g_pE43E18 = 0;             // d3ddll dword_E43E18 (RVA 0x43E18) — EndScene re-lock flag
typedef int(__stdcall* fn_begin_t)();
static fn_begin_t oBeginScene = nullptr;

static void Patch_SurfClawback()
{
	uint8_t* p = (uint8_t*)0x4CAEC0;
	Dbg("[SURF] prologue %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
		p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10]);
	if (p[0] == 0xE9) { Dbg("[SURF] 4CAEC0 still trampolined (childprofile surf hook?) — set childprofile=0 or keep the surf-skip. ABORT\n"); return; }
	// EXPECT: 05:83 EC 08 (sub esp,8 KEEP) | 08:50 (push eax) | 09:FF 15 (call Vid_GetSurface)
	if (p[5] != 0x83 || p[6] != 0xEC || p[8] != 0x50 || p[9] != 0xFF || p[10] != 0x15) {
		Dbg("[SURF] prologue mismatch — ABORT (send me the dump line)\n"); return;
	}
	DWORD o; VirtualProtect(p + 8, 7, PAGE_EXECUTE_READWRITE, &o);
	memset(p + 8, 0x90, 7);        // push eax + call Vid_GetSurface -> 7 NOPs; sub esp,8 preserved
	VirtualProtect(p + 8, 7, o, &o);
	Dbg("[SURF] LOCK#1 removed: 7 NOPs @4CAEC8 (push+call Vid_GetSurface); sub esp,8 kept\n");
}
// (2) Force the re-lock flag so gbh_EndScene STILL locks → grab's +80 stays valid.
static int __stdcall Hook_BeginScene()
{
	int r = oBeginScene();
	if (g_surfClawback && g_pE43E18) *(volatile int*)g_pE43E18 = 1;  // force LOCK#2 so grab's +80 stays valid
	return r;
}

static const int FL_STAGING_COUNT = 3;                              // max ring depth (compile-time array)
static ID3D11Texture2D* s_flStaging[FL_STAGING_COUNT] = { nullptr, nullptr, nullptr };
static int g_flStageRingN = 3;        // ini "flip_staging_ring" 1..3 (1 = old single-buffer behaviour)
static int s_flStageRing = 0;         // ring cursor, advances per present
static int s_flStageCur = 0;         // buffer grabbed this frame (consumed by present)
// Install lazily: Frontend_LoadMenuBgImages (0x452E20) runs at frontend-enter,
// long after our Init() and after d3ddll is loaded -> no boot race.
typedef void(__cdecl* fn_loadbg_t)();
static fn_loadbg_t o_FrontendLoadMenuBgImages = nullptr;

// ---- windowed / borderless / resizable (window_mode) ----
static int  g_windowMode = 0;      // "window_mode": 0 exclusive-FS(engine default) / 1 borderless / 2 windowed-resizable
static int  g_winW = 0, g_winH = 0;        // live client size (also what the reg hook feeds window_width/height)
static volatile bool g_inVidReconf = false;// re-entrancy guard around Vid_Reconfigure
// 4K+ scanline-table insurance (dword_E43F40[1700]; D3D path never reads  it, so clamping the write is free)
static bool g_highresScanlineFix = true;   // "highres_scanline_fix"
static int  g_scanlineMax = 1700;          // "scanline_max" = real dword_E43F40 capacity (DO NOT exceed)
// SceneNode pool-full guard, 1000 sprites total across all 7 buckets)
static bool g_sceneNodeGuard = true;       // "scenenode_guard"
static const uintptr_t ADDR_VidSetWindowed = 0x4CB730;
static const uintptr_t ADDR_VidWindowedAllowed = 0x4CB720;
static const uintptr_t ADDR_VidReconfigure = 0x4CC5C0;
static const uintptr_t ADDR_VidReapplyViewport = 0x4CB880;   // (palettes + ApplyResolutionToSession)
static const uintptr_t ADDR_GameWndProc = 0x4D0A00;
static int* const g_pStartMode = (int*)0x595014;       // g_startMode
static int* const g_pLastViewportDim = (int*)0x6735A0;       // g_lastViewportDim (relayout guard)

// 4K+ scanline clamp 
typedef int(__stdcall* fn_makescreentable_t)(int, int, int);
static fn_makescreentable_t oMakeScreenTable = nullptr;
static int __stdcall Hook_MakeScreenTable(int base, int pitch, int height)
{
	if (g_highresScanlineFix && height > g_scanlineMax) {
		static bool warned = false;
		if (!warned) { warned = true; Dbg("[SCAN] height %d > cap %d -> clamp (table unused by D3D)\n", height, g_scanlineMax); }
		height = g_scanlineMax;
	}
	return oMakeScreenTable(base, pitch, height);
}
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
			if (g_highresScanlineFix) {
				void* ms = (void*)GetProcAddress(h, "MakeScreenTable");
				if (ms && MH_CreateHook(ms, (void*)&Hook_MakeScreenTable, (void**)&oMakeScreenTable) == MH_OK) MH_EnableHook(ms);
				Dbg("[SCAN] MakeScreenTable %s\n", oMakeScreenTable ? "hooked" : "FAIL");
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
static double g_lastSceneMs = 0, g_lastGrabMs = 0, g_lastWaitMs = 0, g_lastPresentMs = 0;
// steady-state flip EMAs: raw per-frame g_last* only ever surface in [DROP] (which never fires at 60),
// so these are the ONLY way to see grab/present cost during a healthy run. Reported in the [FLIP] heartbeat.
static double g_grabMsEma = 0.0, g_presentMsEma = 0.0, g_waitMsEma = 0.0, g_grabMsMax = 0.0;
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
static bool    g_nextVblankAlpha = true;   // ini "nextvblank_alpha" (default ON)
static double  g_refreshMs = 16.667;        // EMA of flip->flip interval (~one display refresh)
static int64_t g_lastVblankQpc = 0;
static bool    g_haveVblank = false;
static double  g_lastPA = -1.0; static bool g_havePA = false;
static int32_t g_lastPCamX = 0, g_lastPCamY = 0;
static double  g_dASum = 0, g_dASumSq = 0, g_dAMin = 1e9, g_dAMax = -1e9; static uint32_t g_dAN = 0;
static double  g_csSum = 0, g_csSumSq = 0; static uint32_t g_csN = 0;
static double  g_latSum = 0, g_latSumSq = 0, g_latMin = 1e18, g_latMax = 0; static uint32_t g_latN = 0;
//
static double g_fSurfMs = 0, g_fMapMs = 0, g_fSpr2Ms = 0, g_fUiMs = 0; // per-FRAME child split

static void __fastcall Hook_SceneDraw(void* thisp, void* edx)
{
	DWORD t = timeGetTime();
	int64_t s0 = g_qcSurf, m0 = g_qcMap, p0 = g_qcSpr2, u0 = g_qcUi;
	LARGE_INTEGER pa; QueryPerformanceCounter(&pa);
	oSceneDraw(thisp, edx);
	LARGE_INTEGER pb; QueryPerformanceCounter(&pb);
	double invms = g_qpf.QuadPart ? 1000.0 / (double)g_qpf.QuadPart : 0.0;
	g_fSurfMs = (double)(g_qcSurf - s0) * invms;
	g_fMapMs = (double)(g_qcMap - m0) * invms;
	g_fSpr2Ms = (double)(g_qcSpr2 - p0) * invms;
	g_fUiMs = (double)(g_qcUi - u0) * invms;
	double pres = (double)(pb.QuadPart - pa.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
	g_lastSceneMs = pres;   // the QPC 'pres' you already compute
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
	uintptr_t s = *(uintptr_t*)g.pSession;
	return s && *(int*)s == 2;          // *session==2 == paused 
}

static uint32_t g_frame = 0;
typedef int(__fastcall* fn_camcommit_t)(void*, void*);
static fn_camcommit_t oCamCommit = nullptr;
static int __fastcall Hook_CamCommit(void* thisp, void* edx)
{
	bool applied = false; double cullRatio = 1.0;
	if (g_zoom && g_zoomOffset && !SessionIsPaused())
		__try {
		int32_t* cam = (int32_t*)thisp;
		int32_t z0 = cam[6], zoom0 = cam[7], off = g_zoomOffset;      // staged alt (speed-varying) + zoom (const)
		double B = (double)g_zoomPerspBias, p = (double)g_zoomPersp * 0.01;
		double cam40 = (double)z0 + (1.0 - p) * (double)off;        // split the zoom-out, alt vs zoom
		double denom = (double)z0 + (double)off + B;
		double zoom = (denom > 1.0) ? ((cam40 + B) / denom) * (double)zoom0 : (double)zoom0;  // keep (cam40+8)  / zoom const
		if (cam40 > (double)Z_ENGINE_MAX) cam40 = (double)Z_ENGINE_MAX;
		if (cam40 < 0.0) cam40 = 0.0;
		int64_t zlo = (int64_t)zoom0 / 8; if (zlo < 1)zlo = 1; int64_t zhi = (int64_t)zoom0 * 2;
		int64_t zi = (int64_t)llround(zoom); if (zi < zlo)zi = zlo; else if (zi > zhi)zi = zhi;
		cam[6] = (int32_t)llround(cam40); cam[7] = (int32_t)zi; applied = true;

		double ca = (double)z0 + (double)off + (double)Z_CULL_BIAS, cc = cam40 + (double)Z_CULL_BIAS;
		if (cc > 1.0) cullRatio = ca / cc;
		if (g_zoomPerspSnap && g_zoomSnapTick == g_tick) {        // instant A<->B flip, no smoother ramp
			cam[40] = cam[6]; cam[41] = cam[7]; cam[2] = cam[6]; cam[3] = cam[7];
			Dbg("[ZOOMSNAP] p=%d%% z0=%d off=%d -> cam40=%d zoom=%d cull=%.2f\n", g_zoomPersp, z0, off, cam[6], cam[7], cullRatio);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { applied = false; cullRatio = 1.0; }
	int r = oCamCommit(thisp, edx); 
	if (applied && g_zoomCullFix && cullRatio > 1.0001 && cullRatio < 16.0)
		__try {
		int32_t* cam = (int32_t*)thisp;
		int32_t cx = cam[38], cy = cam[39];
		int64_t hwx = (int64_t)llround(((int64_t)cam[31] - cam[30]) / 2.0 * cullRatio);
		int64_t hwy = (int64_t)llround(((int64_t)cam[33] - cam[32]) / 2.0 * cullRatio);
		int64_t x0 = cx - hwx, x1 = cx + hwx, y0 = cy - hwy, y1 = cy + hwy;
		const int64_t WMAX = (int64_t)256 << 14;
		if (x0 < 0)x0 = 0; if (y0 < 0)y0 = 0; if (x1 > WMAX)x1 = WMAX; if (y1 > WMAX)y1 = WMAX;
		cam[30] = (int32_t)x0; cam[31] = (int32_t)x1; cam[32] = (int32_t)y0; cam[33] = (int32_t)y1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return r;
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
// ---- camera velocity-Hermite 
static bool     g_camHermite = true;          // ini "cam_smooth_hermite"
static int      g_camHermiteStrength = 100;   // ini "cam_smooth_hermite_strength" (0..100)
static int32_t  g_camChord[3] = { 0,0,0 };    // this tick's chord (cur-prev)
static int32_t  g_camPrevVel[3] = { 0,0,0 };    // last tick's chord = velocity entering this keyframe
static uint32_t g_camVelTick = 0xFFFFFFFFu;


// ---- windowed always permitted (getter hook beats the boot race) ----
typedef char(__cdecl* fn_vidwinallowed_t)();
static fn_vidwinallowed_t oVidWindowedAllowed = nullptr;
static char __cdecl Hook_VidWindowedAllowed()
{
	if (g_windowMode != 0) return 1;      // Vid_InitScreen zeroed it on 32-bit desktop; override per-call
	return oVidWindowedAllowed();
}
// borderless
static void ApplyBorderlessStyle()
{
	HWND h = *(HWND*)0x673D18;                      // g_hWnd
	if (!h || !IsWindow(h)) return;
	int x = 0, y = 0;                              // primary-monitor origin (v1: primary only)
	SetWindowLongA(h, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
	// cover size = surface size (= g_winW/H), so window == client == backbuffer -> crisp, no stretch
	SetWindowPos(h, HWND_TOP, x, y, g_winW, g_winH,
		SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
}
typedef char(__cdecl* fn_vidsetwindowed_t)();
static fn_vidsetwindowed_t oVidSetWindowed = nullptr;
static char __cdecl Hook_VidSetWindowed()
{
	char r = oVidSetWindowed();                     // engine stamps WS_CAPTION + creates surface at client size
	if (r && g_windowMode == 1) ApplyBorderlessStyle();  // re-fires on every resume/alt-tab -> persistent
	return r;
}
// resize and rebuild surface + re-propagate viewport at the new client size ----
static void ApplyWindowedResize(int w, int h)
{
	if (w < 320) w = 320; if (w > 7680) w = 7680;
	if (h < 240) h = 240; if (h > 4320) h = 4320;
	if (w == g_winW && h == g_winH) return;
	g_winW = w; g_winH = h;                          // reg hook now feeds these to Vid_ModeGate
	*g_pLastViewportDim = -1;                        // force Cameras_SetViewportSize (guard is width-only)
	g_inVidReconf = true;
	*(int*)0x673578 = w;   // g_cfgWindowW
	*(int*)0x6732E8 = h;   // g_cfgWindowH

	((int(__cdecl*)())ADDR_VidReconfigure)();        // CloseScreen -> Vid_SetWindowed(client size) -> new surface
	((void(__cdecl*)())ADDR_VidReapplyViewport)();   // palettes + Vid_ApplyResolutionToSession (cam[26..29]/[42])
	g_inVidReconf = false;
	Dbg("[WIN] resized -> %dx%d\n", w, h);           // Flip_EnsureSize auto-resizes staging+swapchain next render
}

static bool g_hideCursorWindowed = true;   // "window_hide_cursor" is new


//Block the silent exclusive-FS jumps ----
typedef LRESULT(__stdcall* fn_wndproc_t)(HWND, UINT, WPARAM, LPARAM);
static fn_wndproc_t oWndProc = nullptr;
static LRESULT __stdcall Hook_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (g_windowMode != 0) {
		switch (msg) {
		case WM_SYSCOMMAND:
			if ((wParam & 0xFFF0) == SC_MAXIMIZE) return 0;      // engine would -> Vid_SetStartMode(1) = exclusive FS
			break;
		case WM_SYSKEYDOWN:
			if (wParam == VK_RETURN) return 0;                   // Alt+Enter: block exclusive-FS toggle
			break;
		case WM_SIZING:                                          // 0x0214 — engine overwrites lParam = PIN
			if (g_windowMode == 2)
				return DefWindowProcA(hWnd, msg, wParam, lParam);// let the drag rect through
			break;
		case WM_EXITSIZEMOVE:                                    // 0x0232 , engine doesn't handle it
			if (g_windowMode == 2 && !g_inVidReconf) {
				RECT rc; GetClientRect(hWnd, &rc);
				ApplyWindowedResize(rc.right - rc.left, rc.bottom - rc.top);
				return 0;
			}
			break;
		case WM_SETCURSOR:

			if (g_hideCursorWindowed && LOWORD(lParam) == HTCLIENT && GetForegroundWindow() == hWnd) {
				SetCursor(nullptr);
				return TRUE;         // We handled it. So, DefWindowProc never restores the arrow
			}
			break;
		}
	}
	return oWndProc(hWnd, msg, wParam, lParam);
}

// SceneNode pool-full guard: prevent null deref
static uint32_t g_scratchSceneNode[3] = { 0,0,0 };
typedef uint32_t* (__fastcall* fn_scnalloc_t)(uint32_t*, void*);
static fn_scnalloc_t oSceneNodeAlloc = nullptr;
static uint32_t* __fastcall Hook_SceneNodeAlloc(uint32_t* thisp, void* edx)
{
	uint32_t v = thisp[3000];
	if (v >= 1000) {                                 // pool full: drop overflow safely 
		static bool warned = false;
		if (!warned && g_debugLogic) { warned = true; Dbg("[SCENE] node pool full (>1000 visible sprites) — overflow dropped\n"); }
		return g_scratchSceneNode;
	}
	thisp[3000] = v + 1;
	return &thisp[3 * v];
}
static bool           g_busCountReset = true;      // ini "bus_count_reset"

//static bool g_bNoGangMeterBlink = false;
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
	if (g_zoomMax > 73728) g_zoomMax = 73728;
	g_zoomMin = GetPrivateProfileIntA("60fps", "zoom_min", 0, path);
	g_zoomKeys = GetPrivateProfileIntA("60fps", "zoom_keys", 1, path);

	g_zoomPerspAllowTogglingWithNumKeys = GetPrivateProfileIntA("60fps", "zoom_perspective_allow_numkeys_to_toggle", 0, path) != 0;
	g_zoomPersp = GetPrivateProfileIntA("60fps", "zoom_perspective", 0, path);
	g_zoomPerspAlt = GetPrivateProfileIntA("60fps", "zoom_perspective_alt", 25, path);

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

	g_nextVblankAlpha = GetPrivateProfileIntA("60fps", "nextvblank_alpha", 1, path) != 0;
	g_flipPresent = GetPrivateProfileIntA("60fps", "flip_present", 1, path) != 0;
	g_flipLog = GetPrivateProfileIntA("60fps", "flip_log", 0, path) != 0;
	g_flipVsync = GetPrivateProfileIntA("60fps", "flip_vsync", 1, path) != 0;
	g_flipSuppressEngine = GetPrivateProfileIntA("60fps", "flip_suppress_engine", 1, path) != 0;

	g_flipLatency = GetPrivateProfileIntA("60fps", "flip_latency", 2, path);
	if (g_flipLatency < 1) g_flipLatency = 1;
	if (g_flipLatency > 3) g_flipLatency = 3;
	g_surfClawback = GetPrivateProfileIntA("60fps", "surf_clawback", 1, path) != 0;
	g_flStageRingN = GetPrivateProfileIntA("60fps", "flip_staging_ring", 3, path);
	if (g_flStageRingN < 1) g_flStageRingN = 1;
	if (g_flStageRingN > FL_STAGING_COUNT) g_flStageRingN = FL_STAGING_COUNT;

	g_camHermite = GetPrivateProfileIntA("60fps", "cam_smooth_hermite", 1, path) != 0;
	g_camHermiteStrength = GetPrivateProfileIntA("60fps", "cam_smooth_hermite_strength", 100, path);

	g_collectibleCounter = GetPrivateProfileIntA("60fps", "collectible_counter", 0, path) != 0;
	g_collectibleShowTotal = GetPrivateProfileIntA("60fps", "collectible_show_total", 0, path) != 0;
	g_collectibleMsgTicks = GetPrivateProfileIntA("60fps", "collectible_msg_ticks", 90, path);
	g_windowMode = GetPrivateProfileIntA("60fps", "window_mode", 0, path);
	g_highresScanlineFix = GetPrivateProfileIntA("60fps", "highres_scanline_fix", 1, path) != 0;
	g_scanlineMax = GetPrivateProfileIntA("60fps", "scanline_max", 1700, path);
	g_sceneNodeGuard = GetPrivateProfileIntA("60fps", "scenenode_guard", 1, path) != 0;

	// 
	g_hideCursorWindowed = GetPrivateProfileIntA("60fps", "window_hide_cursor", 1, path) != 0;

	g_hideEndMarkerWhenFurtherThan = GetPrivateProfileIntA("60fps", "hide_endlevel_arrow_when_further_than", 12, path);
	g_endArrowGraceSeconds = GetPrivateProfileIntA("60fps", "endlevel_arrow_grace_seconds", 25, path);
	if (g_endArrowGraceSeconds < 0) g_endArrowGraceSeconds = 0;
	g_bhideEndLevelArrowIf_Far = GetPrivateProfileIntA("60fps", "hide_endlevel_arrow_if_far", 0, path) != 0;
	

//	g_bNoGangMeterBlink = GetPrivateProfileIntA("60fps", "gangmeter_disable_blink_at_max", 0, path) != 0;
	g_busCountReset = GetPrivateProfileIntA("60fps", "bus_count_reset", 1, path) != 0;
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

static double g_frametimeEMA = 33.333;   // smoothed tick period feeding Alpha()

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

/// expereiment

static ID3D11Device* s_flDev = nullptr;
static ID3D11DeviceContext* s_flCtx = nullptr;
static IDXGISwapChain1* s_flSwap = nullptr;
static IDCompositionDevice* s_flDcDev = nullptr;
static IDCompositionTarget* s_flDcTgt = nullptr;
static IDCompositionVisual* s_flDcVis = nullptr;



static HANDLE s_flWaitable = nullptr;
static int   s_flW = 0, s_flH = 0;
static bool  s_flReady = false, s_flGrabbed = false, s_flShown = false;
static volatile LONG s_flDead = 0;
static uint32_t s_flGrabs = 0, s_flPresents = 0;

//  DXGI hardware-vblank sampling 
#ifndef DXGI_ERROR_FRAME_STATISTICS_DISJOINT
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT _HRESULT_TYPEDEF_(0x887A000BL)
#endif
static UINT     g_flSyncRefresh = 0;   // last DXGI SyncRefreshCount (real  display-refresh counter)
static int64_t  g_flSyncQpc = 0;       // QPC of that refresh's scanout
static uint32_t g_flHbStatsOk = 0, g_flHbStatsFail = 0;   // per-heartbeat tally 
static bool     s_flVblankFromStats = false;              // did stats drive the anchor this present?

typedef void(__cdecl* fn_cxx_t)();
static fn_cxx_t oCxxRestore = nullptr;
// Per-row copy DD-surface -> mapped staging. Non-temporal stores, the CPU never reads this buffer back
// (GPU consumes it via CopyResource)
// so streaming the writes keeps 5-8MB/frame out of the cache.
// _mm_sfence() publishes the weakly-ordered stores before Unmap/GPU read. Falls back to memcpy when
// alignment doesn't permit movntdq (guards dst base, dst pitch, and rowBytes, src uses unaligned load)
// Really gotta learn to kepe things simpler, it's 5 AM rofl
static inline void CopyRowsToStaging(uint8_t* dst, int dstPitch,
	const uint8_t* src, int srcPitch, int rowBytes, int h)
{
	bool canStream = ((rowBytes & 15) == 0)
		&& ((((uintptr_t)dst) & 15) == 0) && ((dstPitch & 15) == 0);
	if (canStream) {
		int n = rowBytes >> 4;
		for (int y = 0; y < h; ++y) {
			const __m128i* s = (const __m128i*)(src + (size_t)y * srcPitch);
			__m128i* d = (__m128i*)(dst + (size_t)y * dstPitch);
			for (int i = 0; i < n; ++i)
				_mm_stream_si128(d + i, _mm_loadu_si128(s + i));
		}
		_mm_sfence();
	}
	else {
		for (int y = 0; y < h; ++y)
			memcpy(dst + (size_t)y * dstPitch, src + (size_t)y * srcPitch, rowBytes);
	}
}
static void Flip_ReleaseAll()
{
	for (int i = 0; i < FL_STAGING_COUNT; ++i)
		if (s_flStaging[i]) { s_flStaging[i]->Release(); s_flStaging[i] = nullptr; }
	s_flStageRing = 0; s_flStageCur = 0;
	if (s_flDcVis) { s_flDcVis->Release();   s_flDcVis = nullptr; }
	if (s_flDcTgt) { s_flDcTgt->Release();   s_flDcTgt = nullptr; }
	if (s_flDcDev) { s_flDcDev->Release();   s_flDcDev = nullptr; }
	if (s_flSwap) { s_flSwap->Release();    s_flSwap = nullptr; }
	if (s_flCtx) { s_flCtx->Release();     s_flCtx = nullptr; }
	if (s_flDev) { s_flDev->Release();     s_flDev = nullptr; }
	s_flW = s_flH = 0; s_flShown = false; s_flGrabbed = false;
	s_flWaitable = nullptr;
}
static HWND g_flFoundHwnd = nullptr;
static BOOL CALLBACK Flip_EnumProc(HWND h, LPARAM)
{
	DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(h)) { g_flFoundHwnd = h; return FALSE; }
	return TRUE;
}
static HWND Flip_FindHwnd()
{
	__try {                                             // surface+1216 = hWnd 
		uintptr_t surf = *(uintptr_t*)0x673D20;
		if (surf > 0x10000 && surf < 0x7FFFFFFF) { HWND h = *(HWND*)(surf + 1216); if (h && IsWindow(h)) return h; }
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	__try {                                             // g_hWnd (WinMain)
		HWND h = *(HWND*)0x673D18; if (h && IsWindow(h)) return h;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	g_flFoundHwnd = nullptr; EnumWindows(Flip_EnumProc, 0); return g_flFoundHwnd;   // last resort
}
static bool Flip_ReadSurfaceDims(int& w, int& h)
{
	__try {
		uintptr_t surf = *(uintptr_t*)0x673D20;
		if (surf > 0x10000 && surf < 0x7FFFFFFF) {
			w = *(int*)(surf + 72); h = *(int*)(surf + 76);
			return w >= 320 && w <= 7680 && h >= 240 && h <= 4320;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return false;
}
static bool Flip_EnsureSize(int w, int h)
{
	if (w == s_flW && h == s_flH && s_flStaging[0]) return true;
	for (int i = 0; i < FL_STAGING_COUNT; ++i)
		if (s_flStaging[i]) { s_flStaging[i]->Release(); s_flStaging[i] = nullptr; }
	if ((w != s_flW || h != s_flH) && s_flSwap) {
		HRESULT hr = s_flSwap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
			DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
		if (FAILED(hr)) { Dbg("[FLIP] ResizeBuffers %dx%d hr=%08X\n", w, h, (unsigned)hr); return false; }
	}
	s_flW = w; s_flH = h;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	for (int i = 0; i < g_flStageRingN; ++i) {
		HRESULT hr = s_flDev->CreateTexture2D(&td, nullptr, &s_flStaging[i]);
		if (FAILED(hr)) {
			Dbg("[FLIP] staging[%d] %dx%d hr=%08X\n", i, w, h, (unsigned)hr);
			for (int j = 0; j < FL_STAGING_COUNT; ++j)
				if (s_flStaging[j]) { s_flStaging[j]->Release(); s_flStaging[j] = nullptr; }
			return false;
		}
	}
	s_flStageRing = 0; s_flStageCur = 0;
	Dbg("[FLIP] staging ring: %d x %dx%d (%.1f MB total)\n",
		g_flStageRingN, w, h, (double)g_flStageRingN * w * h * 4.0 / (1024.0 * 1024.0));
	return true;
}
static void Flip_InstallEngineHijack();
static bool Flip_Init(HWND hwnd, int w, int h)
{
	__try {
		D3D_FEATURE_LEVEL fl;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &s_flDev, &fl, &s_flCtx);
		if (FAILED(hr) || !s_flDev) { Dbg("[FLIP] CreateDevice hr=%08X\n", (unsigned)hr); return false; }
		IDXGIDevice1* dxdev = nullptr;
		hr = s_flDev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev);
		if (FAILED(hr) || !dxdev) { Dbg("[FLIP] QI DXGIDevice1 hr=%08X\n", (unsigned)hr); return false; }
		dxdev->SetMaximumFrameLatency(g_flipLatency);  // was 1
		IDXGIAdapter* ad = nullptr;
		hr = dxdev->GetAdapter(&ad);
		if (FAILED(hr) || !ad) { Dbg("[FLIP] GetAdapter hr=%08X\n", (unsigned)hr); dxdev->Release(); return false; }
		IDXGIFactory2* f = nullptr;
		hr = ad->GetParent(__uuidof(IDXGIFactory2), (void**)&f); ad->Release();
		if (FAILED(hr) || !f) { Dbg("[FLIP] GetFactory2 hr=%08X\n", (unsigned)hr); dxdev->Release(); return false; }
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = (UINT)(g_flipLatency + 1);   // was 2  -> latency+1 (3 for latency 2)
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		hr = f->CreateSwapChainForComposition(s_flDev, &sd, nullptr, &s_flSwap); f->Release();
		if (FAILED(hr) || !s_flSwap) { Dbg("[FLIP] CreateSwapChainForComposition hr=%08X\n", (unsigned)hr); dxdev->Release(); return false; }
		s_flW = w; s_flH = h;
		{   // pacing mechanism without this, Present() never blocks,90fps + tear
			IDXGISwapChain2* sc2 = nullptr;
			if (SUCCEEDED(s_flSwap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) && sc2) {
				sc2->SetMaximumFrameLatency(g_flipLatency);
				s_flWaitable = sc2->GetFrameLatencyWaitableObject();
				sc2->Release();
			}
			if (s_flWaitable) Dbg("[FLIP] waitable acquired — vblank pacing armed\n");
			else Dbg("[FLIP] WARNING no waitable (QI IDXGISwapChain2 failed) — will tear/90fps\n");
		}
		{   // declare SDR full-range, harmless, and stops any downstream limited"assumption on  content
			IDXGISwapChain3* sc3 = nullptr;
			if (SUCCEEDED(s_flSwap->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) && sc3) {
				UINT sup = 0;
				if (SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &sup))
					&& (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
					sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
				sc3->Release();
			}
		}
		hr = DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice), (void**)&s_flDcDev); dxdev->Release();
		if (FAILED(hr) || !s_flDcDev) { Dbg("[FLIP] DCompositionCreateDevice hr=%08X\n", (unsigned)hr); return false; }
		hr = s_flDcDev->CreateTargetForHwnd(hwnd, TRUE, &s_flDcTgt);
		if (FAILED(hr) || !s_flDcTgt) { Dbg("[FLIP] CreateTargetForHwnd hr=%08X\n", (unsigned)hr); return false; }
		hr = s_flDcDev->CreateVisual(&s_flDcVis);
		if (FAILED(hr) || !s_flDcVis) { Dbg("[FLIP] CreateVisual hr=%08X\n", (unsigned)hr); return false; }
		s_flDcVis->SetContent(s_flSwap);
		s_flDcTgt->SetRoot(nullptr);
		s_flDcDev->Commit();
		Dbg("[FLIP] init OK %dx%d hwnd=%p — DComp armed (expect Independent Flip)\n", w, h, hwnd);
		if (g_flipSuppressEngine) Flip_InstallEngineHijack();   // <<< ADD

		DEVMODEA dm = {}; dm.dmSize = sizeof dm;
		if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			g_refreshMs = 1000.0 / (double)dm.dmDisplayFrequency;

		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { Dbg("[FLIP] init EXCEPTION\n"); return false; }
}
// Toggle the engine's own vsync. enable=false -> set NOVSYNC (surface+4|0x200): engine flip stops
// blocking, so our Present(1,0) is the SOLE vsync wait (no double-block -> no 30fps). enable=true ->
// clear it (menu: engine paces itself). Doc §4: nothing else writes 0x200, we're the only owner.
static void Flip_EngineVsync(bool enable)
{
	__try {
		uintptr_t surf = *(uintptr_t*)0x673D20;
		if (surf <= 0x10000 || surf >= 0x7FFFFFFF) return;
		DWORD* flags = (DWORD*)(surf + 4);
		if (enable) *flags &= ~0x200u;   // vsync (default engine behaviour)
		else        *flags |= 0x200u;   // NOVSYNC (we own the vblank)
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Single-presenter mode: swap the engine's Vid_FlipBuffers (@0x595044, populated at boot by
// Vid_LoadSysDLL) for a stub. While OUR overlay is shown, the stub skips the engine's DDraw flip
// entirely -> only our vsync'd DComp Present reaches the display -> no two-plane tear. The stub
// SELF-GATES on s_flShown, so the menu (shown=0) flips normally. Same thread as the caller (render
// thread) -> no locking needed.
typedef int(__stdcall* fn_vidflip_t)(uintptr_t);
static void* s_realFlipFn = nullptr;
static int  __stdcall Flip_EngineFlipStub(uintptr_t surf)
{
	if (s_flShown && !s_flDead) return 0;                         // our present owns the screen: skip engine flip
	if (s_realFlipFn) return ((fn_vidflip_t)s_realFlipFn)(surf);  // menu / dead: normal flip
	return 0;
}
static void Flip_InstallEngineHijack()
{
	if (s_realFlipFn) return;
	__try {
		void* cur = *(void**)0x595044;                           // Vid_FlipBuffers fn-ptr
		if (!cur || cur == (void*)&Flip_EngineFlipStub) return;
		s_realFlipFn = cur;
		DWORD o; VirtualProtect((void*)0x595044, sizeof(void*), PAGE_READWRITE, &o);
		*(void**)0x595044 = (void*)&Flip_EngineFlipStub;
		VirtualProtect((void*)0x595044, sizeof(void*), o, &o);
		Dbg("[FLIP] engine flip hijacked (real=%p) — single-presenter mode\n", s_realFlipFn);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void Flip_Show()
{
	if (!s_flReady || s_flShown) return;
	__try {
		s_flDcTgt->SetRoot(s_flDcVis); s_flDcDev->Commit(); s_flShown = true;
		if (!g_flipSuppressEngine && g_flipVsync) Flip_EngineVsync(false);   // novsync fallback ONLY when not suppressing
		if (g_flipLog) Dbg("[FLIP] SHOWN (suppress=%d vsync=%d)\n", (int)g_flipSuppressEngine, (int)g_flipVsync);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void Flip_Hide()
{
	if (!s_flShown) return;
	__try {
		s_flDcTgt->SetRoot(nullptr); s_flDcDev->Commit(); s_flShown = false; s_flGrabbed = false;
		if (!g_flipSuppressEngine) Flip_EngineVsync(true);                   // fallback path restores engine vsync
		if (g_flipLog) Dbg("[FLIP] HIDDEN\n");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// called from Hook_CxxRestore (0x4CAF50) — surface is still locked here
static void Flip_Grab()
{
	if (!s_flReady || s_flDead) return;
	__try {
		LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
		uintptr_t surf = *(uintptr_t*)0x673D20;
		if (surf <= 0x10000 || surf >= 0x7FFFFFFF) return;
		uint8_t* src = *(uint8_t**)(surf + 80);
		int pitch = *(int*)(surf + 84);
		int w = *(int*)(surf + 72), h = *(int*)(surf + 76);
		if (!src || (uintptr_t)src <= 0x10000) return;
		if (w < 320 || w > 7680 || h < 240 || h > 4320) return;
		if (pitch < w * 4) {                            // 16-bpp surface: needs 32bit_mode=1
			static bool warned = false;
			if (!warned) {
				warned = true; Dbg("[FLIP] surface ~%d-bpp (pitch=%d) — flip_present needs 32bit_mode=1; disabling\n",
					w ? pitch * 8 / w : 0, pitch);
			}
			InterlockedExchange(&s_flDead, 1); return;
		}
		if (!Flip_EnsureSize(w, h)) { InterlockedExchange(&s_flDead, 1); return; }
		int idx = s_flStageRing;                        // write a DIFFERENT buffer than the GPU is reading
		ID3D11Texture2D* stg = s_flStaging[idx];
		if (!stg) return;
		D3D11_MAPPED_SUBRESOURCE m;
		if (FAILED(s_flCtx->Map(stg, 0, D3D11_MAP_WRITE, 0, &m))) return;
		CopyRowsToStaging((uint8_t*)m.pData, (int)m.RowPitch, src, pitch, w * 4, h);
		s_flCtx->Unmap(stg, 0);
		s_flStageCur = idx;
		s_flGrabbed = true; ++s_flGrabs;
		QueryPerformanceCounter(&b);
		g_lastGrabMs = (double)(b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart;
		g_grabMsEma = g_grabMsEma * 0.9 + g_lastGrabMs * 0.1;
		if (g_lastGrabMs > g_grabMsMax) g_grabMsMax = g_lastGrabMs;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Real vblank anchor from DXGI. SyncQPCTime = QPC of the vblank the last DISPLAYED present latched
// onto; SyncRefreshCount = the real display-refresh counter. That's a hardware-locked PHASE + GRID,
// which is exactly what Alpha()'s next-vblank lookahead needs. Replaces "pb after Present == a vblank" false under a latency-2 waitable (the queue depth offsets pb from the true scanout, and the pump's
// 2nd present per tick drags the pb-interval EMA below 16.67). The grid here is deltaQPC/deltaRefresh,
// so it's immune to how often we present. Fails cleanly -> pb fallback in Hook_Render takes over.
static bool Flip_SampleVblank()
{
	if (!s_flSwap || !g_qpf.QuadPart) { ++g_flHbStatsFail; return false; }
	__try {
		DXGI_FRAME_STATISTICS st = {};
		HRESULT hr = s_flSwap->GetFrameStatistics(&st);
		if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
			g_flSyncRefresh = 0; g_flSyncQpc = 0;      // mode/composition change -> re-baseline next good sample
			++g_flHbStatsFail; return false;
		}
		if (FAILED(hr) || !st.SyncQPCTime.QuadPart || !st.SyncRefreshCount) {
			++g_flHbStatsFail; return false;           // composited / not yet live -> no scanout data
		}
		if (st.SyncRefreshCount != g_flSyncRefresh) {
			// derive refreshMs from REAL refreshes between two good samples (pump-immune)
			if (g_flSyncRefresh && st.SyncRefreshCount > g_flSyncRefresh
				&& st.SyncQPCTime.QuadPart > g_flSyncQpc) {
				double per = (double)(st.SyncQPCTime.QuadPart - g_flSyncQpc) * 1000.0 / (double)g_qpf.QuadPart
					/ (double)(st.SyncRefreshCount - g_flSyncRefresh);
				if (per > 5.0 && per < 25.0) g_refreshMs = g_refreshMs * 0.9 + per * 0.1;
			}
			g_flSyncRefresh = st.SyncRefreshCount;
			g_flSyncQpc = st.SyncQPCTime.QuadPart;
		}
		g_lastVblankQpc = st.SyncQPCTime.QuadPart;     // the anchor nvba interpolates toward
		g_haveVblank = true;
		s_flVblankFromStats = true;
		++g_flHbStatsOk;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { ++g_flHbStatsFail; return false; }
}
// called from Hook_Render, AFTER oRender() (engine's own flip already paced us to vsync)
static void Flip_Present()
{
	s_flVblankFromStats = false;    // reset every present, SampleVblank sets it true on success
	if (!s_flReady || s_flDead || !s_flGrabbed || !s_flSwap) return;
	ID3D11Texture2D* stg = s_flStaging[s_flStageCur];
	if (!stg) return;
	__try {
		LARGE_INTEGER w0, w1, p1; QueryPerformanceCounter(&w0);
		if (s_flWaitable) WaitForSingleObject(s_flWaitable, 100);
		QueryPerformanceCounter(&w1);
		ID3D11Texture2D* bb = nullptr;
		if (FAILED(s_flSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
		s_flCtx->CopyResource(bb, stg);
		bb->Release();
		UINT sync = g_flipVsync ? 1u : 0u;
		HRESULT hr = s_flSwap->Present(sync, 0);
		s_flGrabbed = false; ++s_flPresents;
		s_flStageRing = (s_flStageRing + 1) % g_flStageRingN;   // advance AFTER the copy is queued
		QueryPerformanceCounter(&p1);
		g_lastWaitMs = (double)(w1.QuadPart - w0.QuadPart) * 1000.0 / g_qpf.QuadPart;
		g_lastPresentMs = (double)(p1.QuadPart - w1.QuadPart) * 1000.0 / g_qpf.QuadPart;
		g_waitMsEma = g_waitMsEma * 0.9 + g_lastWaitMs * 0.1;
		g_presentMsEma = g_presentMsEma * 0.9 + g_lastPresentMs * 0.1;
		if (FAILED(hr)) {
			Dbg("[FLIP] Present hr=%08X -> permanent fallback to engine present\n", (unsigned)hr);
			InterlockedExchange(&s_flDead, 1); Flip_Hide(); return;
		}
		Flip_SampleVblank();          // real DXGI vblank anchor + refreshMs grid (drives nvba)
		if (!s_flShown) Flip_Show();
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void Flip_EnsureInit()
{
	if (!g_flipPresent || s_flReady || s_flDead) return;
	if (!CameraPhys()) return;                          // only arm during a real game render
	HWND hwnd = Flip_FindHwnd(); if (!hwnd) return;
	int w = 0, h = 0; if (!Flip_ReadSurfaceDims(w, h)) return;
	if (Flip_Init(hwnd, w, h) && Flip_EnsureSize(w, h)) s_flReady = true;
	else {
		Flip_ReleaseAll(); InterlockedExchange(&s_flDead, 1);
		Dbg("[FLIP] init failed -> engine present fallback (screen uncovered)\n");
	}
}
static void __cdecl Hook_CxxRestore()
{
	Flip_Grab();        // grab BEFORE the unlock (guards internally on ready/dead)
	oCxxRestore();      // Vid_FreeSurface(g_surface): the real final unlock
}

///


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
	if (g_debugLogic)
		Dbg("[60] banner dedup: sets cleared (level start)\n");
}
/// fixing one vanilla crash and one mod crash
static uint8_t* const g_pBusRouteNodeCount = (uint8_t*)0x66BB49;
static const int      kBusRouteNodeCap = 100;

static volatile bool  g_sessionLive = false;
static uint32_t       g_lastPumpTick = 0xFFFFFFFFu;
static void Mod_ResetSessionState(const char* why)
{
	size_t o = g_obj.size(), d = g_dead.size(), m = g_moved.size(), ar = g_arrow.size();
	g_obj.clear();
	g_dead.clear();
	g_moved.clear();
	g_savedObj.clear();
	g_live.clear();
	g_liveVec.clear();
	g_arrow.clear();
	g_tiles.clear();
	if (++g_tileGen == 0) { memset(g_tileStamp, 0, sizeof g_tileStamp); g_tileGen = 1; }
	memset(g_camChord, 0, sizeof g_camChord);
	memset(g_camPrevVel, 0, sizeof g_camPrevVel);
	g_camVelTick = 0xFFFFFFFFu;
	g_savedRectValid = false;
	g_lastPumpTick = 0xFFFFFFFFu;
	g_havePA = false;
	g_pedKind = -1; g_pedTries = 0;
	g_lastObj = nullptr;

	Dbg("[SESS] reset (%s): dropped objs=%zu dead=%zu moved=%zu arrows=%zu\n", why, o, d, m, ar);
}

static char* __fastcall Hook_LevelStart(void* thisp, void* edx)
{
	Dbg("[Level] Level Start\n");
	g_endArrowReached = false;
	g_endArrowReachTick = 0;
	static bool s_surfOnce = false;
	if (g_surfClawback && !s_surfOnce) {
		s_surfOnce = true;
		if (!g_d3dBase) { HMODULE h = GetModuleHandleA("d3ddll.dll"); if (h) g_d3dBase = (uintptr_t)h; }
		if (g_d3dBase) {
			g_pE43E18 = g_d3dBase + 0x43E18;
			void* bs = (void*)GetProcAddress((HMODULE)g_d3dBase, "gbh_BeginScene");
			if (!bs) bs = (void*)(g_d3dBase + 0x4BF0);   // RVA fallback
			if (bs && MH_CreateHook(bs, (void*)&Hook_BeginScene, (void**)&oBeginScene) == MH_OK
				&& MH_EnableHook(bs) == MH_OK)
				Dbg("[SURF] gbh_BeginScene hooked @%p (E43E18 force armed)\n", bs);
			else Dbg("[SURF] gbh_BeginScene hook FAILED\n");
			Patch_SurfClawback();
		}
		else Dbg("[SURF] d3ddll not found — clawback aborted\n");
	}
	if (g_bannerDedup || g_bannerDedupCar) BannerDedupReset();
	if (g_audioWarmUp) StartAudioWarm();
	uint8_t busBefore = *g_pBusRouteNodeCount;
	if (g_busCountReset) *g_pBusRouteNodeCount = 0;    // fresh transitMgr we ensure it's fresh count
	char* r = oLevelStart(thisp, edx);                 // Transit_SpawnFromZones refills it here
	uint8_t busAfter = *g_pBusRouteNodeCount;
	if (g_busCountReset && busAfter > kBusRouteNodeCap) {
		*g_pBusRouteNodeCount = (uint8_t)kBusRouteNodeCap;   // never let the bus scan walk off the block
	}
	Dbg("[BUS] route nodes before=%u after=%u cap=%d%s\n", busBefore, busAfter, kBusRouteNodeCap,
		busAfter > kBusRouteNodeCap ? "  *** map exceeds cap, clamped ***" : "");
	return r;
}

static char* __cdecl Hook_SessionTeardown()
{
	Dbg("[Level] Level End/Game Quit?\n");
	g_sessionLive = false;             // stop render nterp before pools die
	Flip_Hide();                       // reveal engine content so the menu isn't under a stale frame
	char* r = oSessionTeardown();      // pools freed here (Hook_Dtor floods g_dead)
	Mod_ResetSessionState("teardown"); // wipe after the dtors ran
	return r;
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
// Alpha() = interp fraction into the current 30Hz logic tick.

static float Alpha()
{
	DWORD due = *(DWORD*)g.pNextDue;
	double ftd = (g_frametimeEMA > 1.0) ? g_frametimeEMA : 33.333;
	DWORD now = timeGetTime();
	double t = (double)(int)(now - due) + ftd;   // (now-due): DWORD-wrap-safe small signed; +ftd keeps the 0.333
	if (g_nextVblankAlpha && g_haveVblank && g_qpf.QuadPart)
	{
		LARGE_INTEGER q; QueryPerformanceCounter(&q);
		double sinceV = (double)(q.QuadPart - g_lastVblankQpc) * 1000.0 / (double)g_qpf.QuadPart;
		double refresh = (g_refreshMs > 1.0) ? g_refreshMs : 16.667;
		if (sinceV >= 0.0 && sinceV < 1000.0)
		{
			double ready = sinceV + g_sceneQPCema;         // frame isn't submitted until scene build done
			double k = ceil(ready / refresh - 1e-6);       // which upcoming vblank catches this frame
			double latency = k * refresh - sinceV;         // ms from now until that vblank displays it
			if (latency < 0.0) latency = 0.0;
			double capL = refresh + g_sceneQPCema + 1.0;
			if (latency > capL) latency = capL;
			t += latency;
		}
	}
	double a = t / ftd;                  // was t / (double)ft
	return a < 0 ? 0.f : (a > 1 ? 1.f : (float)a);
}
static int __fastcall Hook_Slots(void* thisp, void* edx)
{
	if (!g_doArrows) return oSlots(thisp, edx);
	if (BadPtr((uintptr_t)thisp)) return oSlots(thisp, edx);   // never probe a junk base
	struct { int32_t* s; int32_t x, y; } touched[17];
	int   nTouched = 0;
	float al = Alpha();
	__try
	{
		for (int i = 0; i < 17; i++)
		{
			int32_t* s = (int32_t*)((char*)thisp + (size_t)124 * i);
			if (BadPtr((uintptr_t)s)) continue;                   // guard BEFORE the read, not just __except
			uint8_t  en; int32_t liveX, liveY;
			en = *((uint8_t*)s + 45); liveX = s[0]; liveY = s[1];
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
					if (g_debugLogic)
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

			s[0] = lx; s[1] = ly;
			touched[nTouched].s = s; touched[nTouched].x = liveX; touched[nTouched].y = liveY; ++nTouched;

		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	int r = oSlots(thisp, edx);
	__try
	{
		for (int i = 0; i < nTouched; i++)
		{
			touched[i].s[0] = touched[i].x; touched[i].s[1] = touched[i].y;
		}
	}
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
			if (g_debugLogic)
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

static void PostCenteredText(const wchar_t* text, int ticks)
{
	__try {
		if (!g_pHud) return;
		uintptr_t hud = *(uintptr_t*)g_pHud;
		if (hud < 0x10000 || hud > 0x7FFFFFFF) return;
		uint8_t* slot = (uint8_t*)(hud + 10168);           // CenteredText1
		int t = ticks; if (t < 1) t = 1; if (t > 255) t = 255;
		slot[0] = (uint8_t)t;                              // display timer
		lstrcpynW((wchar_t*)(slot + 2), text, 60);
		Hud_LayoutCenteredText1_fn(slot, nullptr);         // centered width -> +132
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

static char __cdecl Hook_Logic()
{
	CP(10);
	++g_tick;
	
/*	if (GetAsyncKeyState(VK_RCONTROL) & 0x8000)
	{
		wchar_t text[64];
		
		int iScore = GetPlayerScore();

		if (*(uintptr_t*)ADDR_pMissionState != 0)
		{
			int iScoreMax = *((*(uintptr_t**)ADDR_pMissionState) + 196);

			_snwprintf(text, 63, L"%d score / %d",
				iScore, iScoreMax);
			text[63] = 0;
			PostCenteredText(text, 75);
		}

	}*/
	

	if (g_zoom) {
		int vkOut = g_zoomKeys ? VK_OEM_MINUS : VK_SUBTRACT;
		int vkIn = g_zoomKeys ? VK_OEM_PLUS : VK_ADD;
		if (GetAsyncKeyState(vkOut) & 0x8000) g_zoomOffset += g_zoomStep;
		if (GetAsyncKeyState(vkIn) & 0x8000) g_zoomOffset -= g_zoomStep;
		if (g_zoomOffset > g_zoomMax) g_zoomOffset = g_zoomMax;
		if (g_zoomOffset < g_zoomMin) g_zoomOffset = g_zoomMin;
		if (g_zoomPerspKey && g_zoomPerspAllowTogglingWithNumKeys) {                                   
		// just failed experiment, i should probably just remove this, 
		// num2: A<->alt preset
			static bool pk = false; bool d = (GetAsyncKeyState(g_zoomPerspKey) & 0x8000) != 0;
			if (d && !pk) {
				int t = g_zoomPersp; g_zoomPersp = g_zoomPerspAlt; g_zoomPerspAlt = t;
				g_zoomSnapTick = g_tick;
				Dbg("[ZOOM] perspective=%d%% (%s)\n", g_zoomPersp,
					g_zoomPersp <= 0 ? "flat/A" : g_zoomPersp >= 100 ? "splay/B" : "blend");
			}
			pk = d;
		}
		if (g_zoomPerspStep && g_zoomPerspAllowTogglingWithNumKeys) {                                  // num1/num3: nudge live
			static bool pd = false, pu = false;
			bool dn = (GetAsyncKeyState(g_zoomPerspDnKey) & 0x8000) != 0;
			bool up = (GetAsyncKeyState(g_zoomPerspUpKey) & 0x8000) != 0;
			if ((dn && !pd) || (up && !pu)) {
				g_zoomPersp += up ? g_zoomPerspStep : -g_zoomPerspStep;
				if (g_zoomPersp < 0) g_zoomPersp = 0; if (g_zoomPersp > 100) g_zoomPersp = 100;
				g_zoomSnapTick = g_tick;          // <-- ADD: snap, no ramp-wobble while dialing
				Dbg("[ZOOMPERSP] %d%%\n", g_zoomPersp);
			}
			pd = dn; pu = up;
		}
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
	g_sessionLive = true;              // first tick of this session done -> g_moved is trustworthy
	// end-level arrow grace: detect score>=max rising edge, anchor the countdown
	if (g_bhideEndLevelArrowIf_Far && g_hideEndMarkerWhenFurtherThan > 0 && g_endArrowGraceSeconds > 0)
	{
		int scoreMax = GetScoreMax();
		bool reached = (scoreMax > 0 && GetPlayerScore() >= scoreMax);
		if (reached && !g_endArrowReached)
			g_endArrowReachTick = g_tick;   // estart the grace clock on the rising edge
		g_endArrowReached = reached;
	}
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
		if (g_flipPresent && g_debugLogic)
			Dbg("[FLIP] ready=%d shown=%d dead=%d grabs=%u pres=%u %dx%d | grabEMA=%.2f grabMAX=%.2f presentEMA=%.2f waitEMA=%.2f ring=%d | stats ok=%u fail=%u refresh=%.3f\n",
				(int)s_flReady, (int)s_flShown, (int)s_flDead, s_flGrabs, s_flPresents, s_flW, s_flH,
				g_grabMsEma, g_grabMsMax, g_presentMsEma, g_waitMsEma, g_flStageRingN,
				g_flHbStatsOk, g_flHbStatsFail, g_refreshMs);
		s_flGrabs = 0; s_flPresents = 0;
		g_grabMsMax = 0.0;
		g_flHbStatsOk = 0; g_flHbStatsFail = 0;

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
			if (g_dAN)
			{
				double mA = g_dASum / g_dAN, vA = g_dASumSq / g_dAN - mA * mA; if (vA < 0) vA = 0;
				double mS = g_csN ? g_csSum / g_csN : 0.0, vS = g_csN ? g_csSumSq / g_csN - mS * mS : 0.0; if (vS < 0) vS = 0;
				double mL = g_latN ? g_latSum / g_latN : 0.0, vL = g_latN ? g_latSumSq / g_latN - mL * mL : 0.0; if (vL < 0) vL = 0;
				double cv = mS > 1.0 ? sqrt(vS) / mS : 0.0;
				Dbg("[PERF6] nvba=%d refresh=%.2f sc=%.1f | dA m=%.3f[%.3f..%.3f]sd=%.3f | camStep(t) m=%.4f sd=%.4f CV=%.2f | lat m=%.1f[%.1f..%.1f]sd=%.2f n=%u\n",
					(int)g_nextVblankAlpha, g_refreshMs, g_sceneQPCema,
					mA, g_dAMin, g_dAMax, sqrt(vA),
					mS / 16384.0, sqrt(vS) / 16384.0, cv,
					mL, g_latMin, g_latMax, sqrt(vL), g_dAN);
			}
			g_dASum = g_dASumSq = 0; g_dAMin = 1e9; g_dAMax = -1e9; g_dAN = 0;
			g_csSum = g_csSumSq = 0; g_csN = 0;
			g_latSum = g_latSumSq = 0; g_latMin = 1e18; g_latMax = 0; g_latN = 0;
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
		if (g_debugLogic)
			Dbg("[ENV] screen=%dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
		EnumWindows(EnumWndProc, 0);
	}

	LogVideoMode();
	Flip_EnsureInit();
	g_camPtr = CameraPhys();
	LARGE_INTEGER qComposeStart; QueryPerformanceCounter(&qComposeStart);   // [PERF6] compose-time stamp
	float a = Alpha();
	if (g_doCamera && g_camPtr)
	{
		int n = g_camZ ? 3 : 2;
		bool smoothed = false;
		__try { smoothed = (g_camPtr[15] == 1); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		// once per tick (first render after Camera_Commit): promote chords
		if (g_camHermite && smoothed && g_camVelTick != g_tick) {
			for (int i = 0; i < 3; i++) {
				g_camPrevVel[i] = g_camChord[i];
				g_camChord[i] = g_camPtr[38 + i] - g_camPtr[34 + i];
			}
			g_camVelTick = g_tick;
		}
		double al = (double)a;
		for (int i = 0; i < n; i++)
		{
			int32_t p = g_camPtr[34 + i];
			int32_t c = g_camPtr[38 + i];
			g_savedXY[i] = c;
			int64_t D = (int64_t)c - p;
			if (llabs(D) > TELE) { g_camPrevVel[i] = (int32_t)D; continue; }  // teleport: skip + reseed
			int32_t off;
			if (g_camHermite && smoothed) {
				int64_t v = g_camPrevVel[i];
				int64_t lo = (D > 0) ? 0 : 2 * D, hi = (D > 0) ? 2 * D : 0;       // same sign as D, |v|<=2|D|
				if (v < lo) v = lo; if (v > hi) v = hi;                       // -> monotonic, no overshoot/hook
				double t = al, t2 = t * t, t3 = t2 * t;
				double herm = (double)D * (2 * t2 - t3) + (double)v * (t3 - 2 * t2 + t);
				double lin = (double)D * t;
				off = (int32_t)llround(lin + (herm - lin) * (g_camHermiteStrength * 0.01));
			}
			else {
				off = (int32_t)llround((double)D * al);                      // unchanged linear path
			}
			g_camPtr[38 + i] = p + off;
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
	int32_t g6cx = 0, g6cy = 0;                                             // [PERF6] interpolated cam this present
	if (g_camPtr) __try { g6cx = g_camPtr[38]; g6cy = g_camPtr[39]; }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	CP(21);
	g_savedObj.clear();
	if (g_doObjs && g_sessionLive)     // never touch disps before this session's first tick
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
			if (kk < 2)
				continue;    // kind 0/1 never render (Disp_KindIsDrawable = kind>1): skip interp/restore/SNAP spam
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
						if (g_debugLogic)
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
					if (g_debugLogic)
						Dbg("[SEH] dead obj on WRITE d=%p tick=%u\n", d, g_tick);
				}
			}
		}
	}

	CP(24);
	CP(25);
	LARGE_INTEGER pa; QueryPerformanceCounter(&pa);
	char r = oRender();                 // scene build + grab(@4CAF50) + engine flip (NOVSYNC while shown)
	Flip_Present();                     // Present(1,0) = the vsync block
	LARGE_INTEGER pb; QueryPerformanceCounter(&pb);   // pb AFTER the real present/vblank (same semantics as before)
	double pres = (double)(pb.QuadPart - pa.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
	g_presentQPCema = g_presentQPCema * 0.9 + pres * 0.1;
	// nvba anchor/grid: prefer the hardware vblank sampled in Flip_SampleVblank (phase and refreshMs,
	// immune to present frequency). Only when DXGI stats are unavailable — no presenter, kicked to DWM
	// composition or disjoint — fall back to pb a vblnak thing (contaminated by the pump,
	// but it's all we've got on that path).
	if (!s_flVblankFromStats) {
		if (g_haveVblank) {
			double iv = (double)(pb.QuadPart - g_lastVblankQpc) * 1000.0 / (double)g_qpf.QuadPart;
			if (iv > 5.0 && iv < 25.0) g_refreshMs = g_refreshMs * 0.9 + iv * 0.1;
		}
		g_lastVblankQpc = pb.QuadPart; g_haveVblank = true;
	}

	if (g_debugLogic) {   // phase/judder stats only when logging
		double lat = (double)(pb.QuadPart - qComposeStart.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
		if (g_debugLogic && lat > 17.9)
			Dbg("[DROP] lat=%.1f scene=%.1f grab=%.1f wait=%.1f present=%.1f | surf=%.1f map=%.1f spr2=%.1f ui=%.1f remain=%.1f\n",
				lat, g_lastSceneMs, g_lastGrabMs, g_lastWaitMs, g_lastPresentMs,
				g_fSurfMs, g_fMapMs, g_fSpr2Ms, g_fUiMs,
				g_lastSceneMs - g_fSurfMs - g_fMapMs - g_fSpr2Ms - g_fUiMs);
		if (lat < g_latMin) g_latMin = lat;
		if (lat > g_latMax) g_latMax = lat;
		g_latSum += lat; g_latSumSq += lat * lat; ++g_latN;
		if (g_havePA) {
			double dA = (double)a - g_lastPA; if (dA < -0.5) dA += 1.0;   // unwrap tick boundary
			if (dA < g_dAMin) g_dAMin = dA;
			if (dA > g_dAMax) g_dAMax = dA;
			g_dASum += dA; g_dASumSq += dA * dA; ++g_dAN;
			double ddx = (double)(g6cx - g_lastPCamX), ddy = (double)(g6cy - g_lastPCamY);
			double step = sqrt(ddx * ddx + ddy * ddy);
			g_csSum += step; g_csSumSq += step * step; ++g_csN;
		}
		g_lastPA = a; g_lastPCamX = g6cx; g_lastPCamY = g6cy; g_havePA = true;
	}
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
		if (g_prevDue && ft > 0 && ft < 500) {

			double s = (double)ft;
			if (s <= 45.0) g_frametimeEMA = g_frametimeEMA * 0.90 + s * 0.10;
			g_frametime = (DWORD)(g_frametimeEMA + 0.5);
		}
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
	if (g_doPump && CameraPhys() && timeLeft > need && g_lastPumpTick != g_tick) {
		DWORD cap = (DWORD)(1000.0 / g_capFps);
		if ((now - g_lastPump) >= cap) {
			g_lastPumpTick = g_tick;    // at most ONE pump per logic tick
			g_lastPump = now; ++g_pumpN; CP(2);
			((fn_t)g.render)();
		}
	}
	else if (g_doPump) ++g_pumpSkips;
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
		if (g_skipIntro && !_stricmp(name, "do_play_movie")) return 0;
		if (g_windowMode != 0) {
			if (!_stricmp(name, "start_mode"))     return 0;        // force the windowed path
			if (!_stricmp(name, "window_width"))   return g_winW;   // live client size
			if (!_stricmp(name, "window_height"))  return g_winH;
		}
		if (g_resWidth > 0 && g_resHeight > 0) {                    // legacy exclusive-FS high-res override
			if (!_stricmp(name, "full_width") || !_stricmp(name, "window_width"))  return g_resWidth;
			if (!_stricmp(name, "full_height") || !_stricmp(name, "window_height")) return g_resHeight;
		}
	}
	return oRegGetInt(name, def);
}
static bool ResIsRealDisplayMode(int w, int h)
{
	DEVMODEA dm = {}; dm.dmSize = sizeof dm;
	for (int i = 0; EnumDisplaySettingsA(nullptr, i, &dm); ++i)
		if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h && dm.dmBitsPerPel >= 32)
			return true;
	return false;
}
static void ValidateConfig()
{

	bool surface32 = g_bApply32bpp || (g_windowMode != 0);   // windowed surface = desktop format = 32-bit
	if (!surface32) {
		if (g_flipPresent) { g_flipPresent = false;  Dbg("[CFG] flip_present needs 32bit (exclusive) -> OFF\n"); }
		if (g_surfClawback) { g_surfClawback = false; Dbg("[CFG] surf_clawback needs 32bit (exclusive) -> OFF\n"); }
	}
	// flip_* sub-knobs do nothing without the presenter -- nudge once, don't fight the user.
	if (!g_flipPresent &&
		(!g_flipVsync || !g_flipSuppressEngine || g_flipLatency != 2 || g_flStageRingN != 3))
		Dbg("[CFG] flip_present=0 -> flip_vsync/latency/suppress_engine/staging_ring are inert\n");

	if (g_camHermiteStrength < 0)   g_camHermiteStrength = 0;
	if (g_camHermiteStrength > 100) g_camHermiteStrength = 100;

	if (g_zoomMax < 0) g_zoomMax = 0;
	if (g_zoomMin < 0) g_zoomMin = 0;
	if (g_zoomMin > g_zoomMax)
	{
		Dbg("[CFG] zoom_min(%d) > zoom_max(%d) -> zoom_min=zoom_max\n", g_zoomMin, g_zoomMax);
		g_zoomMin = g_zoomMax;
	}
	if (g_zoomOffset > g_zoomMax) g_zoomOffset = g_zoomMax;
	if (g_zoomOffset < g_zoomMin) g_zoomOffset = g_zoomMin;
	// only 60 is actually tested; keep capfps out of the absurd (crash-safe either way)
	if (g_capFps > 240.0) { Dbg("[CFG] capfps clamped to 240\n"); g_capFps = 240.0; }

	if (g_windowMode < 0 || g_windowMode > 2) g_windowMode = 0;
	if (g_scanlineMax < 480)  g_scanlineMax = 480;
	if (g_scanlineMax > 1700) g_scanlineMax = 1700;   // hard cap = real dword_E43F40 capacity
	// pick the target client size up-front (reg hook feeds it to Vid_ModeGate)
	if (g_windowMode == 1) {          // borderless, fill primary monitor (res_* overrides for downsample)
		g_winW = g_resWidth > 0 ? g_resWidth : GetSystemMetrics(SM_CXSCREEN);
		g_winH = g_resHeight > 0 ? g_resHeight : GetSystemMetrics(SM_CYSCREEN);
	}
	else if (g_windowMode == 2) {   // windowed: res_* or a sane default
		g_winW = g_resWidth > 0 ? g_resWidth : 1280;
		g_winH = g_resHeight > 0 ? g_resHeight : 960;
	}
	if (g_winW < 320) g_winW = 320;  if (g_winW > 7680) g_winW = 7680;
	if (g_winH < 240) g_winH = 240;  if (g_winH > 4320) g_winH = 4320;
	if (g_windowMode) Dbg("[WIN] window_mode=%d target=%dx%d\n", g_windowMode, g_winW, g_winH);

	// Exclusive FS can only  use an enumerated display mode. A bogus one (e.g. 1290x900)
	// -> Vid_CheckMode fails -> 640 fallback leaves surface+72/76/80/84 stale ->
	// frontend D3D-init (nvd3dum AV) or intro Bink BinkCopyToBuffer (binkw32 AV) writes OOB.
	if (g_windowMode == 0 && g_resWidth > 0 && g_resHeight > 0
		&& !ResIsRealDisplayMode(g_resWidth, g_resHeight)) {
		Dbg("[CFG] %dx%d is NOT a hardware display mode -> exclusive FS would crash. "
			"Override DROPPED. Use window_mode=1 (borderless) for arbitrary res.\n",
			g_resWidth, g_resHeight);
		g_resWidth = 0; g_resHeight = 0;
	}

	if (g_zoomPersp < 0) g_zoomPersp = 0;        if (g_zoomPersp > 100) g_zoomPersp = 100;
	if (g_zoomPerspAlt < 0) g_zoomPerspAlt = 0;  if (g_zoomPerspAlt > 100) g_zoomPerspAlt = 100;
}
static void Init()
{
	LoadConfig();
	if (g_logEnabled) { LogOpen(); LogInitRing(); }
	ValidateConfig();

	// broken currently
	//	{
	//		ModFlags mf{ &g_bApply32bpp, &g_menuScale, &g_skipIntro, &g_unifyRes, &g_zoom,
	//					 &g_doCamera, &g_childProfile, &g_mapProfile, &g_resWidth, &g_resHeight };
	//		Compat_Run(mf, g_wsCompat);
	//	}

	if (g_bApply32bpp)
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

	Dbg("[60] ==== ddraw proxy start ==== pump=%d(adapt=%d,budget=%.0f) objs=%d cam=%d camz=%d fdirty=%d predirty=%d unifyres=%d deadset=%d log=%d child=%d map=%d cap=%.0f nvba=%d\n",
		g_doPump, g_adaptivePump, g_pumpBudgetMs, g_doObjs, g_doCamera, g_camZ, g_forceDirty, g_preDirty, g_unifyRes, g_useDeadSet, g_logEnabled, g_childProfile, g_mapProfile, g_capFps, (int)g_nextVblankAlpha);
	Dbg("[FLIP] flip_present=%d vsync=%d suppress_engine=%d log=%d staging_ring=%d (needs 32bit_mode=%d)\n",
		(int)g_flipPresent, (int)g_flipVsync, (int)g_flipSuppressEngine,
		(int)g_flipLog, g_flStageRingN, (int)g_bApply32bpp);
	Dbg("[ZOOM] enabled=%d perspective=%d%% (alt=%d%%) off=%d step=%d max=%d min=%d keys=%s\n",
		(int)g_zoom, g_zoomPersp, g_zoomPerspAlt, g_zoomOffset, g_zoomStep, g_zoomMax, g_zoomMin,
		g_zoomKeys ? "-/=" : "numpad +/-");
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
	if (g_Watchdog)
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
	if (g_collectibleCounter) {
		if (MH_CreateHook((void*)0x43E550, (void*)&Hook_TryCollectPickup, (void**)&oTryCollect) != MH_OK)
			Dbg("[60] hook TryCollectPickup FAILED\n");
	}
	if (g_flipPresent) {
		if (MH_CreateHook((void*)0x4CAF50, (void*)&Hook_CxxRestore, (void**)&oCxxRestore) != MH_OK)
			Dbg("[FLIP] hook CxxRestore@4CAF50 FAILED\n");
	}

	if (g_bApply32bpp || g_menuScale) {
		if (MH_CreateHook((void*)0x452E20, (void*)&Hook_FrontendLoadMenuBgImages,
			(void**)&o_FrontendLoadMenuBgImages) != MH_OK)
			Dbg("[60] hook FrontendLoadMenuBgImages FAILED\n");
	}
	// reg hook now also needed for window_mode (start_mode / window size)
	if (g_skipIntro || (g_resWidth > 0 && g_resHeight > 0) || g_windowMode != 0)
		if (MH_CreateHook((void*)0x4B5660, (void*)&Hook_RegGetInt, (void**)&oRegGetInt) != MH_OK)
			Dbg("[60] hook Registry_GetInt FAILED\n");
	if (g_windowMode != 0) {
		if (MH_CreateHook((void*)ADDR_VidWindowedAllowed, (void*)&Hook_VidWindowedAllowed, (void**)&oVidWindowedAllowed) != MH_OK) Dbg("[WIN] hook WindowedAllowed FAILED\n");
		if (MH_CreateHook((void*)ADDR_VidSetWindowed, (void*)&Hook_VidSetWindowed, (void**)&oVidSetWindowed) != MH_OK) Dbg("[WIN] hook SetWindowed FAILED\n");
		if (MH_CreateHook((void*)ADDR_GameWndProc, (void*)&Hook_WndProc, (void**)&oWndProc) != MH_OK) Dbg("[WIN] hook WndProc FAILED\n");
	}
	if (g_sceneNodeGuard)
		if (MH_CreateHook((void*)0x4C4B40, (void*)&Hook_SceneNodeAlloc, (void**)&oSceneNodeAlloc) != MH_OK)
			Dbg("[SCENE] hook SceneNodePool_Alloc FAILED\n");

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
		if (!g_surfClawback) {
			if (MH_CreateHook((void*)g.surf, (void*)&Hook_Surf, (void**)&oSurf) != MH_OK) Dbg("[60] hook surf FAILED\n");
		}
		else Dbg("[60] childprofile: surf hook SKIPPED (surf_clawback owns 4CAEC0)\n");
		if (MH_CreateHook((void*)g.spr2, (void*)&Hook_Spr2, (void**)&oSpr2) != MH_OK) Dbg("[60] hook spr2 FAILED\n");
		if (MH_CreateHook((void*)g.ui, (void*)&Hook_Ui, (void**)&oUi) != MH_OK) Dbg("[60] hook ui FAILED\n");
	}

	if (g_doArrows)
	{
		if (MH_CreateHook((void*)g.slots, (void*)&Hook_Slots, (void**)&oSlots) != MH_OK)
			Dbg("[60] hook slots(arrows) FAILED\n");
	}

	if (g_hideEndMarkerWhenFurtherThan > 0)
	{
		if (MH_CreateHook((void*)g.slotsOne, (void*)&Hook_SlotsOne, (void**)&oDrawOne) != MH_OK)
			Dbg("[60] hook slotsOne(arrows) FAILED\n");
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


	// im lazy and im setting speed of text messages in-game here:
	DWORD o;
	VirtualProtect((void*)0x4C6DC3, 64, PAGE_EXECUTE_READWRITE, &o);
	memset((void*)0x4C6DC3, 0x90, 12);

	unsigned char* magic = (unsigned char*)0x4C6DCF;
	magic[0] = 0xB8;
	magic[1] = 0x01;
	magic[2] = 0x00;
	magic[3] = 0x00;
	magic[4] = 0x00;
	VirtualProtect((void*)0x4C6DC3, 64, o, &o);
/*
	if (g_bNoGangMeterBlink)
	{
		// fix blinking annoying thing:
		VirtualProtect((void*)0x4C751E, 64, PAGE_EXECUTE_READWRITE, &o);

		unsigned char* magic2 = (unsigned char*)0x4C751C;
		magic2[1] = 0xC0;
		VirtualProtect((void*)0x4C751E, 64, o, &o);
		//
	}
	*/
	LogLaunchEnv();
}
 
static bool IsTargetProcess()
{
	wchar_t path[MAX_PATH];
	DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);

	if (len == 0 || len == MAX_PATH)
		return false;

	const wchar_t* name = wcsrchr(path, L'\\');
	name = name ? name + 1 : path;

	return _wcsicmp(name, L"gta2.exe") == 0 ||
		_wcsicmp(name, L"gta.exe") == 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID)
{
	if (r == DLL_PROCESS_ATTACH && IsTargetProcess())
	{
		DisableThreadLibraryCalls(h);
		CreateThread(nullptr, 0, [](LPVOID)->DWORD { Init(); return 0; }, nullptr, 0, nullptr);
	}
	return TRUE;
}
