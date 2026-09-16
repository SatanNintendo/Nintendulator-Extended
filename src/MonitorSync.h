/* Nintendulator - Win32 NES emulator written in C++
 * MonitorSync: real monitor-rate matching for the "Match Monitor Rate" feature.
 *
 * The critical invariant for Match Monitor Rate is now:
 *
 *   - Measure the monitor refresh independently of emulator frame timing
 *     (DWM exposes the fractional rate; display-mode enumeration is the
 *     fallback).
 *   - Pace the EMULATION/AUDIO frame cadence itself to that measured rate
 *     when the monitor is close enough to the NES region rate. This avoids
 *     the 60.0988 Hz vs 59.94/60.00 Hz beat that otherwise drops or repeats
 *     a visual frame every several seconds.
 *   - The render thread presents queued frames; it does not add a second
 *     independent software timer on top of DwmFlush/SwapBuffers.
 *   - Update DRC to the same target cadence so the DirectSound buffer sees
 *     a stable producer/consumer relationship.
 *
 * Compatibility: Windows 7 and later. The only Windows API used directly
 * (vs the ones loaded dynamically) is EnumDisplaySettings, available on
 * every Windows version since NT 4. WGL_EXT_swap_control is loaded via
 * wglGetProcAddress and is supported by every Windows OpenGL ICD since
 * at least 2005; if it is missing on a particular driver, the module
 * silently falls back to the previous behaviour (no vsync, DRC operating
 * purely on DirectSound buffer-fill feedback).
 */

#pragma once

namespace MonitorSync
{
        // Called once at program startup with the main window handle.
        // Loads WGL function pointers and queries the QPC frequency.
        void    Init (HWND hwnd);

        // Toggle Match Monitor Rate on/off.
        // When enabling: measures monitor Hz, enables OpenGL vsync, resets
        // pacing state, recomputes the DRC target frequency.
        // When disabling: resets DRC frequency to standard 44100 Hz.
        void    Enable (BOOL on);

        // Re-attempt vsync initialization. Called by GFX::Start after the
        // OpenGL context has been created, in case Enable(TRUE) was invoked
        // earlier when no context existed yet. Safe to call repeatedly.
        void    ReinitVSync ();

        // True while Match Monitor Rate is active.
        bool    IsEnabled ();

        // True if OpenGL vsync was successfully enabled. When false,
        // SwapBuffers does not block until the next vblank, and the
        // DRC must NOT apply the (monitorHz / nesHz) base target —
        // the emulator is still running at the NES rate, not the
        // monitor rate, so the base target would cause constant
        // buffer drift and audible stutter.
        bool    IsVSyncActive ();

        // Re-measure the monitor refresh rate. Call from WM_DISPLAYCHANGE
        // (resolution / monitor changed) and from any other place that may
        // invalidate the cached measurement.
        void    OnDisplayChange ();

        // Called from GFX::DrawScreen after a frame is produced. Kept only
        // as a lightweight compatibility hook for the existing diagnostics.
        // It no longer calibrates the monitor rate from emulator timing.
        void    OnFrameEnd ();

        // Authoritative MMR cadence hook. Kept as a compatibility wrapper;
        // new code should use PaceSlot(), which targets the display clock.
        void    PaceFrame ();

        // Reset per-frame timing state. Called when (re)starting emulation
        // or toggling the feature on so the first frame does not produce a
        // bogus QPC delta.
        void    ResetState ();

        // Feed back the actual presentation boundary observed by the render
        // thread. P61 uses this as the authoritative display clock when it
        // is stable enough; the QPC pacer remains the bounded fallback.
        void    OnPresentationFeedback (LONGLONG qpc, bool synchronizedBoundary);

        // Presentation feedback diagnostics used by the MMR timing log.
        bool    IsPresentationClockLocked ();
        double  GetPresentationHz ();
        double  GetLastPresentationIntervalMs ();
        double  GetLastPresentationErrorMs ();

        // Switch between DWM-sync mode and ordinary GL-vsync. P61 uses DWM-sync
        // only as the post-SwapBuffers presentation boundary in DWM-composited
        // modes; exclusive fullscreen remains on ordinary GL-vsync.
        // In DWM-sync mode the GL swap interval is set to 0 (no driver vsync)
        // because P61 calls DwmFlush() AFTER SwapBuffers and uses that boundary
        // for presentation feedback. Stacking driver vsync on top of DwmFlush
        // would block for TWO vblank periods per frame (~33ms at 60Hz),
        // halving the effective frame rate to 30fps.
        // useDwm=TRUE  -> post SwapInterval(0), keep g_VSyncActive=true for PaceFrame
        // useDwm=FALSE -> post SwapInterval(1), normal GL-vsync path
        //
        // NOTE: As of Session 8, DwmFlush is DISABLED by default in GL_DrawFrame
        // (see USE_DWMFLUSH define in GFX.cpp). SetDwmSyncMode(true) is never
        // called when DwmFlush is disabled, so the GL swap interval stays at 1
        // in both windowed and fullscreen modes. SetDwmSyncMode(false) is still
        // called from GFX::Start and GFX::Stop to ensure interval=1. The
        // useDwm=TRUE path is retained only for the case where a user re-enables
        // DwmFlush via USE_DWMFLUSH=1.
        void    SetDwmSyncMode (bool useDwm);

        // Current measured monitor refresh rate, in Hz.
        // Fractional rates such as 59.940 Hz are preserved when Windows exposes
        // them through DWM. Falls back to the current display mode otherwise.
        double  GetMonitorHz ();

        // Actual target cadence used by MMR. When the monitor is within 5% of
        // the current NES region rate, this is the monitor rate; for a gross
        // mismatch (for example 75/120/144 Hz with NTSC), the emulator stays
        // at its native NES rate rather than changing game speed.
        double  GetTargetHz ();

        // Nominal frame cadence of the selected NES video mode (60.0 Hz NTSC,
        // 50.0 Hz PAL/Dendy). Kept separate from the native PPU master clock.
        double  GetFrameHz ();

        // Native NES refresh rate for the current region, in Hz.
        // NTSC = 60.0988, PAL = 50.0069, Dendy = 50.0039.
        double  GetNESHz ();

        // Tell MonitorSync which NES region is active so GetNESHz returns
        // the correct value. Called from APU::SetRegion.
        void    SetNESRegion (int region);

        // Apply a pending wglSwapIntervalEXT change posted by Enable().
        // Must be called from the NES emulation thread (the OpenGL context
        // owner), at the start of GL_DrawFrame after wglMakeCurrent.
        // No-op if no change is pending. Safe to call every frame.
        void    ApplyPendingVSync ();

        // Legacy P28 DXGI vblank bypass API. P39 disabled the bypass permanently;
        // these functions are retained only for compatibility/diagnostics.
        //
        // HasDXGIVBlank() returns true if IDXGIOutput::WaitForVBlank is
        // available on this system (Windows 7+, any dGPU or iGPU with
        // DXGI 1.0 support -- which is essentially every machine since 2009).
        // Call once after Init(); result is cached; never blocks.
        //
        // WaitForDXGIVBlank() is a legacy compatibility API. P39 permanently
        // disabled the DXGI bypass, so this remains a bounded no-op in the
        // active MMR path. New presentation code must not call it.
        //
        // P36 (session 13): as of GFX.cpp's GL_DrawFrame, this is called
        // in BOTH windowed and fullscreen mode whenever MMR is active. It
        // used to be windowed-only, on the assumption that GL's own driver
        // vsync (interval=1) already paces fullscreen correctly. That
        // assumption doesn't hold: once InitDXGI() succeeds, the swap
        // interval is set to 0 for ALL frames (see g_DXGISwapInterval),
        // fullscreen included -- there is no separate "fullscreen keeps
        // interval=1" code path. Skipping this call in fullscreen therefore
        // left fullscreen with no video-side pacing at all.
        bool    HasDXGIVBlank ();
        void    WaitForDXGIVBlank ();

        // P37 (session 14): call this around any real display-mode switch
        // that can invalidate the cached IDXGIOutput* -- confirmed trigger
        // is ExclusiveFullscreen's ChangeDisplaySettingsEx() call in
        // GFX::Start() (entering) and GFX::Stop() (leaving). Before this
        // existed, InitDXGI() ran exactly once per process and latched its
        // result forever (by design, to avoid retry storms on machines
        // where DXGI is genuinely unavailable) -- but that also meant a
        // *previously working* IDXGIOutput* that goes stale mid-session
        // was never replaced, and VBlankThreadProc span at full CPU on the
        // broken pointer for the rest of the process's life, in fullscreen
        // and after returning to windowed alike. This stops the vblank
        // thread, releases the cached factory/adapter/output, clears the
        // "tried" latch, and -- if MMR is currently enabled -- immediately
        // re-runs InitDXGI() and restarts the thread against a fresh
        // output. Safe to call even if MMR is off or DXGI was never
        // available (degrades to a cheap no-op in that case).
        void    ReacquireDXGIOutput ();

        // P34: human-readable reason InitDXGI() succeeded or failed --
        // e.g. "LoadLibraryW(dxgi.dll) failed", "no adapter had an output
        // (tried 2 adapter(s), 2 had zero outputs)", or "OK (found output
        // on adapter index 1 of 2 tried)". Valid any time after Enable(TRUE)
        // has been called at least once; "not attempted yet" before that.
        const TCHAR* GetDXGIFailReason ();

        // P50 (session 25): diagnostic getter for the DWM-sync mode flag.
        // Returns 1 if SetDwmSyncMode(true) has been called (GL swap
        // interval posted = 0, DwmFlush is the intended pacer), 0
        // otherwise. Used by the timing-log header so a log where
        // swap=0.03ms can be cross-checked against whether the
        // interval=0 switch actually happened.
        int     GetDwmSyncMode ();


        // Authoritative emulator-frame/audio-slot pacer. Each slot is paced
        // from the PREVIOUS slot write using GetTargetHz(), so the emulator
        // cadence itself matches the monitor clock (for supported near-rate
        // combinations) instead of relying on frame-dropping in the renderer.
        void    PaceSlot ();

}
