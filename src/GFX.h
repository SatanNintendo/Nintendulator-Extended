/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) QMT Productions
 */

#pragma once

#define DIRECTDRAW_VERSION 0x0700
#include <ddraw.h>

// OpenGL for hardware scaling
#include <GL/gl.h>

namespace GFX
{
enum PALETTE { PALETTE_NTSC, PALETTE_PAL, PALETTE_PC10, PALETTE_VS1, PALETTE_VS2, PALETTE_VS3, PALETTE_VS4, PALETTE_EXT, PALETTE_PC10_ALT, PALETTE_MAX };

extern unsigned char RawPalette[8][64][3];
extern unsigned long Palette32[512];
extern BOOL Fullscreen, Scanlines, Bilinear, IntegerScale, MatchMonitorRate;
extern BOOL AlwaysOnTop, ExclusiveFullscreen;

// Borders and multiplier for Integer Scaling
extern int ISBorderX, ISBorderY, ISMult;

extern int FPSnum, FPSCnt, FSkip;
extern BOOL aFSkip;
extern int forceNoSkip;

extern int WantFPS;

extern BOOL SlowDown;
extern int SlowRate;

// The following two array lengths must equal NES::REGION_MAX
extern PALETTE Palette[4];
extern TCHAR CustPalette[4][MAX_PATH];
extern int NTSChue, NTSCsat, PALsat;
extern BOOL PC10compat;

// DirectDraw - used for normal modes and Scanlines
extern LPDIRECTDRAW7 DirectDraw;

// OpenGL context - used for Integer Scaling and Bilinear
extern HGLRC hGLRC;
extern HDC   hGLDC;

void    Init (void);
void    Destroy (void);
void    SetRegion (void);
void    Start (void);
void    Stop (void);
void    SaveSettings (HKEY);
void    LoadSettings (HKEY);
void    DrawScreen (void);
void    Draw1x (void);
void    Draw2x (void);
void    DrawIntegerScale (void);
void    Update (void);
void    Repaint (void);
void    GL_Resize (int, int);
void    PostGLResize (int, int);   // thread-safe deferred resize (posts to NES thread)
void    ApplyGLFilter (void);
// P44 (session 21): bind/unbind the GL context ONCE per NES-thread session
// instead of every frame. Called from NES::Thread() at start/end. See the
// block comment above these functions' definitions in GFX.cpp for why.
void    AcquireGLContext (void);
void    ReleaseGLContext (void);
// P47: reset DwmFlush warmup/arm state. Called from MonitorSync::Enable(TRUE)
// so cold starts (MMR on from registry) and runtime toggles also run the
// 180-frame GL-vsync warmup before DwmFlush+interval=0 takes over. See
// MATCH_MONITOR_RATE.md section 9.
void    ResetDwmWarmup (void);
BOOL    UseOpenGL (void);

// P54 (Stage 2, two-threaded): when MMR is active and the render thread is
// running, the emulation thread does NOT call GL_DrawFrame. Instead it
// produces a frame (PPU palette -> RGBA) into a lock-free queue, and the
// render thread consumes it and calls GL_DrawFrameFromBuffer. This
// decouples video presentation (DwmFlush/SwapBuffers on vblank) from
// emulation, so a DwmFlush stall no longer freezes audio.
bool    IsRenderThreadActive (void);

// P54: produce a frame to the render queue. Called from DrawScreen on the
// emulation thread. src must point to a 256*240*4 byte RGBA/BGRA buffer.
// No-op if the render thread is not active (falls back to single-thread).
void    ProduceFrameToQueue (const unsigned char *rgba);

// P54: start/stop the render thread. Start is called from GFX::Start after
// GL_Init when MMR is on; Stop is called from GFX::Stop before GL_Destroy.
// Stop is synchronous: it signals the thread to exit and waits for it.
void    StartRenderThread (void);
void    StopRenderThread (void);
void    SyncMenuChecks (void);
void    LoadPalette (PALETTE);
void    SetFrameskip (int);
void    ForceNoSkip (BOOL);
BOOL    NeedSkip (void);
void    PaletteConfig (void);
void    GetCursorPos (POINT *);
void    SetCursorPos (int, int);
BOOL    ZapperHit (int);
} // namespace GFX
