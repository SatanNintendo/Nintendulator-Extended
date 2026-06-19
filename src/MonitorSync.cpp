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
 *      sampling using an exponential moving average (EMA). With vsync on,
 *      the measured FPS equals the monitor refresh rate, which we feed
 *      into the DRC target so the audio playback rate tracks
 *      (monitor_hz / nes_hz) * 44100 instead of being pinned to 44100.
 *      The EMA filter (alpha ~1/120) is much smoother than the previous
 *      block-of-60-frames averaging and reacts faster to real changes
 *      while still rejecting transient spikes (window drag, etc.).
 *
 *   3. Detect missed vblanks: if a frame took longer than 1.5 / monitor_hz
 *      seconds, the driver skipped a vsync (rare but possible under load).
 *      Such samples are excluded from the EMA so the calibration doesn't
 *      drift toward a wrong value.
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
        // Calibration: refine g_MonitorHz via exponential moving average (EMA)
        // of per-frame QPC deltas. With OpenGL vsync on, the measured FPS
        // equals the monitor refresh rate.
        //
        // EMA is preferred over block-of-N-frames averaging because:
        //   - It reacts continuously (no step changes every 60 frames).
        //   - It weights recent samples more, so a real monitor-rate change
        //     (e.g. user switched from 60 Hz to 144 Hz) propagates within
        //     ~2 seconds instead of ~30 seconds.
        //   - With alpha = 1/60, the effective time constant is ~60 frames
        //     (~1 second at 60 Hz), which is long enough to reject per-frame
        //     jitter but short enough to track real changes.
        //
        // Missed-vblank detection: if a frame took longer than 1.5 / monitor_hz
        // seconds, the driver skipped a vsync (rare but possible under load).
        // Such samples are excluded from the EMA so a transient stall doesn't
        // pull the calibration toward a wrong value.
        // ------------------------------------------------------------------
        static const double   CALIB_EMA_ALPHA  = 1.0 / 60.0;  // ~1 second at 60 Hz
        static const double   CALIB_MISS_FACTOR= 1.5;          // >1.5/Hz = missed vblank
        static LARGE_INTEGER  g_LastFrameQPC   = {0, 0};
        static bool           g_CalibSeeded    = false;        // first valid sample taken?
        static bool           g_CalibActive    = false;

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
                // sampling at the next opportunity. Clearing g_LastFrameQPC
                // forces the next OnFrameEnd to re-seed the EMA rather than
                // computing a delta across the display-change boundary (which
                // could be a multi-second gap and would produce a garbage rate).
                g_LastFrameQPC.QuadPart = 0;
                g_CalibSeeded = false;
                g_CalibActive = IsEnabled();
        }

        void Enable(BOOL on)
        {
                if (!g_Initialized) return;

                BOOL prev = (BOOL)InterlockedExchange(&g_Enabled, on ? TRUE : FALSE);
                bool prevBool = (prev != FALSE);
                bool newBool  = (on != FALSE);

                // IMPORTANT: we deliberately do NOT early-return on (prevBool == newBool).
                // Toggling "Match Monitor Rate" off and back on at runtime (the
                // "hot toggle" scenario) requires us to re-enable vsync, re-seed
                // the calibration, and reset the DRC accumulator even though the
                // *current* call sees the same target state as the last successful
                // Enable(TRUE). If we skipped that, the second Enable(TRUE) would
                // leave the emulator in a half-initialised state (vsync may have
                // been turned off by the OS during the disable phase, the DRC
                // integrator would retain stale state, etc.) and the user would
                // see constant stutter until they restarted the ROM.
                //
                // The two callers are the menu handler (always toggles) and
                // GFX::Start after GL_Init (always passes TRUE), so re-running
                // the full init path on a no-op call is acceptable.
                if (newBool)
                {
                        // Fresh measurement before we start depending on it.
                        OnDisplayChange();

                        // Load WGL swap control (needs an active context) and turn
                        // vsync on so SwapBuffers blocks until the next vblank.
                        g_VSyncActive = false;
                        if (GFX::hGLDC && GFX::hGLRC)
                        {
                                wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
                                LoadWGLSwapControl();
                                wglMakeCurrent(NULL, NULL);
                                g_VSyncActive = SetOpenGLVSync(1);
                        }

                        // Reset DRC integrator and frequency so the new sync session
                        // starts from a clean state. Without this, stale integrator
                        // accumulation from a previous MatchMonitorRate session would
                        // produce a transient frequency spike and audible glitch.
                        APU::ResetDRC();

                        ResetState();
                }
                else
                {
                        // When disabled, do NOT touch vsync. The original emulator
                        // left vsync under driver control, and forcing it on or off
                        // here would surprise users whose driver defaults differ.
                        //
                        // We do, however, reset the DRC frequency back to the
                        // standard 44100 Hz so any accumulated adjustment from the
                        // MatchMonitorRate session is cleared.
                        g_VSyncActive = false;
                        APU::ResetDRC();
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
                g_LastFrameQPC.QuadPart = 0;
                g_CalibSeeded = false;
                g_CalibActive = true;
        }

        void OnFrameEnd()
        {
                if (!g_Initialized || !IsEnabled())
                        return;

                // Calibration: refine g_MonitorHz via EMA of per-frame QPC deltas.
                // With vsync on, measured FPS = monitor refresh.
                // Note: we do NOT call DwmFlush here — SwapBuffers (called from
                // GFX::Update just before this) has already blocked until the
                // next vblank thanks to OpenGL vsync. Calling DwmFlush on top
                // of that would block for an additional vblank period, halving
                // the effective frame rate.
                if (!g_CalibActive || g_QPCFreq.QuadPart <= 0)
                        return;

                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);

                if (g_LastFrameQPC.QuadPart == 0)
                {
                        // First sample: seed the EMA with the current monitor rate
                        // (already obtained from EnumDisplaySettings at enable time).
                        // Don't try to compute a rate from a single QPC delta.
                        g_LastFrameQPC = now;
                        g_CalibSeeded  = true;
                        return;
                }

                LONGLONG delta = now.QuadPart - g_LastFrameQPC.QuadPart;
                g_LastFrameQPC = now;
                if (delta <= 0)
                        return; // QPC went backwards (shouldn't happen, but guard anyway)

                double frameSec = (double)delta / (double)g_QPCFreq.QuadPart;
                if (frameSec <= 0.0)
                        return;

                double measuredHz = 1.0 / frameSec;

                // Reject outliers: if the frame was much longer than expected
                // (missed vblank) or much shorter (impossible with vsync on),
                // skip this sample so it doesn't poison the EMA.
                if (measuredHz < 30.0 || measuredHz > 1000.0)
                        return;
                double expectedSec = 1.0 / g_MonitorHz;
                if (frameSec > expectedSec * CALIB_MISS_FACTOR)
                        return; // missed vblank — discard

                if (!g_CalibSeeded)
                {
                        g_MonitorHz     = measuredHz;
                        g_CalibSeeded   = true;
                }
                else
                {
                        // EMA update: new = old + alpha * (sample - old)
                        g_MonitorHz = g_MonitorHz + CALIB_EMA_ALPHA * (measuredHz - g_MonitorHz);
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
                // 2. VSync is NOT active (driver lacks WGL_EXT_swap_control,
                //    or context creation failed, or SetOpenGLVSync returned
                //    false): SwapBuffers returns immediately and the emulator
                //    runs at full NES speed. The previous implementation had a
                //    500us waitable-timer hack here; we replace it with a
                //    Sleep(0) busy-yield which is at least as good (the timer
                //    was a fixed delay unrelated to vblank anyway). The
                //    DirectSound buffer drain itself throttles the loop.
                if (g_VSyncActive)
                        Sleep(1);
                else
                        Sleep(0);
        }
}
