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
 * silently skipped — the emulator continues to work, just without the
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
//
// IMPORTANT: WGL extensions are NOT exposed through glGetString(GL_EXTENSIONS)
// — that returns GL extensions only. We must query WGL extensions via
// wglGetExtensionsStringARB (WGL_ARB_extensions_string) or the older
// wglGetExtensionsStringEXT (WGL_EXT_extensions_string), both of which
// are themselves WGL extension entry points loaded via wglGetProcAddress.
// If neither is available, we probe wglSwapIntervalEXT directly.
// ------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
typedef int  (WINAPI *PFN_wglGetSwapIntervalEXT)(void);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringARB)(HDC);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringEXT)(void);

namespace MonitorSync
{
        // ------------------------------------------------------------------
        // State
        // ------------------------------------------------------------------
        static bool           g_Initialized   = false;
        static volatile LONG  g_Enabled       = FALSE;

        static HWND           g_hWnd          = NULL;

        static LARGE_INTEGER  g_QPCFreq       = {0, 0};

        // Function pointers (optional; NULL on systems that lack them).
        static PFN_wglSwapIntervalEXT         pfnWglSwapIntervalEXT         = NULL;
        static PFN_wglGetSwapIntervalEXT      pfnWglGetSwapIntervalEXT      = NULL;

        // Measured / cached values.
        static double         g_MonitorHz      = 60.0;     // last confirmed monitor rate
        static double         g_NESHz          = 60.0988;  // NTSC default; updated by SetNESRegion

        // Did we successfully enable OpenGL vsync?
        // If false, SwapBuffers will return immediately and we need a
        // fallback pacing strategy (busy-wait + Sleep(0)) inside PaceFrame
        // and the DRC must NOT apply the (monitorHz / nesHz) base target
        // because the emulator is still running at the NES rate, not the
        // monitor rate.
        static bool           g_VSyncActive    = false;

        // ------------------------------------------------------------------
        // Calibration: refine g_MonitorHz by sampling frame-to-frame QPC
        // deltas. With OpenGL vsync on, the emulator's measured FPS equals
        // the monitor refresh rate, so averaging over N frames gives us a
        // very accurate reading. This is more reliable than
        // DwmGetCompositionTimingInfo's rateRefresh field (which is sometimes
        // wrong by ~0.1 Hz on certain driver / monitor combinations).
        // ------------------------------------------------------------------
        static const int      CALIB_FRAMES     = 60;       // sampling window
        static int            g_CalibCount     = 0;
        static LARGE_INTEGER  g_CalibStartQPC  = {0, 0};
        static bool           g_CalibActive    = false;

        // ------------------------------------------------------------------
        // Deferred vsync interval.
        //
        // wglSwapIntervalEXT must be called from the thread that owns the
        // OpenGL context (the NES emulation thread, via GL_DrawFrame).
        // Enable() is called from the UI thread (WM_COMMAND), so it cannot
        // safely call wglSwapIntervalEXT directly without racing with
        // GL_DrawFrame's wglMakeCurrent / glDraw / SwapBuffers sequence.
        // A race here causes GL_DrawFrame to silently drop frames because
        // the UI thread's wglMakeCurrent steals the context mid-draw.
        //
        // Enable() writes the desired interval here; ApplyPendingVSync()
        // reads and applies it from the rendering thread at the start of
        // GL_DrawFrame, where the context is always current. -1 = no change.
        // ------------------------------------------------------------------
        static volatile LONG  g_PendingVSyncInterval = -1;

        // ------------------------------------------------------------------
        // Helpers
        // ------------------------------------------------------------------

        // Read the system-reported refresh rate from EnumDisplaySettings.
        // Returns 0 on failure.
        static DWORD GetDisplayFrequencyFromEnum()
        {
                DEVMODE dm;
                ZeroMemory(&dm, sizeof(dm));
                dm.dmSize  = sizeof(dm);
                dm.dmDriverExtra = 0;
                if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm))
                {
                        DWORD hz = dm.dmDisplayFrequency;
                        // Some buggy drivers report 0 or 1 when the rate is "default".
                        if (hz >= 30 && hz <= 1000)
                                return hz;
                }
                return 0;
        }

        // Query the WGL_EXT_swap_control entry points through the active
        // OpenGL context. The caller manages wglMakeCurrent.
        //
        // Note: WGL extensions are exposed via wglGetExtensionsStringARB
        // (WGL_ARB_extensions_string) or wglGetExtensionsStringEXT
        // (WGL_EXT_extensions_string), NOT via glGetString(GL_EXTENSIONS).
        // The latter only returns GL-side extensions and will not contain
        // "WGL_EXT_swap_control" — checking it would always report vsync
        // as unsupported and silently disable the feature.
        static void LoadWGLSwapControl()
        {
                if (pfnWglSwapIntervalEXT) return;
                if (!GFX::hGLDC || !GFX::hGLRC) return;

                // Try to obtain the WGL extension string. Both entry points are
                // themselves WGL extensions, but they are universally supported
                // on Windows OpenGL ICDs from the last 15+ years.
                PFN_wglGetExtensionsStringARB pfnGetARB =
                        (PFN_wglGetExtensionsStringARB)wglGetProcAddress("wglGetExtensionsStringARB");
                PFN_wglGetExtensionsStringEXT pfnGetEXT =
                        (PFN_wglGetExtensionsStringEXT)wglGetProcAddress("wglGetExtensionsStringEXT");

                const char* exts = NULL;
                if (pfnGetARB)
                        exts = pfnGetARB(GFX::hGLDC);
                else if (pfnGetEXT)
                        exts = pfnGetEXT();

                bool hasSwapControl = false;
                if (exts)
                {
                        hasSwapControl = (strstr(exts, "WGL_EXT_swap_control")      != NULL) ||
                                         (strstr(exts, "WGL_ARB_swap_control")      != NULL) ||
                                         (strstr(exts, "WGL_EXT_swap_control_tear") != NULL);
                }
                else
                {
                        // No WGL extension string available. Most modern drivers
                        // still expose wglSwapIntervalEXT, so probe it directly —
                        // wglGetProcAddress returns NULL if the function is absent.
                }

                if (!hasSwapControl && exts)
                        return; // extension string is present but lacks swap_control

                // Load the entry points. wglGetProcAddress returns NULL when the
                // function is not supported, so this doubles as a presence check.
                pfnWglSwapIntervalEXT = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
                if (pfnWglSwapIntervalEXT)
                        pfnWglGetSwapIntervalEXT = (PFN_wglGetSwapIntervalEXT)wglGetProcAddress("wglGetSwapIntervalEXT");
        }

        // Returns true if vsync was successfully set to the requested interval.
        // Caller must have a current OpenGL context on the calling thread; we
        // do wglMakeCurrent ourselves to be safe but never leave the context
        // current on this thread (the rendering thread owns it).
        static bool SetOpenGLVSync(int interval)
        {
                if (!pfnWglSwapIntervalEXT || !GFX::hGLDC || !GFX::hGLRC)
                        return false;
                wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
                BOOL ok = pfnWglSwapIntervalEXT(interval);

                // Best-effort verification: read back the current interval. If the
                // driver reports something different from what we asked for, vsync
                // is effectively not active and we should let the caller know so
                // it can fall back to a different sync strategy.
                bool verified = (ok != FALSE);
                if (verified && pfnWglGetSwapIntervalEXT)
                {
                        int actual = pfnWglGetSwapIntervalEXT();
                        verified = (actual == interval);
                }
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

                // Initial measurement from EnumDisplaySettings. The QPC sampler
                // in OnFrameEnd will refine this once emulation starts.
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
                        // Only update if the change is meaningful (>0.05 Hz) so a
                        // noisy report doesn't yank the DRC target around.
                        if (fabs((double)hz - g_MonitorHz) > 0.05)
                                g_MonitorHz = (double)hz;
                }

                // Restart calibration so the new rate is confirmed via QPC
                // sampling at the next opportunity.
                g_CalibCount = 0;
                g_CalibStartQPC.QuadPart = 0;
                g_CalibActive = IsEnabled();
        }

        void Enable(BOOL on)
        {
                if (!g_Initialized) return;

                BOOL prev = (BOOL)InterlockedExchange(&g_Enabled, on ? TRUE : FALSE);
                bool prevBool = (prev != FALSE);
                bool newBool  = (on != FALSE);

                // Even if the flag did not change, we may need to (re)initialize
                // vsync — for example, when Enable(TRUE) was called before the
                // OpenGL context existed (so vsync could not be set), and now
                // the context is ready. The caller handles this by invoking
                // ReinitVSync() after GL_Init; we don't do it here to keep the
                // Enable() path idempotent.
                if (prevBool == newBool)
                        return;

                if (newBool)
                {
                        // Fresh measurement before we start depending on it.
                        OnDisplayChange();

                        // We CANNOT call wglSwapIntervalEXT here directly.
                        // Enable() is called from the UI thread (WM_COMMAND),
                        // but the OpenGL context belongs to the NES emulation
                        // thread (GL_DrawFrame calls wglMakeCurrent / SwapBuffers).
                        // Calling wglMakeCurrent from the UI thread while
                        // GL_DrawFrame is running on the NES thread steals the
                        // context, silently breaking the in-progress GL calls
                        // (glTexSubImage2D etc.) and producing a corrupted or
                        // missing frame — visible as a brief black flash or
                        // stutter on hot-toggle.
                        //
                        // Solution: post a deferred request. The rendering thread
                        // reads g_PendingVSyncInterval at the start of GL_DrawFrame
                        // via ApplyPendingVSync() and applies wglSwapIntervalEXT
                        // safely while the context is already current there.
                        //
                        // We still load the WGL function pointers here (safe:
                        // LoadWGLSwapControl only calls wglGetProcAddress, which
                        // is thread-safe and does not touch the rendering context).
                        g_VSyncActive = false;
                        if (GFX::hGLDC && GFX::hGLRC)
                        {
                                wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
                                LoadWGLSwapControl();
                                wglMakeCurrent(NULL, NULL);
                        }
                        // Post the deferred enable; the NES thread picks this up.
                        InterlockedExchange(&g_PendingVSyncInterval, 1);

                        ResetState();
                }
                else
                {
                        // Post a deferred disable for the same thread-safety reason.
                        // We set g_VSyncActive = false immediately so PaceFrame()
                        // switches to Sleep(0) right away and DRC is reset, while
                        // the actual wglSwapIntervalEXT(0) call happens on the next
                        // GL_DrawFrame iteration (at most 16ms later — imperceptible).
                        InterlockedExchange(&g_PendingVSyncInterval, 0);
                        g_VSyncActive = false;
                        g_CalibActive = false;
                        APU::ResetDRC();
                }
        }

        // Re-attempt vsync initialization. Called by GFX::Start after the
        // OpenGL context has been created, in case Enable(TRUE) was invoked
        // earlier when no context existed yet. Safe to call repeatedly.
        // GFX::Start runs on the NES emulation thread, so wglMakeCurrent is
        // safe here — no concurrent rendering in flight at this point.
        void ReinitVSync()
        {
                if (!g_Initialized || !IsEnabled())
                        return;
                if (g_VSyncActive)
                        return; // already initialised

                if (GFX::hGLDC && GFX::hGLRC)
                {
                        wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
                        LoadWGLSwapControl();
                        wglMakeCurrent(NULL, NULL);
                        // Apply immediately: we ARE on the NES thread, context is ours.
                        g_VSyncActive = SetOpenGLVSync(1);
                        // Clear any pending deferred request that would overwrite us.
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
                        case 1: /* NES::REGION_NTSC  */ g_NESHz = 60.0988; break;
                        case 2: /* NES::REGION_PAL   */ g_NESHz = 50.0069; break;
                        case 3: /* NES::REGION_DENDY */ g_NESHz = 50.0039; break;
                        default:                       g_NESHz = 60.0988; break;
                }
        }

        void ResetState()
        {
                g_CalibCount = 0;
                g_CalibStartQPC.QuadPart = 0;
                g_CalibActive = true;
        }

        void OnFrameEnd()
        {
                if (!g_Initialized || !IsEnabled())
                        return;

                // Calibration: refine g_MonitorHz by averaging frame-to-frame
                // QPC deltas. With vsync on, measured FPS = monitor refresh.
                // Note: we do NOT call DwmFlush here — SwapBuffers (called from
                // GFX::Update just before this) has already blocked until the
                // next vblank thanks to OpenGL vsync. Calling DwmFlush on top
                // of that would block for an additional vblank period, halving
                // the effective frame rate.
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
                                                        // Outlier rejection: if this sample
                                                        // deviates from the running estimate by
                                                        // more than 2 Hz, it was almost certainly
                                                        // produced by a system hiccup (window drag,
                                                        // DWM stall, scheduler jitter). Discard it
                                                        // and restart the calibration window so we
                                                        // don't corrupt the DRC target.
                                                        if (fabs(measuredHz - g_MonitorHz) > 2.0)
                                                        {
                                                                g_CalibCount    = 0;
                                                                g_CalibStartQPC = now;
                                                                return;
                                                        }

                                                        // Heavy low-pass filter: 90% old value,
                                                        // 10% new sample. Previously 50/50, which
                                                        // let a single borderline-valid sample shift
                                                        // g_MonitorHz by ~1 Hz, causing UpdateDRC
                                                        // to overcorrect the audio frequency and
                                                        // produce audible/visible stutter for
                                                        // several seconds until subsequent samples
                                                        // pulled it back. With 90/10 the maximum
                                                        // shift from one sample is ~0.2 Hz, which
                                                        // the buffer-fill correction in UpdateDRC
                                                        // absorbs invisibly.
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
                // Called from the audio-buffer fill wait loop.
                //
                // Two scenarios:
                //
                // 1. VSync is active (the common case): SwapBuffers inside
                //    GFX::Update has already blocked until the next monitor
                //    vblank, throttling the emulator to monitor_hz. Here we
                //    only need to wait for DirectSound to drain its buffer
                //    chunk (~16ms at 60 FPS), and Sleep(1) with the 1ms timer
                //    resolution enabled by WinMain is precise enough without
                //    burning a CPU core.
                //
                //    However, Sleep(1) can actually sleep 1–2ms due to timer
                //    granularity, and that extra 1ms pushes us past the NEXT
                //    vblank — SwapBuffers then returns immediately (vblank already
                //    happened) and the whole cadence slips by one frame, producing
                //    the intermittent micro-stutter that remains even after DRC
                //    and frameskip fixes.
                //
                //    SwitchToThread() de-schedules us for the remainder of the
                //    current time-slice and returns as soon as the scheduler gives
                //    us back the CPU (typically 0.1–0.5ms). We re-check the DS
                //    position far sooner, keeping the wait-loop below its 1ms
                //    budget and preventing vblank misses.
                //
                // 2. VSync is NOT active: Sleep(0) yields without sleeping, letting
                //    the DirectSound buffer drain be the sole throttle.
                if (g_VSyncActive)
                        SwitchToThread();
                else
                        Sleep(0);
        }

        // Apply a pending vsync interval change. Must be called from the
        // NES emulation thread at the start of GL_DrawFrame, after
        // wglMakeCurrent has been called and before any GL draw commands.
        // This is the ONLY place wglSwapIntervalEXT is called during normal
        // operation (Enable/ReinitVSync only post a request here).
        void ApplyPendingVSync()
        {
                LONG pending = InterlockedExchange(&g_PendingVSyncInterval, -1);
                if (pending < 0)
                        return; // nothing pending

                if (!pfnWglSwapIntervalEXT)
                {
                        // Try to load WGL pointers now; context is current here.
                        LoadWGLSwapControl();
                        if (!pfnWglSwapIntervalEXT)
                                return; // driver doesn't support swap control
                }

                int interval = (int)pending; // 0 or 1
                if (interval == 1)
                {
                        g_VSyncActive = SetOpenGLVSync(1);
                }
                else
                {
                        // Disabling vsync. g_VSyncActive was already set to false
                        // in Enable(FALSE) so PaceFrame already uses Sleep(0).
                        SetOpenGLVSync(0);
                }
        }
}
