/* Nintendulator - Win32 NES emulator written in C++
 * MonitorSync implementation. See MonitorSync.h for the design overview.
 *
 * Strategy:
 *   1. Enable OpenGL vsync via WGL_EXT_swap_control. SwapBuffers then
 *      blocks until the next monitor vblank, throttling the emulation
 *      thread to the monitor's true refresh rate. This is what eliminates
 *      the ~10-second micro-stutter when monitor and NES rates differ.
 *
 *   2. Continuously measure the actual frame rate via QueryPerformanceCounter
 *      sampling. With vsync on, the measured FPS equals the monitor refresh
 *      rate, which we feed into the DRC target so the audio playback rate
 *      tracks (monitor_hz / nes_hz) * 44100 instead of being pinned to 44100.
 *
 * Every external API is loaded dynamically so the binary still runs on a
 * fresh Windows 7 install without any redistributables. Missing APIs are
 * silently skipped -- the emulator continues to work, just without the
 * benefit that API would have provided.
 */

#include "stdafx.h"
#include "Nintendulator.h"
#include "resource.h"
#include "MapperInterface.h"
#include "MonitorSync.h"
#include "GFX.h"
#include "NES.h"
#include "APU.h"

// ------------------------------------------------------------------
// WGL swap-control extension (present in every Windows OpenGL ICD
// since at least 2005; still loaded dynamically to be safe).
// ------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
typedef int  (WINAPI *PFN_wglGetSwapIntervalEXT)(void);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringARB)(HDC);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringEXT)(void);

// ------------------------------------------------------------------
// DXGI WaitForVBlank -- P28: bypass DWM for windowed vsync.
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

// Lazy-initialize: enumerate adapter 0, output 0, cache IDXGIOutput*.
// Returns true if WaitForVBlank is ready to use.
static bool InitDXGI()
{
    if (g_DXGITried) return g_DXGIAvailable;
    g_DXGITried = true; // mark so we never retry on failure

    HMODULE hDxgi = LoadLibraryW(L"dxgi.dll");
    if (!hDxgi) return false;

    PFN_CreateDXGIFactory pfnCF =
        (PFN_CreateDXGIFactory)GetProcAddress(hDxgi, "CreateDXGIFactory");
    if (!pfnCF) return false;

    void* pFac = NULL;
    if (FAILED(pfnCF(&s_IID_IDXGIFactory, &pFac)) || !pFac) return false;
    g_pDXGIFactory = pFac;

    void** fvt = *(void***)pFac;
    void* pAdp = NULL;
    if (FAILED(((PFN_EnumAdapters)fvt[7])(pFac, 0, &pAdp)) || !pAdp)
    { DXGI_Release(pFac); g_pDXGIFactory = NULL; return false; }
    g_pDXGIAdapter = pAdp;

    void** avt = *(void***)pAdp;
    void* pOut = NULL;
    if (FAILED(((PFN_EnumOutputs)avt[7])(pAdp, 0, &pOut)) || !pOut)
    { DXGI_Release(pAdp); DXGI_Release(pFac); g_pDXGIAdapter = NULL; g_pDXGIFactory = NULL; return false; }
    g_pDXGIOutput = pOut;

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
static double         g_MonitorHz  = 60.0;     // last confirmed monitor rate
static double         g_NESHz      = 60.0988;  // NTSC default; updated by SetNESRegion

// Did we successfully enable OpenGL vsync?
static bool           g_VSyncActive  = false;

// ------------------------------------------------------------------
// Calibration: refine g_MonitorHz by sampling frame-to-frame QPC deltas.
// ------------------------------------------------------------------
static const int      CALIB_FRAMES    = 60;
static int            g_CalibCount    = 0;
static LARGE_INTEGER  g_CalibStartQPC = {0, 0};
static bool           g_CalibActive   = false;

// QPC timestamp of last OnFrameEnd() call; used by PaceFrame() for
// drift correction.
static LARGE_INTEGER  g_LastFrameEndQPC = {0, 0};

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

static DWORD GetDisplayFrequencyFromEnum()
{
    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm))
    {
        DWORD hz = dm.dmDisplayFrequency;
        if (hz >= 30 && hz <= 1000)
            return hz;
    }
    return 0;
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

    DWORD hz = GetDisplayFrequencyFromEnum();
    g_MonitorHz = (hz > 0) ? (double)hz : 60.0;

    g_Initialized = true;
}

void OnDisplayChange()
{
    if (!g_Initialized) return;

    DWORD hz = GetDisplayFrequencyFromEnum();
    if (hz > 0)
    {
        if (fabs((double)hz - g_MonitorHz) > 0.05)
            g_MonitorHz = (double)hz;
    }

    g_CalibCount = 0;
    g_CalibStartQPC.QuadPart = 0;
    g_CalibActive = IsEnabled();
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
    }
    else
    {
        InterlockedExchange(&g_PendingVSyncInterval, 1L);
        g_VSyncActive = false;
        g_CalibActive = false;
        StopVBlankThread();
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
    return g_MonitorHz;
}

double GetNESHz()
{
    return g_NESHz;
}

void SetNESRegion(int region)
{
    switch (region)
    {
        case 1: g_NESHz = 60.0988; break;  // NTSC
        case 2: g_NESHz = 50.0069; break;  // PAL
        case 3: g_NESHz = 50.0039; break;  // Dendy
        default: g_NESHz = 60.0988; break;
    }
}

void ResetState()
{
    g_CalibCount = 0;
    g_CalibStartQPC.QuadPart = 0;
    g_CalibActive = true;
    g_LastFrameEndQPC.QuadPart = 0;
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
    if (!g_Initialized || !IsEnabled())
        return;

    if (g_QPCFreq.QuadPart > 0)
        QueryPerformanceCounter(&g_LastFrameEndQPC);

    if (g_CalibActive && g_QPCFreq.QuadPart > 0)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (g_CalibStartQPC.QuadPart == 0)
        {
            g_CalibStartQPC = now;
            g_CalibCount = 0;
        }
        else
        {
            g_CalibCount++;
            if (g_CalibCount >= CALIB_FRAMES)
            {
                double elapsedSec =
                    (double)(now.QuadPart - g_CalibStartQPC.QuadPart) /
                    (double)g_QPCFreq.QuadPart;
                if (elapsedSec > 0.0)
                {
                    double measuredHz = (double)g_CalibCount / elapsedSec;
                    if (measuredHz >= 30.0 && measuredHz <= 1000.0)
                    {
                        if (fabs(measuredHz - g_MonitorHz) > 2.0)
                        {
                            g_CalibCount = 0;
                            g_CalibStartQPC = now;
                            return;
                        }
                        // 90/10 low-pass filter
                        g_MonitorHz = 0.9 * g_MonitorHz + 0.1 * measuredHz;
                    }
                }
                g_CalibCount = 0;
                g_CalibStartQPC = now;
            }
        }
    }
}

void PaceFrame()
{
    if (!g_VSyncActive)
    {
        Sleep(0);
        return;
    }

    if (g_PaceTimer == NULL)
    {
        PFN_CreateWaitableTimerExW pfnEx =
            (PFN_CreateWaitableTimerExW)GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW");
        HANDLE ht = NULL;
        if (pfnEx)
            ht = pfnEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_FLAG, TIMER_ALL_ACCESS);
        if (!ht)
            ht = CreateWaitableTimer(NULL, FALSE, NULL);
        g_PaceTimer = ht ? ht : INVALID_HANDLE_VALUE;
    }

    if (g_PaceTimer != INVALID_HANDLE_VALUE)
    {
        double slotMs = (g_NESHz > 0.0) ? (1000.0 / g_NESHz) - 0.5 : 16.167;
        if (slotMs < 1.0) slotMs = 1.0;

        if (g_QPCFreq.QuadPart > 0 && g_LastFrameEndQPC.QuadPart != 0)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsedMs = (double)(now.QuadPart - g_LastFrameEndQPC.QuadPart)
                * 1000.0 / (double)g_QPCFreq.QuadPart;
            slotMs -= elapsedMs;
            if (slotMs < 1.0) slotMs = 1.0;
        }

        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(slotMs * 10000.0);
        SetWaitableTimer(g_PaceTimer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(g_PaceTimer, 20);
    }
    else
    {
        SwitchToThread();
    }
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
        // interval=0 means either DXGI path or DWM-sync mode.
        // In both cases g_VSyncActive should reflect whether we have
        // a working sync mechanism:
        //   - DXGI path: g_VSyncActive=true (WaitForDXGIVBlank provides sync)
        //   - Enable(FALSE): g_VSyncActive stays false (was set in Enable)
        //   - DWM-sync mode: g_VSyncActive=true (DwmFlush provides sync)
        if (g_DXGIAvailable || InterlockedExchangeAdd(&g_DwmSyncMode, 0) != 0)
            g_VSyncActive = (ok != FALSE);
        // else: g_VSyncActive was already set false by Enable(FALSE)
    }
    else // interval == 1
    {
        bool verified = (ok != FALSE);
        if (verified && pfnWglGetSwapIntervalEXT)
            verified = (pfnWglGetSwapIntervalEXT() == 1);
        g_VSyncActive = verified;
    }
}

// ------------------------------------------------------------------
// P28: DXGI vblank bypass public API
// ------------------------------------------------------------------

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
            ((PFN_WaitForVBlank)vtbl[10])(g_pDXGIOutput);
            if (g_VBlankEvent)
                SetEvent(g_VBlankEvent);
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

bool HasDXGIVBlank()
{
    return g_DXGIAvailable && (g_VBlankThread != NULL);
}

// Called from GL_DrawFrame (NES thread) just before SwapBuffers(interval=0).
// Waits for the next vblank signal with a hard deadline so a stuck driver
// WaitForVBlank() on the background thread never stalls the NES thread
// beyond 1.5 * frame_period (~25ms at 60Hz).
void WaitForDXGIVBlank()
{
    if (!g_VBlankEvent || !g_VBlankThread) return;
    double frameMs    = (g_NESHz > 0.0) ? 1000.0 / g_NESHz : 16.7;
    DWORD  deadlineMs = (DWORD)(frameMs * 1.5);
    if (deadlineMs < 20) deadlineMs = 20;
    if (deadlineMs > 50) deadlineMs = 50;
    WaitForSingleObject(g_VBlankEvent, deadlineMs);
}

// ==================================================================
} // namespace MonitorSync
// ==================================================================
