/* Nintendulator - Win32 NES emulator written in C++
 * MonitorSync implementation. See MonitorSync.h for the design overview.
 *
 * Strategy:
 *   1. Measure the display cadence independently of emulator timing. Prefer
 *      DWM's fractional refresh timing and fall back to the active display mode.
 *      This avoids the circular calibration of measuring an emulator that is
 *      itself already being paced to the display.
 *
 *   2. Pace the emulator's 60/50-Hz audio/video slot cadence against that
 *      measured display rate when the rates are close. This removes the
 *      60.0988-vs-59.94/60.00 beat that otherwise forces occasional dropped
 *      or repeated visual frames.
 *
 *   3. The render thread is separately responsible for presentation/vblank
 *      alignment. It does not run a second software frame timer.
 *
 * Every external API is loaded dynamically where practical so the binary still
 * runs on Windows 7+. Missing optional APIs fall back to the display-mode
 * timing path.
 */

#include "StdAfx.h"
#include "Nintendulator.h"
#include "resource.h"
#include "MapperInterface.h"
#include "MonitorSync.h"
#include "GFX.h"
#include "NES.h"
#include "APU.h"
#include <math.h>               // fabs (GetTargetHz rate comparison)
#include <dwmapi.h>

// ------------------------------------------------------------------
// WGL swap-control extension (present in every Windows OpenGL ICD
// since at least 2005; still loaded dynamically to be safe).
// ------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
typedef int  (WINAPI *PFN_wglGetSwapIntervalEXT)(void);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringARB)(HDC);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringEXT)(void);

// ------------------------------------------------------------------
// DXGI WaitForVBlank -- P28: bypass DWM for vsync (windowed and, as of
// P36, fullscreen too -- see the call site in GFX.cpp's GL_DrawFrame).
//
// ROOT CAUSE of the periodic ~20-30 second dropout:
//
//   wglSwapIntervalEXT(1) in WINDOWED mode on Windows Vista+ with DWM
//   enabled does NOT wait for the raw GPU vblank hardware interrupt.
//   Instead, the OpenGL driver waits for DWM's "composition ready"
//   signal, which is normally delivered at each vblank but is subject
//   to DWM's own internal maintenance cycles.  Approximately every
//   20-60 seconds DWM performs housekeeping (composition re-sync,
//   resource GC, GPU timeline cleanup) during which it delays its
//   "ready" signal by 1-2 extra vblank periods (~16-33 ms at 60 Hz).
//
//   SwapBuffers therefore blocks for ~33-50 ms instead of ~16 ms,
//   causing the NES emulation thread to miss the next vblank deadline.
//   Because APU::Run is called from the same thread as the renderer,
//   the audio buffer also loses one slot -- producing a simultaneous
//   video stutter and audio dropout.
//
//   This is the SAME DWM maintenance stall that caused DwmFlush() to
//   occasionally freeze (P26, fixed by disabling DwmFlush).  Disabling
//   DwmFlush removed one path through DWM but SwapBuffers(interval=1)
//   in windowed mode still routes through DWM's composition signal.
//
// SOLUTION:
//
//   IDXGIOutput::WaitForVBlank() delivers the vblank notification
//   directly from the GPU hardware interrupt counter, bypassing DWM's
//   composition scheduler entirely.  We:
//     1. Call WaitForDXGIVBlank() to block until the raw vblank.
//     2. Call SwapBuffers with interval=0 immediately after, so the
//        present is submitted into that vblank slot without going
//        through DWM's timing path a second time.
//
//   DWM still composites the frame (we cannot bypass composition in
//   windowed mode without exclusive fullscreen), but our TIMING
//   decision -- when to start the next frame -- is no longer subject
//   to DWM's stall-prone maintenance cycle.
//
// COMPATIBILITY:
//
//   dxgi.dll is always present on Windows Vista and later.  We load it
//   via LoadLibraryW and call COM methods through vtable pointers, so
//   no dxgi.lib link or DXGI SDK header is required.  The existing
//   build system is unchanged.  On failure at any step (driver returns
//   error, dxgi.dll not found, etc.) the code falls back silently to
//   SwapBuffers(interval=1) -- exactly the pre-P28 behaviour.
//
// VTABLE OFFSETS (from DXGI 1.0 SDK, stable across all DXGI versions):
//
//   All DXGI objects derive from IDXGIObject which derives from IUnknown:
//     slot 0 = IUnknown::QueryInterface
//     slot 1 = IUnknown::AddRef
//     slot 2 = IUnknown::Release
//     slot 3 = IDXGIObject::SetPrivateData
//     slot 4 = IDXGIObject::SetPrivateDataInterface
//     slot 5 = IDXGIObject::GetPrivateData
//     slot 6 = IDXGIObject::GetParent
//   IDXGIFactory own methods start at slot 7:
//     slot 7 = IDXGIFactory::EnumAdapters
//   IDXGIAdapter own methods start at slot 7:
//     slot 7 = IDXGIAdapter::EnumOutputs
//   IDXGIOutput own methods start at slot 7:
//     slot 7  = IDXGIOutput::GetDesc
//     slot 8  = IDXGIOutput::GetDisplayModeList
//     slot 9  = IDXGIOutput::FindClosestMatchingMode
//     slot 10 = IDXGIOutput::WaitForVBlank
// ------------------------------------------------------------------

typedef HRESULT (__stdcall *PFN_Release_t)(void*);
typedef HRESULT (__stdcall *PFN_CreateDXGIFactory)(const GUID*, void**);
typedef HRESULT (__stdcall *PFN_EnumAdapters)(void*, UINT, void**);
typedef HRESULT (__stdcall *PFN_EnumOutputs)(void*, UINT, void**);
typedef HRESULT (__stdcall *PFN_WaitForVBlank)(void*);

// {7b7166ec-21c7-44ae-b21a-c9ae321ae369}
static const GUID s_IID_IDXGIFactory = {
    0x7b7166ec, 0x21c7, 0x44ae,
    {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}
};

// State for DXGI vblank. All pointers start NULL.
// g_DXGITried=true means InitDXGI() has run (result in g_DXGIAvailable).
static bool  g_DXGITried     = false;
static bool  g_DXGIAvailable = false;
static void* g_pDXGIFactory  = NULL;
static void* g_pDXGIAdapter  = NULL;
static void* g_pDXGIOutput   = NULL;

// P34: human-readable reason InitDXGI() succeeded/failed, surfaced through
// GetDXGIFailReason() into the timing log (see GFX.cpp DiagWriteLogFile).
// Added after two rounds of the log showing a bare "DXGI vblank bypass
// active: NO" with no way to tell WHERE it failed -- LoadLibrary,
// CreateDXGIFactory, or adapter/output enumeration all look identical
// from outside. g_DXGIFailBuf backs the sprintf'd variants;
// g_DXGIFailReason points at either that buffer or a static string
// literal for the fixed-text cases.
static TCHAR       g_DXGIFailBuf[128] = { 0 };
static const TCHAR *g_DXGIFailReason  = _T("not attempted yet");
// Swap interval to use in GL_DrawFrame: 0 when DXGI vblank is active
// (we wait ourselves), 1 otherwise (driver handles it via DWM).
// Written once by InitDXGI(); read every frame from GL_DrawFrame.
static volatile LONG g_DXGISwapInterval = 1;

// Forward declarations -- defined after namespace MonitorSync ends.
static void StartVBlankThread();
static void StopVBlankThread();

static void DXGI_Release(void* p)
{
    if (!p) return;
    void** vtbl = *(void***)p;
    ((PFN_Release_t)vtbl[2])(p);
}

// Lazy-initialize: enumerate adapters/outputs, cache the first working
// IDXGIOutput*. Returns true if WaitForVBlank is ready to use.
//
// P33 (session 11, confirmed by the user's timing log: "DXGI vblank
// bypass active: NO"): this used to hardcode adapter 0 / output 0 and
// bail out completely if that single combination failed. That fails
// outright on any system where the FIRST enumerated adapter has no
// attached display -- the most common case being a laptop with hybrid
// graphics (NVIDIA Optimus / AMD PowerXpress), where a muxless discrete
// GPU with zero outputs is commonly enumerated before the integrated GPU
// that actually drives the screen. On such systems EnumOutputs(adapter 0)
// returns DXGI_ERROR_NOT_FOUND immediately, InitDXGI silently fails
// (g_DXGIAvailable stays false), and the entire P28 DXGI vblank bypass
// never activates for the rest of the process's lifetime (g_DXGITried
// latches to true so it's never retried). Everything built on top of it
// since -- P28's bounded wait, P30's audio thread, P31's diagnostics --
// still runs, but none of it touches the actual problem, because the
// code never actually left the DWM-composited vsync path in the first
// place; wglSwapIntervalEXT(1) alone is what's throttling frames, with
// all the DWM-maintenance-cycle stall exposure that P28's own header
// comment describes.
//
// FIX: try every adapter (up to a generous cap; no real system has more
// than a handful), and on each adapter try output 0. Use the first
// (adapter, output) pair where EnumOutputs succeeds. This preserves the
// exact previous behavior on any system where adapter 0 already has a
// display (the overwhelming majority of desktops), and now also succeeds
// on hybrid-graphics laptops where it previously failed silently.
//
// KNOWN LIMITATION: this does not try to match the DXGI output to the
// monitor the game window actually lives on in a multi-monitor setup on
// a single adapter -- it takes output 0 of the first adapter with any
// output, same as the pre-P33 code did for adapter 0. If that ever
// matters (vblank signal from the wrong monitor when window is dragged
// to a second display with a different refresh rate), the fix is to
// enumerate every output on every adapter, compare each IDXGIOutput::
// GetDesc().Monitor against MonitorFromWindow(g_hWnd, ...), and re-run
// that match on WM_DISPLAYCHANGE / window move. Not implemented here --
// no evidence yet that this project's reported issue involves more than
// one monitor.
// P39 (session 16): kill switch for the entire P28 DXGI-vblank-bypass
// architecture (InitDXGI/VBlankThreadProc/WaitForDXGIVBlank/g_DXGISwapInterval=0).
//
// Sessions 12-15 chased this architecture through four rounds of fixes
// (P33 hybrid-graphics adapter enumeration, P34 failure-reason logging,
// P36 fullscreen gating, P37 stale-output HRESULT checking, P38 isolating
// wglMakeCurrent(NULL) from OnFrameEnd) and NONE of them fixed the user's
// actual symptom: a permanent ~5-6x frame time inflation (~90-100ms per
// frame instead of ~16.6ms), reproducing in every window mode, that does
// not clear up on its own. The session-16 log, with P38's split columns,
// shows the stall is not concentrated in any single call -- it's spread
// across BOTH `swap` (SwapBuffers + WaitForDXGIVBlank, ~75-83ms) AND `mcr`
// (wglMakeCurrent(NULL), ~16.7ms on top) simultaneously, while `ofe`
// (OnFrameEnd alone) is a confirmed, consistent 0.00ms. A single buggy
// call would show the delay concentrated in ONE column; a delay smeared
// across two completely different, unrelated Win32/GL calls, always
// summing to roughly the same ~90-100ms total, points to something more
// fundamental than any individual call: most plausibly a genuine present-
// queue backlog (frames queuing up on the GPU faster than the display can
// retire them) that the CPU thread ends up waiting out at whichever call
// happens to touch the GPU/context next -- which this architecture's own
// mixing of "wait ourselves" (interval=0 + WaitForDXGIVBlank) with the
// driver's own internal queuing was never designed to prevent, and the
// already-documented KNOWN LIMITATION (the cached IDXGIOutput* is never
// matched to the monitor the window is actually on) means the vblank
// we're waiting for may not even be the right one on multi-monitor
// systems, silently decoupling our own pacing from the real display.
//
// Rather than attempt a fifth targeted fix on unfamiliar hardware with no
// way to test locally, this switch reverts the whole feature to OFF:
// InitDXGI() now always fails immediately and cleanly, exactly as if
// DXGI/WaitForVBlank were unavailable on this machine. That collapses the
// pacing model back to the simple, well-understood path that predates
// P28 entirely: plain OpenGL vsync via wglSwapIntervalEXT(1), no custom
// polling thread, no explicit wait call, no interval=0. SwapBuffers()
// itself blocks until the real vblank, using the same driver mechanism
// every other OpenGL application on Windows relies on. The original P28
// motivation (an occasional ~20-30s DWM-composited-vsync stutter) is a
// far smaller problem than a permanent 5-6x slowdown, so this is a net
// improvement until DXGI bypass can be redesigned and actually verified
// against real hardware rather than iterated on blind from timing logs.
static const bool g_DXGIBypassKillSwitch = true;

static bool InitDXGI()
{
    if (g_DXGIBypassKillSwitch)
    {
        g_DXGITried = true;
        g_DXGIAvailable = false;
        g_DXGIFailReason = _T("disabled (P39: DXGI vblank bypass reverted after repeated unresolved regressions -- see comment above InitDXGI)");
        return false;
    }

    if (g_DXGITried) return g_DXGIAvailable;
    g_DXGITried = true; // mark so we never retry on failure

    HMODULE hDxgi = LoadLibraryW(L"dxgi.dll");
    if (!hDxgi) { g_DXGIFailReason = _T("LoadLibraryW(dxgi.dll) failed"); return false; }

    PFN_CreateDXGIFactory pfnCF =
        (PFN_CreateDXGIFactory)GetProcAddress(hDxgi, "CreateDXGIFactory");
    if (!pfnCF) { g_DXGIFailReason = _T("GetProcAddress(CreateDXGIFactory) failed"); return false; }

    void* pFac = NULL;
    HRESULT hrFac = pfnCF(&s_IID_IDXGIFactory, &pFac);
    if (FAILED(hrFac) || !pFac)
    {
        _stprintf_s(g_DXGIFailBuf, _countof(g_DXGIFailBuf), _T("CreateDXGIFactory() failed, hr=0x%08X"), (unsigned)hrFac);
        g_DXGIFailReason = g_DXGIFailBuf;
        return false;
    }
    g_pDXGIFactory = pFac;

    void** fvt = *(void***)pFac;

    UINT adaptersTried = 0, adaptersWithNoOutput = 0;
    for (UINT ai = 0; ai < 16; ai++)
    {
        void* pAdp = NULL;
        if (FAILED(((PFN_EnumAdapters)fvt[7])(pFac, ai, &pAdp)) || !pAdp)
            break; // DXGI_ERROR_NOT_FOUND -- no more adapters, stop trying
        adaptersTried++;

        void** avt = *(void***)pAdp;
        void* pOut = NULL;
        if (SUCCEEDED(((PFN_EnumOutputs)avt[7])(pAdp, 0, &pOut)) && pOut)
        {
            g_pDXGIAdapter = pAdp;
            g_pDXGIOutput  = pOut;
            break;
        }
        // This adapter has no outputs (e.g. a muxless discrete GPU with
        // no display attached) -- release it and try the next adapter.
        adaptersWithNoOutput++;
        DXGI_Release(pAdp);
    }

    if (!g_pDXGIOutput)
    {
        _stprintf_s(g_DXGIFailBuf, _countof(g_DXGIFailBuf),
                _T("no adapter had an output (tried %u adapter(s), %u had zero outputs)"),
                adaptersTried, adaptersWithNoOutput);
        g_DXGIFailReason = g_DXGIFailBuf;
        DXGI_Release(pFac);
        g_pDXGIFactory = NULL;
        return false;
    }

    _stprintf_s(g_DXGIFailBuf, _countof(g_DXGIFailBuf), _T("OK (found output on adapter index %u of %u tried)"),
            adaptersTried - 1, adaptersTried);
    g_DXGIFailReason = g_DXGIFailBuf;

    // Success: we'll use interval=0 + our own WaitForVBlank call.
    InterlockedExchange(&g_DXGISwapInterval, 0L);
    g_DXGIAvailable = true;
    return true;
}

// ==================================================================
namespace MonitorSync
{
// ==================================================================

// ------------------------------------------------------------------
// State
// ------------------------------------------------------------------
static bool           g_Initialized   = false;
static volatile LONG  g_Enabled       = FALSE;

static HWND           g_hWnd          = NULL;

static LARGE_INTEGER  g_QPCFreq       = {0, 0};

// Function pointers (optional; NULL on systems that lack them).
static PFN_wglSwapIntervalEXT    pfnWglSwapIntervalEXT    = NULL;
static PFN_wglGetSwapIntervalEXT pfnWglGetSwapIntervalEXT = NULL;

// Measured / cached values.
// g_MonitorHz is the ACTUAL display/compositor refresh rate, preferably
// obtained from DWM's fractional timing information (59.940, 59.9986,
// 60.000, etc.) rather than the integer dmDisplayFrequency value.
static volatile LONG  g_MonitorHzMilli = 60000; // 60.000 Hz, milli-Hz fixed point
static double         g_NESHz     = 60.0988;  // hardware/PPU native rate
// APU/GFX produce one audio/video slot per these nominal frame rates
// (60 NTSC/Dendy in this codebase, 50 PAL).  Match Monitor Rate must pace
// against this cadence, not the fractional PPU crystal rate above, because
// APU::LockSize is explicitly derived from WantFPS (60/50).
static double         g_FrameHz   = 60.0;     // emulation slot cadence

// Did we successfully enable OpenGL vsync?
static bool           g_VSyncActive = false;

// Absolute monitor-clock phase used by PaceSlot().  Unlike P51b's
// previous "last actual write" anchor, this schedule is tied to a fixed
// QPC epoch, so timer wake-up jitter does not random-walk the emulation
// phase relative to the display vblank.
static LARGE_INTEGER  g_PaceEpochQPC = {0, 0};
static ULONGLONG      g_PaceFrameIndex = 0;

// Pace diagnostics used by GFX.cpp. These values are diagnostic-only and
// never participate in pacing decisions.
static volatile LONGLONG g_LastPaceTargetQPC = 0;
static volatile LONGLONG g_LastPaceWakeQPC   = 0;
static volatile LONG     g_LastPaceSource   = 0; // 1=presentation, 0=fallback

// P60: display-presentation feedback clock.
//
// PaceSlot() used to run entirely from an independent QPC schedule. That gave
// the correct average cadence, but the schedule could slowly land on slightly
// different phases relative to the actual presentation boundary, producing
// pairs such as 17.9/15.8ms even though the mean stayed at ~16.67ms.
//
// The render thread now reports the QPC timestamp immediately after the
// present call. We use that timestamp as a bounded phase anchor for the NEXT
// frame. A long DWM maintenance stall is deliberately NOT promoted into the
// pacing clock: intervals far outside the normal refresh period temporarily
// unlock the presentation clock, so emulation continues on the independent
// QPC fallback instead of inheriting a 33/50ms stall.
//
// All shared 64-bit values are accessed through Interlocked* operations.
static volatile LONGLONG g_LastPresentationQPC        = 0;
static volatile LONGLONG g_PresentationPeriodQPC      = 0;
static volatile LONG     g_PresentationClockLocked   = FALSE;
static volatile LONG     g_PresentationHzMilli       = 0;
static volatile LONG     g_PresentationIntervalErrUs = 0;
// DWM presentation samples are snapshots, so track the displayed frame id
// separately. This rejects duplicate samples and prevents a 0ms/16ms pair
// from being interpreted as a real presentation cadence.
static volatile ULONGLONG g_LastDwmDisplayedFrame = 0;
static volatile LONG       g_PresentationSampleStreak = 0;
// P86/P88: DWM supplies phase, while PaceSlot owns one display-period target
// per call. In P88 the semantic caller is PaceFrame(), once per NES frame.
static volatile LONGLONG  g_PresentationAnchorQPC = 0;
static volatile ULONGLONG g_PresentationAnchorFrame = 0;
static volatile LONG      g_PresentationAnchorGeneration = 0;

// ------------------------------------------------------------------
// Deferred vsync interval (written by Enable/UI thread, applied by
// NES thread in ApplyPendingVSync inside GL_DrawFrame).
// ------------------------------------------------------------------
static volatile LONG  g_PendingVSyncInterval = -1;

// DWM-sync mode: interval=0 but g_VSyncActive stays true.
static volatile LONG  g_DwmSyncMode = 0;

// ------------------------------------------------------------------
// Waitable timer for PaceFrame.
// ------------------------------------------------------------------
static HANDLE g_PaceTimer = NULL;
typedef HANDLE (WINAPI *PFN_CreateWaitableTimerExW)(
    LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
static const DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_FLAG = 0x00000002;

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_DwmGetCompositionTimingInfo)(HWND, DWM_TIMING_INFO*);

static double QueryDwmMonitorHz()
{
    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (!hDwm)
        hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm)
        return 0.0;

    PFN_DwmGetCompositionTimingInfo pfn =
        (PFN_DwmGetCompositionTimingInfo)GetProcAddress(hDwm, "DwmGetCompositionTimingInfo");
    if (!pfn)
        return 0.0;

    DWM_TIMING_INFO ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);

    // Windows 8.1+ requires hwnd == NULL.  Windows 7 accepts a window
    // handle, so try the actual emulator window first and fall back to NULL
    // for newer systems that reject a non-NULL HWND.
    HRESULT hr = pfn(g_hWnd, &ti);
    if (FAILED(hr) && g_hWnd != NULL)
    {
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        hr = pfn(NULL, &ti);
    }

    if (FAILED(hr))
        return 0.0;

    if (ti.rateRefresh.uiDenominator != 0 && ti.rateRefresh.uiNumerator != 0)
    {
        double hz = (double)ti.rateRefresh.uiNumerator /
                    (double)ti.rateRefresh.uiDenominator;
        if (hz >= 30.0 && hz <= 1000.0)
            return hz;
    }

    // rateRefresh is authoritative, but qpcRefreshPeriod is a useful
    // fallback on drivers where the rational is temporarily unavailable.
    if (ti.qpcRefreshPeriod > 0 && g_QPCFreq.QuadPart > 0)
    {
        double hz = (double)g_QPCFreq.QuadPart /
                    (double)ti.qpcRefreshPeriod;
        if (hz >= 30.0 && hz <= 1000.0)
            return hz;
    }

    return 0.0;
}

static DWORD GetDisplayFrequencyFromEnum()
{
    // Use the monitor that actually contains the emulator window.  The old
    // EnumDisplaySettings(NULL, ...) queried the primary display even when
    // the window was moved to another monitor.
    TCHAR deviceName[CCHDEVICENAME];
    deviceName[0] = 0;
    if (g_hWnd)
    {
        HMONITOR hMon = MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST);
        if (hMon)
        {
            MONITORINFOEX mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfo(hMon, &mi))
                _tcsncpy_s(deviceName, _countof(deviceName), mi.szDevice, _TRUNCATE);
        }
    }

    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    LPCWSTR device = (deviceName[0] != 0) ? deviceName : NULL;
    if (EnumDisplaySettingsEx(device, ENUM_CURRENT_SETTINGS, &dm, 0))
    {
        DWORD hz = dm.dmDisplayFrequency;
        if (hz >= 30 && hz <= 1000)
            return hz;
    }
    return 0;
}

static double QueryMonitorHz()
{
    // In the composited modes DWM exposes the actual fractional monitor rate.
    // This is the value we need to eliminate the classic 60.0988 vs 59.94/60.00
    // cadence mismatch.  For exclusive fullscreen, or if DWM cannot answer,
    // fall back to the current display mode.
    double dwmHz = QueryDwmMonitorHz();
    if (dwmHz >= 30.0 && dwmHz <= 1000.0)
        return dwmHz;

    DWORD enumHz = GetDisplayFrequencyFromEnum();
    return (enumHz >= 30 && enumHz <= 1000) ? (double)enumHz : 60.0;
}

static void LoadWGLSwapControl()
{
    if (pfnWglSwapIntervalEXT) return;
    if (!GFX::hGLDC || !GFX::hGLRC) return;

    PFN_wglGetExtensionsStringARB pfnGetARB =
        (PFN_wglGetExtensionsStringARB)wglGetProcAddress("wglGetExtensionsStringARB");
    PFN_wglGetExtensionsStringEXT pfnGetEXT =
        (PFN_wglGetExtensionsStringEXT)wglGetProcAddress("wglGetExtensionsStringEXT");

    const char* exts = NULL;
    if (pfnGetARB)      exts = pfnGetARB(GFX::hGLDC);
    else if (pfnGetEXT) exts = pfnGetEXT();

    bool hasSwapControl = false;
    if (exts)
    {
        hasSwapControl = (strstr(exts, "WGL_EXT_swap_control")      != NULL) ||
                         (strstr(exts, "WGL_ARB_swap_control")      != NULL) ||
                         (strstr(exts, "WGL_EXT_swap_control_tear") != NULL);
    }

    if (!hasSwapControl && exts)
        return;

    pfnWglSwapIntervalEXT = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    if (pfnWglSwapIntervalEXT)
        pfnWglGetSwapIntervalEXT = (PFN_wglGetSwapIntervalEXT)wglGetProcAddress("wglGetSwapIntervalEXT");
}

static bool SetOpenGLVSync(int interval)
{
    if (!pfnWglSwapIntervalEXT || !GFX::hGLDC || !GFX::hGLRC)
        return false;
    wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
    BOOL ok = pfnWglSwapIntervalEXT(interval);
    bool verified = (ok != FALSE);
    if (verified && pfnWglGetSwapIntervalEXT)
        verified = (pfnWglGetSwapIntervalEXT() == interval);
    wglMakeCurrent(NULL, NULL);
    return verified;
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

void Init(HWND hwnd)
{
    g_hWnd = hwnd;
    QueryPerformanceFrequency(&g_QPCFreq);

    double hz = QueryMonitorHz();
    InterlockedExchange(&g_MonitorHzMilli, (LONG)(hz * 1000.0 + 0.5));

    g_Initialized = true;
}

void OnDisplayChange()
{
    if (!g_Initialized) return;

    double hz = QueryMonitorHz();
    if (hz >= 30.0 && hz <= 1000.0)
        InterlockedExchange(&g_MonitorHzMilli, (LONG)(hz * 1000.0 + 0.5));

    // P60: a display change can move the window to another timing domain.
    // Do not carry the old presentation phase into the new monitor.
    InterlockedExchange64(&g_LastPresentationQPC, 0);
    InterlockedExchange64(&g_PresentationPeriodQPC, 0);
    InterlockedExchange(&g_PresentationClockLocked, FALSE);
    InterlockedExchange(&g_PresentationHzMilli, 0);
    InterlockedExchange(&g_PresentationIntervalErrUs, 0);
    InterlockedExchange64((volatile LONGLONG*)&g_LastDwmDisplayedFrame, 0);
    InterlockedExchange(&g_PresentationSampleStreak, 0);
    InterlockedExchange64(&g_PresentationAnchorQPC, 0);
    InterlockedExchange64((volatile LONGLONG*)&g_PresentationAnchorFrame, 0);
    InterlockedIncrement(&g_PresentationAnchorGeneration);
    InterlockedExchange64(&g_LastPaceTargetQPC, 0);
    InterlockedExchange64(&g_LastPaceWakeQPC, 0);
    InterlockedExchange(&g_LastPaceSource, 0);
}

void Enable(BOOL on)
{
    if (!g_Initialized) return;

    BOOL prev = (BOOL)InterlockedExchange(&g_Enabled, on ? TRUE : FALSE);
    bool prevBool = (prev != FALSE);
    bool newBool  = (on  != FALSE);

    if (prevBool == newBool)
        return;

    if (newBool)
    {
        OnDisplayChange();
        g_VSyncActive = false;
        LoadWGLSwapControl();

        InitDXGI();
        LONG desiredInterval = InterlockedExchangeAdd(&g_DXGISwapInterval, 0L);
        InterlockedExchange(&g_PendingVSyncInterval, desiredInterval);

        // Start the vblank poller thread now that DXGI is initialised.
        // HasDXGIVBlank() returns true only after this succeeds.
        StartVBlankThread();

        ResetState();
        APU::RestartForMonitorSync();

        // P47: reset DwmFlush warmup/arm state on every MMR enable, not
        // just on fullscreen exit (GFX::Stop). Without this a cold start
        // (MMR loaded TRUE from registry) or a runtime menu toggle leaves
        // s_DwmWarmupFrames=0, and the first DwmFlush() fires while the GL
        // swap interval is still 1 -> ~33ms (2-vblank) double-block on
        // frame 1. See GFX::ResetDwmWarmup() and MATCH_MONITOR_RATE.md sec 9.
        GFX::ResetDwmWarmup();
    }
    else
    {
        // MMR OFF must return to the emulator's normal presentation mode.
        // Do not force OpenGL to wait for the monitor here: that would make
        // the supposedly independent 60.0988/50.0 Hz emulator cadence follow
        // the display refresh and can create the exact periodic judder MMR is
        // intended to eliminate.  interval=0 is the standard unsynchronised
        // OpenGL path; the emulator/audio timing remains responsible for its
        // own cadence.
        InterlockedExchange(&g_PendingVSyncInterval, 0L);
        g_VSyncActive = false;
        g_PaceEpochQPC.QuadPart = 0;
        g_PaceFrameIndex = 0;
        StopVBlankThread();
        // P95: disabling MMR only restores the normal external audio-rate
        // state. Do not post a restart request here: NES::Stop() has already
        // stopped the emulation thread, and the subsequent SoundON() on the
        // normal-rate restart selects 44100 Hz directly. Leaving a request
        // behind would otherwise be serviced by a later UpdateDRC() after the
        // mode was already disabled.
        APU::ResetDRC();
    }
}

void ReinitVSync()
{
    if (!g_Initialized || !IsEnabled())
        return;
    if (g_VSyncActive)
        return;

    if (GFX::hGLDC && GFX::hGLRC)
    {
        wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
        LoadWGLSwapControl();
        wglMakeCurrent(NULL, NULL);

        // Also try DXGI now that we know the GL context is up.
        InitDXGI();
        LONG desiredInterval = InterlockedExchangeAdd(&g_DXGISwapInterval, 0L);
        g_VSyncActive = SetOpenGLVSync((int)desiredInterval);
        InterlockedExchange(&g_PendingVSyncInterval, -1);
    }
}

bool IsEnabled()
{
    return InterlockedExchangeAdd(&g_Enabled, 0) != 0;
}

bool IsVSyncActive()
{
    return g_VSyncActive;
}

double GetMonitorHz()
{
    return (double)InterlockedExchangeAdd(&g_MonitorHzMilli, 0L) / 1000.0;
}

double GetNESHz()
{
    return g_NESHz;
}

int GetDwmSyncMode()
{
    return (int)InterlockedExchangeAdd(&g_DwmSyncMode, 0);
}

double GetTargetHz()
{
    double monitorHz = GetMonitorHz();
    double frameHz = g_FrameHz;
    if (monitorHz < 30.0 || monitorHz > 1000.0 || frameHz <= 0.0)
        return (g_NESHz > 0.0) ? g_NESHz : frameHz;

    // Match Monitor Rate removes the small clock mismatch between the native
    // NES master-clock rate and the physical display. Do not accelerate a
    // 60-Hz game to a 75/120/144-Hz desktop refresh: those modes require frame
    // duplication rather than changing emulation speed.
    double relative = fabs(monitorHz - frameHz) / frameHz;
    if (relative <= 0.05)
        return monitorHz;
    // Outside the supported near-rate window, stay at the actual NES-native
    // rate, not the nominal rounded 60/50 Hz label. This keeps MMR from
    // unintentionally slowing NTSC 60.0988 Hz on a 75/120/144 Hz desktop.
    return g_NESHz;
}

double GetFrameHz()
{
    return g_FrameHz;
}

void SetNESRegion(int region)
{
    switch (region)
    {
        case 1:
            g_NESHz = 60.0988;
            g_FrameHz = 60.0;
            break;
        case 2:
            g_NESHz = 50.0069;
            g_FrameHz = 50.0;
            break;
        case 3:
            g_NESHz = 50.0039;
            g_FrameHz = 50.0;
            break;
        default:
            g_NESHz = 60.0988;
            g_FrameHz = 60.0;
            break;
    }
}

void ResetState()
{
    // Reset the absolute monitor-clock phase. The next MMR frame establishes
    // a fresh QPC epoch so a newly loaded ROM never inherits an old cadence.
    g_PaceEpochQPC.QuadPart = 0;
    g_PaceFrameIndex = 0;

    // P60: discard the presentation-derived phase anchor as well. A window /
    // monitor / fullscreen transition can invalidate the old display phase.
    InterlockedExchange64(&g_LastPresentationQPC, 0);
    InterlockedExchange64(&g_PresentationPeriodQPC, 0);
    InterlockedExchange(&g_PresentationClockLocked, FALSE);
    InterlockedExchange(&g_PresentationHzMilli, 0);
    InterlockedExchange(&g_PresentationIntervalErrUs, 0);
    InterlockedExchange64((volatile LONGLONG*)&g_LastDwmDisplayedFrame, 0);
    InterlockedExchange(&g_PresentationSampleStreak, 0);
    InterlockedExchange64(&g_PresentationAnchorQPC, 0);
    InterlockedExchange64((volatile LONGLONG*)&g_PresentationAnchorFrame, 0);
    InterlockedIncrement(&g_PresentationAnchorGeneration);
    InterlockedExchange64(&g_LastPaceTargetQPC, 0);
    InterlockedExchange64(&g_LastPaceWakeQPC, 0);
    InterlockedExchange(&g_LastPaceSource, 0);
}

void SetDwmSyncMode(bool useDwm)
{
    // NOTE: As of P26/P28, DwmFlush is disabled and this function is only
    // called with useDwm=false (from GFX::Start and GFX::Stop).
    // The code below is retained for completeness if USE_DWMFLUSH is re-enabled.
    InterlockedExchange(&g_DwmSyncMode, useDwm ? 1 : 0);
    if (g_VSyncActive || IsEnabled())
    {
        if (useDwm)
            InterlockedExchange(&g_PendingVSyncInterval, 0L);
        else
        {
            LONG desiredInterval = InterlockedExchangeAdd(&g_DXGISwapInterval, 0L);
            InterlockedExchange(&g_PendingVSyncInterval, desiredInterval);
        }
    }
}

void OnFrameEnd()
{
    // Kept as a lightweight timing hook for the existing diagnostic path.
    // IMPORTANT: monitor-rate calibration must NOT use emulation frame timing
    // here. Once MMR is active the emulator itself is paced to the monitor, so
    // feeding that timing back into g_MonitorHz would make the measurement
    // circular and could slowly chase its own clock. The display rate is now
    // obtained independently from DWM/display-mode timing in OnDisplayChange.
}

static HANDLE CreatePaceTimer()
{
    PFN_CreateWaitableTimerExW pfnEx =
        (PFN_CreateWaitableTimerExW)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW");
    HANDLE ht = NULL;
    if (pfnEx)
        ht = pfnEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_FLAG, TIMER_ALL_ACCESS);
    if (!ht)
        ht = CreateWaitableTimer(NULL, FALSE, NULL);
    return ht;
}

static void RecordPaceDiagnostic(LONGLONG targetQPC, LONGLONG wakeQPC, LONG source)
{
    InterlockedExchange64(&g_LastPaceTargetQPC, targetQPC);
    InterlockedExchange64(&g_LastPaceWakeQPC, wakeQPC);
    InterlockedExchange(&g_LastPaceSource, source);
}

static void PaceSlot()
{
    if (g_QPCFreq.QuadPart <= 0)
    {
        SwitchToThread();
        return;
    }

    const double targetHz = (GetTargetHz() > 0.0) ? GetTargetHz() : 60.0;
    const LONGLONG nominalPeriod =
            (LONGLONG)((double)g_QPCFreq.QuadPart / targetHz + 0.5);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // P86: presentation feedback is a PHASE ANCHOR, not the cadence itself.
    // P85 recalculated lastCompose + period - lead on every call. If PaceSlot
    // is entered more often than DWM advances cFrame, multiple producer calls
    // can reuse one sample and the emulator can run faster than the monitor.
    // Keep an independent target sequence: DWM can move phase, but every
    // PaceSlot call consumes exactly one presentation-period slot.
    static ULONGLONG s_seenAnchorFrame = 0;
    static LONG      s_seenAnchorGeneration = 0;
    static LONGLONG  s_nextPresentationTargetQPC = 0;

    LONG anchorGeneration = InterlockedExchangeAdd(&g_PresentationAnchorGeneration, 0);
    if (anchorGeneration != s_seenAnchorGeneration)
    {
        s_seenAnchorGeneration = anchorGeneration;
        s_seenAnchorFrame = 0;
        s_nextPresentationTargetQPC = 0;
    }

    if (InterlockedExchangeAdd(&g_PresentationClockLocked, 0) != FALSE)
    {
        LONGLONG anchorQPC = InterlockedExchangeAdd64(&g_PresentationAnchorQPC, 0);
        ULONGLONG anchorFrame = (ULONGLONG)InterlockedExchangeAdd64(
                (volatile LONGLONG*)&g_PresentationAnchorFrame, 0);
        LONGLONG period = InterlockedExchangeAdd64(&g_PresentationPeriodQPC, 0);

        if (anchorQPC > 0 && anchorFrame > 0 && period > 0)
        {
            const LONGLONG leadTicks =
                    (LONGLONG)((double)g_QPCFreq.QuadPart * 0.007 + 0.5);

            if (anchorFrame != s_seenAnchorFrame || s_nextPresentationTargetQPC <= 0)
            {
                s_seenAnchorFrame = anchorFrame;
                s_nextPresentationTargetQPC = anchorQPC + nominalPeriod - leadTicks;
            }

            LONGLONG targetQPC = s_nextPresentationTargetQPC;

            // If a host stall made the scheduled target historical, skip only
            // the stale phase targets. This call still consumes one current slot.
            while (targetQPC <= now.QuadPart)
                targetQPC += nominalPeriod;

            double remainMs = (double)(targetQPC - now.QuadPart) *
                              1000.0 / (double)g_QPCFreq.QuadPart;

            if (remainMs >= 1.0)
            {
                if (g_PaceTimer == NULL)
                {
                    HANDLE ht = CreatePaceTimer();
                    g_PaceTimer = ht ? ht : INVALID_HANDLE_VALUE;
                }

                if (g_PaceTimer != INVALID_HANDLE_VALUE)
                {
                    LARGE_INTEGER due;
                    due.QuadPart = -(LONGLONG)(remainMs * 10000.0);
                    SetWaitableTimer(g_PaceTimer, &due, 0, NULL, NULL, FALSE);
                    WaitForSingleObject(g_PaceTimer, 20);
                }
                else
                {
                    SwitchToThread();
                }
            }
            else if (remainMs > 0.0)
            {
                SwitchToThread();
            }

            LARGE_INTEGER paceWake;
            QueryPerformanceCounter(&paceWake);
            RecordPaceDiagnostic(targetQPC, paceWake.QuadPart, 1);

            // Critical P86/P88 rule: one display-period target is consumed per
            // PaceFrame invocation.
            s_nextPresentationTargetQPC = targetQPC + nominalPeriod;
            return;
        }
    }

    // QPC fallback remains authoritative whenever the presentation clock has
    // not qualified or has been invalidated by a display transition.
    if (g_PaceEpochQPC.QuadPart == 0)
    {
        g_PaceEpochQPC = now;
        g_PaceFrameIndex = 1;
        RecordPaceDiagnostic(now.QuadPart, now.QuadPart, 0);
        return;
    }

    LONGLONG targetQPC = g_PaceEpochQPC.QuadPart +
                         (LONGLONG)((double)g_PaceFrameIndex *
                                   (double)nominalPeriod + 0.5);

    while (targetQPC <= now.QuadPart)
    {
        ++g_PaceFrameIndex;
        targetQPC = g_PaceEpochQPC.QuadPart +
                    (LONGLONG)((double)g_PaceFrameIndex *
                              (double)nominalPeriod + 0.5);
    }

    double remainMs = (double)(targetQPC - now.QuadPart) *
                      1000.0 / (double)g_QPCFreq.QuadPart;

    if (remainMs >= 1.0)
    {
        if (g_PaceTimer == NULL)
        {
            HANDLE ht = CreatePaceTimer();
            g_PaceTimer = ht ? ht : INVALID_HANDLE_VALUE;
        }

        if (g_PaceTimer != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(remainMs * 10000.0);
            SetWaitableTimer(g_PaceTimer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(g_PaceTimer, 20);
        }
        else
        {
            SwitchToThread();
        }
    }
    else if (remainMs > 0.0)
    {
        SwitchToThread();
    }

    LARGE_INTEGER paceWake;
    QueryPerformanceCounter(&paceWake);
    RecordPaceDiagnostic(targetQPC, paceWake.QuadPart, 0);

    ++g_PaceFrameIndex;
}

LONGLONG GetLastPaceTargetQPC()
{
    return InterlockedExchangeAdd64(&g_LastPaceTargetQPC, 0);
}

LONGLONG GetLastPaceWakeQPC()
{
    return InterlockedExchangeAdd64(&g_LastPaceWakeQPC, 0);
}

bool WasLastPacePresentationAnchored()
{
    return InterlockedExchangeAdd(&g_LastPaceSource, 0) != 0;
}

void NotifyDwmCompositionSample(LONGLONG qpcDisplayed, ULONGLONG dwmFrameDisplayed)
{
    if (qpcDisplayed <= 0 || dwmFrameDisplayed == 0 || g_QPCFreq.QuadPart <= 0)
        return;

    ULONGLONG previousFrame =
            (ULONGLONG)InterlockedExchange64(
                    (volatile LONGLONG*)&g_LastDwmDisplayedFrame,
                    (LONGLONG)dwmFrameDisplayed);

    // DwmGetCompositionTimingInfo() returns a snapshot. The same displayed
    // frame can therefore be observed more than once. Never turn duplicate
    // snapshots into artificial 0ms/16ms presentation samples.
    if (previousFrame == dwmFrameDisplayed)
        return;

    // The caller supplies qpcFrameDisplayed/cFrameDisplayed here. These are
    // application-specific display events, rather than DWM's global
    // composition counter, so the phase master follows the frames that
    // actually reached the application's presentation path.
    LONGLONG previousQpc =
            InterlockedExchange64(&g_LastPresentationQPC, qpcDisplayed);

    if (previousQpc <= 0 || qpcDisplayed <= previousQpc)
    {
        InterlockedExchange(&g_PresentationClockLocked, FALSE);
        InterlockedExchange(&g_PresentationSampleStreak, 0);
        InterlockedExchange64(&g_PresentationAnchorQPC, 0);
        InterlockedExchange64((volatile LONGLONG*)&g_PresentationAnchorFrame, 0);
        InterlockedIncrement(&g_PresentationAnchorGeneration);
        return;
    }

    ULONGLONG frameDelta = dwmFrameDisplayed - previousFrame;
    const double targetHz = (GetTargetHz() > 0.0) ? GetTargetHz() : 60.0;
    const LONGLONG nominal =
            (LONGLONG)((double)g_QPCFreq.QuadPart / targetHz + 0.5);
    if (nominal <= 0)
        return;

    LONGLONG delta = qpcDisplayed - previousQpc;

    // Only a consecutive one-displayed-frame-per-refresh stream is good
    // enough to become the phase master. A skipped display resets the startup
    // qualification instead of promoting a 33ms interval into the clock.
    if (frameDelta != 1 ||
        delta < (nominal * 3) / 4 || delta > (nominal * 5) / 4)
    {
        InterlockedExchange(&g_PresentationClockLocked, FALSE);
        InterlockedExchange(&g_PresentationSampleStreak, 0);
        InterlockedExchange64(&g_PresentationAnchorQPC, 0);
        InterlockedExchange64((volatile LONGLONG*)&g_PresentationAnchorFrame, 0);
        InterlockedIncrement(&g_PresentationAnchorGeneration);
        InterlockedExchange(&g_PresentationIntervalErrUs,
                            (LONG)(((delta - nominal) * 1000000LL) /
                                   g_QPCFreq.QuadPart));
        return;
    }

    LONGLONG oldPeriod =
            InterlockedExchangeAdd64(&g_PresentationPeriodQPC, 0);
    if (oldPeriod <= 0)
        oldPeriod = delta;

    // Slow filter: keep the actual DWM display period as the phase master
    // without allowing one noisy sample to move the schedule by a full ms.
    LONGLONG filtered = oldPeriod + (delta - oldPeriod) / 8;
    if (filtered <= 0)
        filtered = delta;
    InterlockedExchange64(&g_PresentationPeriodQPC, filtered);

    LONG errUs = (LONG)(((delta - nominal) * 1000000LL) /
                        g_QPCFreq.QuadPart);
    InterlockedExchange(&g_PresentationIntervalErrUs, errUs);

    LONG hzMilli = (LONG)(((double)g_QPCFreq.QuadPart /
                           (double)filtered) * 1000.0 + 0.5);
    InterlockedExchange(&g_PresentationHzMilli, hzMilli);

    LONG streak = InterlockedIncrement(&g_PresentationSampleStreak);
    // Qualify after three consecutive unique displayed-frame samples.
    // The displayed QPC/frame pair becomes the phase anchor used by the
    // producer-side target sequence.
    if (streak >= 3)
    {
        InterlockedExchange64(&g_PresentationAnchorQPC, qpcDisplayed);
        InterlockedExchange64((volatile LONGLONG*)&g_PresentationAnchorFrame,
                              (LONGLONG)dwmFrameDisplayed);
        InterlockedExchange(&g_PresentationClockLocked, TRUE);
    }
}

bool HasPresentationClock()
{
    return InterlockedExchangeAdd(&g_PresentationClockLocked, 0) != FALSE;
}

double GetPresentationHz()
{
    return (double)InterlockedExchangeAdd(&g_PresentationHzMilli, 0) / 1000.0;
}

double GetPresentationIntervalErrorMs()
{
    return (double)InterlockedExchangeAdd(&g_PresentationIntervalErrUs, 0) / 1000.0;
}

void PaceFrame()
{
    // P88: this is the authoritative MMR frame boundary. PaceSlot() contains
    // the actual monitor-clock wait logic for historical API compatibility,
    // but callers now invoke this wrapper exactly once per NES video frame.
    PaceSlot();
}

void ApplyPendingVSync()
{
    LONG pending = InterlockedExchange(&g_PendingVSyncInterval, -1L);
    if (pending < 0)
        return;

    if (!pfnWglSwapIntervalEXT)
    {
        LoadWGLSwapControl();
        if (!pfnWglSwapIntervalEXT)
            return;
    }

    // Call directly -- context is already current, do NOT call SetOpenGLVSync
    // which would deassociate the context mid-draw via its own wglMakeCurrent.
    int interval = (int)pending;
    BOOL ok = pfnWglSwapIntervalEXT(interval);

    if (interval == 0)
    {
        // P84: interval=0 has two different synchronization meanings.
        //
        // DXGI mode:
        //   WaitForDXGIVBlank() is the real presentation synchronizer.
        //
        // DWM-sync mode:
        //   DwmFlush() is the real presentation synchronizer. A successful
        //   wglSwapIntervalEXT(0) call must NOT make g_VSyncActive=true,
        //   otherwise GFX.cpp could incorrectly skip the DwmFlush path.
        if (InterlockedExchangeAdd(&g_DwmSyncMode, 0) != 0)
            g_VSyncActive = false;
        else if (g_DXGIAvailable)
            g_VSyncActive = (ok != FALSE);
        // else: keep g_VSyncActive false when there is no verified vblank source.
    }
    else // interval == 1
    {
        bool verified = (ok != FALSE);
        if (verified && pfnWglGetSwapIntervalEXT)
            verified = (pfnWglGetSwapIntervalEXT() == 1);
        g_VSyncActive = verified;
    }
}

// Shutdown helper executed by the GL render thread while its own context
// is current. Never call SetOpenGLVSync() here because that function performs
// wglMakeCurrent() and can steal the context from the thread that owns it.
void PrepareForRenderShutdown()
{
    if (!pfnWglSwapIntervalEXT)
        LoadWGLSwapControl();

    if (pfnWglSwapIntervalEXT)
        pfnWglSwapIntervalEXT(0);

    g_VSyncActive = false;
    InterlockedExchange(&g_DwmSyncMode, 0L);
    // Invalidate any interval queued by the UI thread immediately before
    // shutdown so there is no second, blocking vsync change on the final frame.
    InterlockedExchange(&g_PendingVSyncInterval, -1L);
}

// ------------------------------------------------------------------
// P28: DXGI vblank bypass public API
// ------------------------------------------------------------------

// ==================================================================
// } // namespace MonitorSync -- temporarily closed so the vblank
// thread helpers are at file scope (matching the forward declarations
// at the top of the file and the 'static' storage class specifier).
// ==================================================================
} // namespace MonitorSync

// ==================================================================
// Vblank thread + event (P28 revised).
//
// PROBLEM WITH NAIVE WaitForVBlank() on the NES thread:
//   IDXGIOutput::WaitForVBlank() has no timeout parameter.
//   Confirmed by timing log: swap=166ms (10 vblanks) on frame 3,
//   swap=20ms on frame 6. The naive call occasionally blocks for
//   multiple vblank periods during GPU power state transitions or
//   driver-internal events, making stalls worse than before.
//
// SOLUTION: dedicated vblank poller thread.
//   A HIGH-priority background thread calls WaitForVBlank() in a loop
//   and SetEvent() after each return. The NES thread waits on that
//   event with WaitForSingleObject(deadline = 1.5 * frame_period).
//
//   If WaitForVBlank blocks for 166ms, the NES thread times out at
//   ~25ms and presents immediately -- bounded stall, not 10 frames.
//   If everything is fine, the event fires at ~16.7ms and we present
//   at exactly the right vblank with zero DWM involvement.
// ==================================================================

static HANDLE         g_VBlankEvent  = NULL; // auto-reset; fired by vblank thread
static HANDLE         g_VBlankThread = NULL; // the poller thread handle
static volatile LONG  g_VBlankStop   = 0L;   // 1 = ask thread to exit
static volatile LONG  g_VBlankReady  = 0L;   // 1 once thread loop has started

static DWORD WINAPI VBlankThreadProc(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    InterlockedExchange(&g_VBlankReady, 1L);

    while (!InterlockedExchangeAdd(&g_VBlankStop, 0L))
    {
        if (g_pDXGIOutput)
        {
            void** vtbl = *(void***)g_pDXGIOutput;
            // P37 (session 14): the HRESULT was never checked. If the
            // cached IDXGIOutput* becomes stale -- the confirmed trigger
            // is ExclusiveFullscreen's ChangeDisplaySettingsEx() call in
            // GFX::Start()/Stop(), which can invalidate the adapter/output
            // pair obtained before the mode switch -- WaitForVBlank()
            // starts failing and returning almost instantly instead of
            // blocking for ~16.6ms. With no check, this loop then spun as
            // fast as the CPU allows at THREAD_PRIORITY_HIGHEST, calling
            // SetEvent() on every near-instant "failure" as if a real
            // vblank had just happened. Two visible symptoms, both
            // confirmed by the session-13/14 timing logs:
            //   - swap (WaitForDXGIVBlank on the NES thread) reads ~0.01ms
            //     every frame, because the auto-reset event is already
            //     signalled by the time the NES thread checks it -- the
            //     spin loop fires it far faster than any real 60Hz vblank.
            //   - ofe/tot balloon to ~90-100ms on nearly every frame, not
            //     because OnFrameEnd() itself blocks (it's pure QueryPerformanceCounter
            //     arithmetic -- see OnFrameEnd() below) but because this
            //     thread, spinning flat-out at HIGHEST priority with zero
            //     yields, starves the NES thread of scheduler time.
            // This also explains why the slowdown outlives the fullscreen
            // session: g_DXGITried latches true forever after the first
            // InitDXGI() call, so the stale pointer is never re-acquired,
            // and this thread keeps spinning on it for the rest of the
            // process -- in fullscreen AND after returning to windowed.
            //
            // Fix: check the HRESULT. On failure, do NOT SetEvent() (that
            // would still fake a vblank signal) -- back off with a short
            // Sleep so the thread stops fighting the NES thread for CPU
            // time, and let the caller side (GFX.cpp, around the
            // ChangeDisplaySettingsEx calls) actively re-acquire a fresh
            // output via MonitorSync::ReacquireDXGIOutput() rather than
            // silently spinning on the broken one indefinitely.
            HRESULT hr = ((PFN_WaitForVBlank)vtbl[10])(g_pDXGIOutput);
            if (SUCCEEDED(hr))
            {
                if (g_VBlankEvent)
                    SetEvent(g_VBlankEvent);
            }
            else
            {
                Sleep(8);
            }
        }
        else
        {
            Sleep(1);
        }
    }
    return 0;
}

static void StartVBlankThread()
{
    if (g_VBlankThread || !g_DXGIAvailable) return;

    g_VBlankEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    if (!g_VBlankEvent) return;

    InterlockedExchange(&g_VBlankStop,  0L);
    InterlockedExchange(&g_VBlankReady, 0L);

    g_VBlankThread = CreateThread(NULL, 0, VBlankThreadProc, NULL, 0, NULL);
    if (!g_VBlankThread)
    {
        CloseHandle(g_VBlankEvent);
        g_VBlankEvent = NULL;
        return;
    }
    // Wait until thread loop starts before first frame.
    for (int i = 0; i < 100 && !InterlockedExchangeAdd(&g_VBlankReady, 0L); i++)
        Sleep(1);
}

static void StopVBlankThread()
{
    if (!g_VBlankThread) return;
    InterlockedExchange(&g_VBlankStop, 1L);
    // WaitForVBlank can't be cancelled; wait up to 100ms for thread to return.
    WaitForSingleObject(g_VBlankThread, 100);
    CloseHandle(g_VBlankThread); g_VBlankThread = NULL;
    if (g_VBlankEvent) { CloseHandle(g_VBlankEvent); g_VBlankEvent = NULL; }
    InterlockedExchange(&g_VBlankReady, 0L);
}

// ==================================================================
// Re-open namespace MonitorSync for the public API functions.
// ==================================================================
namespace MonitorSync
{

bool HasDXGIVBlank()
{
    return g_DXGIAvailable && (g_VBlankThread != NULL);
}

// P34: exposes the reason string set by InitDXGI(), so the timing log
// (GFX.cpp DiagWriteLogFile) can print exactly where DXGI init succeeded
// or failed instead of a bare yes/no. Safe to call from any thread: it
// only ever points at a static buffer written once, before g_DXGITried
// latches true, from the NES thread inside Enable(TRUE) -> InitDXGI().
const TCHAR* GetDXGIFailReason()
{
    return g_DXGIFailReason;
}

// Called from GL_DrawFrame (NES thread) just before SwapBuffers(interval=0).
// Waits for the next vblank signal with a hard deadline so a stuck driver
// WaitForVBlank() on the background thread never stalls the NES thread
// beyond 1.5 * frame_period (~25ms at 60Hz).
void WaitForDXGIVBlank()
{
    if (!g_VBlankEvent || !g_VBlankThread) return;
    double frameMs    = (GetTargetHz() > 0.0) ? 1000.0 / GetTargetHz() : 16.7;
    DWORD  deadlineMs = (DWORD)(frameMs * 1.5);
    if (deadlineMs < 20) deadlineMs = 20;
    if (deadlineMs > 50) deadlineMs = 50;
    WaitForSingleObject(g_VBlankEvent, deadlineMs);
}

// P37 (session 14): re-acquire a fresh IDXGIOutput* after a real display
// mode change. See the header comment on the declaration for the full
// rationale; in short, InitDXGI() intentionally latches its result
// forever to avoid retrying on machines where DXGI is genuinely
// unavailable, but that means a previously-good output that goes stale
// mid-session (confirmed trigger: ExclusiveFullscreen's
// ChangeDisplaySettingsEx() in GFX::Start()/Stop()) was never replaced,
// and the vblank thread kept spinning on the dead pointer -- with the
// P37 HRESULT check above, it now backs off instead of spinning, but it
// still can't recover a working vblank signal on its own. This function
// does the actual recovery: stop the thread, release the old COM
// objects, clear every "already tried" flag, and -- if MMR is currently
// on -- immediately redo InitDXGI() and restart the thread so the very
// next frame gets a real vblank wait again instead of the ~25ms timeout
// path (WaitForDXGIVBlank degrades gracefully either way, but a fresh
// output is what actually fixes the pacing rather than just bounding
// the damage).
void ReacquireDXGIOutput()
{
    if (!g_Initialized) return;

    bool wasEnabled = IsEnabled();

    StopVBlankThread();

    if (g_pDXGIOutput)  { DXGI_Release(g_pDXGIOutput);  g_pDXGIOutput  = NULL; }
    if (g_pDXGIAdapter) { DXGI_Release(g_pDXGIAdapter); g_pDXGIAdapter = NULL; }
    if (g_pDXGIFactory) { DXGI_Release(g_pDXGIFactory); g_pDXGIFactory = NULL; }

    g_DXGITried      = false;
    g_DXGIAvailable  = false;
    g_DXGIFailReason = _T("not attempted yet");
    InterlockedExchange(&g_DXGISwapInterval, 1L);

    if (!wasEnabled)
        return; // MMR off right now -- leave clean state for the next Enable(TRUE)

    InitDXGI();
    LONG desiredInterval = InterlockedExchangeAdd(&g_DXGISwapInterval, 0L);
    InterlockedExchange(&g_PendingVSyncInterval, desiredInterval);
    StartVBlankThread();
}

} // namespace MonitorSync
