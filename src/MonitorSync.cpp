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
#include "MonitorSync.h"
#include "GFX.h"
#include "NES.h"
#include "APU.h"

// ------------------------------------------------------------------
// WGL swap-control extension (present in every Windows OpenGL ICD
// since at least 2005; still loaded dynamically to be safe).
// ------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
typedef const char* (WINAPI *PFN_wglGetExtensionsStringARB)(HDC);

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

	// Measured / cached values.
	static double         g_MonitorHz      = 60.0;     // last confirmed monitor rate
	static double         g_NESHz          = 60.0988;  // NTSC default; updated by SetNESRegion

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
	static void LoadWGLSwapControl()
	{
		if (pfnWglSwapIntervalEXT) return;
		if (!GFX::hGLDC || !GFX::hGLRC) return;

		PFN_wglGetExtensionsStringARB pfnGetExt =
			(PFN_wglGetExtensionsStringARB)wglGetProcAddress("wglGetExtensionsStringARB");

		const char* exts = NULL;
		if (pfnGetExt)
			exts = pfnGetExt(GFX::hGLDC);
		else
			exts = (const char*)glGetString(GL_EXTENSIONS);

		if (!exts) return;

		bool hasSwapControl = (strstr(exts, "WGL_EXT_swap_control")      != NULL) ||
		                      (strstr(exts, "WGL_ARB_swap_control")      != NULL) ||
		                      (strstr(exts, "WGL_EXT_swap_control_tear") != NULL);
		if (!hasSwapControl) return;

		pfnWglSwapIntervalEXT = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
	}

	static void SetOpenGLVSync(int interval)
	{
		if (!pfnWglSwapIntervalEXT || !GFX::hGLDC || !GFX::hGLRC)
			return;
		wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
		pfnWglSwapIntervalEXT(interval);
		wglMakeCurrent(NULL, NULL);
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
		if (prevBool == newBool)
			return; // no change

		if (newBool)
		{
			// Fresh measurement before we start depending on it.
			OnDisplayChange();

			// Load WGL swap control (needs an active context) and turn
			// vsync on so SwapBuffers blocks until the next vblank.
			if (GFX::hGLDC && GFX::hGLRC)
			{
				wglMakeCurrent(GFX::hGLDC, GFX::hGLRC);
				LoadWGLSwapControl();
				wglMakeCurrent(NULL, NULL);
			}
			SetOpenGLVSync(1);

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
			APU::ResetDRC();
		}
	}

	bool IsEnabled()
	{
		return InterlockedExchangeAdd(&g_Enabled, 0) != 0;
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
							// Low-pass filter: blend 50/50 with the
							// previous value so a single bad sample
							// (e.g. window drag) can't yank the DRC
							// target around.
							g_MonitorHz = 0.5 * g_MonitorHz + 0.5 * measuredHz;
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
		// Called from the audio-buffer fill wait loop. With OpenGL vsync
		// enabled, the real frame pacing happens inside SwapBuffers
		// (called from GFX::Update) which blocks until the next monitor
		// vblank. Here we just need to wait efficiently for DirectSound
		// to consume the next buffer chunk.
		//
		// timeBeginPeriod(1) is already active (set up in WinMain), so
		// Sleep(1) has ~1ms granularity rather than the default 15.6ms.
		// This is precise enough for the DirectSound wait (the buffer
		// chunk takes ~16ms to drain at 44100 Hz / 60 FPS) and does not
		// burn a CPU core on a busy-wait. The previous 500us waitable-
		// timer hack was actually LESS accurate because it was a fixed
		// delay unrelated to the actual vblank position.
		Sleep(1);
	}
}
