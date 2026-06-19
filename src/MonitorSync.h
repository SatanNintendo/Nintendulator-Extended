/* Nintendulator - Win32 NES emulator written in C++
 * MonitorSync: real monitor-rate matching for the "Match Monitor Rate" feature.
 *
 * This module replaces the previous frame-throttle hack (CreateWaitableTimerExW
 * with a fixed 500us delay) with a proper monitor-rate aware sync layer:
 *
 *   - Measures the actual monitor refresh rate via QPC sampling (refined
 *     every 60 frames using the frame-to-frame delta, which equals the
 *     monitor refresh when OpenGL vsync is on) and falls back to
 *     EnumDisplaySettings when no measurement has been taken yet.
 *   - Enables OpenGL vsync (WGL_EXT_swap_control) so SwapBuffers blocks
 *     until the next monitor vblank, throttling the emulation thread to
 *     the monitor's true refresh rate.
 *   - Provides a monitor-aware Dynamic Rate Control target so the audio
 *     playback frequency tracks (monitor_hz / nes_hz) * 44100, eliminating
 *     the periodic audio-buffer drift that caused the ~10-second micro-
 *     stutter when the monitor and NES rates differ by ~0.1 Hz.
 *   - Re-measures on WM_DISPLAYCHANGE so changing monitors or refresh
 *     rates at runtime keeps the sync correct.
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

        // Called from GFX::DrawScreen after Update() has finished the
        // SwapBuffers / Blt. Updates the calibration counter used to refine
        // the monitor-rate measurement, which the next APU::UpdateDRC call
        // picks up to compute the DRC target frequency.
        void    OnFrameEnd ();

        // Called from APU::Run inside the audio-buffer fill wait loop in
        // place of the old Sleep(1) / waitable-timer hack. Currently uses
        // Sleep(1) (precise because timeBeginPeriod(1) is active), but kept
        // as a separate function so future versions can swap in a smarter
        // wait without touching APU.cpp.
        void    PaceFrame ();

        // Reset per-frame timing state. Called when (re)starting emulation
        // or toggling the feature on so the first frame does not produce a
        // bogus QPC delta.
        void    ResetState ();

        // Current measured monitor refresh rate, in Hz.
        // Always returns a value in [30, 1000]; falls back to 60.0 if no
        // measurement has been taken yet.
        double  GetMonitorHz ();

        // Native NES refresh rate for the current region, in Hz.
        // NTSC = 60.0988, PAL = 50.0069, Dendy = 50.0039.
        double  GetNESHz ();

        // Tell MonitorSync which NES region is active so GetNESHz returns
        // the correct value. Called from APU::SetRegion.
        void    SetNESRegion (int region);
}
