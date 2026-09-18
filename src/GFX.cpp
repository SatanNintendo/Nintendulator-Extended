/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) QMT Productions
 */

#include "stdafx.h"
#include "Nintendulator.h"
#include "resource.h"
#include "MapperInterface.h"
#include "Controllers.h"
#include "NES.h"
#include "GFX.h"
#define NES_WIDTH  256
#define NES_HEIGHT 240
#include "PPU.h"
#include "AVI.h"
#include <commctrl.h>
#include <dwmapi.h>
#include "Lang.h"
#include "APU.h"
#include "Theme.h"
#include "MonitorSync.h"

#if (_MSC_VER < 1400)
// newer versions of the DirectX SDK helpfully fail to include ddraw.lib
// and those newer versions can only be used in Visual Studio 2005 and later
// If we're using .NET 2003 or earlier, it's definitely available
// Otherwise, we need to do LoadLibrary/GetProcAddress
#pragma comment(lib, "ddraw.lib")
#endif
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "opengl32.lib")

#define _USE_MATH_DEFINES
#include <math.h>

#ifndef M_PI
#define M_PI    3.14159265358979323846
#endif  /* !M_PI */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// DwmFlush is loaded dynamically so the binary stays compatible with
// Windows XP/2003 where dwmapi.dll does not exist.
//
// DwmFlush is kept as legacy code for historical diagnostics, but the runtime
// path is currently disabled (USE_DWMFLUSH=0).  DwmFlush is a synchronous OS
// call with no cancellation mechanism; allowing the render thread to block in
// it makes safe MMR shutdown impossible if the compositor/driver stalls.
//
// Windowed/borderless MMR therefore uses the deterministic PaceFrame clock and
// optional DWM composition *diagnostics* only. Exclusive fullscreen never uses
// DWM as a presentation master.
typedef HRESULT (WINAPI *PFN_DwmFlush)(void);
static PFN_DwmFlush s_pfnDwmFlush = reinterpret_cast<PFN_DwmFlush>(1); // 1 = not yet loaded
typedef HRESULT (WINAPI *PFN_DwmGetCompositionTimingInfo)(HWND, DWM_TIMING_INFO *);
static PFN_DwmGetCompositionTimingInfo s_pfnDwmGetCompositionTimingInfo = reinterpret_cast<PFN_DwmGetCompositionTimingInfo>(1);

#define USE_DWMFLUSH 0

// P84: synchronize from the first frame. The wait is on the render thread,
// so there is no reason to keep the old multi-second unsynchronised warmup.
#define DWM_WARMUP_FRAMES 0
static int  s_DwmWarmupFrames = 0;
static bool s_DwmModeArmed    = false;
// Diagnostic counter: render passes that actually enter the DwmFlush path.
static volatile LONG s_DwmFlushPathReached = 0;

namespace GFX
{
unsigned char RawPalette[8][64][3];
unsigned short Palette15[512];
unsigned short Palette16[512];
unsigned long Palette32[512];
char Depth;
BOOL Fullscreen, Scanlines, Bilinear, MatchMonitorRate;
BOOL AlwaysOnTop, ExclusiveFullscreen;

LARGE_INTEGER ClockFreq;
LARGE_INTEGER LastClockVal;
int FPSnum, FPSCnt, FSkip;
BOOL aFSkip;


int Pitch;
int WantFPS;
int aFPScnt;
LONGLONG aFPSnum;
int forceNoSkip;

BOOL SlowDown;
int SlowRate;
int FullscreenBorder;
BOOL IntegerScale;
int ISBorderX, ISBorderY, ISMult;  // Borders and multiplier for Integer Scaling

BOOL InError;

// Saved window position before entering fullscreen mode
static int SavedWindowX = 0;
static int SavedWindowY = 0;
static BOOL HasSavedWindowPos = FALSE;

// Saved display mode for exclusive fullscreen mode
static DEVMODE SavedDisplayMode;
static BOOL HasSavedDisplayMode = FALSE;

PALETTE DefaultPalette[NES::REGION_MAX];
PALETTE Palette[NES::REGION_MAX];
int NTSChue, NTSCsat, PALsat;
TCHAR CustPalette[NES::REGION_MAX][MAX_PATH];
BOOL PC10compat;

LPDIRECTDRAW7           DirectDraw;
LPDIRECTDRAWSURFACE7    PrimarySurf, SecondarySurf;
LPDIRECTDRAWCLIPPER     Clipper;
DDSURFACEDESC2          SurfDesc;
DWORD                   SurfSize;

// OpenGL - for Bilinear and Integer Scaling
HGLRC hGLRC = NULL;
HDC   hGLDC = NULL;
static GLuint glTex  = 0;
static int    glWinW = 0;
static int    glWinH = 0;
static BOOL   UsingOpenGL = FALSE;

// ------------------------------------------------------------------
// PBO (Pixel Buffer Object) streaming for glTexSubImage2D.
//
// P83 update: the current Log(10) does NOT implicate SwapBuffers. Its ~16.6ms
// `tex` value is exactly the t0->t1 interval around the PBO/texture upload.
// With the two-PBO implementation, the render thread can be forced to recycle
// a still-in-flight PBO because the windowed SwapBuffers path provides little
// CPU backpressure. The most likely blocking point is therefore glMapBuffer.
// The P83 fix increases the PBO pool to four and splits the diagnostic timing
// so the next run can confirm whether pboMap is the full-refresh stall.
//
// ROOT CAUSE of the older client-pointer tex=24ms stall (historical P29 reason):
//
//   glTexSubImage2D with a client-side pointer (no PBO) is a SYNCHRONOUS
//   operation: the GL driver must wait until the GPU has finished reading
//   the previous frame's texture before it can overwrite the memory with
//   new pixel data. Normally this wait is ~0 ms because the GPU has
//   already consumed the texture during rendering. But once every
//   ~20-60 seconds, the GPU pipeline falls slightly behind schedule
//   (driver internal GC, VRAM eviction, power state transition) and
//   glTexSubImage2D blocks for an entire extra vblank period (~16ms)
//   waiting for the GPU to catch up. This produces the tex=24ms* stall.
//
// FIX: Pixel Buffer Objects (GL_PIXEL_UNPACK_BUFFER).
//
//   With a PBO, glTexSubImage2D becomes ASYNCHRONOUS:
//     1. CPU writes pixel data into PBO memory (via glMapBuffer).
//     2. glTexSubImage2D reads from the PBO, not from client memory.
//        The driver queues the DMA transfer and returns immediately --
//        no waiting for the GPU.
//   3. glMapBuffer of a PBO that is still being consumed by the GPU must wait.
//      P29 originally used only TWO PBOs, assuming one intervening frame was
//      always enough for the DMA. That assumption is false when the windowed
//      presentation path lets SwapBuffers return without strong GPU backpressure:
//      the driver/compositor can keep a submitted PBO in flight for more than
//      one frame. P83 increases the pool to FOUR PBOs so normal multi-frame
//      in-flight latency does not force a map wait.
//
// COMPATIBILITY:
//   GL_ARB_pixel_buffer_object / GL_EXT_pixel_buffer_object has been
//   available on every discrete and integrated GPU since ~2006 (NVIDIA
//   GeForce 6+, AMD Radeon X1000+, Intel HD 2000+). On Windows 7 with
//   any non-software renderer this will always succeed.
//   If PBO creation fails (ancient hardware / software renderer / driver
//   bug), we fall back to the original glTexSubImage2D path transparently.
//
// Extension function pointers (loaded dynamically in GL_Init):
// ------------------------------------------------------------------
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER     0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW             0x88E0
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY              0x88B9
#endif

typedef void   (WINAPI *PFN_glGenBuffers)   (GLsizei, GLuint*);
typedef void   (WINAPI *PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void   (WINAPI *PFN_glBindBuffer)   (GLenum, GLuint);
typedef void   (WINAPI *PFN_glBufferData)   (GLenum, ptrdiff_t, const void*, GLenum);
typedef void*  (WINAPI *PFN_glMapBuffer)    (GLenum, GLenum);
typedef GLboolean (WINAPI *PFN_glUnmapBuffer)(GLenum);

static PFN_glGenBuffers    pfn_glGenBuffers    = NULL;
static PFN_glDeleteBuffers pfn_glDeleteBuffers = NULL;
static PFN_glBindBuffer    pfn_glBindBuffer    = NULL;
static PFN_glBufferData    pfn_glBufferData    = NULL;
static PFN_glMapBuffer     pfn_glMapBuffer     = NULL;
static PFN_glUnmapBuffer   pfn_glUnmapBuffer   = NULL;

// PBO streaming buffers.
//
// The previous P29 implementation used only two PBOs in a ping-pong pattern.
// That is unsafe once SwapBuffers stops providing CPU backpressure (as in the
// current DWM-composited/windowed path): a PBO submitted on frame N can still
// be owned by the GPU when the same PBO is mapped again on frame N+2.
// glMapBuffer() must then wait for the old DMA to retire, which can consume
// almost exactly one refresh period (~16.6 ms) and shift presentation phase.
//
// Keep several frames of storage in flight so normal compositor/driver latency
// does not force the CPU to recycle a live PBO. Four 245760-byte buffers are
// still under 1 MiB total and add no visible frame latency because the render
// queue remains latest-wins.
#define PBO_SIZE  (256 * 240 * 4)
#define PBO_COUNT 4
static GLuint  s_PBO[PBO_COUNT] = {0, 0, 0, 0};  // 0 = not created / PBO unavailable
static int     s_PBOIndex  = 0;                  // PBO to write this frame
static BOOL    s_PBOReady  = FALSE;              // TRUE once all PBOs are allocated

// Deferred GL viewport resize.
// WM_SIZE arrives on the UI thread; GL_Resize calls wglMakeCurrent which races
// with GL_DrawFrame on the NES thread. Instead, WM_SIZE calls PostGLResize()
// which stores the new size atomically. GL_DrawFrame reads and applies it at
// the start of each frame while the context is safely current on the NES thread.
// Packed as a single 64-bit value: high 32 = width, low 32 = height.
// -1 means "no pending resize".
static __declspec(align(8)) volatile LONGLONG g_PendingResize = -1LL;

// Deferred bilinear filter toggle.
// ApplyGLFilter() used to call wglMakeCurrent from the UI thread (WM_COMMAND →
// ID_PPU_BILINEAR), which races with GL_DrawFrame exactly like the old Enable()
// and WM_SIZE bugs. Fix: UI thread posts g_PendingBilinear; GL_DrawFrame applies
// it at the start of the frame while the context is already current.
// 0 = nearest (off), 1 = linear (on), -1 = no pending change.
static volatile LONG g_PendingBilinear = -1L;

// Render-thread wake event. Declared here because UI-side resize/filter posts
// may need to wake the render thread even when emulation is paused.
static HANDLE s_FrameEvent = NULL;

void ApplyGLFilter(void)
{
        // Post the change for deferred application in GL_DrawFrame.
        // Never call wglMakeCurrent from the UI thread while the NES thread
        // may be inside GL_DrawFrame — it steals the context and produces
        // a black frame or corrupted output.
        if (!UsingOpenGL || !glTex)
                return;
        InterlockedExchange(&g_PendingBilinear, Bilinear ? 1L : 0L);
}

#if (_MSC_VER >= 1400)
typedef HRESULT (WINAPI *LPDIRECTDRAWCREATEEX)(GUID FAR *, LPVOID *, REFIID,IUnknown FAR *);
HINSTANCE dDrawInst;
LPDIRECTDRAWCREATEEX DirectDrawCreateEx;
#endif

// ============================================================
// OpenGL Helper Functions
// ============================================================

BOOL UseOpenGL(void)
{
        return TRUE;
}

static BOOL GL_Init(int winW, int winH)
{
        hGLDC = GetDC(hMainWnd);
        if (!hGLDC)
                return FALSE;

        PIXELFORMATDESCRIPTOR pfd;
        ZeroMemory(&pfd, sizeof(pfd));

        pfd.nSize      = sizeof(pfd);
        pfd.nVersion   = 1;
        pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.iLayerType = PFD_MAIN_PLANE;

        int fmt = ChoosePixelFormat(hGLDC, &pfd);
        if (!fmt || !SetPixelFormat(hGLDC, fmt, &pfd))
        {
                ReleaseDC(hMainWnd, hGLDC);
                hGLDC = NULL;
                return FALSE;
        }

        hGLRC = wglCreateContext(hGLDC);
        if (!hGLRC)
        {
                ReleaseDC(hMainWnd, hGLDC);
                hGLDC = NULL;
                return FALSE;
        }

        wglMakeCurrent(hGLDC, hGLRC);

        glGenTextures(1, &glTex);
        glBindTexture(GL_TEXTURE_2D, glTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 240, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_MIN_FILTER,
                Bilinear ? GL_LINEAR : GL_NEAREST);

        glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_MAG_FILTER,
                Bilinear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glEnable(GL_TEXTURE_2D);

        // Attempt to load PBO extension functions and create the streaming
        // buffer set. On failure s_PBOReady stays FALSE and we fall back to
        // the original synchronous glTexSubImage2D path transparently.
        s_PBOReady = FALSE;
        s_PBOIndex = 0;
        ZeroMemory(s_PBO, sizeof(s_PBO));

        pfn_glGenBuffers    = (PFN_glGenBuffers)   wglGetProcAddress("glGenBuffers");
        pfn_glDeleteBuffers = (PFN_glDeleteBuffers)wglGetProcAddress("glDeleteBuffers");
        pfn_glBindBuffer    = (PFN_glBindBuffer)   wglGetProcAddress("glBindBuffer");
        pfn_glBufferData    = (PFN_glBufferData)   wglGetProcAddress("glBufferData");
        pfn_glMapBuffer     = (PFN_glMapBuffer)    wglGetProcAddress("glMapBuffer");
        pfn_glUnmapBuffer   = (PFN_glUnmapBuffer)  wglGetProcAddress("glUnmapBuffer");

        if (pfn_glGenBuffers && pfn_glDeleteBuffers && pfn_glBindBuffer &&
            pfn_glBufferData && pfn_glMapBuffer && pfn_glUnmapBuffer)
        {
                pfn_glGenBuffers(PBO_COUNT, s_PBO);
                bool allPBOsValid = true;
                for (int i = 0; i < PBO_COUNT; i++)
                {
                        if (!s_PBO[i])
                        {
                                allPBOsValid = false;
                                break;
                        }
                }
                if (allPBOsValid)
                {
                        // Pre-allocate all buffers with STREAM_DRAW hint
                        // (written once per frame by CPU, read once by GPU).
                        for (int i = 0; i < PBO_COUNT; i++)
                        {
                                pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s_PBO[i]);
                                pfn_glBufferData(GL_PIXEL_UNPACK_BUFFER, PBO_SIZE,
                                        NULL, GL_STREAM_DRAW);
                        }
                        pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                        s_PBOReady = TRUE;
                }
                else
                {
                        // glGenBuffers did not allocate the full PBO set.
                        // Clean up and fall back.
                        for (int i = 0; i < PBO_COUNT; i++)
                                if (s_PBO[i]) pfn_glDeleteBuffers(1, &s_PBO[i]);
                        ZeroMemory(s_PBO, sizeof(s_PBO));
                }
        }
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        glWinW = winW;
        glWinH = winH;
        glViewport(0, 0, winW, winH);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, winW, winH, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(hGLDC);
        wglMakeCurrent(NULL, NULL);
        UsingOpenGL = TRUE;
        return TRUE;
}

static void GL_Destroy(void)
{
        if (hGLRC)
        {
                wglMakeCurrent(hGLDC, hGLRC);
                if (glTex) { glDeleteTextures(1, &glTex); glTex = 0; }
                // Clean up PBOs if they were created.
                if (s_PBOReady && pfn_glDeleteBuffers)
                {
                        pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                        pfn_glDeleteBuffers(PBO_COUNT, s_PBO);
                        ZeroMemory(s_PBO, sizeof(s_PBO));
                        s_PBOReady = FALSE;
                }
                wglMakeCurrent(NULL, NULL);
                wglDeleteContext(hGLRC);
                hGLRC = NULL;
        }
        if (hGLDC)
        {
                ReleaseDC(hMainWnd, hGLDC);
                hGLDC = NULL;
        }
        glWinW = glWinH = 0;
        UsingOpenGL = FALSE;
}

// ------------------------------------------------------------------
// P44 (session 21) — stop binding/unbinding the GL context every frame.
//
// ROOT CAUSE (found via the P43 "gap" column + P38's isolated mcr/ofe
// split, session 21 log analysis): GL_DrawFrame used to call
// wglMakeCurrent(hGLDC, hGLRC) at entry and wglMakeCurrent(NULL, NULL)
// right after SwapBuffers, every single frame. The P38 comment predicted
// that some GL drivers defer the actual vblank-wait from SwapBuffers to
// the NEXT call that touches the context -- and the logs confirm it
// exactly: across 360 logged frames, "ofe" (OnFrameEnd, pure QPC math)
// never once moved off 0.00ms, while "mcr" (this wglMakeCurrent(NULL,NULL)
// call) swung from ~0.3ms up to sustained ~13ms blocks and isolated 17ms
// spikes. A generically starved/descheduled thread would show BOTH
// columns jittering; only one of them ever moving means the stall is
// deterministically attached to this specific call, not to scheduling
// in general. Those mcr spikes eat most of the frame's slack, and the
// "gap" column shows the real symptom: periodic genuine dropped frames
// (~2x period followed by a short catch-up gap) landing disproportionately
// during those same low-slack windows -- the visible scroll stutter,
// worse in windowed mode because DWM composition (unavoidable there) is
// what makes the driver take this deferred-wait path in the first place.
//
// FIX: bind the context ONCE when the NES thread starts and release it
// ONCE when the NES thread is about to exit, instead of every frame.
// GL_DrawFrame no longer calls wglMakeCurrent at all in its hot path.
// This is safe because:
//   - GL_DrawFrame is only ever called from the NES thread (via
//     DrawScreen, only reached from PPU.cpp during CPU::ExecOp, which
//     only runs inside NES::Thread()'s loop).
//   - Every UI-thread trigger that used to call wglMakeCurrent directly
//     (WM_SIZE resize, ApplyGLFilter, MatchMonitorRate toggle) was
//     already converted in earlier sessions to a deferred
//     post-and-apply-in-GL_DrawFrame pattern specifically to avoid
//     racing the NES thread's ownership of the context -- see the
//     g_PendingResize / g_PendingBilinear comments above.
//   - GFX::Stop() (which calls GL_Destroy(), the only other code path
//     that touches hGLRC while a session might have been active) is
//     only ever called from the UI thread AFTER NES::Stop() has fully
//     joined the NES thread (NES::Stop()'s "while (Running) Sleep(1)"
//     wait), so by the time GL_Destroy() tries to take the context back,
//     ReleaseGLContext() has already run on the NES thread and the
//     thread itself has exited. No two threads ever contend for hGLRC.
// ------------------------------------------------------------------
void AcquireGLContext(void)
{
        if (UsingOpenGL && hGLDC && hGLRC)
                wglMakeCurrent(hGLDC, hGLRC);
}

void ReleaseGLContext(void)
{
        if (UsingOpenGL && hGLDC && hGLRC)
                wglMakeCurrent(NULL, NULL);
}

// P47: reset the DwmFlush warmup / arm / sentinel state. Called from
// MonitorSync::Enable(TRUE) (and from GFX::Stop) so that a COLD start of
// MMR -- MMR loaded TRUE from registry at first ROM start, or toggled ON
// via the menu at runtime -- also runs the 180-frame GL-vsync-only warmup
// before DwmFlush+interval=0 takes over pacing.
//
// Before P47, s_DwmWarmupFrames was reset to DWM_WARMUP_FRAMES only in
// GFX::Stop's "if (UsingOpenGL && Fullscreen)" branch (i.e. only when
// leaving fullscreen). On a cold start that branch never runs, so
// s_DwmWarmupFrames stayed 0 and the DwmFlush path in GL_DrawFrame
// immediately entered its "armed" branch on frame 1 -- but the GL swap
// interval was still 1 at that point (posted by SetDwmSyncMode(false) in
// GFX::Start, and ApplyPendingVSync on frame 1 applies interval=1 before
// the DwmFlush path runs). DwmFlush blocks ~16ms AND SwapBuffers with
// interval=1 blocks another ~16ms in the SAME frame = ~33ms (2 vblank)
// double-block on frame 1 -- a visible stutter right after start. This
// is exactly the "F000002-F000005 stutter right after start" report from
// session 24 that the handoff doc attributed to warmup; the audit showed
// the warmup was in fact not running at all on cold start.
//
// Thread safety: Enable() runs on the UI thread (WM_COMMAND). The three
// statics are read frame-by-frame on the NES thread inside GL_DrawFrame's
// DwmFlush block. int/bool writes are atomic on x86; SetDwmSyncMode(true)
// is only ever called from the NES thread's armed branch (gated on
// s_DwmModeArmed), so a UI-thread reset before the next NES-frame read is
// safe -- the NES thread will observe s_DwmWarmupFrames=180 and count
// down from there. See MATCH_MONITOR_RATE.md section 9.
void ResetDwmWarmup(void)
{
        s_pfnDwmFlush      = reinterpret_cast<PFN_DwmFlush>(1);
        s_DwmWarmupFrames  = DWM_WARMUP_FRAMES;
        s_DwmModeArmed     = false;
}


// Called ONLY from the NES emulation thread (via ApplyPendingResize inside
// GL_DrawFrame), where the GL context is already current. Never call directly
// from the UI thread — use PostGLResize instead.
static void GL_ResizeInternal(int w, int h)
{
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;

        glWinW = w;
        glWinH = h;

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, w, h, 0, -1, 1);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
}

// Legacy entry point — kept for GFX::Start() which runs on the NES thread
// before rendering has started (no concurrent GL_DrawFrame in flight).
void GL_Resize(int w, int h)
{
        if (!UsingOpenGL || !hGLRC || !hGLDC)
                return;

        // If the NES thread is already running (emulation active), defer the
        // resize so it lands inside GL_DrawFrame where the context is current.
        // If we are NOT yet running (called from GFX::Start setup), it is safe
        // to apply directly — there is no concurrent GL_DrawFrame.
        //
        // P57: ALSO defer when the P54 render thread is active, even if
        // NES::Running is false. GFX::Start's first-init path calls
        // StartRenderThread() (which acquires the GL context on the render
        // thread) BEFORE reaching the fullscreen GL_Resize(scrW, scrH) call.
        // A direct wglMakeCurrent here would STEAL the context from the
        // render thread; the subsequent wglMakeCurrent(NULL,NULL) leaves it
        // current on NO thread. The render thread's glViewport/glClear then
        // fail silently → viewport stays at the windowed dimensions captured
        // by GL_Init → "fullscreen shows only part of the screen" regression.
        // Deferring via PostGLResize lets the render thread's ApplyPendingResize
        // (called in both GL_DrawFrameFromBuffer and the idle loop) apply the
        // resize on the correct thread where the context is current.
        if (NES::Running || IsRenderThreadActive())
        {
                PostGLResize(w, h);
                return;
        }

        if (w <= 0) w = 1;
        if (h <= 0) h = 1;

        wglMakeCurrent(hGLDC, hGLRC);
        GL_ResizeInternal(w, h);
        wglMakeCurrent(NULL, NULL);
}

// Post a deferred resize from the UI thread (WM_SIZE). The new dimensions are
// packed into a single 64-bit atomic so the read in GL_DrawFrame is always
// consistent — no partial-word tearing between width and height.
void PostGLResize(int w, int h)
{
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;

        LONGLONG packed = ((LONGLONG)(DWORD)w << 32) | (LONGLONG)(DWORD)h;
        InterlockedExchange64(&g_PendingResize, packed);
        if (s_FrameEvent)
                SetEvent(s_FrameEvent);
}

// Read and apply any deferred resize. Called at the start of GL_DrawFrame,
// after wglMakeCurrent, so the GL context is current on this thread.
static void ApplyPendingResize()
{
        LONGLONG packed = InterlockedExchange64(&g_PendingResize, -1LL);
        if (packed == -1LL)
                return;

        int w = (int)(DWORD)(packed >> 32);
        int h = (int)(DWORD)(packed & 0xFFFFFFFFLL);
        GL_ResizeInternal(w, h);
}

// ============================================================
// P82: SwapBuffers is timed separately with thread CPU time and cycle count.
// Diagnostic-only; it does not feed back into pacing.
// ============================================================
// ============================================================
// Per-frame timing diagnostics (active when MatchMonitorRate is on).
//
// Each frame we record QPC timestamps across the emulation/render hand-off, GL,
// presentation and DWM stages:
//   t0 = entry to GL_DrawFrame (after palette conversion)
//   t1 = after glTexSubImage2D
//   t2 = after SwapBuffers (= vblank wakeup)
//   t3 = after MonitorSync::OnFrameEnd
//   t4 = after APU::UpdateDRC
//
// The buffer holds the last DIAG_FRAMES frames (~5 seconds at 60fps).
// When any inter-checkpoint delta exceeds DIAG_STALL_MS milliseconds,
// the buffer is flushed to %TEMP%\nintendulator_timing.log so you can
// inspect which call caused the stall.
//
// Overhead: ~3 QPC reads (150ns total) per frame — negligible.
//
// P31 (session 10 full-project audit): the dump itself used to be
// synchronous fopen/fwrite/fclose of a ~360-line file, called directly
// from DrawScreen on the NES thread, with NO rate limiting. That means:
//   - The exact moment a stall is detected (i.e. the exact moment we can
//     least afford more delay), we also do a synchronous disk write --
//     opening/creating a file in %TEMP%, which on many systems is picked
//     up by antivirus real-time scanning or the search indexer. A
//     borderline, otherwise-bounded 20-25ms hiccup (e.g. WaitForDXGIVBlank
//     legitimately hitting its own deadline once) could be extended into
//     a much longer, clearly audible/visible one purely by the act of
//     diagnosing it.
//   - With no rate limiting, a single external event that affects several
//     consecutive frames (plausible for a GPU power-state transition)
//     re-triggers the dump on EVERY one of those frames, rewriting the
//     entire 6-second ring buffer to disk each time -- compounding one
//     hiccup into several.
// Both are fixed below: DiagDumpLogAsync() snapshots the ring buffer
// (cheap memcpy, no I/O) on the NES thread, then hands the snapshot to
// the Windows thread pool (QueueUserWorkItem) to do the actual write.
// A 3-second cooldown prevents back-to-back re-triggering.
// ============================================================
#define DIAG_FRAMES    360          // 6 seconds of history at 60fps
#define DIAG_STALL_MS  20.0        // stall threshold: 20ms (>1 vblank)

struct FrameTimingEntry {
        LONGLONG t0;        // render-thread draw entry / queue consume
        LONGLONG t1;        // after texture upload
        LONGLONG t2;        // after SwapBuffers
        LONGLONG t2b;       // immediately after t2, no GL call between them
        LONGLONG t3;        // after OnFrameEnd (single-threaded path)
        LONGLONG t4;        // after UpdateDRC  (single-threaded path)
        LONGLONG tProd;     // emulation-thread frame publication QPC
        LONGLONG paceTarget;// P67: PaceSlot target QPC for this emulation frame
        LONGLONG paceWake;  // P67: PaceSlot wake QPC after its wait/yield
        ULONGLONG emuFrame; // emulation-frame sequence number
        LONG     fqSkipped; // queued frames skipped before this frame
        LONG     fqDepth;   // queue depth observed at consume time
        LONG     paceSource;// P67: 1=presentation anchor, 0=QPC fallback
        LONGLONG dwmDisplayed;
        LONGLONG dwmVBlank;
        LONGLONG dwmRefreshPeriod;
        LONGLONG dwmCompose;
        ULONGLONG dwmRefresh;
        ULONGLONG dwmFrame;
        ULONGLONG dwmFrameDisplayed;
        ULONGLONG dwmFramesLate;
        ULONGLONG dwmFramesOutstanding;
        ULONGLONG dwmFramesDisplayed;
        ULONGLONG dwmFramesAvailable;
        ULONGLONG dwmFramesMissed;
        ULONGLONG dwmFramesDropped;
        LONG     dwmValid;
        LONG     dwmSource; // 1=window handle, 0=NULL/system, -1=query failed
        LONG     dwmHr;
        // P71: passive producer/render hand-off diagnostics.
        LONGLONG fqProduceQPC;
        LONGLONG fqProduceCsEnterQPC;
        LONGLONG fqProduceCsLeaveQPC;
        LONGLONG fqSignalQPC;
        LONGLONG fqWaitReturnQPC;
        LONGLONG fqConsumeBeginQPC;
        LONGLONG fqConsumeCsEnterQPC;
        LONGLONG fqConsumeCsLeaveQPC;
        LONGLONG fqConsumeEndQPC;
        LONGLONG mmrRunEnterQPC, mmrPaceEnterQPC, mmrPaceWakeQPC;
        LONGLONG mmrPaceCpuWake100ns, mmrSafetyBeginQPC, mmrSafetyEndQPC;
        ULONGLONG mmrPaceCpuWakeCycles;
        LONG mmrSafetyLoops, mmrPaceTimerUsed;
        LONGLONG mmrTraceSeq, prodCpuEnd100ns;
        ULONGLONG prodCpuEndCycles;
        // P81: diagnostic-only frame-build timing between PaceSlot wake and
        // FrameQueue publication. These values never affect pacing.
        LONGLONG buildStartQPC, buildEndQPC;
        LONGLONG buildStartCPU100ns, buildEndCPU100ns;
        ULONGLONG buildStartCycles, buildEndCycles;
        // P82: diagnostic-only SwapBuffers thread CPU/cycle timing.
        LONGLONG swapStartCPU100ns, swapEndCPU100ns;
        ULONGLONG swapStartCycles, swapEndCycles;
        // P83: passive split of the texture/PBO stage. These timestamps are
        // render-thread QPC samples only; they never affect pacing.
        LONGLONG pboSetupEndQPC;
        LONGLONG pboOrphanEndQPC;
        LONGLONG pboMapEndQPC;
        LONGLONG pboCopyEndQPC;
        LONGLONG pboUnmapEndQPC;
        LONGLONG pboSubmitEndQPC;
        DWORD    frameNum;  // render/diagnostic sequence
};
static FrameTimingEntry s_diagBuf[DIAG_FRAMES];
static int  s_diagHead      = 0;
static DWORD s_diagFrameNum = 0;

// ============================================================
// P54 (Stage 2): Two-threaded architecture — FrameQueue + RenderThread.
//
// When MMR is active, the emulation thread (NES::Thread) does NOT call
// GL_DrawFrame. Instead, DrawScreen converts the PPU palette buffer to
// RGBA and pushes it into a small ring buffer (FrameQueue). A separate
// render thread (TIME_CRITICAL) consumes the latest frame and calls
// GL_DrawFrameFromBuffer, which does the texture upload + DwmFlush +
// SwapBuffers on vblank.
//
// This decouples video presentation from emulation: when DwmFlush stalls
// (periodic DWM maintenance, ~33ms), the render thread simply drops a
// frame and catches the next vblank, while the emulation thread keeps
// running — audio never drops.
//
// The GL context is owned by the render thread (AcquireGLContext at
// thread start, ReleaseGLContext at exit). The emulation thread does
// NOT acquire the context when the render thread is active.
// ============================================================

#define FQ_SLOTS 3
#define FQ_FRAME_SIZE (256 * 240 * 4)

struct FQ_Packet {
        unsigned char pixels[FQ_FRAME_SIZE];
        ULONGLONG     emuFrame;
        LONGLONG      producedQPC;
        LONGLONG      paceTargetQPC;
        LONGLONG      paceWakeQPC;
        LONG          fqSkipped;
        LONG          fqDepth;
        LONG          paceSource;
        // P71: diagnostic-only queue/thread hand-off timestamps.
        LONGLONG      fqProduceQPC;
        LONGLONG      fqProduceCsEnterQPC;
        LONGLONG      fqProduceCsLeaveQPC;
        LONGLONG      fqSignalQPC;
        LONGLONG      fqWaitReturnQPC;
        LONGLONG      fqConsumeBeginQPC;
        LONGLONG      fqConsumeCsEnterQPC;
        LONGLONG      fqConsumeCsLeaveQPC;
        LONGLONG      fqConsumeEndQPC;
        // P73: diagnostic-only producer/emulation trace copied through the queue.
        LONGLONG      mmrRunEnterQPC, mmrPaceEnterQPC, mmrPaceWakeQPC;
        LONGLONG      mmrPaceCpuWake100ns, mmrSafetyBeginQPC, mmrSafetyEndQPC;
        ULONGLONG     mmrPaceCpuWakeCycles;
        LONG          mmrSafetyLoops, mmrPaceTimerUsed;
        LONGLONG      mmrTraceSeq, prodCpuEnd100ns;
        ULONGLONG     prodCpuEndCycles;
        // P81: diagnostic-only frame-build timing copied through the queue.
        LONGLONG      buildStartQPC, buildEndQPC;
        LONGLONG      buildStartCPU100ns, buildEndCPU100ns;
        ULONGLONG     buildStartCycles, buildEndCycles;
        // P82: diagnostic-only SwapBuffers thread CPU/cycle timing.
        LONGLONG      swapStartCPU100ns, swapEndCPU100ns;
        ULONGLONG     swapStartCycles, swapEndCycles;
};

static FQ_Packet s_FQ_Buf[FQ_SLOTS];
static CRITICAL_SECTION s_FQ_CS;
static bool s_FQ_CS_Init = false;
static int  s_FQ_Head = 0, s_FQ_Tail = 0, s_FQ_Count = 0;

// Consumer-side scratch packet: FQ_Consume copies the complete metadata and
// pixels so the returned pointer remains stable even after the producer
// overwrites the ring slot.
static FQ_Packet s_FQ_ConsumeCopy;
static volatile LONG s_FQOverflowDrops = 0;
static volatile LONG s_FQSkippedFrames = 0;
static ULONGLONG s_FQEmuFrameCounter = 0;

// P73: passive producer trace published by the emulation thread.
static volatile LONGLONG s_MmrRunEnterQPC = 0, s_MmrPaceEnterQPC = 0, s_MmrPaceWakeQPC = 0;
static volatile LONGLONG s_MmrPaceCpuWake100ns = 0, s_MmrSafetyBeginQPC = 0, s_MmrSafetyEndQPC = 0;
static volatile LONGLONG s_MmrPaceCpuWakeCyclesHi = 0;
static volatile LONG s_MmrPaceCpuWakeCyclesLo = 0;
static volatile LONG s_MmrSafetyLoops = 0, s_MmrPaceTimerUsed = 0;
static volatile LONGLONG s_MmrTraceSeq = 0;
// P81: diagnostic-only timestamps for the frame conversion stage.
static volatile LONGLONG s_MmrBuildStartQPC = 0, s_MmrBuildEndQPC = 0;
static volatile LONGLONG s_MmrBuildStartCPU100ns = 0, s_MmrBuildEndCPU100ns = 0;
static volatile LONGLONG s_MmrBuildStartCycles = 0, s_MmrBuildEndCycles = 0;
static bool DiagGetThreadCpu100ns(LONGLONG *out)
{
        FILETIME c={0},e={0},k={0},u={0};
        if (!out || !GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return false;
        ULARGE_INTEGER a,b; a.LowPart=k.dwLowDateTime; a.HighPart=k.dwHighDateTime;
        b.LowPart=u.dwLowDateTime; b.HighPart=u.dwHighDateTime; *out=(LONGLONG)(a.QuadPart+b.QuadPart); return true;
}
void SetMMRProducerTrace(LONGLONG runEnterQPC, LONGLONG paceEnterQPC, LONGLONG paceWakeQPC, LONGLONG paceCpuWake100ns, ULONGLONG paceCpuWakeCycles, LONGLONG safetyBeginQPC, LONGLONG safetyEndQPC, LONG safetyLoops, LONG paceTimerUsed)
{
        InterlockedExchange64(&s_MmrRunEnterQPC,runEnterQPC); InterlockedExchange64(&s_MmrPaceEnterQPC,paceEnterQPC);
        InterlockedExchange64(&s_MmrPaceWakeQPC,paceWakeQPC); InterlockedExchange64(&s_MmrPaceCpuWake100ns,paceCpuWake100ns);
        InterlockedExchange64(&s_MmrPaceCpuWakeCyclesHi, (LONGLONG)paceCpuWakeCycles);
        InterlockedExchange64(&s_MmrSafetyBeginQPC,safetyBeginQPC); InterlockedExchange64(&s_MmrSafetyEndQPC,safetyEndQPC);
        InterlockedExchange(&s_MmrSafetyLoops,safetyLoops); InterlockedExchange(&s_MmrPaceTimerUsed,paceTimerUsed); InterlockedIncrement64(&s_MmrTraceSeq);
}

static volatile LONG s_RenderThreadActive = 0;
static HANDLE        s_RenderThread = NULL;
static volatile LONG s_RenderThreadStop = 0;

static void FQ_Init(void)
{
        if (!s_FQ_CS_Init)
        {
                InitializeCriticalSection(&s_FQ_CS);
                s_FQ_CS_Init = true;
        }

        EnterCriticalSection(&s_FQ_CS);
        s_FQ_Head = s_FQ_Tail = s_FQ_Count = 0;
        InterlockedExchange(&s_FQOverflowDrops, 0);
        InterlockedExchange(&s_FQSkippedFrames, 0);
        s_FQEmuFrameCounter = 0;
        InterlockedExchange64(&s_MmrBuildStartQPC, 0);
        InterlockedExchange64(&s_MmrBuildEndQPC, 0);
        InterlockedExchange64(&s_MmrBuildStartCPU100ns, 0);
        InterlockedExchange64(&s_MmrBuildEndCPU100ns, 0);
        InterlockedExchange64(&s_MmrBuildStartCycles, 0);
        InterlockedExchange64(&s_MmrBuildEndCycles, 0);
        ZeroMemory(&s_FQ_ConsumeCopy, sizeof(s_FQ_ConsumeCopy));
        LeaveCriticalSection(&s_FQ_CS);

        if (!s_FrameEvent) s_FrameEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
}

static void FQ_Destroy(void)
{
        if (s_FrameEvent) { CloseHandle(s_FrameEvent); s_FrameEvent = NULL; }
        s_FQ_Head = s_FQ_Tail = s_FQ_Count = 0;
}

// Producer (emulation thread): write a frame, timestamp it, then signal the
// render thread. The queue remains latest-wins, but the dropped/skipped count
// is now observable in the timing log so a future session can distinguish
// genuine presentation jitter from queue pressure.
static void FQ_Produce(const unsigned char *src)
{
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        ULONGLONG frameSeq = ++s_FQEmuFrameCounter;
        int slot = s_FQ_Head;

        s_FQ_Buf[slot].fqProduceQPC = qpc.QuadPart;
        EnterCriticalSection(&s_FQ_CS);
        QueryPerformanceCounter(&qpc);
        s_FQ_Buf[slot].fqProduceCsEnterQPC = qpc.QuadPart;
        memcpy(s_FQ_Buf[slot].pixels, src, FQ_FRAME_SIZE);
        QueryPerformanceCounter(&qpc);
        s_FQ_Buf[slot].producedQPC = qpc.QuadPart;
        { LONGLONG cpu100=0; ULONGLONG cpuCycles=0; DiagGetThreadCpu100ns(&cpu100);
          QueryThreadCycleTime(GetCurrentThread(), &cpuCycles);
          s_FQ_Buf[slot].prodCpuEnd100ns=cpu100;
          s_FQ_Buf[slot].prodCpuEndCycles=cpuCycles;
          s_FQ_Buf[slot].mmrPaceCpuWakeCycles=(ULONGLONG)InterlockedExchangeAdd64(&s_MmrPaceCpuWakeCyclesHi,0);
          s_FQ_Buf[slot].mmrRunEnterQPC=InterlockedExchangeAdd64(&s_MmrRunEnterQPC,0);
          s_FQ_Buf[slot].mmrPaceEnterQPC=InterlockedExchangeAdd64(&s_MmrPaceEnterQPC,0);
          s_FQ_Buf[slot].mmrPaceWakeQPC=InterlockedExchangeAdd64(&s_MmrPaceWakeQPC,0);
          s_FQ_Buf[slot].mmrPaceCpuWake100ns=InterlockedExchangeAdd64(&s_MmrPaceCpuWake100ns,0);
          s_FQ_Buf[slot].mmrSafetyBeginQPC=InterlockedExchangeAdd64(&s_MmrSafetyBeginQPC,0);
          s_FQ_Buf[slot].mmrSafetyEndQPC=InterlockedExchangeAdd64(&s_MmrSafetyEndQPC,0);
          s_FQ_Buf[slot].mmrSafetyLoops=InterlockedExchangeAdd(&s_MmrSafetyLoops,0);
          s_FQ_Buf[slot].mmrPaceTimerUsed=InterlockedExchangeAdd(&s_MmrPaceTimerUsed,0);
          s_FQ_Buf[slot].mmrTraceSeq=InterlockedExchangeAdd64(&s_MmrTraceSeq,0);
          s_FQ_Buf[slot].buildStartQPC=InterlockedExchangeAdd64(&s_MmrBuildStartQPC,0);
          s_FQ_Buf[slot].buildEndQPC=InterlockedExchangeAdd64(&s_MmrBuildEndQPC,0);
          s_FQ_Buf[slot].buildStartCPU100ns=InterlockedExchangeAdd64(&s_MmrBuildStartCPU100ns,0);
          s_FQ_Buf[slot].buildEndCPU100ns=InterlockedExchangeAdd64(&s_MmrBuildEndCPU100ns,0);
          s_FQ_Buf[slot].buildStartCycles=(ULONGLONG)InterlockedExchangeAdd64(&s_MmrBuildStartCycles,0);
          s_FQ_Buf[slot].buildEndCycles=(ULONGLONG)InterlockedExchangeAdd64(&s_MmrBuildEndCycles,0); }
        s_FQ_Buf[slot].paceTargetQPC = MonitorSync::GetLastPaceTargetQPC();
        s_FQ_Buf[slot].paceWakeQPC = MonitorSync::GetLastPaceWakeQPC();
        s_FQ_Buf[slot].paceSource = MonitorSync::WasLastPacePresentationAnchored() ? 1 : 0;
        s_FQ_Buf[slot].emuFrame = frameSeq;
        s_FQ_Buf[slot].fqSkipped = 0;
        s_FQ_Buf[slot].fqDepth = 0;
        QueryPerformanceCounter(&qpc);
        s_FQ_Buf[slot].fqProduceCsLeaveQPC = qpc.QuadPart;
        s_FQ_Head = (s_FQ_Head + 1) % FQ_SLOTS;

        if (s_FQ_Count >= FQ_SLOTS)
        {
                s_FQ_Tail = (s_FQ_Tail + 1) % FQ_SLOTS;
                InterlockedIncrement(&s_FQOverflowDrops);
        }
        else
        {
                s_FQ_Count++;
        }
        LeaveCriticalSection(&s_FQ_CS);

        QueryPerformanceCounter(&qpc);
        s_FQ_Buf[slot].fqSignalQPC = qpc.QuadPart;
        if (s_FrameEvent) SetEvent(s_FrameEvent);
}

// Consumer (render thread): get the newest available frame. If several frames
// are queued, discard older ones, but retain exact metadata for the frame that
// is actually presented.
static const FQ_Packet *FQ_Consume(LONG *skipped, LONG *depth, LONGLONG waitReturnQPC)
{
        if (skipped) *skipped = 0;
        if (depth) *depth = 0;

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        LONGLONG consumeBeginQPC = qpc.QuadPart;
        LONGLONG consumeCsEnterQPC = 0;
        LONGLONG consumeCsLeaveQPC = 0;

        EnterCriticalSection(&s_FQ_CS);
        QueryPerformanceCounter(&qpc);
        consumeCsEnterQPC = qpc.QuadPart;
        if (s_FQ_Count <= 0)
        {
                LeaveCriticalSection(&s_FQ_CS);
                return NULL;
        }

        int queued = s_FQ_Count;
        if (depth) *depth = queued;
        if (skipped) *skipped = queued - 1;

        if (queued > 1)
                InterlockedExchangeAdd(&s_FQSkippedFrames, queued - 1);

        while (s_FQ_Count > 1)
        {
                s_FQ_Tail = (s_FQ_Tail + 1) % FQ_SLOTS;
                s_FQ_Count--;
        }

        memcpy(&s_FQ_ConsumeCopy, &s_FQ_Buf[s_FQ_Tail], sizeof(s_FQ_ConsumeCopy));
        s_FQ_ConsumeCopy.fqSkipped = queued - 1;
        s_FQ_ConsumeCopy.fqDepth = queued;
        s_FQ_Tail = (s_FQ_Tail + 1) % FQ_SLOTS;
        s_FQ_Count--;
        QueryPerformanceCounter(&qpc);
        consumeCsLeaveQPC = qpc.QuadPart;
        LeaveCriticalSection(&s_FQ_CS);
        QueryPerformanceCounter(&qpc);
        s_FQ_ConsumeCopy.fqWaitReturnQPC = waitReturnQPC;
        s_FQ_ConsumeCopy.fqConsumeBeginQPC = consumeBeginQPC;
        s_FQ_ConsumeCopy.fqConsumeCsEnterQPC = consumeCsEnterQPC;
        s_FQ_ConsumeCopy.fqConsumeCsLeaveQPC = consumeCsLeaveQPC;
        s_FQ_ConsumeCopy.fqConsumeEndQPC = qpc.QuadPart;
        return &s_FQ_ConsumeCopy;
}

bool IsRenderThreadActive(void)
{
        return InterlockedExchangeAdd(&s_RenderThreadActive, 0) != 0;
}

void ProduceFrameToQueue(const unsigned char *rgba)
{
        if (!IsRenderThreadActive()) return;
        FQ_Produce(rgba);
}

// P54: forward declarations — GL_DrawFrameFromBuffer (below) uses these
// static helpers which are defined later in the file. Forward-declaring
// them here keeps the two-threaded code grouped together for readability.
static double DiagQPCFreq(void);
static void DiagCompleteFrame(LONGLONG t3, LONGLONG t4);
static bool DiagQueryDwmTiming(FrameTimingEntry &e);
static void ApplyPendingResize(void);

// P54: GL_DrawFrameFromBuffer — renders a frame from a pre-filled RGBA
// buffer (produced by the emulation thread) instead of reading PPU's
// palette array. Called on the RENDER THREAD, where the GL context is
// current. This is GL_DrawFrame with the palette-conversion step
// removed (the buffer is already converted) and the diagnostic timing
// kept (gap/tex/swap still measured here).
static void GL_DrawFrameFromBuffer(const FQ_Packet *packet)
{
        LONGLONG diagT0 = 0, diagT1 = 0;
        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); diagT0 = qpc.QuadPart;
                s_diagFrameNum++;
                int idx = s_diagHead;
                s_diagBuf[idx].t0       = diagT0;
                s_diagBuf[idx].t1       = 0;
                s_diagBuf[idx].t2       = 0;
                s_diagBuf[idx].t2b      = 0;
                s_diagBuf[idx].t3       = 0;
                s_diagBuf[idx].t4       = 0;
                s_diagBuf[idx].tProd    = packet ? packet->producedQPC : 0;
                s_diagBuf[idx].paceTarget = packet ? packet->paceTargetQPC : 0;
                s_diagBuf[idx].paceWake   = packet ? packet->paceWakeQPC : 0;
                s_diagBuf[idx].paceSource = packet ? packet->paceSource : 0;
                // P70: initialize every DWM diagnostic field for the new
                // ring-buffer entry. Without this, a failed DWM query could
                // leave the previous frame's values marked as valid.
                s_diagBuf[idx].dwmDisplayed = 0;
                s_diagBuf[idx].dwmVBlank = 0;
                s_diagBuf[idx].dwmRefreshPeriod = 0;
                s_diagBuf[idx].dwmCompose = 0;
                s_diagBuf[idx].dwmRefresh = 0;
                s_diagBuf[idx].dwmFrame = 0;
                s_diagBuf[idx].dwmFrameDisplayed = 0;
                s_diagBuf[idx].dwmFramesLate = 0;
                s_diagBuf[idx].dwmFramesOutstanding = 0;
                s_diagBuf[idx].dwmFramesDisplayed = 0;
                s_diagBuf[idx].dwmFramesAvailable = 0;
                s_diagBuf[idx].dwmFramesMissed = 0;
                s_diagBuf[idx].dwmFramesDropped = 0;
                s_diagBuf[idx].dwmValid = 0;
                s_diagBuf[idx].dwmSource = -1;
                s_diagBuf[idx].dwmHr = 0;
                s_diagBuf[idx].fqProduceQPC = packet ? packet->fqProduceQPC : 0;
                s_diagBuf[idx].fqProduceCsEnterQPC = packet ? packet->fqProduceCsEnterQPC : 0;
                s_diagBuf[idx].fqProduceCsLeaveQPC = packet ? packet->fqProduceCsLeaveQPC : 0;
                s_diagBuf[idx].fqSignalQPC = packet ? packet->fqSignalQPC : 0;
                s_diagBuf[idx].fqWaitReturnQPC = packet ? packet->fqWaitReturnQPC : 0;
                s_diagBuf[idx].fqConsumeBeginQPC = packet ? packet->fqConsumeBeginQPC : 0;
                s_diagBuf[idx].fqConsumeCsEnterQPC = packet ? packet->fqConsumeCsEnterQPC : 0;
                s_diagBuf[idx].fqConsumeCsLeaveQPC = packet ? packet->fqConsumeCsLeaveQPC : 0;
                s_diagBuf[idx].fqConsumeEndQPC = packet ? packet->fqConsumeEndQPC : 0;
                s_diagBuf[idx].mmrRunEnterQPC = packet ? packet->mmrRunEnterQPC : 0;
                s_diagBuf[idx].mmrPaceEnterQPC = packet ? packet->mmrPaceEnterQPC : 0;
                s_diagBuf[idx].mmrPaceWakeQPC = packet ? packet->mmrPaceWakeQPC : 0;
                s_diagBuf[idx].mmrPaceCpuWake100ns = packet ? packet->mmrPaceCpuWake100ns : 0;
                s_diagBuf[idx].mmrPaceCpuWakeCycles = packet ? packet->mmrPaceCpuWakeCycles : 0;
                s_diagBuf[idx].mmrSafetyBeginQPC = packet ? packet->mmrSafetyBeginQPC : 0;
                s_diagBuf[idx].mmrSafetyEndQPC = packet ? packet->mmrSafetyEndQPC : 0;
                s_diagBuf[idx].mmrSafetyLoops = packet ? packet->mmrSafetyLoops : 0;
                s_diagBuf[idx].mmrPaceTimerUsed = packet ? packet->mmrPaceTimerUsed : 0;
                s_diagBuf[idx].mmrTraceSeq = packet ? packet->mmrTraceSeq : 0;
                s_diagBuf[idx].prodCpuEnd100ns = packet ? packet->prodCpuEnd100ns : 0;
                s_diagBuf[idx].prodCpuEndCycles = packet ? packet->prodCpuEndCycles : 0;
                s_diagBuf[idx].buildStartQPC = packet ? packet->buildStartQPC : 0;
                s_diagBuf[idx].buildEndQPC = packet ? packet->buildEndQPC : 0;
                s_diagBuf[idx].buildStartCPU100ns = packet ? packet->buildStartCPU100ns : 0;
                s_diagBuf[idx].buildEndCPU100ns = packet ? packet->buildEndCPU100ns : 0;
                s_diagBuf[idx].buildStartCycles = packet ? packet->buildStartCycles : 0;
                s_diagBuf[idx].buildEndCycles = packet ? packet->buildEndCycles : 0;
                s_diagBuf[idx].swapStartCPU100ns = 0;
                s_diagBuf[idx].swapEndCPU100ns = 0;
                s_diagBuf[idx].swapStartCycles = 0;
                s_diagBuf[idx].swapEndCycles = 0;
                s_diagBuf[idx].pboSetupEndQPC = 0;
                s_diagBuf[idx].pboOrphanEndQPC = 0;
                s_diagBuf[idx].pboMapEndQPC = 0;
                s_diagBuf[idx].pboCopyEndQPC = 0;
                s_diagBuf[idx].pboUnmapEndQPC = 0;
                s_diagBuf[idx].pboSubmitEndQPC = 0;
                s_diagBuf[idx].emuFrame = packet ? packet->emuFrame : 0;
                s_diagBuf[idx].fqSkipped = packet ? packet->fqSkipped : 0;
                s_diagBuf[idx].fqDepth = packet ? packet->fqDepth : 0;
                s_diagBuf[idx].frameNum = s_diagFrameNum;
                s_diagHead = (s_diagHead + 1) % DIAG_FRAMES;
        }

        MonitorSync::ApplyPendingVSync();
        ApplyPendingResize();
        {
                LONG pendingBilinear = InterlockedExchange(&g_PendingBilinear, -1L);
                if (pendingBilinear >= 0)
                        Bilinear = (pendingBilinear != 0) ? TRUE : FALSE;
        }

        glViewport(0, 0, glWinW, glWinH);
        glBindTexture(GL_TEXTURE_2D, glTex);

        // P83: split the old broad "tex" measurement into the actual PBO
        // operations. The first diagnostic revision showed tex ~= one full
        // refresh while SwapBuffers itself was effectively non-blocking.
        // That makes an internal PBO reuse wait the leading suspect.
        LONGLONG pboSetupEndQPC = 0;
        LONGLONG pboOrphanEndQPC = 0;
        LONGLONG pboMapEndQPC = 0;
        LONGLONG pboCopyEndQPC = 0;
        LONGLONG pboUnmapEndQPC = 0;
        LONGLONG pboSubmitEndQPC = 0;
        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                pboSetupEndQPC = qpc.QuadPart;
        }

        // Texture upload from the pre-filled buffer (no palette conversion).
        if (s_PBOReady)
        {
                GLuint writePBO = s_PBO[s_PBOIndex];
                pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, writePBO);
                pfn_glBufferData(GL_PIXEL_UNPACK_BUFFER, PBO_SIZE, NULL, GL_STREAM_DRAW);
                if (MatchMonitorRate)
                {
                        LARGE_INTEGER qpc;
                        QueryPerformanceCounter(&qpc);
                        pboOrphanEndQPC = qpc.QuadPart;
                }

                void* pboMem = pfn_glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
                if (MatchMonitorRate)
                {
                        LARGE_INTEGER qpc;
                        QueryPerformanceCounter(&qpc);
                        pboMapEndQPC = qpc.QuadPart;
                }

                if (pboMem)
                {
                        memcpy(pboMem, packet->pixels, FQ_FRAME_SIZE);
                        if (MatchMonitorRate)
                        {
                                LARGE_INTEGER qpc;
                                QueryPerformanceCounter(&qpc);
                                pboCopyEndQPC = qpc.QuadPart;
                        }

                        pfn_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                        if (MatchMonitorRate)
                        {
                                LARGE_INTEGER qpc;
                                QueryPerformanceCounter(&qpc);
                                pboUnmapEndQPC = qpc.QuadPart;
                        }

                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240,
                                GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
                        if (MatchMonitorRate)
                        {
                                LARGE_INTEGER qpc;
                                QueryPerformanceCounter(&qpc);
                                pboSubmitEndQPC = qpc.QuadPart;
                        }
                }
                else
                {
                        pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240,
                                GL_BGRA_EXT, GL_UNSIGNED_BYTE, packet->pixels);
                        if (MatchMonitorRate)
                        {
                                LARGE_INTEGER qpc;
                                QueryPerformanceCounter(&qpc);
                                pboSubmitEndQPC = qpc.QuadPart;
                        }
                }
                pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                s_PBOIndex = (s_PBOIndex + 1) % PBO_COUNT;
        }
        else
        {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240,
                        GL_BGRA_EXT, GL_UNSIGNED_BYTE, packet->pixels);
                if (MatchMonitorRate)
                {
                        LARGE_INTEGER qpc;
                        QueryPerformanceCounter(&qpc);
                        pboSubmitEndQPC = qpc.QuadPart;
                }
        }

        if (MatchMonitorRate)
        {
                int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                s_diagBuf[idx].pboSetupEndQPC = pboSetupEndQPC;
                s_diagBuf[idx].pboOrphanEndQPC = pboOrphanEndQPC;
                s_diagBuf[idx].pboMapEndQPC = pboMapEndQPC;
                s_diagBuf[idx].pboCopyEndQPC = pboCopyEndQPC;
                s_diagBuf[idx].pboUnmapEndQPC = pboUnmapEndQPC;
                s_diagBuf[idx].pboSubmitEndQPC = pboSubmitEndQPC;
        }

        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); diagT1 = qpc.QuadPart;
                int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                s_diagBuf[idx].t1 = diagT1;
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int dstX, dstY, dstW, dstH;
        if (IntegerScale)
        {
                int scale = 1;
                for (int m = 2; m <= 16; m++)
                {
                        if (256 * m <= glWinW && 240 * m <= glWinH) scale = m;
                        else break;
                }
                dstW = 256 * scale; dstH = 240 * scale;
                dstX = (glWinW - dstW) / 2; dstY = (glWinH - dstH) / 2;
                ISMult = scale; ISBorderX = dstX; ISBorderY = dstY;
        }
        else
        {
                int hw = 240 * glWinW;
                int wh = 256 * glWinH;
                if (hw > wh) { dstW = wh / 240; dstH = glWinH; }
                else         { dstW = glWinW; dstH = hw / 256; }
                dstX = (glWinW - dstW) / 2; dstY = (glWinH - dstH) / 2;
        }

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, glTex);
        {
                static BOOL s_lastBilinear = -1;
                if (Bilinear != s_lastBilinear)
                {
                        s_lastBilinear = Bilinear;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                Bilinear ? GL_LINEAR : GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                Bilinear ? GL_LINEAR : GL_NEAREST);
                }
        }
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f); glVertex2i(dstX,        dstY);
                glTexCoord2f(1.0f, 0.0f); glVertex2i(dstX + dstW, dstY);
                glTexCoord2f(1.0f, 1.0f); glVertex2i(dstX + dstW, dstY + dstH);
                glTexCoord2f(0.0f, 1.0f); glVertex2i(dstX,        dstY + dstH);
        glEnd();

        if (Scanlines)
        {
                glDisable(GL_TEXTURE_2D);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glColor4f(0.0f, 0.0f, 0.0f, 0.25f);
                glBegin(GL_LINES);
                for (int y = dstY; y < dstY + dstH; y += 2)
                {
                        glVertex2i(dstX, y);
                        glVertex2i(dstX + dstW, y);
                }
                glEnd();
                glDisable(GL_BLEND);
                glEnable(GL_TEXTURE_2D);
                glColor4f(1, 1, 1, 1);
        }

        // P84: in the current windowed path SwapBuffers() is not a
        // presentation boundary. The runtime log showed swap ~= 0 ms while
        // Match Monitor Rate was running from a QPC-only presentation clock.
        //
        // That lets the producer run at nominal 60 Hz while DWM independently
        // chooses which composition tick consumes each submit. The result can
        // be a repeated frame or a missed composition phase even when the
        // producer's average frame rate is correct.
        //
        // Use the render thread as a bridge between OpenGL and DWM:
        //
        //     SwapBuffers(interval=0) -> DwmFlush() -> next frame
        //
        // The return from DwmFlush is fed into MonitorSync as the observed
        // presentation timestamp. Audio/CPU pacing never waits here because
        // this code executes only on the dedicated GL render thread.

        const bool renderStopping =
                (InterlockedExchangeAdd(&s_RenderThreadStop, 0) != 0);
        const bool useDwmPresentation =
                (!renderStopping &&
                 MatchMonitorRate &&
                 !(Fullscreen && ExclusiveFullscreen) &&
                 !MonitorSync::HasDXGIVBlank());

#if USE_DWMFLUSH
        if (useDwmPresentation)
                InterlockedIncrement(&s_DwmFlushPathReached);
#endif

        if (useDwmPresentation)
        {
                // Keep SwapBuffers non-blocking. DwmFlush below is the sole
                // compositor synchronizer for the DWM path.
                if (!s_DwmModeArmed)
                {
                        MonitorSync::SetDwmSyncMode(true);
                        s_DwmModeArmed = true;
                }
        }
        else if (s_DwmModeArmed)
        {
                MonitorSync::SetDwmSyncMode(false);
                s_DwmModeArmed = false;
        }

        // DXGI vblank and DwmFlush are alternative presentation masters.
        // Never wait on both for the same frame.
        if (MatchMonitorRate && MonitorSync::HasDXGIVBlank() &&
                !useDwmPresentation)
                MonitorSync::WaitForDXGIVBlank();

        int diagSwapIdxPre = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
        if (MatchMonitorRate)
        {
                DiagGetThreadCpu100ns(&s_diagBuf[diagSwapIdxPre].swapStartCPU100ns);
                QueryThreadCycleTime(GetCurrentThread(), &s_diagBuf[diagSwapIdxPre].swapStartCycles);
        }

        SwapBuffers(hGLDC);

        if (MatchMonitorRate)
        {
                DiagGetThreadCpu100ns(&s_diagBuf[diagSwapIdxPre].swapEndCPU100ns);
                QueryThreadCycleTime(GetCurrentThread(), &s_diagBuf[diagSwapIdxPre].swapEndCycles);
        }

        // t2 = submit boundary. In the DWM path this is deliberately not the
        // presentation timestamp: SwapBuffers only queues the new back buffer.
        LARGE_INTEGER swapDoneQpc;
        QueryPerformanceCounter(&swapDoneQpc);
        int diagSwapIdx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
        s_diagBuf[diagSwapIdx].t2 = swapDoneQpc.QuadPart;

#if USE_DWMFLUSH
        if (useDwmPresentation)
        {
                if (s_pfnDwmFlush == reinterpret_cast<PFN_DwmFlush>(1))
                {
                        HMODULE hDwm = LoadLibrary(_T("dwmapi.dll"));
                        s_pfnDwmFlush = hDwm ?
                                (PFN_DwmFlush)GetProcAddress(hDwm, "DwmFlush") : NULL;
                }

                if (s_pfnDwmFlush)
                {
                        LARGE_INTEGER flushQpc;
                        HRESULT hr = s_pfnDwmFlush();
                        QueryPerformanceCounter(&flushQpc);
                        (void)hr;

                        // t2b = DwmFlush completion / presentation feedback.
                        int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                        s_diagBuf[idx].t2b = flushQpc.QuadPart;
                        // P85: DwmFlush completion is NOT a display timestamp.
                        // Do not feed it into PaceSlot(). DWM composition timing
                        // is sampled below via qpcCompose + cFrame.
                }
        }
#endif
        if (MatchMonitorRate)
        {
                int idxDwm = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                if (DiagQueryDwmTiming(s_diagBuf[idxDwm]) &&
                    s_diagBuf[idxDwm].dwmFrame > 0 &&
                    s_diagBuf[idxDwm].dwmCompose > 0)
                {
                        // P85: qpcCompose is tied to a concrete DWM composition
                        // frame. NotifyDwmCompositionSample rejects duplicate
                        // snapshots and does not require DwmFlush timing.
                        MonitorSync::NotifyDwmCompositionSample(
                                s_diagBuf[idxDwm].dwmCompose,
                                s_diagBuf[idxDwm].dwmFrame);
                }
        }

        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc2; QueryPerformanceCounter(&qpc2);
                int idx2 = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;

                // t2b is the DwmFlush completion time for diagnostics. It is NOT the
                // presentation timestamp used by MonitorSync.
                if (s_diagBuf[idx2].t2b == 0)
                        s_diagBuf[idx2].t2b = qpc2.QuadPart;

                // P54: complete the diag entry from the render thread.
                // t3/t4 are the same as t2b here (no OnFrameEnd/UpdateDRC
                // on the render thread — those run on the emulation thread).
                s_diagBuf[idx2].t3 = qpc2.QuadPart;
                s_diagBuf[idx2].t4 = qpc2.QuadPart;
                DiagCompleteFrame(qpc2.QuadPart, qpc2.QuadPart);
        }
}

// P54: render thread entry point. Owns the GL context for its lifetime.
static HANDLE CreateRenderPhaseTimer()
{
        typedef HANDLE (WINAPI *PFN_CreateWaitableTimerExW)(
                LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        static const DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_FLAG = 0x00000002;

        PFN_CreateWaitableTimerExW pfnEx = (PFN_CreateWaitableTimerExW)
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                               "CreateWaitableTimerExW");
        HANDLE hTimer = NULL;
        if (pfnEx)
                hTimer = pfnEx(NULL, NULL,
                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_FLAG, TIMER_ALL_ACCESS);
        if (!hTimer)
                hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
        return hTimer;
}

// P62: phase-aware presentation gate. This is intentionally NOT an independent
// frame timer: it is anchored to the last real DWM presentation timestamp and
// the filtered presentation period maintained by MonitorSync. Its only job is
// to prevent the render thread from submitting a frame materially early in the
// composition cycle after a previous missed refresh. If presentation feedback
// is unavailable, the old event-driven path is left completely unchanged.
static DWORD WINAPI RenderThreadProc(void *)
{
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        AcquireGLContext();
        InterlockedExchange(&s_RenderThreadActive, 1);

        while (!InterlockedExchangeAdd(&s_RenderThreadStop, 0))
        {
                DWORD wait = WaitForSingleObject(s_FrameEvent, INFINITE);
                LARGE_INTEGER waitQpc;
                QueryPerformanceCounter(&waitQpc);
                if (wait != WAIT_OBJECT_0)
                        continue;
                if (InterlockedExchangeAdd(&s_RenderThreadStop, 0))
                        break;

                const FQ_Packet *packet = FQ_Consume(NULL, NULL, waitQpc.QuadPart);
                if (packet)
                {
                        // P64: do not add a software phase wait in front of
                        // SwapBuffers/DwmFlush. DwmFlush is already the presentation
                        // synchronizer in this path; the P62/P63 pre-wait introduced
                        // a repeatable 33 ms cadence on real systems by making the
                        // render submission itself phase-sensitive. Keep frame
                        // consumption event-driven and let the native presentation
                        // path provide the backpressure.
                        GL_DrawFrameFromBuffer(packet);
                }
                else
                {
                        // No frame is pending. Do not clear or call
                        // SwapBuffers: keeping the already-displayed surface
                        // visible is both cheaper and visually correct.
                        ApplyPendingResize();
                }
        }

        ReleaseGLContext();
        InterlockedExchange(&s_RenderThreadActive, 0);
        return 0;
}

void StartRenderThread(void)
{
        if (s_RenderThread) return;
        FQ_Init();
        InterlockedExchange(&s_RenderThreadStop, 0);
        s_RenderThread = CreateThread(NULL, 0, RenderThreadProc, NULL, 0, NULL);
        // Wait for the thread to signal it's running (context acquired).
        for (int i = 0; i < 200 && !IsRenderThreadActive(); i++)
                Sleep(1);
}

void StopRenderThread(void)
{
        if (!s_RenderThread) return;

        InterlockedExchange(&s_RenderThreadStop, 1);
        if (s_FrameEvent) SetEvent(s_FrameEvent);  // wake it up if blocked

        // P95: the render thread owns the OpenGL context for its entire
        // lifetime.  Closing its HANDLE after a timeout and immediately
        // destroying the GL context is unsafe if the thread is still inside
        // SwapBuffers/DwmFlush.  That is exactly the kind of shutdown race
        // that can turn MMR-off into a process-wide hang.  Join the thread
        // first; only after it has released the context do we destroy the
        // queue and close the handle.
        //
        // P98: the join above used to be a raw WaitForSingleObject(..., INFINITE)
        // executed on the UI thread. That blocks the UI thread's message queue
        // completely while waiting. On this UI thread, SwapBuffers()/the GL
        // driver/DWM can -- on some systems/drivers, especially right around a
        // window-style or display-mode change -- need the owning window's
        // message queue to be pumped before a pending GL/DWM call on the
        // render thread can complete. With the UI thread frozen in a message-
        // less wait, that produces a genuine cross-thread deadlock: the UI
        // thread waits for the render thread to exit, and the render thread
        // (indirectly, via the graphics stack) waits for the UI thread to pump
        // messages. The window then stops responding entirely and can only be
        // killed from Task Manager -- this is exactly the reported "unchecking
        // Match Monitor Rate freezes the emulator" symptom.
        //
        // Fix: wait for the thread while still pumping the UI thread's message
        // queue, the same pattern NES::Stop()/NES::Pause() already use for the
        // emulation thread (see NES.cpp). This does not change shutdown
        // ordering or resource lifetime at all -- GL_Destroy()/FQ_Destroy()
        // still only run after the render thread has actually exited -- it
        // only keeps the UI thread's message loop alive while we wait, which
        // is enough to break the deadlock without weakening the join.
        while (WaitForSingleObject(s_RenderThread, 0) == WAIT_TIMEOUT)
        {
                ProcessMessages();
                Sleep(1);
        }
        CloseHandle(s_RenderThread);
        s_RenderThread = NULL;
        FQ_Destroy();
}


// Returns the QPC frequency (cached).
static double DiagQPCFreq()
{
        static double freq = 0.0;
        if (freq == 0.0)
        {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                freq = (double)f.QuadPart;
        }
        return freq;
}

static void DiagWriteLogFile(const FrameTimingEntry *buf, int head)
{
        TCHAR tmpPath[MAX_PATH];
        GetTempPath(MAX_PATH, tmpPath);
        TCHAR logPath[MAX_PATH];
        _stprintf_s(logPath, MAX_PATH, _T("%snintendulator_timing.log"), tmpPath);
        FILE *f = _tfopen(logPath, _T("w"));
        if (!f) return;

        double freq = DiagQPCFreq();
        _ftprintf(f, _T("Nintendulator frame timing log\n"));
        _ftprintf(f, _T("Stall threshold: %.1f ms\n"), DIAG_STALL_MS);
        _ftprintf(f, _T("DXGI vblank bypass active: %s\n"),
                MonitorSync::HasDXGIVBlank() ? _T("YES") : _T("NO (falling back to DWM-composited vsync)"));
        _ftprintf(f, _T("DXGI init detail: %s\n"), MonitorSync::GetDXGIFailReason());
        // P45 (session 22): does wglSwapIntervalEXT(1) actually block
        // SwapBuffers on this system? IsVSyncActive() reflects whether the
        // driver *accepted and verified* the interval request -- it says
        // nothing about whether the request actually produces real
        // backpressure once DWM composition is in the picture (see the
        // USE_DWMFLUSH comment block in GL_DrawFrame for why those are two
        // different questions on Windows 7/8 with older drivers). Printed
        // here so a log where `swap` reads ~0.00ms on every single frame
        // can be cross-checked against whether the interval request even
        // nominally succeeded.
        _ftprintf(f, _T("GL vsync request verified by driver: %s   DwmFlush windowed-sync mode: %s\n"),
                MonitorSync::IsVSyncActive() ? _T("YES") : _T("NO"),
                (USE_DWMFLUSH ? _T("compiled IN") : _T("compiled OUT")));
        // P48/P50 (session 25): the post-P47 log showed swap=0.03ms on every
        // frame AND a 20/20/10ms (3:2 pulldown) gap pattern — meaning NEITHER
        // GL vsync NOR DwmFlush was providing any backpressure on this
        // system. "verified by driver: NO" only tells us wglGetSwapIntervalEXT
        // didn't confirm interval=1; it does not tell us whether DwmFlush
        // loaded, armed, or actually blocks. Print those facts here so the
        // next log can be diagnosed without another round-trip:
        //   - pfnWglSwapIntervalEXT loaded? (if NULL, no vsync path at all)
        //   - DwmFlush loaded? (NULL = LoadLibrary/GetProcAddress failed;
        //     sentinel = not yet attempted; valid = loaded)
        //   - DwmFlush armed? (s_DwmModeArmed — true after SetDwmSyncMode(true))
        //   - DwmFlush warmup remaining? (s_DwmWarmupFrames — counts down
        //     from DWM_WARMUP_FRAMES; while >0 DwmFlush is intentionally
        //     not called and the timer paces alone)
        //   - g_DwmSyncMode? (1 = SetDwmSyncMode(true) posted interval=0)
        //   - glWinW x glWinH? (the GL viewport; P49 fullscreen bug check)
        {
            const TCHAR *dwmState;
            if (s_pfnDwmFlush == reinterpret_cast<PFN_DwmFlush>(1))
                dwmState = _T("sentinel (not yet loaded)");
            else if (s_pfnDwmFlush == NULL)
                dwmState = _T("NULL (LoadLibrary/GetProcAddress failed)");
            else
                dwmState = _T("loaded");
            _ftprintf(f, _T("DwmFlush: %s, armed=%s, warmup=%d, g_DwmSyncMode=%d, glViewport=%dx%d\n"),
                    dwmState,
                    s_DwmModeArmed ? _T("YES") : _T("NO"),
                    s_DwmWarmupFrames,
                    MonitorSync::GetDwmSyncMode(),
                    glWinW, glWinH);
            // P52: the post-P51b log showed DwmFlush=sentinel, warmup=180
            // even after 781+ frames — meaning the DwmFlush path never
            // executed despite USE_DWMFLUSH=1, MatchMonitorRate=TRUE, and
            // ExclusiveFullscreen=FALSE. Print those values here so the
            // next log can confirm whether the #if block is compiling and
            // whether the runtime condition evaluates as expected.
            // s_DwmFlushPathReached increments on every entry to the
            // #if USE_DWMFLUSH block (before the MatchMonitorRate check).
            // If it's >0 but warmup=180, LoadLibrary failed. If it's 0,
            // the #if block isn't compiling (impossible per rg) or
            // GL_DrawFrame isn't being called (impossible per diagT0).
            _ftprintf(f, _T("DwmFlushPath: reached=%ld, MatchMonitorRate=%d, ExclusiveFullscreen=%d, UsingOpenGL=%d, USE_DWMFLUSH=%d\n"),
                    (long)InterlockedExchangeAdd(&s_DwmFlushPathReached, 0),
                    MatchMonitorRate, ExclusiveFullscreen, UsingOpenGL, USE_DWMFLUSH);
        }
        // P46 (session 23): so a log can be identified as windowed /
        // borderless-fullscreen / exclusive-fullscreen at a glance, since
        // as of this session those first two now share the same DwmFlush
        // pacing path (see the P46 comment above the USE_DWMFLUSH guard in
        // GL_DrawFrame) and are expected to behave the same way, while
        // exclusive fullscreen deliberately does not use DwmFlush at all.
        _ftprintf(f, _T("Window mode: %s\n"),
                !Fullscreen ? _T("windowed") :
                (ExclusiveFullscreen ? _T("fullscreen (exclusive, DwmFlush not used)")
                                      : _T("fullscreen (borderless/DWM-composited, DwmFlush path applies)")));
        // P40 (session 17): with the P39 rollback, real stalls are rare, so
        // dumps (which only fire when a column exceeds DIAG_STALL_MS) are
        // now rare too -- but the user's remaining symptom is a periodic
        // scroll judder consistent with an uncorrected NES-vs-monitor Hz
        // drift (NES ~60.0988Hz native, this monitor reports 60Hz), which
        // MatchMonitorRate's DRC (APU::UpdateDRC, driven by
        // MonitorSync::GetMonitorHz()) exists specifically to correct via
        // a small audio pitch shift. Printing the live calibrated value on
        // every dump -- even ones triggered by something unrelated -- lets
        // us see across several dumps over a play session whether it is
        // converging toward the real display rate (good: DRC is working,
        // the remaining judder is a separate/residual issue) or sitting at
        // a suspicious round number like exactly 60.0 (bad: calibration
        // never ran or never moved, so DRC is not correcting anything).
        _ftprintf(f, _T("Monitor refresh Hz: %.6f | MMR target Hz: %.6f | Frame cadence Hz: %.6f | NES native Hz: %.6f\n"),
                MonitorSync::GetMonitorHz(), MonitorSync::GetTargetHz(), MonitorSync::GetFrameHz(), MonitorSync::GetNESHz());
        _ftprintf(f, _T("Presentation clock: %s | Presentation Hz: %.6f | Last interval error: %.3f ms\n"),
                MonitorSync::HasPresentationClock() ? _T("LOCKED") : _T("QPC FALLBACK"),
                MonitorSync::GetPresentationHz(), MonitorSync::GetPresentationIntervalErrorMs());
        _ftprintf(f, _T("FrameQueue counters: overflow_drop=%ld, latest_wins_skip=%ld\n"),
                (long)InterlockedExchangeAdd(&s_FQOverflowDrops, 0),
                (long)InterlockedExchangeAdd(&s_FQSkippedFrames, 0));
        _ftprintf(f, _T("PBO streaming: ready=%d count=%d\n"),
                s_PBOReady ? 1 : 0, PBO_COUNT);
        _ftprintf(f, _T("Audio MMR state: mode=deterministic workerPolls=%ld setFreq=%ld playStarts=%ld playPending=%ld primeSlots=%ld currentFreq=%ld safetyWaits=%ld notifyActive=0 notifySignals=0 playSlot=0 notifyPeriodUs=0\n"),
                APU::GetAudioWorkerPolls(),
                APU::GetAudioSetFreqCalls(),
                APU::GetAudioPlayStarts(),
                APU::GetAudioPlayPending(),
                APU::GetAudioPrimeSlots(),
                APU::GetAudioCurrentFreq(),
                APU::GetAudioSafetyWaits());
        _ftprintf(f, _T("Columns: frame | emuFrame | prod->consume | paceErr | paceSrc | paceEnter | paceWait | pace->produce | postPaceCPU | postPaceWall | postPaceCycles | buildWall | buildCPU | buildCycles | swapCPU | swapCycles | pboOrphan | pboMap | pboCopy | pboUnmap | pboSubmit | safetyMs | safetyLoops | traceSeq | prodGap | renderGap | consume->present | presentInterval | presentErr | fqP2C | fqPcs | fqCcs | fqSched | fqCS2 | fqPHold | fqCHold | fqSigWait | render2t0 | submit2dwm | dwmDispInt | dwmFrameStep | dwmMissStep | dwmDropStep | dwmLateStep | dwmLate | dwmSrc | dwmHr | dwmFrame | dwmRefresh | dwmVBlankInt | dwmComposeInt | dwmLateCount | dwmOutstanding | dwmUnique | dwmAvail | dwmMiss | dwmDrop | fqSkip/fqDepth | tex | swap | t2->t2b | ofe | drc | total\n\n"));

        // P43 (session 20): t0->t4 only spans GL_DrawFrame+OnFrameEnd+
        // UpdateDRC -- the video-draw slice of a frame. It does NOT cover
        // CPU::ExecOp/PPU rendering/APU::Run/PaceFrame, which all run on
        // the same NES thread BEFORE GL_DrawFrame is even called for the
        // next frame. Session 19's log showed "tot" consistently well
        // under one real vblank period (~9.7-12ms vs. an expected ~16.67ms
        // at this monitor's confirmed 59.9986Hz) -- that is NOT a bug by
        // itself: if CPU/PPU/APU work already consumed several ms of the
        // vblank budget before GL_DrawFrame was reached, SwapBuffers only
        // needs to wait out the REMAINDER, so "tot" reading under 16.67ms
        // is expected and fine on its own. What "tot" can never show is
        // the one thing that actually matters for a visible stutter: the
        // TRUE frame-to-frame period, including that unmeasured CPU/PPU/
        // APU time. This new "gap" column is exactly that -- the distance
        // between this frame's t0 and the PREVIOUS logged frame's t0 --
        // and is where a real dropped/duplicated video frame (the kind
        // that would actually look like a scroll stutter) will show up:
        // a gap far from ~16.67ms, regardless of how the t0->t4 slice
        // inside it happens to be split.
        LONGLONG prevT0 = 0;
        LONGLONG prevPresentation = 0;
        LONGLONG prevTProd = 0;
        LONGLONG prevDwmDisplayed = 0;
        LONGLONG prevDwmVBlank = 0;
        LONGLONG prevDwmCompose = 0;
        ULONGLONG prevDwmFrameDisplayed = 0;
        ULONGLONG prevDwmMiss = 0;
        ULONGLONG prevDwmDrop = 0;
        ULONGLONG prevDwmLate = 0;
        bool     havePrevT0 = false;
        bool     havePrevPresentation = false;
        bool     havePrevTProd = false;
        bool     havePrevDwm = false;

        // Walk the circular buffer from oldest to newest
        for (int i = 0; i < DIAG_FRAMES; i++)
        {
                int idx = (head + i) % DIAG_FRAMES;
                const FrameTimingEntry &e = buf[idx];
                if (e.frameNum == 0 || e.t4 == 0) continue;

                double d01 = (e.t1 > e.t0 && e.t0 > 0) ?
                             (e.t1  - e.t0)  * 1000.0 / freq : 0.0;
                double d12 = (e.t2 > e.t1 && e.t1 > 0) ?
                             (e.t2  - e.t1)  * 1000.0 / freq : 0.0;
                double d2b = (e.t2b > e.t2 && e.t2 > 0) ?
                             (e.t2b - e.t2)  * 1000.0 / freq : 0.0;
                double d23 = (e.t3 > e.t2b && e.t2b > 0) ?
                             (e.t3  - e.t2b) * 1000.0 / freq : 0.0;
                double d34 = (e.t4 > e.t3 && e.t3 > 0) ?
                             (e.t4  - e.t3)  * 1000.0 / freq : 0.0;
                double dtot= (e.t4 > e.t0 && e.t0 > 0) ?
                             (e.t4  - e.t0)  * 1000.0 / freq : 0.0;
                double pboOrphanMs = (e.pboOrphanEndQPC > e.pboSetupEndQPC && e.pboSetupEndQPC > 0) ?
                                    (e.pboOrphanEndQPC - e.pboSetupEndQPC) * 1000.0 / freq : 0.0;
                double pboMapMs = (e.pboMapEndQPC > e.pboOrphanEndQPC && e.pboOrphanEndQPC > 0) ?
                                  (e.pboMapEndQPC - e.pboOrphanEndQPC) * 1000.0 / freq : 0.0;
                double pboCopyMs = (e.pboCopyEndQPC > e.pboMapEndQPC && e.pboMapEndQPC > 0) ?
                                   (e.pboCopyEndQPC - e.pboMapEndQPC) * 1000.0 / freq : 0.0;
                double pboUnmapMs = (e.pboUnmapEndQPC > e.pboCopyEndQPC && e.pboCopyEndQPC > 0) ?
                                    (e.pboUnmapEndQPC - e.pboCopyEndQPC) * 1000.0 / freq : 0.0;
                double pboSubmitMs = (e.pboSubmitEndQPC > e.pboUnmapEndQPC && e.pboUnmapEndQPC > 0) ?
                                     (e.pboSubmitEndQPC - e.pboUnmapEndQPC) * 1000.0 / freq :
                                     ((e.pboSubmitEndQPC > e.pboSetupEndQPC && e.pboSetupEndQPC > 0) ?
                                      (e.pboSubmitEndQPC - e.pboSetupEndQPC) * 1000.0 / freq : 0.0);
                double dprod = (e.tProd > 0 && e.t0 >= e.tProd) ?
                               (e.t0 - e.tProd) * 1000.0 / freq : 0.0;
                double dpace2prod = (e.paceWake > 0 && e.tProd >= e.paceWake) ?
                                    (e.tProd - e.paceWake) * 1000.0 / freq : 0.0;
                double paceEnterMs = (e.mmrPaceEnterQPC > 0 && e.mmrRunEnterQPC > 0 && e.mmrPaceEnterQPC >= e.mmrRunEnterQPC) ?
                                     (e.mmrPaceEnterQPC - e.mmrRunEnterQPC) * 1000.0 / freq : 0.0;
                double paceWaitMs = (e.mmrPaceWakeQPC > 0 && e.mmrPaceEnterQPC > 0 && e.mmrPaceWakeQPC >= e.mmrPaceEnterQPC) ?
                                    (e.mmrPaceWakeQPC - e.mmrPaceEnterQPC) * 1000.0 / freq : 0.0;
                double postPaceWallMs = (e.tProd > 0 && e.mmrPaceWakeQPC > 0 && e.tProd >= e.mmrPaceWakeQPC) ?
                                        (e.tProd - e.mmrPaceWakeQPC) * 1000.0 / freq : 0.0;
                double postPaceCpuMs = (e.prodCpuEnd100ns > 0 && e.mmrPaceCpuWake100ns > 0 && e.prodCpuEnd100ns >= e.mmrPaceCpuWake100ns) ?
                                       (e.prodCpuEnd100ns - e.mmrPaceCpuWake100ns) / 10000.0 : 0.0;
                ULONGLONG postPaceCycles = (e.prodCpuEndCycles > 0 && e.mmrPaceCpuWakeCycles > 0 && e.prodCpuEndCycles >= e.mmrPaceCpuWakeCycles) ?
                                             (e.prodCpuEndCycles - e.mmrPaceCpuWakeCycles) : 0;
                double postPaceDeschedMs = postPaceWallMs > postPaceCpuMs ? (postPaceWallMs - postPaceCpuMs) : 0.0;
                double buildWallMs = (e.buildEndQPC > e.buildStartQPC && e.buildStartQPC > 0) ?
                                     (e.buildEndQPC - e.buildStartQPC) * 1000.0 / freq : 0.0;
                double buildCpuMs = (e.buildEndCPU100ns >= e.buildStartCPU100ns && e.buildStartCPU100ns > 0) ?
                                    (e.buildEndCPU100ns - e.buildStartCPU100ns) / 10000.0 : 0.0;
                ULONGLONG buildCycles = (e.buildEndCycles > e.buildStartCycles) ?
                                        (e.buildEndCycles - e.buildStartCycles) : 0;
                double swapCpuMs = (e.swapEndCPU100ns >= e.swapStartCPU100ns && e.swapStartCPU100ns > 0) ?
                                    (e.swapEndCPU100ns - e.swapStartCPU100ns) / 10000.0 : 0.0;
                ULONGLONG swapCycles = (e.swapEndCycles > e.swapStartCycles) ?
                                       (e.swapEndCycles - e.swapStartCycles) : 0;
                double safetyMs = (e.mmrSafetyEndQPC > 0 && e.mmrSafetyBeginQPC > 0 && e.mmrSafetyEndQPC >= e.mmrSafetyBeginQPC) ?
                                  (e.mmrSafetyEndQPC - e.mmrSafetyBeginQPC) * 1000.0 / freq : 0.0;
                double dprodGap = (havePrevTProd && e.tProd > prevTProd) ?
                                  (e.tProd - prevTProd) * 1000.0 / freq : 0.0;
                if (e.tProd > 0) { prevTProd = e.tProd; havePrevTProd = true; }
                // P84: "present" must use the observed presentation boundary,
                // not merely SwapBuffers submit time. DwmFlush supplies t2b
                // in the DWM path; other paths fall back to t2.
                LONGLONG presentationStamp = (e.t2b > 0) ? e.t2b : e.t2;
                double dpresent = (havePrevPresentation &&
                                    presentationStamp > prevPresentation) ?
                                   (presentationStamp - prevPresentation) * 1000.0 / freq : 0.0;

                bool   haveGap = havePrevT0;
                double dgap = haveGap ? (e.t0 - prevT0) * 1000.0 / freq : 0.0;
                prevT0 = e.t0;
                prevPresentation = presentationStamp;
                havePrevT0 = true;
                havePrevPresentation = (presentationStamp > 0);
                double centerMs = (MonitorSync::GetTargetHz() > 1.0) ?
                                  1000.0 / MonitorSync::GetTargetHz() : 16.667;
                double presentErr = havePrevPresentation && dpresent > 0.0 ?
                                    (dpresent - centerMs) : 0.0;
                // P67: this isolates timer/scheduler wake-up error from the
                // later render/presentation stages. Positive values mean the
                // emulation pacing wake-up happened after its target.
                double paceErr = (e.paceTarget > 0 && e.paceWake > 0) ?
                                 (e.paceWake - e.paceTarget) * 1000.0 / freq : 0.0;
                double fqP2C = (e.fqWaitReturnQPC > 0 && e.fqProduceQPC > 0) ? (e.fqWaitReturnQPC - e.fqProduceQPC) * 1000.0 / freq : 0.0;
                double fqPcs = (e.fqProduceCsEnterQPC > 0 && e.fqProduceQPC > 0) ? (e.fqProduceCsEnterQPC - e.fqProduceQPC) * 1000.0 / freq : 0.0;
                double fqCcs = (e.fqConsumeCsEnterQPC > 0 && e.fqConsumeBeginQPC > 0) ? (e.fqConsumeCsEnterQPC - e.fqConsumeBeginQPC) * 1000.0 / freq : 0.0;
                double fqSched = (e.fqConsumeBeginQPC > 0 && e.fqWaitReturnQPC > 0) ? (e.fqConsumeBeginQPC - e.fqWaitReturnQPC) * 1000.0 / freq : 0.0;
                double fqCS2 = (e.fqConsumeEndQPC > 0 && e.fqConsumeCsLeaveQPC > 0) ? (e.fqConsumeEndQPC - e.fqConsumeCsLeaveQPC) * 1000.0 / freq : 0.0;
                double fqPHold = (e.fqProduceCsLeaveQPC > 0 && e.fqProduceCsEnterQPC > 0) ? (e.fqProduceCsLeaveQPC - e.fqProduceCsEnterQPC) * 1000.0 / freq : 0.0;
                double fqCHold = (e.fqConsumeCsLeaveQPC > 0 && e.fqConsumeCsEnterQPC > 0) ? (e.fqConsumeCsLeaveQPC - e.fqConsumeCsEnterQPC) * 1000.0 / freq : 0.0;
                double fqSigWait = (e.fqWaitReturnQPC > 0 && e.fqSignalQPC > 0) ? (e.fqWaitReturnQPC - e.fqSignalQPC) * 1000.0 / freq : 0.0;
                double render2t0 = (e.fqConsumeEndQPC > 0 && e.t0 > 0) ? (e.t0 - e.fqConsumeEndQPC) * 1000.0 / freq : 0.0;
                double submit2dwm = (e.dwmDisplayed > 0 && e.t2 > 0) ? (e.dwmDisplayed - e.t2) * 1000.0 / freq : 0.0;
                double dwmDispInt = (e.dwmValid && havePrevDwm && e.dwmDisplayed > prevDwmDisplayed) ?
                                    (e.dwmDisplayed - prevDwmDisplayed) * 1000.0 / freq : 0.0;
                double dwmVBlankInt = (e.dwmValid && havePrevDwm && e.dwmVBlank > prevDwmVBlank) ?
                                    (e.dwmVBlank - prevDwmVBlank) * 1000.0 / freq : 0.0;
                double dwmComposeInt = (e.dwmValid && havePrevDwm && e.dwmCompose > prevDwmCompose) ?
                                    (e.dwmCompose - prevDwmCompose) * 1000.0 / freq : 0.0;
                long long dwmFrameStep = (e.dwmValid && havePrevDwm) ?
                                    (long long)e.dwmFrameDisplayed - (long long)prevDwmFrameDisplayed : 0;
                long long dwmMissStep = (e.dwmValid && havePrevDwm) ?
                                    (long long)e.dwmFramesMissed - (long long)prevDwmMiss : 0;
                long long dwmDropStep = (e.dwmValid && havePrevDwm) ?
                                    (long long)e.dwmFramesDropped - (long long)prevDwmDrop : 0;
                long long dwmLateStep = (e.dwmValid && havePrevDwm) ?
                                    (long long)e.dwmFramesLate - (long long)prevDwmLate : 0;
                bool dwmLate = (e.dwmValid && havePrevDwm && e.dwmFrameDisplayed == prevDwmFrameDisplayed);
                if (e.dwmValid)
                {
                        prevDwmDisplayed = e.dwmDisplayed;
                        prevDwmVBlank = e.dwmVBlank;
                        prevDwmCompose = e.dwmCompose;
                        prevDwmFrameDisplayed = e.dwmFrameDisplayed;
                        prevDwmMiss = e.dwmFramesMissed;
                        prevDwmDrop = e.dwmFramesDropped;
                        prevDwmLate = e.dwmFramesLate;
                        havePrevDwm = true;
                }

                // A real dropped/duplicated frame shows up as a gap far
                // from one vblank period (~16.67ms at 60Hz) in EITHER
                // direction -- flag anything more than 8ms off center,
                // not just "too slow": a too-SHORT gap means two video
                // frames got drawn within one real vblank (one of them
                // presumably invisible), which is just as much a visible
                // stutter as a dropped one.
                // Mark stalled stages with '*'. renderGap remains useful for
                // spotting queue/presentation skips, while presentInterval and
                // presentErr expose the phase variation that motivated P60.
                bool gapStalled = haveGap && (fabs(dgap - centerMs) > 8.0);
                bool presentStalled = (dpresent > 0.0 && fabs(presentErr) > 2.0);

                _ftprintf(f,
                        _T("F%06u  emu=%-6I64u prod2cons=%6.2f  paceErr=%+6.2f  paceSrc=%d  paceEnter=%6.2f  paceWait=%6.2f  pace->prod=%6.2f  postPaceCPU=%6.2f  postPaceWall=%6.2f  postPaceCycles=%I64u  buildWall=%6.2f  buildCPU=%6.2f  buildCycles=%I64u  swapCPU=%6.2f  swapCycles=%I64u  pboOrphan=%6.3f  pboMap=%6.3f  pboCopy=%6.3f  pboUnmap=%6.3f  pboSubmit=%6.3f  safetyMs=%6.2f  safetyLoops=%d  traceSeq=%I64d  prodGap=%7.2f  renderGap=%7.2f%s  cons2pres=%6.2f  present=%7.2f%s  err=%+6.2f  fqP2C=%6.2f  fqPcs=%5.2f  fqCcs=%5.2f  fqSched=%6.2f  fqCS2=%5.2f  fqPHold=%5.2f  fqCHold=%5.2f  fqSigWait=%6.2f  render2t0=%6.2f  submit2dwm=%7.2f  dwmDisp=%7.2f  dwmFrameStep=%2lld  dwmMissStep=%2lld  dwmDropStep=%2lld  dwmLateStep=%2lld  dwmLate=%d  dwmSrc=%d  dwmHr=0x%08lX  dwmFrame=%I64u  dwmRefresh=%I64u  dwmVBlankInt=%7.2f  dwmComposeInt=%7.2f  dwmLateCount=%I64u  dwmOutstanding=%I64u  dwmUnique=%I64u  dwmAvail=%I64u  dwmMiss=%I64u  dwmDrop=%I64u  fq=%d/%d  tex=%5.2f%s  swap=%6.2f%s  t2b=%5.2f%s  ofe=%5.2f%s  drc=%5.2f%s  tot=%6.2f%s\n"),
                        e.frameNum,
                        (unsigned __int64)e.emuFrame,
                        dprod,
                        paceErr,
                        (int)e.paceSource,
                        paceEnterMs,
                        paceWaitMs,
                        dpace2prod,
                        postPaceCpuMs,
                        postPaceWallMs,
                        postPaceCycles,
                        buildWallMs,
                        buildCpuMs,
                        buildCycles,
                        swapCpuMs,
                        swapCycles,
                        pboOrphanMs,
                        pboMapMs,
                        pboCopyMs,
                        pboUnmapMs,
                        pboSubmitMs,
                        safetyMs,
                        (int)e.mmrSafetyLoops,
                        (long long)e.mmrTraceSeq,
                        dprodGap,
                        dgap, (gapStalled ? _T("*") : _T(" ")),
                        (e.t0 > 0 && e.t2 >= e.t0) ? (e.t2 - e.t0) * 1000.0 / freq : 0.0,
                        dpresent, (presentStalled ? _T("*") : _T(" ")),
                        presentErr,
                        fqP2C,
                        fqPcs,
                        fqCcs,
                        fqSched,
                        fqCS2,
                        fqPHold,
                        fqCHold,
                        fqSigWait,
                        render2t0,
                        submit2dwm,
                        dwmDispInt,
                        dwmFrameStep,
                        dwmMissStep,
                        dwmDropStep,
                        dwmLateStep,
                        dwmLate ? 1 : 0,
                        (int)e.dwmSource,
                        (unsigned long)(e.dwmHr),
                        (unsigned __int64)(e.dwmValid ? e.dwmFrame : 0),
                        (unsigned __int64)(e.dwmValid ? e.dwmRefresh : 0),
                        dwmVBlankInt,
                        dwmComposeInt,
                        (unsigned __int64)(e.dwmValid ? e.dwmFramesLate : 0),
                        (unsigned __int64)(e.dwmValid ? e.dwmFramesOutstanding : 0),
                        (unsigned __int64)(e.dwmValid ? e.dwmFramesDisplayed : 0),
                        (unsigned __int64)(e.dwmValid ? e.dwmFramesAvailable : 0),
                        (unsigned __int64)(e.dwmValid ? e.dwmFramesMissed : 0),
                        (unsigned __int64)(e.dwmValid ? e.dwmFramesDropped : 0),
                        (int)e.fqSkipped, (int)e.fqDepth,
                        d01, (d01 > DIAG_STALL_MS ? _T("*") : _T(" ")),
                        d12, (d12 > DIAG_STALL_MS ? _T("*") : _T(" ")),
                        d2b, (d2b > DIAG_STALL_MS ? _T("*") : _T(" ")),
                        d23, (d23 > DIAG_STALL_MS ? _T("*") : _T(" ")),
                        d34, (d34 > DIAG_STALL_MS ? _T("*") : _T(" ")),
                        dtot, (dtot > DIAG_STALL_MS * 1.5 ? _T("*") : _T(" "))
                );
        }
        fclose(f);
}

// Kept for the (extremely unlikely) case QueueUserWorkItem submission
// fails -- see DiagDumpLogAsync. Operates directly on the live buffer,
// exactly as before P31.
static void DiagDumpLog()
{
        DiagWriteLogFile(s_diagBuf, s_diagHead);
}

// P31: snapshot payload handed to the thread pool. Freed by the worker.
struct DiagSnapshot
{
        FrameTimingEntry buf[DIAG_FRAMES];
        int              head;
};

static volatile LONG  s_diagDumping     = 0;   // 1 while an async dump is in flight
static LONGLONG       s_diagLastDumpQPC = 0;   // NES-thread-only, no lock needed

static DWORD CALLBACK DiagDumpThreadProc(LPVOID param)
{
        DiagSnapshot *snap = (DiagSnapshot*)param;
        DiagWriteLogFile(snap->buf, snap->head);
        free(snap);
        InterlockedExchange(&s_diagDumping, 0L);
        return 0;
}

// P31: replacement for the direct DiagDumpLog() call on the hot path.
// Snapshots the ring buffer (cheap memcpy, no I/O, stays on the NES
// thread) and hands the snapshot to the Windows thread pool to write to
// disk -- the NES thread never touches the filesystem. Rate-limited to
// one dump per 3 seconds so a multi-frame stall cascade produces one
// file instead of several back-to-back asynchronous writes competing
// with each other.
static void DiagDumpLogAsync()
{
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double freq = DiagQPCFreq();
        if (s_diagLastDumpQPC != 0 && freq > 0.0 &&
            (double)(now.QuadPart - s_diagLastDumpQPC) / freq < 3.0)
                return;

        if (InterlockedCompareExchange(&s_diagDumping, 1L, 0L) != 0)
                return; // a previous dump is still writing

        s_diagLastDumpQPC = now.QuadPart;

        // Plain malloc, not new: DiagSnapshot is POD (array + int), and this
        // keeps us out of C++ exception territory (this codebase is built
        // C-Win32-style throughout; malloc.h is already pulled in via
        // StdAfx.h). NULL on failure is handled explicitly below.
        DiagSnapshot *snap = (DiagSnapshot*)malloc(sizeof(DiagSnapshot));
        if (!snap)
        {
                InterlockedExchange(&s_diagDumping, 0L);
                return;
        }
        memcpy(snap->buf, s_diagBuf, sizeof(s_diagBuf));
        snap->head = s_diagHead;

        if (!QueueUserWorkItem(DiagDumpThreadProc, snap, WT_EXECUTELONGFUNCTION))
        {
                // Thread pool submission failed (extremely rare). Rather than
                // silently losing the diagnostic, fall back to the old
                // synchronous write -- a guaranteed rare stall beats no
                // dump at all when actively debugging a glitch.
                free(snap);
                InterlockedExchange(&s_diagDumping, 0L);
                DiagDumpLog();
        }
}

// Called from DrawScreen to complete the timing entry for this frame.
// t3 and t4 are filled here (after SwapBuffers returns) and a stall check
// is performed.
static void DiagCompleteFrame(LONGLONG t3, LONGLONG t4)
{
        if (!MatchMonitorRate) return;

        // Find the entry we opened in GL_DrawFrame for this frame number.
        // It's always at (s_diagHead - 1 + DIAG_FRAMES) % DIAG_FRAMES.
        int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
        s_diagBuf[idx].t3 = t3;
        s_diagBuf[idx].t4 = t4;

        // Stall check: SwapBuffers duration > DIAG_STALL_MS
        double freq = DiagQPCFreq();
        double swapMs = (s_diagBuf[idx].t2  - s_diagBuf[idx].t1)  * 1000.0 / freq;
        double texMs  = (s_diagBuf[idx].t1  - s_diagBuf[idx].t0)  * 1000.0 / freq;
        double mcrMs  = (s_diagBuf[idx].t2b - s_diagBuf[idx].t2)  * 1000.0 / freq;
        double ofeMs  = (s_diagBuf[idx].t3  - s_diagBuf[idx].t2b) * 1000.0 / freq;
        double drcMs  = (s_diagBuf[idx].t4  - s_diagBuf[idx].t3)  * 1000.0 / freq;

        // P35: ofeMs (OnFrameEnd duration) was computed for display in the
        // log columns but never included in the trigger condition below.
        // A frame where ONLY OnFrameEnd stalls (swap/tex/drc all fine) would
        // never fire a dump at all -- it would only show up in the log if
        // some other, unrelated checkpoint on the same frame also happened
        // to exceed the threshold. That is exactly what happened in the one
        // log we did get: frame 2 shows ofe=202.92ms, and the dump only
        // fired because texMs (21.56ms, first-frame texture/shader warmup)
        // separately tripped the tex>DIAG_STALL_MS branch. Any OnFrameEnd
        // stall during normal play, with tex/swap/drc all under threshold,
        // would previously have been invisible to this logger.
        //
        // P38 (session 15): sessions 13/14's logs kept showing a ~90-100ms
        // stall attributed to "ofe" even though OnFrameEnd() itself is
        // pure QueryPerformanceCounter arithmetic with zero blocking calls
        // -- and the same pattern reproduced in plain windowed mode with no
        // fullscreen transition involved at all, which rules out both P36
        // (fullscreen WaitForDXGIVBlank gating) and P37 (stale DXGI output
        // after ChangeDisplaySettingsEx) as the cause. The one call that
        // WAS silently folded into the old t2->t3 "ofe" bucket without ever
        // being measured on its own is wglMakeCurrent(NULL, NULL) right
        // after SwapBuffers -- some GL drivers defer the actual present
        // wait from SwapBuffers to the NEXT call that touches the context,
        // which would land exactly here. t2b isolates that call so the
        // next log tells us definitively whether it's the culprit (mcr
        // stalls, ofe stays near zero) or whether the real cause is
        // something else entirely, e.g. generic thread starvation (both
        // mcr AND ofe would show elevated, near-random splits between them
        // from frame to frame, since starvation can land the descheduling
        // point anywhere).
        // P42 (session 19): the stall-triggered dump above is exactly the
        // wrong tool for watching P41's calibration converge -- once P39
        // fixed the main slowdown, real stalls became rare, so the log
        // file just sits forever on whichever dump fired first (almost
        // always the frame-2 startup texture/shader warmup, at which
        // point calibration hasn't completed even one 60-frame window
        // yet, let alone the 3 windows P41 needs to snap). Every log the
        // user has sent since P39 has been this same stale frame-2 dump,
        // not a "calibration isn't converging" result. Force one dump
        // periodically as well, purely to keep the "Live calibrated
        // monitor Hz" header line current -- DiagDumpLogAsync's existing
        // 3-second cooldown keeps this cheap.
        static LONGLONG s_diagLastPeriodicQPC = 0;
        LONGLONG nowQPC = s_diagBuf[idx].t4;
        bool periodicDue = (s_diagLastPeriodicQPC == 0) ||
                ((double)(nowQPC - s_diagLastPeriodicQPC) / freq >= 10.0);

        if (swapMs > DIAG_STALL_MS || texMs > DIAG_STALL_MS || mcrMs > DIAG_STALL_MS || ofeMs > DIAG_STALL_MS || drcMs > DIAG_STALL_MS || periodicDue)
        {
                if (periodicDue)
                        s_diagLastPeriodicQPC = nowQPC;
                DiagDumpLogAsync();
        }
}

static bool DiagQueryDwmTiming(FrameTimingEntry &e)
{
        if (!MatchMonitorRate || !UsingOpenGL) return false;
        if (s_pfnDwmGetCompositionTimingInfo == reinterpret_cast<PFN_DwmGetCompositionTimingInfo>(1))
        {
                HMODULE hDwm = LoadLibrary(_T("dwmapi.dll"));
                s_pfnDwmGetCompositionTimingInfo = hDwm ? (PFN_DwmGetCompositionTimingInfo)GetProcAddress(hDwm, "DwmGetCompositionTimingInfo") : NULL;
        }
        if (!s_pfnDwmGetCompositionTimingInfo) return false;

        // P70: Windows 7/8-era DWM expects a real HWND for this query,
        // while Windows 8.1+ requires NULL. Try the actual NES window first
        // (the important path for the legacy systems this project supports),
        // then fall back to NULL for newer DWM implementations. This is
        // diagnostics-only and never affects rendering or pacing.
        DWM_TIMING_INFO ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        HRESULT hr = E_FAIL;
        LONG source = -1;

        if (hMainWnd)
        {
                hr = s_pfnDwmGetCompositionTimingInfo(hMainWnd, &ti);
                if (SUCCEEDED(hr)) source = 1;
        }
        if (FAILED(hr))
        {
                ZeroMemory(&ti, sizeof(ti));
                ti.cbSize = sizeof(ti);
                hr = s_pfnDwmGetCompositionTimingInfo(NULL, &ti);
                if (SUCCEEDED(hr)) source = 0;
        }

        e.dwmSource = source;
        e.dwmHr = (LONG)hr;
        if (FAILED(hr))
                return false;

        e.dwmDisplayed = (LONGLONG)ti.qpcFrameDisplayed;
        e.dwmVBlank = (LONGLONG)ti.qpcVBlank;
        e.dwmRefreshPeriod = (LONGLONG)ti.qpcRefreshPeriod;
        e.dwmCompose = (LONGLONG)ti.qpcCompose;
        e.dwmRefresh = (ULONGLONG)ti.cRefresh;
        e.dwmFrame = (ULONGLONG)ti.cFrame;
        e.dwmFrameDisplayed = (ULONGLONG)ti.cFrameDisplayed;
        e.dwmFramesLate = (ULONGLONG)ti.cFramesLate;
        e.dwmFramesOutstanding = (ULONGLONG)ti.cFramesOutstanding;
        e.dwmFramesDisplayed = (ULONGLONG)ti.cFramesDisplayed;
        e.dwmFramesAvailable = (ULONGLONG)ti.cFramesAvailable;
        e.dwmFramesMissed = (ULONGLONG)ti.cFramesMissed;
        e.dwmFramesDropped = (ULONGLONG)ti.cFramesDropped;
        e.dwmValid = 1;

        // P85: qpcCompose is still a sampled snapshot, so it is not safe to
        // treat every call as a new presentation. GL_DrawFrame uses the DWM
        // cFrame id together with qpcCompose; MonitorSync rejects duplicate
        // snapshots and only locks after three consecutive one-frame samples.
        // qpcFrameDisplayed remains diagnostic-only because it can legitimately
        // repeat when the sampled DWM state is unchanged.
        return true;
}

static void GL_DrawFrame(void)
{
        static uint32_t frameBuf[256 * 240];

        // Diagnostic: record frame entry time.
        LONGLONG diagT0 = 0, diagT1 = 0;
        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); diagT0 = qpc.QuadPart;
                s_diagFrameNum++;
                int idx = s_diagHead;
                s_diagBuf[idx].t0       = diagT0;
                s_diagBuf[idx].t1       = 0;
                s_diagBuf[idx].t2       = 0;
                s_diagBuf[idx].t2b      = 0;
                s_diagBuf[idx].t3       = 0;
                s_diagBuf[idx].t4       = 0;
                s_diagBuf[idx].paceTarget = 0;
                s_diagBuf[idx].paceWake   = 0;
                s_diagBuf[idx].paceSource = 0;
                s_diagBuf[idx].dwmDisplayed = 0;
                s_diagBuf[idx].dwmVBlank = 0;
                s_diagBuf[idx].dwmRefreshPeriod = 0;
                s_diagBuf[idx].dwmCompose = 0;
                s_diagBuf[idx].dwmRefresh = 0;
                s_diagBuf[idx].dwmFrame = 0;
                s_diagBuf[idx].dwmFrameDisplayed = 0;
                s_diagBuf[idx].dwmFramesLate = 0;
                s_diagBuf[idx].dwmFramesOutstanding = 0;
                s_diagBuf[idx].dwmFramesDisplayed = 0;
                s_diagBuf[idx].dwmFramesAvailable = 0;
                s_diagBuf[idx].dwmFramesMissed = 0;
                s_diagBuf[idx].dwmFramesDropped = 0;
                s_diagBuf[idx].dwmValid = 0;
                s_diagBuf[idx].dwmSource = -1;
                s_diagBuf[idx].dwmHr = 0;
                s_diagBuf[idx].pboSetupEndQPC = 0;
                s_diagBuf[idx].pboOrphanEndQPC = 0;
                s_diagBuf[idx].pboMapEndQPC = 0;
                s_diagBuf[idx].pboCopyEndQPC = 0;
                s_diagBuf[idx].pboUnmapEndQPC = 0;
                s_diagBuf[idx].pboSubmitEndQPC = 0;
                s_diagBuf[idx].frameNum = s_diagFrameNum;
                s_diagHead = (s_diagHead + 1) % DIAG_FRAMES;
        }

        unsigned short *src = PPU::DrawArray;
        for (int i = 0; i < 256 * 240; i++)
                frameBuf[i] = Palette32[src[i]];

        // P44 (session 21): no per-frame wglMakeCurrent(hGLDC, hGLRC) here
        // anymore -- AcquireGLContext() bound it once for the whole session
        // (see NES::Thread()). The context is already current on this thread.

        // Apply any pending vsync interval change posted by MonitorSync::Enable()
        // from the UI thread. Must happen here, after wglMakeCurrent and before
        // any GL draw calls, so the context is current and owned by this thread.
        MonitorSync::ApplyPendingVSync();

        // Apply any pending viewport resize posted by WM_SIZE from the UI thread.
        // GL_Resize cannot be called directly from WM_SIZE while NES is running
        // because it would call wglMakeCurrent and race with this draw sequence.
        ApplyPendingResize();

        // Apply any pending bilinear filter change posted by ApplyGLFilter()
        // from the UI thread. Must be here (context current, before draw calls).
        {
                LONG pendingBilinear = InterlockedExchange(&g_PendingBilinear, -1L);
                if (pendingBilinear >= 0)
                        Bilinear = (pendingBilinear != 0) ? TRUE : FALSE;
        }

        glViewport(0, 0, glWinW, glWinH);

        glBindTexture(GL_TEXTURE_2D, glTex);

        if (s_PBOReady)
        {
                // -------------------------------------------------------
                // ASYNC PBO PATH (P29/P83).
                //
                // P83 uses four streaming PBOs instead of two. With the
                // windowed path's weak SwapBuffers backpressure, two buffers
                // were not enough to guarantee that the buffer selected on
                // this frame was no longer in flight on the GPU.
                //
                // The PBO still decouples CPU frame construction from texture
                // DMA; the larger pool gives the driver more room to retire
                // submitted transfers without forcing glMapBuffer() to
                // synchronize with an earlier frame.
                // -------------------------------------------------------
                GLuint writePBO = s_PBO[s_PBOIndex];
                pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, writePBO);

                // Orphan the buffer before mapping (glBufferData with NULL data).
                // This tells the driver to allocate a fresh memory region for
                // this mapping rather than waiting for the previous one to be
                // freed. It's the key technique that makes PBO streaming fast:
                // the driver can keep the old buffer in-flight for the GPU DMA
                // while giving us a brand-new region to write into immediately.
                pfn_glBufferData(GL_PIXEL_UNPACK_BUFFER, PBO_SIZE, NULL, GL_STREAM_DRAW);

                void* pboMem = pfn_glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
                if (pboMem)
                {
                        memcpy(pboMem, frameBuf, PBO_SIZE);
                        pfn_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                        // NULL offset = read from the currently bound PBO.
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240,
                                GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
                }
                else
                {
                        // Map failed (driver ran out of staging memory?).
                        // Fall back to synchronous path for this frame only.
                        pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240,
                                GL_BGRA_EXT, GL_UNSIGNED_BYTE, frameBuf);
                }

                pfn_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                // Advance to the other PBO for the next frame.
                s_PBOIndex = (s_PBOIndex + 1) % PBO_COUNT;
        }
        else
        {
                // Fallback: original synchronous path.
                // Used when PBO extension is unavailable (very old hardware
                // or software renderer). Functionally identical to pre-P29.
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240,
                        GL_BGRA_EXT, GL_UNSIGNED_BYTE, frameBuf);
        }

        // Diagnostic: record time after texture upload.
        // With PBO this measures the CPU-side memcpy + map/unmap overhead
        // (~0.1ms) rather than the GPU stall (~24ms peak). The stall has
        // been moved out of the critical path -- it now overlaps with the
        // previous frame's GPU work during the inter-frame period.
        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc); diagT1 = qpc.QuadPart;
                int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                s_diagBuf[idx].t1 = diagT1;
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int dstX, dstY, dstW, dstH;
        if (IntegerScale)
        {
                int scale = 1;
                for (int m = 2; m <= 16; m++)
                {
                        if (256 * m <= glWinW && 240 * m <= glWinH)
                                scale = m;
                        else
                                break;
                }

                dstW = 256 * scale;
                dstH = 240 * scale;
                dstX = (glWinW - dstW) / 2;
                dstY = (glWinH - dstH) / 2;

                ISMult    = scale;
                ISBorderX = dstX;
                ISBorderY = dstY;
        }
        else
        {
                int hw = 240 * glWinW;
                int wh = 256 * glWinH;

                if (hw > wh)
                {
                        dstW = wh / 240;
                        dstH = glWinH;
                }
                else
                {
                        dstW = glWinW;
                        dstH = hw / 256;
                }

                dstX = (glWinW - dstW) / 2;
                dstY = (glWinH - dstH) / 2;
        }

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, glTex);

        // Only call glTexParameteri when the filter setting actually changes.
        // These calls flush state to the driver even when the value is identical,
        // adding unnecessary overhead on every frame. The filter rarely changes
        // (only when the user toggles Bilinear in the menu), so caching the
        // last-applied value eliminates the per-frame driver round-trip.
        {
                static BOOL s_lastBilinear = -1;
                if (Bilinear != s_lastBilinear)
                {
                        s_lastBilinear = Bilinear;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                Bilinear ? GL_LINEAR : GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                Bilinear ? GL_LINEAR : GL_NEAREST);
                }
        }

        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

        glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f); glVertex2i(dstX,        dstY);
                glTexCoord2f(1.0f, 0.0f); glVertex2i(dstX + dstW, dstY);
                glTexCoord2f(1.0f, 1.0f); glVertex2i(dstX + dstW, dstY + dstH);
                glTexCoord2f(0.0f, 1.0f); glVertex2i(dstX,        dstY + dstH);
        glEnd();

        if (Scanlines)
        {
                glDisable(GL_TEXTURE_2D);

                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                glColor4f(0.0f, 0.0f, 0.0f, 0.25f);

                glBegin(GL_LINES);

                for (int y = dstY; y < dstY + dstH; y += 2)
                {
                        glVertex2i(dstX, y);
                        glVertex2i(dstX + dstW, y);
                }

                glEnd();

                glDisable(GL_BLEND);

                glEnable(GL_TEXTURE_2D);

                glColor4f(1, 1, 1, 1);
        }

        // Synchronise with DWM before presenting (windowed + MMR only).
        //
        // *** DWMFLUSH DISABLED BY DEFAULT — see USE_DWMFLUSH below ***
        //
        // HISTORY:
        // Previous versions called DwmFlush() just before SwapBuffers in
        // windowed mode (with GL swap interval=0) to synchronise the GL
        // present with the DWM composition tick. The pattern was:
        //   DwmFlush()  — blocks until DWM finishes its composition pass
        //   SwapBuffers(interval=0) — presents immediately into next cycle
        //
        // This worked MOST of the time, but DwmFlush can occasionally block
        // for 2+ vblank periods (~33 ms at 60 Hz) when DWM performs internal
        // maintenance:
        //   - periodic composition re-sync
        //   - DWM state refresh cycles (roughly every 20-60 seconds
        //     depending on Windows version and GPU driver)
        //   - GPU driver periodic events that DWM waits on
        //
        // Each such stall produces a 2-frame stutter on the NES thread,
        // which also drives audio (APU::Run is called from CPU::ExecOp).
        // The result is the EXACT reported symptom:
        //   - periodic (~30 sec) simultaneous video+audio dropout
        //   - "everything is perfectly smooth in between"
        //   - resistant to all fixes that targeted SetFrequency / DRC /
        //     thread priority / waitable timers, because the stall is
        //     in DwmFlush itself, not in any of those subsystems.
        //
        // This also explains the "3-second slowdown after exiting
        // fullscreen" problem: after exiting fullscreen, the warmup
        // counter (DWM_WARMUP_FRAMES) keeps DwmFlush off for a while,
        // but once warmup ends, SetDwmSyncMode(true) fires → interval=0
        // → DwmFlush+SwapBuffers(0) takes over. If DWM has not fully
        // stabilised yet (it can take 5-10 seconds on some systems),
        // every DwmFlush blocks for ~33 ms → effective 30 fps for
        // several seconds. Increasing DWM_WARMUP_FRAMES only shifts
        // the slowdown later; it does not eliminate it.
        //
        // FIX:
        // On Windows 10/11 with modern GPU drivers, DWM composition is
        // phase-locked to the monitor vblank, and OpenGL vsync
        // (SwapBuffers with interval=1) is also synced to the same
        // vblank. GL-vsync alone is therefore sufficient for smooth
        // windowed presentation. DwmFlush is redundant and only adds
        // stall risk.
        //
        // The GL swap interval is set by ReinitVSync()/SetDwmSyncMode(false)
        // to whatever g_DXGISwapInterval says: 1 (driver vsync) if the P28
        // DXGI bypass below is unavailable, or 0 (our own WaitForDXGIVBlank
        // call takes over pacing) if it is. SetDwmSyncMode(true) is never
        // called, so DwmFlush itself never runs. Either way SwapBuffers
        // ends up synced to the real vblank — and, as of P36, the same is
        // true in fullscreen mode (WaitForDXGIVBlank is no longer skipped
        // there). No DWM warmup is needed because there is no transition
        // to interval=0 caused BY DwmFlush specifically.
        //
        // TRADE-OFF:
        // On older drivers (Windows 7/8 with legacy GPU drivers), DWM
        // composition and GL vblank may not be perfectly phase-locked.
        // Without DwmFlush, this can occasionally cause a frame to be
        // displayed one composition cycle late (1-frame latency, NOT
        // doubling/skipping). This is less perceptible than the dropout
        // symptom it fixes. If frame doubling/skipping is observed on
        // a specific system, set USE_DWMFLUSH=1 to re-enable the old
        // DwmFlush path (with its warmup logic).
        //
        // P45 (session 22): that is exactly what session 21's log showed
        // in windowed mode -- see the USE_DWMFLUSH define near the top of
        // this file for the data and reasoning. USE_DWMFLUSH is now 1.
        //
        // P46 (session 23): the guard below used to be `!Fullscreen`, which
        // excludes BOTH fullscreen variants this codebase has:
        //   - Fullscreen=true, ExclusiveFullscreen=false: a WS_POPUP window
        //     sized to the screen (see ID_PPU_FULLSCREEN in Nintendulator.cpp)
        //     -- still just a normal window as far as DWM is concerned, still
        //     composited exactly like the windowed case P45 just fixed. There
        //     is no reason this mode would behave any differently from
        //     windowed mode with respect to wglSwapIntervalEXT(1) not
        //     providing real backpressure under DWM -- it was excluded from
        //     the P45 fix purely because the old guard tested the wrong flag.
        //   - Fullscreen=true, ExclusiveFullscreen=true: calls
        //     ChangeDisplaySettingsEx(..., CDS_FULLSCREEN, ...) (see
        //     ID_PPU_EXCLUSIVEFS), a real exclusive display-mode switch that
        //     bypasses DWM composition entirely. This is the one case where
        //     DwmFlush is actually pointless (nothing is compositing us) and
        //     could only add latency -- this is the case that should stay
        //     excluded.
        // Changed the condition to test ExclusiveFullscreen instead of
        // Fullscreen so borderless fullscreen gets the same fix as windowed
        // mode, while true exclusive fullscreen (the only mode where GL
        // vsync alone should already be correct) is left untouched.
#if USE_DWMFLUSH
        // P61: DwmFlush must run AFTER SwapBuffers in the interval=0 path.
        // Calling it before the swap synchronizes the previous composition
        // cycle, while the newly rendered frame is then submitted too late;
        // the resulting post-SwapBuffers timestamps were the source of P60's
        // 23ms/9ms phase alternation.
        InterlockedIncrement(&s_DwmFlushPathReached);
#endif

        if (MatchMonitorRate)
                MonitorSync::WaitForDXGIVBlank();

        SwapBuffers(hGLDC);

        // Diagnostic timestamp is overwritten with the post-DwmFlush
        // composition boundary below when that path is active.
        if (MatchMonitorRate)
        {
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                s_diagBuf[idx].t2 = qpc.QuadPart;
        }

#if USE_DWMFLUSH
        if (MatchMonitorRate && !(Fullscreen && ExclusiveFullscreen))
        {
                if (s_pfnDwmFlush == reinterpret_cast<PFN_DwmFlush>(1))
                {
                        HMODULE hDwm = LoadLibrary(_T("dwmapi.dll"));
                        s_pfnDwmFlush = hDwm
                                ? (PFN_DwmFlush)GetProcAddress(hDwm, "DwmFlush")
                                : NULL;
                }

                if (s_pfnDwmFlush)
                {
                        if (s_DwmWarmupFrames > 0)
                        {
                                --s_DwmWarmupFrames;
                        }
                        else
                        {
                                if (!s_DwmModeArmed)
                                {
                                        MonitorSync::SetDwmSyncMode(true);
                                        s_DwmModeArmed = true;
                                }

                                s_pfnDwmFlush();

                                if (MatchMonitorRate)
                                {
                                        LARGE_INTEGER qpc;
                                        QueryPerformanceCounter(&qpc);
                                        int idx = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                                        s_diagBuf[idx].t2 = qpc.QuadPart;
                                        // P85: the DwmFlush return timestamp is
                                        // diagnostic-only; pacing uses DWM timing.
                                }
                        }
                }
        }
#endif // USE_DWMFLUSH

        // DWM composition timing is only a valid presentation master while
        // DWM is actually composing the window.  In exclusive fullscreen
        // ChangeDisplaySettingsEx disables the compositor; querying DWM there
        // can return stale/global timing data and contaminate PaceFrame() with
        // a phase that has nothing to do with the exclusive swap chain.  Keep
        // the diagnostic query/phase feedback for windowed and borderless
        // modes, but force exclusive fullscreen to the deterministic QPC path.
        if (MatchMonitorRate && !(Fullscreen && ExclusiveFullscreen))
        {
                int idxDwm = (s_diagHead + DIAG_FRAMES - 1) % DIAG_FRAMES;
                if (DiagQueryDwmTiming(s_diagBuf[idxDwm]) &&
                    s_diagBuf[idxDwm].dwmFrame > 0 &&
                    s_diagBuf[idxDwm].dwmCompose > 0)
                {
                        MonitorSync::NotifyDwmCompositionSample(
                                s_diagBuf[idxDwm].dwmCompose,
                                s_diagBuf[idxDwm].dwmFrame);
                }
        }

        // Submit the rendered frame exactly once. DwmFlush backpressure, when
        // enabled, is handled on the render thread; pacing feedback comes
        // separately from DWM composition timing.

}

#define Try(action,errormsg) do {\
        if (FAILED(action))\
        {\
                InError = TRUE;\
                Stop();\
                MessageBox(hMainWnd, errormsg _T(", retrying"), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONWARNING);\
                Fullscreen = FALSE;\
                Start();\
                InError = FALSE;\
                if (FAILED(action))\
                {\
                        MessageBox(hMainWnd, _T("Error: ") errormsg, Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);\
                        return;\
                }\
        }\
} while (false)

void    Init (void)
{
        ZeroMemory(&SurfDesc, sizeof(SurfDesc));
        ZeroMemory(Palette15, sizeof(Palette15));
        ZeroMemory(Palette16, sizeof(Palette16));
        ZeroMemory(Palette32, sizeof(Palette32));
        DirectDraw = NULL;
        PrimarySurf = NULL;
        SecondarySurf = NULL;
        Clipper = NULL;
#if (_MSC_VER >= 1400)
        dDrawInst = NULL;
        DirectDrawCreateEx = NULL;
#endif
        Pitch = 0;
        WantFPS = 0;
        FPSCnt = 0;
        FPSnum = 0;
        aFPScnt = 0;
        aFPSnum = 0;
        FSkip = 0;
        aFSkip = TRUE;
        forceNoSkip = 0;
        Depth = 0;
        ClockFreq.QuadPart = 0;
        LastClockVal.QuadPart = 0;
        DefaultPalette[NES::REGION_NONE] = Palette[NES::REGION_NONE] = PALETTE_NTSC; // just in case
        DefaultPalette[NES::REGION_NTSC] = Palette[NES::REGION_NTSC] = PALETTE_NTSC;
        DefaultPalette[NES::REGION_PAL] = Palette[NES::REGION_PAL] = PALETTE_PAL;
        DefaultPalette[NES::REGION_DENDY] = Palette[NES::REGION_DENDY] = PALETTE_PAL;
        NTSChue = 0;
        NTSCsat = 50;
        PALsat = 50;
        PC10compat = FALSE;
        Fullscreen = FALSE;
        IntegerScale = FALSE;
        ISBorderX = 0;
        ISBorderY = 0;
        ISMult = 2;
        Bilinear     = FALSE;
        MatchMonitorRate = FALSE;
        AlwaysOnTop  = FALSE;
        ExclusiveFullscreen = FALSE;
        InError      = FALSE;
        hGLRC        = NULL;
        hGLDC        = NULL;
        glTex        = 0;
        HasSavedDisplayMode = FALSE;

        if (!QueryPerformanceFrequency(&ClockFreq))
        {
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_PERF_COUNTER), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                Stop();
                return;
        }

#if (_MSC_VER >= 1400)
        dDrawInst = LoadLibrary(_T("ddraw.dll"));
        if (!dDrawInst)
        {
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_DDRAW_DLL), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                return;
        }
        DirectDrawCreateEx = (LPDIRECTDRAWCREATEEX)GetProcAddress(dDrawInst, "DirectDrawCreateEx");
        if (!DirectDrawCreateEx)
        {
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_DDRAW_ENTRY), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                Destroy();
                return;
        }
#endif
}

void    Destroy (void)
{
        Stop();
#if (_MSC_VER >= 1400)
        DirectDrawCreateEx = NULL;
        if (dDrawInst)
        {
                FreeLibrary(dDrawInst);
                dDrawInst = NULL;
        }
#endif
}

void    SetRegion (void)
{
        switch (NES::CurRegion)
        {
        case NES::REGION_NTSC:
                WantFPS = 60;
                break;
        case NES::REGION_PAL:
                WantFPS = 50;
                break;
        case NES::REGION_DENDY:
                WantFPS = 50;
                break;
        default:
                EI.DbgOut(_T("Invalid GFX region selected!"));
                return;
        }
        LoadPalette(Palette[NES::CurRegion]);
}

// Syncs menu checkmarks with current variable values
// Called after GFX::Start() and Lang::UpdateMenu() to
// restore checkmarks reset by ModifyMenu/SetMenu
void    SyncMenuChecks (void)
{
        if (Bilinear)
                CheckMenuItem(hMenu, ID_PPU_BILINEAR, MF_CHECKED);
        else
                CheckMenuItem(hMenu, ID_PPU_BILINEAR, MF_UNCHECKED);

        if (Scanlines)
                CheckMenuItem(hMenu, ID_PPU_SCANLINES, MF_CHECKED);
        else
                CheckMenuItem(hMenu, ID_PPU_SCANLINES, MF_UNCHECKED);

        if (IntegerScale)
                CheckMenuItem(hMenu, ID_PPU_INTSCALE, MF_CHECKED);
        else
                CheckMenuItem(hMenu, ID_PPU_INTSCALE, MF_UNCHECKED);

        if (MatchMonitorRate)
                CheckMenuItem(hMenu, ID_PPU_MATCHRATE, MF_CHECKED);
        else
                CheckMenuItem(hMenu, ID_PPU_MATCHRATE, MF_UNCHECKED);

        if (AlwaysOnTop)
                CheckMenuItem(hMenu, ID_PPU_ALWAYSONTOP, MF_CHECKED);
        else
                CheckMenuItem(hMenu, ID_PPU_ALWAYSONTOP, MF_UNCHECKED);

        if (ExclusiveFullscreen)
                CheckMenuItem(hMenu, ID_PPU_EXCLUSIVEFS, MF_CHECKED);
        else
                CheckMenuItem(hMenu, ID_PPU_EXCLUSIVEFS, MF_UNCHECKED);
}

void    Start (void)
{
        // Reset per-session timing state so the first DrawScreen frame
        // doesn't produce a bogus QPC delta (LastClockVal == 0 guard in DrawScreen).
        LastClockVal.QuadPart = 0;
        aFPScnt = 0;
        aFPSnum = 0;
        FPSCnt  = 0;
        FSkip   = 0;

        // Reset diagnostic timing buffer on every session start.
        s_diagHead      = 0;
        s_diagFrameNum  = 0;
        s_FQEmuFrameCounter = 0;
        ZeroMemory(s_diagBuf, sizeof(s_diagBuf));

        if (UseOpenGL())
        {
                // OpenGL path
                RECT rc;
                GetClientRect(hMainWnd, &rc);
                int winW = rc.right - rc.left;
                int winH = rc.bottom - rc.top;

                // If already initialized - just update viewport
                if (UsingOpenGL)
                {
                        // P59: DON'T steal the GL context from the render
                        // thread. The old code did wglMakeCurrent + glViewport
                        // + glClear + SwapBuffers + wglMakeCurrent(NULL,NULL)
                        // here, which (a) stole the context from a running
                        // render thread and (b) left it current on NO thread
                        // after the release — breaking all subsequent GL calls
                        // on the render thread. Instead, post the resize via
                        // PostGLResize; the render thread's ApplyPendingResize
                        // (in GL_DrawFrameFromBuffer and the idle loop) applies
                        // it on the correct thread where the context is current.
                        //
                        // P59: also handle fullscreen here. The old early-return
                        // path never called SetWindowPos for fullscreen — it
                        // just set glViewport to the windowed client rect and
                        // returned, leaving the window at windowed size.
                        if (winW > 0 && winH > 0)
                        {
                                if (Fullscreen)
                                {
                                        int scrW = GetSystemMetrics(SM_CXSCREEN);
                                        int scrH = GetSystemMetrics(SM_CYSCREEN);
                                        SetWindowLongPtr(hMainWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                                        SetMenu(hMainWnd, NULL);
                                        HWND zOrder = (ExclusiveFullscreen || AlwaysOnTop) ? HWND_TOPMOST : HWND_TOP;
                                        SetWindowPos(hMainWnd, zOrder, 0, 0, scrW, scrH, SWP_FRAMECHANGED);
                                        ShowWindow(hMainWnd, SW_MAXIMIZE);
                                        PostGLResize(scrW, scrH);
                                }
                                else
                                {
                                        PostGLResize(winW, winH);
                                }
                        }

                        // P53: Re-enter MMR sync state on every GFX::Start.
                        if (MatchMonitorRate)
                        {
                                MonitorSync::Enable(TRUE);
                                MonitorSync::SetDwmSyncMode(false);
                                MonitorSync::OnDisplayChange();
                                MonitorSync::ResetState();
                                StartRenderThread();
                        }

                        // Restore menu checkmarks (may have been reset)
                        SyncMenuChecks();
                        return;
                }

                // Initial OpenGL setup
                if (winW <= 0) winW = 256 * 2;
                if (winH <= 0) winH = 240 * 2;

                if (!GL_Init(winW, winH))
                {
                        MessageBox(
                                hMainWnd,
                                _T("Failed to initialize OpenGL! Falling back to DirectDraw."),
                                _T("Nintendulator"),
                                MB_OK | MB_ICONWARNING
                        );

                        Bilinear     = FALSE;
                        IntegerScale = FALSE;

                        CheckMenuItem(hMenu, ID_PPU_BILINEAR, MF_UNCHECKED);
                        CheckMenuItem(hMenu, ID_PPU_INTSCALE, MF_UNCHECKED);

                        Start();
                        return;
                }

                // OpenGL context has just been created. If Match Monitor Rate
                // is already enabled (e.g. loaded from settings at startup or
                // toggled on before any ROM was loaded), this is the first
                // moment we can actually load WGL swap control and turn vsync
                // on.
                //
                // P35: this used to call MonitorSync::ReinitVSync() here, on
                // the theory that it's "a no-op if vsync is already active or
                // MatchMonitorRate is disabled". That reasoning is wrong:
                // ReinitVSync() bails out on `!IsEnabled()`, and IsEnabled()
                // reflects MonitorSync's OWN internal g_Enabled flag -- which
                // is only ever set by MonitorSync::Enable(), which in turn is
                // only ever called from the menu toggle handler
                // (Nintendulator.cpp, ID_OPTIONS_MATCHRATE). Nothing calls
                // Enable(TRUE) when MatchMonitorRate is TRUE because it was
                // loaded from the registry at startup. So in that exact
                // scenario -- the one this comment claimed to handle --
                // g_Enabled is still FALSE here, ReinitVSync() no-ops
                // immediately, and InitDXGI()/StartVBlankThread()/
                // APU::StartAudioCtrlThread() never run for the rest of the
                // session. MatchMonitorRate-gated behavior elsewhere (diag
                // logging, frameskip disabled, TIME_CRITICAL thread priority)
                // still activates off the raw flag, so nothing *looks* wrong
                // in the UI -- but none of the actual pacing machinery
                // (P1-P34) is running underneath it. This is very likely the
                // real explanation for the "~20-30 second stutter" chased
                // across sessions 1-12 that a static audit of PaceFrame/
                // DXGI/DRC code could never catch: on any run started with
                // the checkbox already on from a previous session, that code
                // simply never executes at all.
                //
                // Fix: call Enable(TRUE) directly. Enable() itself is
                // idempotent (it compares prev/new state via
                // InterlockedExchange and returns early if already on), so
                // it is safe to call unconditionally here without an
                // IsEnabled() guard -- unlike ReinitVSync(), it does not
                // assume enablement already happened elsewhere.
                if (MatchMonitorRate)
                {
                        MonitorSync::Enable(TRUE);
                        // P84: DWM synchronization is selected after the
                        // fullscreen/windowed transition, when the actual mode
                        // flags are final.
                        MonitorSync::OnDisplayChange();
                        MonitorSync::ResetState();
                        // P59: StartRenderThread() is deliberately NOT called
                        // here — it is moved to AFTER the if(Fullscreen) block
                        // below. GL_Resize(scrW,scrH) in the fullscreen branch
                        // must run while IsRenderThreadActive()=FALSE, so it
                        // applies DIRECTLY (wglMakeCurrent + GL_ResizeInternal)
                        // instead of deferring via PostGLResize. If the render
                        // thread were already running, GL_Resize would defer,
                        // and the render thread's ApplyPendingResize would
                        // race with the first frame draw → "fullscreen shows
                        // only part of screen" for the first 1-2 frames (or
                        // persistently if the race is lost).
                }

                if (Fullscreen)
                {
                        // Save window position before entering fullscreen mode
                        RECT wndRect;
                        if (GetWindowRect(hMainWnd, &wndRect))
                        {
                                SavedWindowX = wndRect.left;
                                SavedWindowY = wndRect.top;
                                HasSavedWindowPos = TRUE;
                        }

                        if (ExclusiveFullscreen)
                        {
                                // Exclusive fullscreen OpenGL mode:
                                // switch real monitor resolution via ChangeDisplaySettingsEx,
                                // which disables DWM compositor and reduces input lag
                                DEVMODE dm;
                                ZeroMemory(&dm, sizeof(dm));
                                dm.dmSize = sizeof(dm);

                                // Save current resolution for restoration
                                if (!HasSavedDisplayMode)
                                {
                                        ZeroMemory(&SavedDisplayMode, sizeof(SavedDisplayMode));
                                        SavedDisplayMode.dmSize = sizeof(SavedDisplayMode);
                                        if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &SavedDisplayMode))
                                                HasSavedDisplayMode = TRUE;
                                }

                                // Activate exclusive mode without changing resolution
                                ChangeDisplaySettingsEx(NULL, &SavedDisplayMode, NULL, CDS_FULLSCREEN, NULL);
                                // The refresh/mode query above ran while the
                                // desktop was still in windowed mode.  Refresh
                                // MonitorSync after the actual exclusive mode
                                // switch so the MMR target and the DirectSound
                                // playback frequency selected by the subsequent
                                // NES::Start()/APU::SoundON() match the real
                                // fullscreen display mode.
                                MonitorSync::OnDisplayChange();
                                // P37 (session 14): same rationale as the restore call
                                // in Stop() -- this mode switch can invalidate the
                                // cached IDXGIOutput*, so get a fresh one now, before
                                // GL_DrawFrame starts calling WaitForDXGIVBlank() against
                                // whatever InitDXGI() found back when the process (or the
                                // previous windowed session) started.
                                MonitorSync::ReacquireDXGIOutput();
                        }

                        SetWindowLongPtr(hMainWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                        SetMenu(hMainWnd, NULL);

                        int scrW = GetSystemMetrics(SM_CXSCREEN);
                        int scrH = GetSystemMetrics(SM_CYSCREEN);
                        HWND zOrder = (ExclusiveFullscreen || AlwaysOnTop) ? HWND_TOPMOST : HWND_TOP;
                        SetWindowPos(hMainWnd, zOrder,
                                0, 0, scrW, scrH, SWP_FRAMECHANGED);
                        ShowWindow(hMainWnd, SW_MAXIMIZE);

                        // P49: force the GL viewport to the fullscreen
                        // dimensions immediately. Before this fix, GL_Init/
                        // the UsingOpenGL re-init block above ran with the
                        // WINDOWED client rect (winW/winH, captured BEFORE
                        // SetWindowPos), so glWinW/glWinH/glViewport were
                        // left at the old windowed size. The WM_SIZE that
                        // SetWindowPos posts only lands as a deferred
                        // PostGLResize (g_PendingResize) which ApplyPendingResize
                        // picks up on the FIRST GL_DrawFrame — but the very
                        // first GL_DrawFrame draws with the stale windowed
                        // viewport, showing only a corner of the screen.
                        // GL_Resize applies directly here (NES::Running is
                        // false at this point — NES::Stop() was synchronous
                        // before GFX::Stop/GFX::Start), so there is no race
                        // with GL_DrawFrame and wglMakeCurrent is safe.
                        GL_Resize(scrW, scrH);

                        if (dbgVisible)
                                ShowWindow(hDebug, SW_MINIMIZE);
                }
                else
                {
                        SetWindowLongPtr(hMainWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
                        SetMenu(hMainWnd, hMenu);

                        RECT rcAdj = { 0, 0, winW, winH };
                        AdjustWindowRect(&rcAdj, WS_OVERLAPPEDWINDOW, TRUE);

                        // Restore window position or center it
                        int posX, posY;
                        if (HasSavedWindowPos)
                        {
                                posX = SavedWindowX;
                                posY = SavedWindowY;
                        }
                        else
                        {
                                int scrW = GetSystemMetrics(SM_CXSCREEN);
                                int scrH = GetSystemMetrics(SM_CYSCREEN);
                                posX = (scrW - (rcAdj.right - rcAdj.left)) / 2;
                                posY = (scrH - (rcAdj.bottom - rcAdj.top)) / 2;
                        }

                        // Restore AlwaysOnTop for windowed mode
                        HWND zPos = AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
                        SetWindowPos(hMainWnd, zPos, posX, posY,
                                rcAdj.right - rcAdj.left,
                                rcAdj.bottom - rcAdj.top,
                                SWP_FRAMECHANGED);

                        ShowWindow(hMainWnd, SW_RESTORE);
                }

                // P59: start the render thread AFTER the fullscreen/windowed
                // setup is complete. This ensures GL_Resize(scrW,scrH) in the
                // fullscreen branch above ran with IsRenderThreadActive()=FALSE
                // → applied DIRECTLY (not deferred) → viewport is correct from
                // the very first frame. The render thread starts with the
                // right GL state already set up.
                if (MatchMonitorRate)
                {
                        // P97: DwmFlush is disabled because it is an
                        // uninterruptible blocking call and can deadlock the
                        // UI during MMR shutdown.  Keep the explicit branch
                        // here so that the setting cannot accidentally report
                        // DWM as the active sync master while USE_DWMFLUSH=0.
#if USE_DWMFLUSH
                        const bool useDwm =
                                !(Fullscreen && ExclusiveFullscreen) &&
                                !MonitorSync::HasDXGIVBlank();
#else
                        const bool useDwm = false;
#endif
                        MonitorSync::SetDwmSyncMode(useDwm);
                        StartRenderThread();
                }

                Depth  = 32;
                FPSCnt = FSkip;

                // Restore menu checkmarks after SetMenu / ModifyMenu
                SyncMenuChecks();

                LoadPalette(PALETTE_MAX);
                return;
        }

        // DirectDraw path
        if (FAILED(DirectDrawCreateEx(NULL, (LPVOID *)&DirectDraw, IID_IDirectDraw7, NULL)))
        {
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_DDRAW7), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                return;
        }
        if (Fullscreen)
        {
                // Save window position before entering fullscreen mode
                RECT wndRect;
                if (GetWindowRect(hMainWnd, &wndRect))
                {
                        SavedWindowX = wndRect.left;
                        SavedWindowY = wndRect.top;
                        HasSavedWindowPos = TRUE;
                }

                // Examine the current screen resolution and try to figure out the current aspect ratio
                // We'll support 4:3, 16:10, and 16:9
                double ratio = (double)GetSystemMetrics(SM_CXSCREEN) / (double)GetSystemMetrics(SM_CYSCREEN);

                if (FAILED(DirectDraw->SetCooperativeLevel(hMainWnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_FULLSCREEN_LEVEL), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        Fullscreen = FALSE;
                        Start();
                        return;
                }
                if (dbgVisible)
                        ShowWindow(hDebug, SW_MINIMIZE);

                // Standard Nintendulator mode: 640x480 (or wider)
                static const int widths[] = {
                        640,            // 4:3 - last offset 0
                        720, 768,       // 16:10 - last offset 2
                        848, 856, 864   // 16:9 - last offset 5
                };
                BOOL widths_ok[] = {
                        TRUE,
                        TRUE, TRUE,
                        TRUE, TRUE, TRUE
                };
                int i;
                if (ratio < 1.4)
                        i = 0;
                else if (ratio < 1.7)
                        i = 2;
                else    i = 5;
                if (!widths_ok[0])
                {
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_NO_FULLSCREEN_RES), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        Fullscreen = FALSE;
                        Start();
                        return;
                }
                while (1)
                {
                        FullscreenBorder = (widths[i] - 512) / 2;
                        if (!widths_ok[i] || FAILED(DirectDraw->SetDisplayMode(widths[i], 480, 32, 0, 0)))
                        {
                                widths_ok[i] = FALSE;
                                if (i == 0)
                                {
                                        Stop();
                                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_NO_FULLSCREEN_RES_REVERT), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                                        Fullscreen = FALSE;
                                        Start();
                                        return;
                                }
                                else    i--;
                        }
                        else    break;
                }
                SetWindowLongPtr(hMainWnd, GWL_STYLE, WS_POPUP);
                SetMenu(hMainWnd, NULL);
                ShowWindow(hMainWnd, SW_MAXIMIZE);
        }
        else 
        {
                if (FAILED(DirectDraw->SetCooperativeLevel(hMainWnd, DDSCL_NORMAL)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_COOP_LEVEL), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }
        }

        ZeroMemory(&SurfDesc, sizeof(SurfDesc));
        SurfDesc.dwSize = sizeof(SurfDesc);

        if (Fullscreen)
        {
                SurfDesc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
                SurfDesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
                SurfDesc.dwBackBufferCount = 1;

                if (FAILED(DirectDraw->CreateSurface(&SurfDesc, &PrimarySurf, NULL)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_PRIMARY_SURFACE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }

                SurfDesc.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER;
                if (FAILED(PrimarySurf->GetAttachedSurface(&SurfDesc.ddsCaps, &SecondarySurf)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_SECONDARY_SURFACE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }
        }
        else
        {
                SurfDesc.dwFlags = DDSD_CAPS;
                SurfDesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

                if (FAILED(DirectDraw->CreateSurface(&SurfDesc, &PrimarySurf, NULL)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_PRIMARY_SURFACE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }

                if (Scanlines || Bilinear)
                {
                        SurfDesc.dwWidth = 512;
                        SurfDesc.dwHeight = 480;
                }
                else
                {
                        SurfDesc.dwWidth = 256;
                        SurfDesc.dwHeight = 240;
                }
                SurfDesc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS;
                SurfDesc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;

                if (FAILED(DirectDraw->CreateSurface(&SurfDesc, &SecondarySurf, NULL)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_CREATE_SECONDARY), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }
        }

        if (!Fullscreen)
        {
                if (FAILED(DirectDraw->CreateClipper(0, &Clipper, NULL)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_CLIPPER), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }

                if (FAILED(Clipper->SetHWnd(0, hMainWnd)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_CLIPPER_WINDOW), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }

                if (FAILED(PrimarySurf->SetClipper(Clipper)))
                {
                        Stop();
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_CLIPPER_ASSIGN), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        return;
                }
        }

        ZeroMemory(&SurfDesc, sizeof(SurfDesc));
        SurfDesc.dwSize = sizeof(SurfDesc);

        if (FAILED(SecondarySurf->GetSurfaceDesc(&SurfDesc)))
        {
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_SURF_DESC), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                return;
        }

        Pitch = SurfDesc.lPitch;
        FPSCnt = FSkip;

        switch (SurfDesc.ddpfPixelFormat.dwRGBBitCount)
        {
        case 16:if (SurfDesc.ddpfPixelFormat.dwRBitMask == 0xF800)
                        Depth = 16;
                else    Depth = 15;     break;
        case 32:Depth = 32;             break;
        default:
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_BIT_DEPTH), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                return;                 break;
        }

        // this will automatically call Update()
        LoadPalette(PALETTE_MAX);
        EI.DbgOut(_T("Created %ix%i %i-bit display surface (%s)"), SurfDesc.dwWidth, SurfDesc.dwHeight, Depth, Fullscreen ? _T("fullscreen") : _T("windowed"));
}

void    Stop (void)
{
        if (UsingOpenGL && Fullscreen)
        {
                // Restore exclusive fullscreen mode
                if (ExclusiveFullscreen && HasSavedDisplayMode)
                {
                        ChangeDisplaySettingsEx(NULL, NULL, NULL, 0, NULL);
                        HasSavedDisplayMode = FALSE;
                        // The display is back in its desktop timing domain.
                        // Refresh the monitor-rate cache before the next
                        // windowed MMR SoundON() chooses its playback rate.
                        MonitorSync::OnDisplayChange();
                        // P37 (session 14): ChangeDisplaySettingsEx can invalidate
                        // the IDXGIOutput* cached by MonitorSync's P28 DXGI vblank
                        // bypass, leaving the background poller thread spinning on
                        // a dead pointer for the rest of the process -- see the
                        // ReacquireDXGIOutput() header comment for the full story.
                        // Get a fresh output now that the real display mode change
                        // has actually happened, before the windowed GL context
                        // starts drawing frames again.
                        MonitorSync::ReacquireDXGIOutput();
                }
                // Reset MonitorSync QPC baseline before destroying the GL context.
                // Without this, PaceFrame() computes elapsed time from the old
                // fullscreen baseline (seconds old) on the first windowed frame,
                // clamps slotMs to 1ms, and causes 2-3 seconds of audio stutter /
                // apparent slowdown after returning to windowed mode.
                MonitorSync::ResetState();
                // P47: reset DwmFlush warmup/arm state via the shared helper
                // (also called from MonitorSync::Enable(TRUE) so cold starts
                // get the warmup too -- see ResetDwmWarmup's block comment).
                ResetDwmWarmup();
                // Restore GL vsync (interval=1) for the windowed context that
                // GFX::Start() is about to create. With DwmFlush disabled, this
                // interval is the sole pacing mechanism in both windowed and
                // fullscreen modes — SwapBuffers blocks at vblank, throttling
                // the emulator to the monitor refresh rate.
                MonitorSync::SetDwmSyncMode(false);
                // P54: stop the render thread BEFORE GL_Destroy so it
                // releases the GL context. StopRenderThread is synchronous
                // — it signals the thread to exit and waits for it.
                StopRenderThread();
                GL_Destroy();
                SetWindowLongPtr(hMainWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
                SetMenu(hMainWnd, hMenu);

                // Restore window position or center it
                if (HasSavedWindowPos)
                        SetWindowPos(hMainWnd, AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, SavedWindowX, SavedWindowY, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
                else
                {
                        int scrW = GetSystemMetrics(SM_CXSCREEN);
                        int scrH = GetSystemMetrics(SM_CYSCREEN);
                        RECT wr;
                        GetWindowRect(hMainWnd, &wr);
                        SetWindowPos(hMainWnd, AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, (scrW - (wr.right - wr.left)) / 2, (scrH - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
                }

                ShowWindow(hMainWnd, SW_RESTORE);
                UpdateWindow(hMainWnd);
                if (dbgVisible)
                        ShowWindow(hDebug, SW_RESTORE);
                NES::UpdateInterface();
                return;
        }
        // P54: stop the render thread BEFORE GL_Destroy (DirectDraw path —
        // render thread shouldn't be active here, but stop defensively).
        StopRenderThread();
        GL_Destroy();

        if (!DirectDraw)
                return;
        if (Clipper)
        {
                if (PrimarySurf)
                        PrimarySurf->SetClipper(NULL);
                Clipper->Release();
                Clipper = NULL;
        }
        if (SecondarySurf)
        {
                SecondarySurf->Release();
                SecondarySurf = NULL;
        }
        if (PrimarySurf)
        {
                PrimarySurf->Release();
                PrimarySurf = NULL;
        }
        if (Fullscreen)
        {
                DirectDraw->RestoreDisplayMode();
                SetWindowLongPtr(hMainWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
                SetMenu(hMainWnd, hMenu);

                // Restore window position or center it
                if (HasSavedWindowPos)
                        SetWindowPos(hMainWnd, AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, SavedWindowX, SavedWindowY, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
                else
                {
                        int scrW = GetSystemMetrics(SM_CXSCREEN);
                        int scrH = GetSystemMetrics(SM_CYSCREEN);
                        RECT wr;
                        GetWindowRect(hMainWnd, &wr);
                        SetWindowPos(hMainWnd, AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, (scrW - (wr.right - wr.left)) / 2, (scrH - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_FRAMECHANGED);
                }

                ShowWindow(hMainWnd, SW_RESTORE);
                if (dbgVisible)
                        ShowWindow(hDebug, SW_RESTORE);
                NES::UpdateInterface();
        }
        if (DirectDraw)
        {
                DirectDraw->Release();
                DirectDraw = NULL;
        }
}

void    SaveSettings (HKEY SettingsBase)
{
        RegSetValueEx(SettingsBase, _T("aFSkip")      , 0, REG_DWORD, (LPBYTE)&aFSkip     , sizeof(BOOL));
        RegSetValueEx(SettingsBase, _T("Scanlines")   , 0, REG_DWORD, (LPBYTE)&Scanlines  , sizeof(BOOL));
        RegSetValueEx(SettingsBase, _T("Bilinear")    , 0, REG_DWORD, (LPBYTE)&Bilinear   , sizeof(BOOL));
        RegSetValueEx(SettingsBase, _T("MatchRate")   , 0, REG_DWORD, (LPBYTE)&MatchMonitorRate, sizeof(BOOL));
        RegSetValueEx(SettingsBase, _T("IntScale")    , 0, REG_DWORD, (LPBYTE)&IntegerScale, sizeof(BOOL));
        RegSetValueEx(SettingsBase, _T("AlwaysOnTop") , 0, REG_DWORD, (LPBYTE)&AlwaysOnTop  , sizeof(BOOL));
        RegSetValueEx(SettingsBase, _T("ExclusiveFS") , 0, REG_DWORD, (LPBYTE)&ExclusiveFullscreen, sizeof(BOOL));

        RegSetValueEx(SettingsBase, _T("FSkip")       , 0, REG_DWORD, (LPBYTE)&FSkip      , sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("NTSChue")     , 0, REG_DWORD, (LPBYTE)&NTSChue    , sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("NTSCsat")     , 0, REG_DWORD, (LPBYTE)&NTSCsat    , sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("PALsat")      , 0, REG_DWORD, (LPBYTE)&PALsat     , sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("PC10compat")  , 0, REG_DWORD, (LPBYTE)&PC10compat , sizeof(DWORD));

        RegSetValueEx(SettingsBase, _T("PaletteNTSC") , 0, REG_DWORD, (LPBYTE)&Palette[NES::REGION_NTSC] , sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("PalettePAL")  , 0, REG_DWORD, (LPBYTE)&Palette[NES::REGION_PAL]  , sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("PaletteDendy"), 0, REG_DWORD, (LPBYTE)&Palette[NES::REGION_DENDY], sizeof(DWORD));

        RegSetValueEx(SettingsBase, _T("CustPaletteNTSC") , 0, REG_SZ, (LPBYTE)CustPalette[NES::REGION_NTSC] , (DWORD)(sizeof(TCHAR) * _tcslen(CustPalette[NES::REGION_NTSC])));
        RegSetValueEx(SettingsBase, _T("CustPalettePAL")  , 0, REG_SZ, (LPBYTE)CustPalette[NES::REGION_PAL]  , (DWORD)(sizeof(TCHAR) * _tcslen(CustPalette[NES::REGION_PAL])));
        RegSetValueEx(SettingsBase, _T("CustPaletteDendy"), 0, REG_SZ, (LPBYTE)CustPalette[NES::REGION_DENDY], (DWORD)(sizeof(TCHAR) * _tcslen(CustPalette[NES::REGION_DENDY])));
}

void    LoadSettings (HKEY SettingsBase)
{
        unsigned long Size;

        aFSkip = 1;
        FSkip = 0;
        NTSChue = 0;
        NTSCsat = 50;
        PALsat = 50;
        PC10compat = FALSE;
        for (int i = 0; i < NES::REGION_MAX; i++)
        {
                Palette[i] = DefaultPalette[i];
                CustPalette[i][0] = 0;
        }

        SlowDown = FALSE;
        SlowRate = 2;
        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_2, MF_BYCOMMAND);

        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("Scanlines")   , 0, NULL, (LPBYTE)&Scanlines  , &Size);
        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("Bilinear")    , 0, NULL, (LPBYTE)&Bilinear   , &Size);
        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("MatchRate")   , 0, NULL, (LPBYTE)&MatchMonitorRate, &Size);
        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("IntScale")    , 0, NULL, (LPBYTE)&IntegerScale, &Size);
        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("AlwaysOnTop") , 0, NULL, (LPBYTE)&AlwaysOnTop  , &Size);
        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("ExclusiveFS") , 0, NULL, (LPBYTE)&ExclusiveFullscreen, &Size);
        Size = sizeof(BOOL);    RegQueryValueEx(SettingsBase, _T("aFSkip")      , 0, NULL, (LPBYTE)&aFSkip     , &Size);

        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("FSkip")       , 0, NULL, (LPBYTE)&FSkip      , &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("NTSChue")     , 0, NULL, (LPBYTE)&NTSChue    , &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("NTSCsat")     , 0, NULL, (LPBYTE)&NTSCsat    , &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("PALsat")      , 0, NULL, (LPBYTE)&PALsat     , &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("PC10compat")  , 0, NULL, (LPBYTE)&PC10compat , &Size);

        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("PaletteNTSC") , 0, NULL, (LPBYTE)&Palette[NES::REGION_NTSC] , &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("PalettePAL")  , 0, NULL, (LPBYTE)&Palette[NES::REGION_PAL]  , &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("PaletteDendy"), 0, NULL, (LPBYTE)&Palette[NES::REGION_DENDY], &Size);

        Size = MAX_PATH * sizeof(TCHAR);        RegQueryValueEx(SettingsBase, _T("CustPaletteNTSC") , 0,NULL, (LPBYTE)&CustPalette[NES::REGION_NTSC] , &Size);
        Size = MAX_PATH * sizeof(TCHAR);        RegQueryValueEx(SettingsBase, _T("CustPalettePAL")  , 0,NULL, (LPBYTE)&CustPalette[NES::REGION_PAL]  , &Size);
        Size = MAX_PATH * sizeof(TCHAR);        RegQueryValueEx(SettingsBase, _T("CustPaletteDendy"), 0,NULL, (LPBYTE)&CustPalette[NES::REGION_DENDY], &Size);

        SetFrameskip(-1);

        if (Scanlines)
                CheckMenuItem(hMenu, ID_PPU_SCANLINES, MF_CHECKED);
        if (Bilinear)
                CheckMenuItem(hMenu, ID_PPU_BILINEAR, MF_CHECKED);
        if (MatchMonitorRate)
                CheckMenuItem(hMenu, ID_PPU_MATCHRATE, MF_CHECKED);
        if (IntegerScale)
                CheckMenuItem(hMenu, ID_PPU_INTSCALE, MF_CHECKED);
        if (AlwaysOnTop)
        {
                CheckMenuItem(hMenu, ID_PPU_ALWAYSONTOP, MF_CHECKED);
                SetWindowPos(hMainWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        if (ExclusiveFullscreen)
                CheckMenuItem(hMenu, ID_PPU_EXCLUSIVEFS, MF_CHECKED);
}

int TitleDelay = 0;
void    DrawScreen (void)
{
        LARGE_INTEGER TmpClockVal;
        if (AVI::IsActive())
                AVI::AddVideo();
        if (SlowDown)
                Sleep(SlowRate * 1000 / WantFPS);

        // P88: authoritative MMR pacing now happens at the NES frame boundary,
        // not when APU::Run crosses its fixed 735-sample audio slot boundary.
        // The old arrangement left the NES producer close to its native
        // 60.0988 Hz while the monitor was 60.000 Hz, so the render queue had
        // to absorb the 0.0988 Hz beat. That can look like a tiny scroll hitch
        // even when every individual present is near 16.67 ms.
        //
        // By pacing here, exactly one emulated video frame consumes exactly one
        // monitor-period target. Audio buffering remains independent and gets
        // its playback-rate correction from targetHz / NESHz in APU::UpdateDRC.
        if (MatchMonitorRate)
        {
                LARGE_INTEGER mmrFramePaceEnter = {0}, mmrFramePaceWake = {0};
                ULONGLONG mmrFramePaceCycles = 0;
                QueryPerformanceCounter(&mmrFramePaceEnter);
                MonitorSync::PaceFrame();
                QueryPerformanceCounter(&mmrFramePaceWake);
                QueryThreadCycleTime(GetCurrentThread(), &mmrFramePaceCycles);
                SetMMRProducerTrace(
                        mmrFramePaceEnter.QuadPart,
                        mmrFramePaceEnter.QuadPart,
                        mmrFramePaceWake.QuadPart,
                        0,
                        mmrFramePaceCycles,
                        0,
                        0,
                        0,
                        0);
        }

        // When MMR is active it deliberately overrides presentation frameskip.
        // PaceFrame() above is therefore consumed exactly once for every
        // frame that reaches the render queue. The saved FSkip setting is left
        // untouched and resumes its normal meaning when MMR is disabled.
        if (MatchMonitorRate || (++FPSCnt > FSkip) || forceNoSkip)
        {
                // P54 (Stage 2): two-threaded path. When the render thread is
                // active (MMR on), the emulation thread does NOT call
                // GL_DrawFrame. Instead it converts the PPU palette buffer to
                // RGBA and pushes it to the FrameQueue. The render thread
                // consumes it and does GL_DrawFrameFromBuffer on vblank.
                // This decouples DwmFlush stalls from emulation/audio.
                if (IsRenderThreadActive())
                {
                        static unsigned char rgbaBuf[256 * 240 * 4];
                        unsigned short *src = PPU::DrawArray;
                        unsigned long *dst = (unsigned long *)rgbaBuf;
                        LARGE_INTEGER p81BuildStartQPC, p81BuildEndQPC;
                        ULONGLONG p81BuildStartCycles = 0, p81BuildEndCycles = 0;
                        LONGLONG p81BuildStartCPU100ns = 0, p81BuildEndCPU100ns = 0;
                        QueryPerformanceCounter(&p81BuildStartQPC);
                        QueryThreadCycleTime(GetCurrentThread(), &p81BuildStartCycles);
                        DiagGetThreadCpu100ns(&p81BuildStartCPU100ns);
                        for (int i = 0; i < 256 * 240; i++)
                                dst[i] = Palette32[src[i]];
                        QueryPerformanceCounter(&p81BuildEndQPC);
                        QueryThreadCycleTime(GetCurrentThread(), &p81BuildEndCycles);
                        DiagGetThreadCpu100ns(&p81BuildEndCPU100ns);
                        InterlockedExchange64(&s_MmrBuildStartQPC, p81BuildStartQPC.QuadPart);
                        InterlockedExchange64(&s_MmrBuildEndQPC, p81BuildEndQPC.QuadPart);
                        InterlockedExchange64(&s_MmrBuildStartCPU100ns, p81BuildStartCPU100ns);
                        InterlockedExchange64(&s_MmrBuildEndCPU100ns, p81BuildEndCPU100ns);
                        InterlockedExchange64(&s_MmrBuildStartCycles, (LONGLONG)p81BuildStartCycles);
                        InterlockedExchange64(&s_MmrBuildEndCycles, (LONGLONG)p81BuildEndCycles);
                        ProduceFrameToQueue(rgbaBuf);
                }
                else
                {
                        Update();
                }
                FPSCnt = 0;
                // MMR cadence was already consumed at this frame boundary by
                // PaceFrame(). OnFrameEnd is now only a lightweight compatibility
                // hook; monitor-rate measurement is independent of emulator
                // frame timing.
                if (MatchMonitorRate)
                {
                        MonitorSync::OnFrameEnd();

                        // Diagnostic t3: after OnFrameEnd.
                        LONGLONG diagT3 = 0, diagT4 = 0;
                        if (MatchMonitorRate && !IsRenderThreadActive())
                        {
                                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
                                diagT3 = qpc.QuadPart;
                        }

                        APU::UpdateDRC();

                        if (MatchMonitorRate && !IsRenderThreadActive())
                        {
                                LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
                                diagT4 = qpc.QuadPart;
                                DiagCompleteFrame(diagT3, diagT4);
                        }
                }
        }
        QueryPerformanceCounter(&TmpClockVal);
        // Guard: on the very first DrawScreen call after ROM load, LastClockVal is 0.
        // Without this check, aFPSnum gets a huge initial value (QPC ticks since boot),
        // causing FPSnum to read near 0 and FSkip to spike to max for the first 20 frames.
        if (LastClockVal.QuadPart != 0)
                aFPSnum += TmpClockVal.QuadPart - LastClockVal.QuadPart;
        LastClockVal = TmpClockVal;

        if (++aFPScnt >= 20)
        {
                if (aFPSnum > 0)
                        FPSnum = (int)((ClockFreq.QuadPart * aFPScnt) / aFPSnum);
                if (aFSkip && !forceNoSkip && !MatchMonitorRate)
                {
                        // When Match Monitor Rate is on, OpenGL vsync already
                        // throttles the emulator to the monitor refresh rate.
                        // The auto-frameskip logic compares FPSnum against the
                        // integer WantFPS (60 or 50) but the measured FPS with
                        // vsync on is the monitor rate — typically not an integer
                        // (e.g. 59.94 or 60.000 vs WantFPS=60). This causes the
                        // auto-skip logic to occasionally decide a frame must be
                        // dropped, which appears as a visible stutter even though
                        // the emulator is running perfectly. Disabling auto-skip
                        // when MatchMonitorRate is active avoids this entirely;
                        // vsync is the sole frame-rate governor in that mode.
                        if ((FSkip < 9) && (FPSnum <= (WantFPS * 9 / 10)))
                                FSkip++;
                        if ((FSkip > 0) && (FPSnum >= (WantFPS - 1)))
                                FSkip--;
                        SetFrameskip(-1);
                }
                else if (MatchMonitorRate && FSkip != 0)
                {
                        // If we just enabled Match Monitor Rate while auto-skip
                        // had already set a non-zero FSkip, clear it now so we
                        // don't keep dropping frames unnecessarily.
                        FSkip = 0;
                        SetFrameskip(0);
                }
                aFPScnt = 0;
                aFPSnum = 0;
        }
        if (!TitleDelay--)
        {
                UpdateTitlebar();
                TitleDelay = 10;

                // Recheck thread priority every ~10 frames (piggybacked on
                // the title-bar update). This ensures that toggling Match
                // Monitor Rate on/off at runtime is reflected promptly in the
                // NES thread's priority without needing a stop/restart cycle.
                // TIME_CRITICAL is used when MMR is on (minimises preemption
                // in the vblank window), ABOVE_NORMAL otherwise.
                SetThreadPriority(GetCurrentThread(),
                        MatchMonitorRate
                                ? THREAD_PRIORITY_TIME_CRITICAL
                                : THREAD_PRIORITY_ABOVE_NORMAL);
        }
}
void    SetFrameskip (int skip)
{
        if (skip >= 0)
                FSkip = skip;

        if (aFSkip)
                CheckMenuItem(hMenu, ID_PPU_FRAMESKIP_AUTO, MF_CHECKED);
        else    CheckMenuItem(hMenu, ID_PPU_FRAMESKIP_AUTO, MF_UNCHECKED);

        switch (FSkip)
        {
        case 0: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_0, MF_BYCOMMAND);    break;
        case 1: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_1, MF_BYCOMMAND);    break;
        case 2: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_2, MF_BYCOMMAND);    break;
        case 3: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_3, MF_BYCOMMAND);    break;
        case 4: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_4, MF_BYCOMMAND);    break;
        case 5: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_5, MF_BYCOMMAND);    break;
        case 6: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_6, MF_BYCOMMAND);    break;
        case 7: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_7, MF_BYCOMMAND);    break;
        case 8: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_8, MF_BYCOMMAND);    break;
        case 9: CheckMenuRadioItem(hMenu, ID_PPU_FRAMESKIP_0, ID_PPU_FRAMESKIP_9, ID_PPU_FRAMESKIP_9, MF_BYCOMMAND);    break;
        }

        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_AUTO, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_0, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_1, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_2, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_3, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_4, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_5, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_6, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_7, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_8, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_PPU_FRAMESKIP_9, (forceNoSkip == 0) ? MF_ENABLED : MF_GRAYED);
}

// Allow parts of the emulator to forcibly disable frameskip,
// such as Zapper emulation and AVI recording
void    ForceNoSkip (BOOL enable)
{
        if (enable)
                forceNoSkip++;
        else    forceNoSkip--;
        SetFrameskip(-1);
}

BOOL    NeedSkip (void)
{
        // MMR owns presentation cadence. While it is active every emulated
        // NES frame must reach the render path; otherwise a user-selected
        // frameskip value would consume a pacing slot without producing a
        // displayed frame, effectively turning 60 Hz MMR into 30/20/15 Hz.
        if (forceNoSkip || MatchMonitorRate)
                return FALSE;
        return FPSCnt < FSkip;
}

// Helper function: blends two 32-bit colors in proportion t (0..255)
static inline unsigned long BlendColors32(unsigned long c1, unsigned long c2, int t)
{
        int it = 255 - t;
        unsigned long r = ((c1 >> 16 & 0xFF) * it + (c2 >> 16 & 0xFF) * t) / 255;
        unsigned long g = ((c1 >>  8 & 0xFF) * it + (c2 >>  8 & 0xFF) * t) / 255;
        unsigned long b = ((c1       & 0xFF) * it + (c2       & 0xFF) * t) / 255;
        return (r << 16) | (g << 8) | b;
}

// Draws NES image with integer multiplier ISMult centered on screen.
// Remaining area is filled with black. Only works in 32-bit mode.
void    DrawIntegerScale (void)
{
        int x, y;
        int scrW = ISBorderX * 2 + 256 * ISMult;
        int scrH = ISBorderY * 2 + 240 * ISMult;

        unsigned long *dst;
        for (y = 0; y < scrH; y++)
        {
                dst = (unsigned long *)((unsigned char *)SurfDesc.lpSurface + y * Pitch);

                // Rows above or below the image - black
                if (y < ISBorderY || y >= ISBorderY + 240 * ISMult)
                {
                        for (x = 0; x < scrW; x++)
                                *dst++ = 0x000000;
                        continue;
                }

                // Which NES row corresponds to this screen row
                int srcY = (y - ISBorderY) / ISMult;

                // Left black border
                unsigned short *dstS = (unsigned short *)dst;
                for (x = 0; x < ISBorderX; x++)
                        *dstS++ = 0x0000;

                // NES pixels - each repeated ISMult times
                unsigned short *src = PPU::DrawArray + srcY * 256;
                for (x = 0; x < 256; x++)
                {
                        unsigned long color = Palette32[*src++];
                        for (int px = 0; px < ISMult; px++)
                                *dstS++ = (unsigned short)color;
                }

                // Right black border
                for (x = ISBorderX + 256 * ISMult; x < scrW; x++)
                        *dstS++ = 0x0000;
        }
}

void    Draw2x (void)
{
        int x, y;
        unsigned short *src = PPU::DrawArray;
        if (Depth == 32)
        {
                unsigned long *dst;
                for (y = 0; y < 480; y++)
                {
                        dst = (unsigned long *)((unsigned char *)SurfDesc.lpSurface + y*Pitch);
                        if (Fullscreen)
                        {
                                for (x = 0; x < FullscreenBorder; x++)
                                        *dst++ = 0x000000;
                        }
                        if (Scanlines && (y & 1))
                        {
                                for (x = 0; x < 256; x++)
                                {
                                        *dst++ = 0x000000;
                                        *dst++ = 0x000000;
                                }
                                src += 256;
                        }
                        else
                        {
                                for (x = 0; x < 256; x++)
                                {
                                        *dst++ = Palette32[*src];
                                        *dst++ = Palette32[*src];
                                        src++;
                                }
                        }
                        if (Fullscreen)
                        {
                                for (x = 0; x < FullscreenBorder; x++)
                                        *dst++ = 0x000000;
                        }
                        if (!(y & 1))
                                src -= 256;
                }
        }
        else if (Depth == 16)
        {
                unsigned short *dst;
                for (y = 0; y < 480; y++)
                {
                        dst = (unsigned short *)((unsigned char *)SurfDesc.lpSurface + y*Pitch);
                        if (Fullscreen)
                        {
                                for (x = 0; x < FullscreenBorder; x++)
                                        *dst++ = 0x0000;
                        }
                        if (Scanlines && (y & 1))
                        {
                                for (x = 0; x < 256; x++)
                                {
                                        *dst++ = 0x0000;
                                        *dst++ = 0x0000;
                                }
                                src += 256;
                        }
                        else
                        {
                                for (x = 0; x < 256; x++)
                                {
                                        *dst++ = Palette16[*src];
                                        *dst++ = Palette16[*src];
                                        src++;
                                }
                        }
                        if (Fullscreen)
                        {
                                for (x = 0; x < FullscreenBorder; x++)
                                        *dst++ = 0x0000;
                        }
                        if (!(y & 1))
                                src -= 256;
                }
        }
        else
        {
                unsigned short *dst;
                for (y = 0; y < 240; y++)
                {
                        dst = (unsigned short *)((unsigned char *)SurfDesc.lpSurface + y*Pitch);
                        if (Fullscreen)
                        {
                                for (x = 0; x < FullscreenBorder; x++)
                                        *dst++ = 0x0000;
                        }
                        if (Scanlines && (y & 1))
                        {
                                for (x = 0; x < 256; x++)
                                {
                                        *dst++ = 0x0000;
                                        *dst++ = 0x0000;
                                }
                                src += 256;
                        }
                        else
                        {
                                for (x = 0; x < 256; x++)
                                {
                                        *dst++ = Palette15[*src];
                                        *dst++ = Palette15[*src];
                                        src++;
                                }
                        }
                        if (Fullscreen)
                        {
                                for (x = 0; x < FullscreenBorder; x++)
                                        *dst++ = 0x0000;
                        }
                        if (!(y & 1))
                                src -= 256;
                }
        }
}

void    Draw1x (void)
{
        int x, y;
        unsigned short *src = PPU::DrawArray;
        if (Depth == 32)
        {
                unsigned long *dst;
                for (y = 0; y < 240; y++)
                {
                        dst = (unsigned long *)((unsigned char *)SurfDesc.lpSurface + y*Pitch);
                        for (x = 0; x < 256; x++)
                                *dst++ = Palette32[*src++];
                }
        }
        else if (Depth == 16)
        {
                unsigned short *dst;
                for (y = 0; y < 240; y++)
                {
                        dst = (unsigned short *)((unsigned char *)SurfDesc.lpSurface + y*Pitch);
                        for (x = 0; x < 256; x++)
                                *dst++ = Palette16[*src++];
                }
        }
        else
        {
                register unsigned short *dst;
                for (y = 0; y < 240; y++)
                {
                        dst = (unsigned short *)((unsigned char *)SurfDesc.lpSurface + y*Pitch);
                        for (x = 0; x < 256; x++)
                                *dst++ = Palette15[*src++];
                }
        }
}

void    Update (void)
{
        // OpenGL path
        if (UsingOpenGL)
        {
                GL_DrawFrame();
                return;
        }

        // DirectDraw path
        if (!SecondarySurf) return;
        if (SecondarySurf->IsLost() == DDERR_SURFACELOST)
                SecondarySurf->Restore();
        if (InError) return;

        Try(SecondarySurf->Lock(NULL, &SurfDesc, DDLOCK_WAIT | DDLOCK_NOSYSLOCK | DDLOCK_WRITEONLY | DDLOCK_SURFACEMEMORYPTR, NULL), _T("Failed to lock secondary surface"));

        if (Fullscreen || Scanlines)
                Draw2x();
        else
                Draw1x();

        Try(SecondarySurf->Unlock(NULL), _T("Failed to unlock secondary surface"));
        Repaint();
}
        
void    Repaint (void)
{
        // OpenGL does its own SwapBuffers in GL_DrawFrame
        if (UsingOpenGL)
                return;

        if (!PrimarySurf)
                return;
        // if it got lost, try to restore it
        if (PrimarySurf->IsLost() == DDERR_SURFACELOST)
                PrimarySurf->Restore();
        // just to be safe, make sure the secondary surface exists too - this is only called by Repaint
        if (!SecondarySurf)
                return;
        if (InError)
                return;

        if (Fullscreen)
        {
                // can't use Try() here, because a failure will make it retry in Windowed mode, where Flip() isn't allowed
                if (SUCCEEDED(PrimarySurf->Flip(NULL, DDFLIP_WAIT)))
                        return;
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_FLIP), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONWARNING);
                Fullscreen = FALSE;
                Start();
        }
        // translate window position appropriately
        RECT rect;
        POINT pt = {0, 0};
        GetClientRect(hMainWnd, &rect);
        if ((rect.right == 0) || (rect.bottom == 0))
                return;
        ClientToScreen(hMainWnd, &pt);
        OffsetRect(&rect, pt.x, pt.y);
        Try(PrimarySurf->Blt(&rect, SecondarySurf, NULL, DDBLT_WAIT, NULL), _T("Failed to blit to primary surface"));
}

void    GetCursorPos (POINT *pos)
{
        ::GetCursorPos(pos);
        if (Fullscreen)
        {
                if (IntegerScale)
                {
                        pos->x = (pos->x - ISBorderX) / ISMult;
                        pos->y = (pos->y - ISBorderY) / ISMult;
                }
                else
                {
                        pos->x -= FullscreenBorder;
                        pos->x /= 2;
                        pos->y /= 2;
                }
        }
        else
        {
                RECT rect;
                ScreenToClient(hMainWnd, pos);
                GetClientRect(hMainWnd, &rect);
                if (rect.left == rect.right)
                        pos->x = 0;
                else    pos->x = pos->x * 256 / (rect.right - rect.left);
                if (rect.top == rect.bottom)
                        pos->y = 0;
                else    pos->y = pos->y * 240 / (rect.bottom - rect.top);
        }
        
}

void    SetCursorPos (int x, int y)
{
        POINT pos;
        pos.x = x;
        pos.y = y;
        if (Fullscreen)
        {
                if (IntegerScale)
                {
                        pos.x = pos.x * ISMult + ISBorderX;
                        pos.y = pos.y * ISMult + ISBorderY;
                }
                else
                {
                        pos.x *= 2;
                        pos.y *= 2;
                        pos.x += FullscreenBorder;
                }
        }
        else
        {
                RECT rect;
                GetClientRect(hMainWnd, &rect);
                pos.x = pos.x * (rect.right - rect.left) / 256;
                pos.y = pos.y * (rect.bottom - rect.top) / 240;
                ClientToScreen(hMainWnd, &pos);
        }
        ::SetCursorPos(pos.x, pos.y);
}

BOOL    ZapperHit (int color)
{
        int val = 0;
        val += (int)(RawPalette[(color >> 6) & 0x7][color & 0x3F][0] * 0.299);
        val += (int)(RawPalette[(color >> 6) & 0x7][color & 0x3F][1] * 0.587);
        val += (int)(RawPalette[(color >> 6) & 0x7][color & 0x3F][2] * 0.114);
        return (val >= 0x40);
}

// TODO - make dynamic
const unsigned char Palette_PAL[8][64][3] =
{
        {       // none
                {0x80,0x80,0x80},{0x00,0x3D,0xA6},{0x00,0x12,0xB0},{0x44,0x00,0x96},{0xA1,0x00,0x5E},{0xC7,0x00,0x28},{0xBA,0x06,0x00},{0x8C,0x17,0x00},{0x5C,0x2F,0x00},{0x10,0x45,0x00},{0x05,0x4A,0x00},{0x00,0x47,0x2E},{0x00,0x41,0x66},{0x00,0x00,0x00},{0x05,0x05,0x05},{0x05,0x05,0x05},
                {0xC7,0xC7,0xC7},{0x00,0x77,0xFF},{0x21,0x55,0xFF},{0x82,0x37,0xFA},{0xEB,0x2F,0xB5},{0xFF,0x29,0x50},{0xFF,0x22,0x00},{0xD6,0x32,0x00},{0xC4,0x62,0x00},{0x35,0x80,0x00},{0x05,0x8F,0x00},{0x00,0x8A,0x55},{0x00,0x99,0xCC},{0x21,0x21,0x21},{0x09,0x09,0x09},{0x09,0x09,0x09},
                {0xFF,0xFF,0xFF},{0x0F,0xD7,0xFF},{0x69,0xA2,0xFF},{0xD4,0x80,0xFF},{0xFF,0x45,0xF3},{0xFF,0x61,0x8B},{0xFF,0x88,0x33},{0xFF,0x9C,0x12},{0xFA,0xBC,0x20},{0x9F,0xE3,0x0E},{0x2B,0xF0,0x35},{0x0C,0xF0,0xA4},{0x05,0xFB,0xFF},{0x5E,0x5E,0x5E},{0x0D,0x0D,0x0D},{0x0D,0x0D,0x0D},
                {0xFF,0xFF,0xFF},{0xA6,0xFC,0xFF},{0xB3,0xEC,0xFF},{0xDA,0xAB,0xEB},{0xFF,0xA8,0xF9},{0xFF,0xAB,0xB3},{0xFF,0xD2,0xB0},{0xFF,0xEF,0xA6},{0xFF,0xF7,0x9C},{0xD7,0xE8,0x95},{0xA6,0xED,0xAF},{0xA2,0xF2,0xDA},{0x99,0xFF,0xFC},{0xDD,0xDD,0xDD},{0x11,0x11,0x11},{0x11,0x11,0x11}
        },
        {       // red
                {0x80,0x66,0x67},{0x00,0x30,0x86},{0x00,0x0E,0x8E},{0x44,0x00,0x79},{0xA1,0x00,0x4C},{0xC7,0x00,0x20},{0xBA,0x04,0x00},{0x8C,0x12,0x00},{0x5C,0x25,0x00},{0x10,0x37,0x00},{0x05,0x3B,0x00},{0x00,0x38,0x25},{0x00,0x34,0x52},{0x00,0x00,0x00},{0x05,0x04,0x04},{0x05,0x04,0x04},
                {0xC7,0x9F,0xA1},{0x00,0x5F,0xCE},{0x21,0x44,0xCE},{0x82,0x2C,0xCA},{0xEB,0x25,0x92},{0xFF,0x20,0x40},{0xFF,0x1B,0x00},{0xD6,0x28,0x00},{0xC4,0x4E,0x00},{0x35,0x66,0x00},{0x05,0x72,0x00},{0x00,0x6E,0x44},{0x00,0x7A,0xA5},{0x21,0x1A,0x1A},{0x09,0x07,0x07},{0x09,0x07,0x07},
                {0xFF,0xCC,0xCE},{0x0F,0xAC,0xCE},{0x69,0x81,0xCE},{0xD4,0x66,0xCE},{0xFF,0x37,0xC4},{0xFF,0x4D,0x70},{0xFF,0x6C,0x29},{0xFF,0x7C,0x0E},{0xFA,0x96,0x19},{0x9F,0xB5,0x0B},{0x2B,0xC0,0x2A},{0x0C,0xC0,0x84},{0x05,0xC8,0xCE},{0x5E,0x4B,0x4C},{0x0D,0x0A,0x0A},{0x0D,0x0A,0x0A},
                {0xFF,0xCC,0xCE},{0xA6,0xC9,0xCE},{0xB3,0xBC,0xCE},{0xDA,0x88,0xBE},{0xFF,0x86,0xC9},{0xFF,0x88,0x90},{0xFF,0xA8,0x8E},{0xFF,0xBF,0x86},{0xFF,0xC5,0x7E},{0xD7,0xB9,0x78},{0xA6,0xBD,0x8D},{0xA2,0xC1,0xB0},{0x99,0xCC,0xCC},{0xDD,0xB0,0xB3},{0x11,0x0D,0x0D},{0x11,0x0D,0x0D}
        },
        {       // green
                {0x63,0x78,0x54},{0x00,0x39,0x6D},{0x00,0x10,0x74},{0x35,0x00,0x63},{0x7D,0x00,0x3E},{0x9B,0x00,0x1A},{0x91,0x05,0x00},{0x6D,0x15,0x00},{0x47,0x2C,0x00},{0x0C,0x40,0x00},{0x03,0x45,0x00},{0x00,0x42,0x1E},{0x00,0x3D,0x43},{0x00,0x00,0x00},{0x03,0x04,0x03},{0x03,0x04,0x03},
                {0x9B,0xBB,0x83},{0x00,0x6F,0xA8},{0x19,0x4F,0xA8},{0x65,0x33,0xA5},{0xB7,0x2C,0x77},{0xC6,0x26,0x34},{0xC6,0x1F,0x00},{0xA6,0x2F,0x00},{0x98,0x5C,0x00},{0x29,0x78,0x00},{0x03,0x86,0x00},{0x00,0x81,0x38},{0x00,0x8F,0x86},{0x19,0x1F,0x15},{0x07,0x08,0x05},{0x07,0x08,0x05},
                {0xC6,0xEF,0xA8},{0x0B,0xCA,0xA8},{0x51,0x98,0xA8},{0xA5,0x78,0xA8},{0xC6,0x40,0xA0},{0xC6,0x5B,0x5B},{0xC6,0x7F,0x21},{0xC6,0x92,0x0B},{0xC3,0xB0,0x15},{0x7C,0xD5,0x09},{0x21,0xE1,0x22},{0x09,0xE1,0x6C},{0x03,0xEB,0xA8},{0x49,0x58,0x3E},{0x0A,0x0C,0x08},{0x0A,0x0C,0x08},
                {0xC6,0xEF,0xA8},{0x81,0xEC,0xA8},{0x8B,0xDD,0xA8},{0xAA,0xA0,0x9B},{0xC6,0x9D,0xA4},{0xC6,0xA0,0x76},{0xC6,0xC5,0x74},{0xC6,0xE0,0x6D},{0xC6,0xE8,0x66},{0xA7,0xDA,0x62},{0x81,0xDE,0x73},{0x7E,0xE3,0x8F},{0x77,0xEF,0xA6},{0xAC,0xCF,0x91},{0x0D,0x0F,0x0B},{0x0D,0x0F,0x0B}
        },
        {       // yellow
                {0x65,0x62,0x50},{0x00,0x2E,0x68},{0x00,0x0D,0x6E},{0x35,0x00,0x5E},{0x7F,0x00,0x3B},{0x9D,0x00,0x19},{0x92,0x04,0x00},{0x6E,0x11,0x00},{0x48,0x24,0x00},{0x0C,0x35,0x00},{0x03,0x38,0x00},{0x00,0x36,0x1C},{0x00,0x32,0x40},{0x00,0x00,0x00},{0x03,0x03,0x03},{0x03,0x03,0x03},
                {0x9D,0x99,0x7D},{0x00,0x5B,0xA0},{0x1A,0x41,0xA0},{0x66,0x2A,0x9D},{0xB9,0x24,0x72},{0xC9,0x1F,0x32},{0xC9,0x1A,0x00},{0xA9,0x26,0x00},{0x9A,0x4B,0x00},{0x29,0x62,0x00},{0x03,0x6E,0x00},{0x00,0x6A,0x35},{0x00,0x75,0x80},{0x1A,0x19,0x14},{0x07,0x06,0x05},{0x07,0x06,0x05},
                {0xC9,0xC4,0xA0},{0x0B,0xA5,0xA0},{0x52,0x7C,0xA0},{0xA7,0x62,0xA0},{0xC9,0x35,0x99},{0xC9,0x4A,0x57},{0xC9,0x68,0x20},{0xC9,0x78,0x0B},{0xC5,0x90,0x14},{0x7D,0xAE,0x08},{0x21,0xB8,0x21},{0x09,0xB8,0x67},{0x03,0xC1,0xA0},{0x4A,0x48,0x3B},{0x0A,0x0A,0x08},{0x0A,0x0A,0x08},
                {0xC9,0xC4,0xA0},{0x83,0xC2,0xA0},{0x8D,0xB5,0xA0},{0xAC,0x83,0x94},{0xC9,0x81,0x9C},{0xC9,0x83,0x70},{0xC9,0xA1,0x6E},{0xC9,0xB8,0x68},{0xC9,0xBE,0x62},{0xA9,0xB2,0x5D},{0x83,0xB6,0x6E},{0x7F,0xBA,0x89},{0x78,0xC4,0x9E},{0xAE,0xAA,0x8B},{0x0D,0x0D,0x0A},{0x0D,0x0D,0x0A}
        },
        {       // blue
                {0x68,0x6A,0x8F},{0x00,0x32,0xB9},{0x00,0x0E,0xC5},{0x37,0x00,0xA8},{0x84,0x00,0x69},{0xA3,0x00,0x2C},{0x98,0x04,0x00},{0x72,0x13,0x00},{0x4B,0x27,0x00},{0x0D,0x39,0x00},{0x04,0x3D,0x00},{0x00,0x3A,0x33},{0x00,0x35,0x72},{0x00,0x00,0x00},{0x04,0x04,0x05},{0x04,0x04,0x05},
                {0xA3,0xA5,0xDE},{0x00,0x62,0xFF},{0x1B,0x46,0xFF},{0x6A,0x2D,0xFF},{0xC0,0x27,0xCA},{0xD1,0x22,0x59},{0xD1,0x1C,0x00},{0xAF,0x29,0x00},{0xA0,0x51,0x00},{0x2B,0x6A,0x00},{0x04,0x76,0x00},{0x00,0x72,0x5F},{0x00,0x7E,0xE4},{0x1B,0x1B,0x24},{0x07,0x07,0x0A},{0x07,0x07,0x0A},
                {0xD1,0xD3,0xFF},{0x0C,0xB2,0xFF},{0x56,0x86,0xFF},{0xAD,0x6A,0xFF},{0xD1,0x39,0xFF},{0xD1,0x50,0x9B},{0xD1,0x70,0x39},{0xD1,0x81,0x14},{0xCD,0x9C,0x23},{0x82,0xBC,0x0F},{0x23,0xC7,0x3B},{0x09,0xC7,0xB7},{0x04,0xD0,0xFF},{0x4D,0x4E,0x69},{0x0A,0x0A,0x0E},{0x0A,0x0A,0x0E},
                {0xD1,0xD3,0xFF},{0x88,0xD1,0xFF},{0x92,0xC3,0xFF},{0xB2,0x8D,0xFF},{0xD1,0x8B,0xFF},{0xD1,0x8D,0xC8},{0xD1,0xAE,0xC5},{0xD1,0xC6,0xB9},{0xD1,0xCD,0xAE},{0xB0,0xC0,0xA6},{0x88,0xC4,0xC4},{0x84,0xC8,0xF4},{0x7D,0xD3,0xFF},{0xB5,0xB7,0xF7},{0x0D,0x0E,0x13},{0x0D,0x0E,0x13}
        },
        {       // magenta
                {0x67,0x5A,0x6F},{0x00,0x2B,0x90},{0x00,0x0C,0x99},{0x37,0x00,0x82},{0x82,0x00,0x51},{0xA1,0x00,0x22},{0x96,0x04,0x00},{0x71,0x10,0x00},{0x4A,0x21,0x00},{0x0C,0x30,0x00},{0x04,0x34,0x00},{0x00,0x32,0x28},{0x00,0x2E,0x58},{0x00,0x00,0x00},{0x04,0x03,0x04},{0x04,0x03,0x04},
                {0xA1,0x8D,0xAD},{0x00,0x54,0xDD},{0x1A,0x3C,0xDD},{0x69,0x27,0xD9},{0xBE,0x21,0x9D},{0xCE,0x1D,0x45},{0xCE,0x18,0x00},{0xAD,0x23,0x00},{0x9E,0x45,0x00},{0x2A,0x5A,0x00},{0x04,0x65,0x00},{0x00,0x61,0x49},{0x00,0x6C,0xB1},{0x1A,0x17,0x1C},{0x07,0x06,0x07},{0x07,0x06,0x07},
                {0xCE,0xB5,0xDD},{0x0C,0x98,0xDD},{0x55,0x73,0xDD},{0xAB,0x5A,0xDD},{0xCE,0x30,0xD3},{0xCE,0x44,0x78},{0xCE,0x60,0x2C},{0xCE,0x6E,0x0F},{0xCA,0x85,0x1B},{0x80,0xA1,0x0C},{0x22,0xAA,0x2E},{0x09,0xAA,0x8E},{0x04,0xB2,0xDD},{0x4C,0x42,0x51},{0x0A,0x09,0x0B},{0x0A,0x09,0x0B},
                {0xCE,0xB5,0xDD},{0x86,0xB2,0xDD},{0x90,0xA7,0xDD},{0xB0,0x79,0xCC},{0xCE,0x77,0xD8},{0xCE,0x79,0x9B},{0xCE,0x95,0x99},{0xCE,0xA9,0x90},{0xCE,0xAF,0x87},{0xAE,0xA4,0x81},{0x86,0xA8,0x98},{0x83,0xAB,0xBD},{0x7B,0xB5,0xDB},{0xB3,0x9C,0xC0},{0x0D,0x0C,0x0E},{0x0D,0x0C,0x0E}
        },
        {       // cyan
                {0x57,0x65,0x65},{0x00,0x30,0x83},{0x00,0x0E,0x8B},{0x2E,0x00,0x76},{0x6D,0x00,0x4A},{0x87,0x00,0x1F},{0x7E,0x04,0x00},{0x5F,0x12,0x00},{0x3E,0x25,0x00},{0x0A,0x36,0x00},{0x03,0x3A,0x00},{0x00,0x38,0x24},{0x00,0x33,0x50},{0x00,0x00,0x00},{0x03,0x03,0x03},{0x03,0x03,0x03},
                {0x87,0x9D,0x9D},{0x00,0x5E,0xC9},{0x16,0x43,0xC9},{0x58,0x2B,0xC5},{0x9F,0x25,0x8E},{0xAD,0x20,0x3F},{0xAD,0x1A,0x00},{0x91,0x27,0x00},{0x85,0x4D,0x00},{0x24,0x65,0x00},{0x03,0x70,0x00},{0x00,0x6D,0x43},{0x00,0x78,0xA1},{0x16,0x1A,0x1A},{0x06,0x07,0x07},{0x06,0x07,0x07},
                {0xAD,0xC9,0xC9},{0x0A,0xA9,0xC9},{0x47,0x7F,0xC9},{0x90,0x65,0xC9},{0xAD,0x36,0xBF},{0xAD,0x4C,0x6D},{0xAD,0x6B,0x28},{0xAD,0x7B,0x0E},{0xAA,0x94,0x19},{0x6C,0xB3,0x0B},{0x1D,0xBD,0x29},{0x08,0xBD,0x81},{0x03,0xC6,0xC9},{0x3F,0x4A,0x4A},{0x08,0x0A,0x0A},{0x08,0x0A,0x0A},
                {0xAD,0xC9,0xC9},{0x70,0xC7,0xC9},{0x79,0xBA,0xC9},{0x94,0x87,0xB9},{0xAD,0x84,0xC4},{0xAD,0x87,0x8D},{0xAD,0xA5,0x8B},{0xAD,0xBC,0x83},{0xAD,0xC3,0x7B},{0x92,0xB7,0x75},{0x70,0xBB,0x8A},{0x6E,0xBF,0xAC},{0x68,0xC9,0xC7},{0x96,0xAE,0xAE},{0x0B,0x0D,0x0D},{0x0B,0x0D,0x0D}
        },
        {       // white
                {0x59,0x59,0x59},{0x00,0x2A,0x74},{0x00,0x0C,0x7B},{0x2F,0x00,0x69},{0x70,0x00,0x41},{0x8B,0x00,0x1C},{0x82,0x04,0x00},{0x62,0x10,0x00},{0x40,0x20,0x00},{0x0B,0x30,0x00},{0x03,0x33,0x00},{0x00,0x31,0x20},{0x00,0x2D,0x47},{0x00,0x00,0x00},{0x03,0x03,0x03},{0x03,0x03,0x03},
                {0x8B,0x8B,0x8B},{0x00,0x53,0xB2},{0x17,0x3B,0xB2},{0x5B,0x26,0xAF},{0xA4,0x20,0x7E},{0xB2,0x1C,0x38},{0xB2,0x17,0x00},{0x95,0x23,0x00},{0x89,0x44,0x00},{0x25,0x59,0x00},{0x03,0x64,0x00},{0x00,0x60,0x3B},{0x00,0x6B,0x8E},{0x17,0x17,0x17},{0x06,0x06,0x06},{0x06,0x06,0x06},
                {0xB2,0xB2,0xB2},{0x0A,0x96,0xB2},{0x49,0x71,0xB2},{0x94,0x59,0xB2},{0xB2,0x30,0xAA},{0xB2,0x43,0x61},{0xB2,0x5F,0x23},{0xB2,0x6D,0x0C},{0xAF,0x83,0x16},{0x6F,0x9E,0x09},{0x1E,0xA8,0x25},{0x08,0xA8,0x72},{0x03,0xAF,0xB2},{0x41,0x41,0x41},{0x09,0x09,0x09},{0x09,0x09,0x09},
                {0xB2,0xB2,0xB2},{0x74,0xB0,0xB2},{0x7D,0xA5,0xB2},{0x98,0x77,0xA4},{0xB2,0x75,0xAE},{0xB2,0x77,0x7D},{0xB2,0x93,0x7B},{0xB2,0xA7,0x74},{0xB2,0xAC,0x6D},{0x96,0xA2,0x68},{0x74,0xA5,0x7A},{0x71,0xA9,0x98},{0x6B,0xB2,0xB0},{0x9A,0x9A,0x9A},{0x0B,0x0B,0x0B},{0x0B,0x0B,0x0B}
        }
};
// RP2C03B, RC2C03C, and RC2C05-03
const unsigned char Palette_PC10[8][64][3] =
{
        {       // none
                {0x6D,0x6D,0x6D},{0x00,0x24,0x92},{0x00,0x00,0xDB},{0x6D,0x49,0xDB},{0x92,0x00,0x6D},{0xB6,0x00,0x6D},{0xB6,0x24,0x00},{0x92,0x49,0x00},{0x6D,0x49,0x00},{0x24,0x49,0x00},{0x00,0x6D,0x24},{0x00,0x92,0x00},{0x00,0x49,0x49},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},
                {0xB6,0xB6,0xB6},{0x00,0x6D,0xDB},{0x00,0x49,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0xFF,0x00,0x92},{0xFF,0x00,0x00},{0xDB,0x6D,0x00},{0x92,0x6D,0x00},{0x24,0x92,0x00},{0x00,0x92,0x00},{0x00,0xB6,0x6D},{0x00,0x92,0x92},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},
                {0xFF,0xFF,0xFF},{0x6D,0xB6,0xFF},{0x92,0x92,0xFF},{0xDB,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0x00},{0xFF,0xB6,0x00},{0xDB,0xDB,0x00},{0x6D,0xDB,0x00},{0x00,0xFF,0x00},{0x49,0xFF,0xDB},{0x00,0xFF,0xFF},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},
                {0xFF,0xFF,0xFF},{0xB6,0xDB,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xB6},{0xFF,0xDB,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xB6,0xFF,0x49},{0x92,0xFF,0x6D},{0x49,0xFF,0xDB},{0x92,0xDB,0xFF},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00}
        },
        {       // red
                {0xFF,0x6D,0x6D},{0xFF,0x24,0x92},{0xFF,0x00,0xDB},{0xFF,0x49,0xDB},{0xFF,0x00,0x6D},{0xFF,0x00,0x6D},{0xFF,0x24,0x00},{0xFF,0x49,0x00},{0xFF,0x49,0x00},{0xFF,0x49,0x00},{0xFF,0x6D,0x24},{0xFF,0x92,0x00},{0xFF,0x49,0x49},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00},
                {0xFF,0xB6,0xB6},{0xFF,0x6D,0xDB},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0x92},{0xFF,0x00,0x00},{0xFF,0x6D,0x00},{0xFF,0x6D,0x00},{0xFF,0x92,0x00},{0xFF,0x92,0x00},{0xFF,0xB6,0x6D},{0xFF,0x92,0x92},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0x00},{0xFF,0xB6,0x00},{0xFF,0xDB,0x00},{0xFF,0xDB,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xB6},{0xFF,0xDB,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xDB,0xFF},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00}
        },
        {       // green
                {0x6D,0xFF,0x6D},{0x00,0xFF,0x92},{0x00,0xFF,0xDB},{0x6D,0xFF,0xDB},{0x92,0xFF,0x6D},{0xB6,0xFF,0x6D},{0xB6,0xFF,0x00},{0x92,0xFF,0x00},{0x6D,0xFF,0x00},{0x24,0xFF,0x00},{0x00,0xFF,0x24},{0x00,0xFF,0x00},{0x00,0xFF,0x49},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00},
                {0xB6,0xFF,0xB6},{0x00,0xFF,0xDB},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xDB,0xFF,0x00},{0x92,0xFF,0x00},{0x24,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x6D},{0x00,0xFF,0x92},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xDB,0xFF,0x00},{0x6D,0xFF,0x00},{0x00,0xFF,0x00},{0x49,0xFF,0xDB},{0x00,0xFF,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xB6,0xFF,0x49},{0x92,0xFF,0x6D},{0x49,0xFF,0xDB},{0x92,0xFF,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00}
        },
        {       // yellow
                {0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xB6},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00}
        },
        {       // blue
                {0x6D,0x6D,0xFF},{0x00,0x24,0xFF},{0x00,0x00,0xFF},{0x6D,0x49,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0xB6,0x24,0xFF},{0x92,0x49,0xFF},{0x6D,0x49,0xFF},{0x24,0x49,0xFF},{0x00,0x6D,0xFF},{0x00,0x92,0xFF},{0x00,0x49,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},
                {0xB6,0xB6,0xFF},{0x00,0x6D,0xFF},{0x00,0x49,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xDB,0x6D,0xFF},{0x92,0x6D,0xFF},{0x24,0x92,0xFF},{0x00,0x92,0xFF},{0x00,0xB6,0xFF},{0x00,0x92,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0x6D,0xB6,0xFF},{0x92,0x92,0xFF},{0xDB,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xDB,0xDB,0xFF},{0x6D,0xDB,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0xB6,0xDB,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0x49,0xFF,0xFF},{0x92,0xDB,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF}
        },
        {       // magenta
                {0xFF,0x6D,0xFF},{0xFF,0x24,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x49,0xFF},{0xFF,0x49,0xFF},{0xFF,0x49,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF}
        },
        {       // cyan
                {0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x92,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0x49,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF}
        },
        {       // white
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF}
        }
};
// RC2C03B
const unsigned char Palette_PC10_Alt[8][64][3] =
{
        {       // none
                {0x6D,0x6D,0x6D},{0x00,0x24,0x92},{0x00,0x00,0xDB},{0x6D,0x49,0xDB},{0x92,0x00,0x6D},{0xB6,0x00,0x6D},{0xB6,0x24,0x00},{0x92,0x49,0x00},{0x6D,0x49,0x00},{0x24,0x00,0x00},{0x00,0x6D,0x24},{0x00,0x92,0x00},{0x00,0x49,0x49},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},
                {0xB6,0xB6,0xB6},{0x00,0x24,0xDB},{0x00,0x49,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0xFF,0x00,0x92},{0xFF,0x00,0x00},{0xDB,0x6D,0x00},{0x92,0x6D,0x00},{0x24,0x92,0x00},{0x00,0x92,0x00},{0x00,0xB6,0x6D},{0x00,0x92,0x92},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},
                {0xFF,0xFF,0xFF},{0x6D,0xB6,0xFF},{0x92,0x92,0xFF},{0xDB,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x92,0x00},{0xFF,0xB6,0x00},{0xDB,0xDB,0x00},{0x6D,0x92,0x00},{0x00,0xFF,0x00},{0x49,0xFF,0xDB},{0x00,0xFF,0xFF},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00},
                {0xFF,0xFF,0xFF},{0xB6,0x92,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xB6},{0xFF,0xDB,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xB6,0xB6,0x49},{0x92,0xFF,0x6D},{0x49,0xFF,0xDB},{0x92,0xDB,0xFF},{0x00,0x00,0x00},{0x00,0x00,0x00},{0x00,0x00,0x00}
        },
        {       // red
                {0xFF,0x6D,0x6D},{0xFF,0x24,0x92},{0xFF,0x00,0xDB},{0xFF,0x49,0xDB},{0xFF,0x00,0x6D},{0xFF,0x00,0x6D},{0xFF,0x24,0x00},{0xFF,0x49,0x00},{0xFF,0x49,0x00},{0xFF,0x00,0x00},{0xFF,0x6D,0x24},{0xFF,0x92,0x00},{0xFF,0x49,0x49},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00},
                {0xFF,0xB6,0xB6},{0xFF,0x24,0xDB},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0x92},{0xFF,0x00,0x00},{0xFF,0x6D,0x00},{0xFF,0x6D,0x00},{0xFF,0x92,0x00},{0xFF,0x92,0x00},{0xFF,0xB6,0x6D},{0xFF,0x92,0x92},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x92,0x00},{0xFF,0xB6,0x00},{0xFF,0xDB,0x00},{0xFF,0x92,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xB6},{0xFF,0xDB,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xB6,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xDB,0xFF},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0x00,0x00}
        },
        {       // green
                {0x6D,0xFF,0x6D},{0x00,0xFF,0x92},{0x00,0xFF,0xDB},{0x6D,0xFF,0xDB},{0x92,0xFF,0x6D},{0xB6,0xFF,0x6D},{0xB6,0xFF,0x00},{0x92,0xFF,0x00},{0x6D,0xFF,0x00},{0x24,0xFF,0x00},{0x00,0xFF,0x24},{0x00,0xFF,0x00},{0x00,0xFF,0x49},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00},
                {0xB6,0xFF,0xB6},{0x00,0xFF,0xDB},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xDB,0xFF,0x00},{0x92,0xFF,0x00},{0x24,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x6D},{0x00,0xFF,0x92},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xDB,0xFF,0x00},{0x6D,0xFF,0x00},{0x00,0xFF,0x00},{0x49,0xFF,0xDB},{0x00,0xFF,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xB6,0xFF,0x49},{0x92,0xFF,0x6D},{0x49,0xFF,0xDB},{0x92,0xFF,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x00}
        },
        {       // yellow
                {0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xB6},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00}
        },
        {       // blue
                {0x6D,0x6D,0xFF},{0x00,0x24,0xFF},{0x00,0x00,0xFF},{0x6D,0x49,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0xB6,0x24,0xFF},{0x92,0x49,0xFF},{0x6D,0x49,0xFF},{0x24,0x00,0xFF},{0x00,0x6D,0xFF},{0x00,0x92,0xFF},{0x00,0x49,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},
                {0xB6,0xB6,0xFF},{0x00,0x24,0xFF},{0x00,0x49,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xDB,0x6D,0xFF},{0x92,0x6D,0xFF},{0x24,0x92,0xFF},{0x00,0x92,0xFF},{0x00,0xB6,0xFF},{0x00,0x92,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0x6D,0xB6,0xFF},{0x92,0x92,0xFF},{0xDB,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xDB,0xDB,0xFF},{0x6D,0x92,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0xB6,0x92,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xB6,0xFF},{0x92,0xFF,0xFF},{0x49,0xFF,0xFF},{0x92,0xDB,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF}
        },
        {       // magenta
                {0xFF,0x6D,0xFF},{0xFF,0x24,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x49,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0x24,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF}
        },
        {       // cyan
                {0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x92,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0x49,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF}
        },
        {       // white
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF}
        }
};
// RP2C04-0001
const unsigned char Palette_VS_0001[8][64][3] =
{
        {       // none
                {0xFF,0xB6,0xB6},{0xDB,0x6D,0xFF},{0xFF,0x00,0x00},{0x92,0x92,0xFF},{0x00,0x92,0x92},{0x24,0x49,0x00},{0x49,0x49,0x49},{0xFF,0x00,0x92},{0xFF,0xFF,0xFF},{0x6D,0x6D,0x6D},{0xFF,0xB6,0x00},{0xB6,0x00,0x6D},{0x92,0x00,0x6D},{0xDB,0xDB,0x00},{0x6D,0x49,0x00},{0xFF,0xFF,0xFF},
                {0x6D,0xB6,0xFF},{0xDB,0xB6,0x6D},{0x6D,0x24,0x00},{0x6D,0xDB,0x00},{0x92,0xDB,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xDB,0x92},{0x00,0x49,0xFF},{0xFF,0xDB,0x00},{0x49,0xFF,0xDB},{0x00,0x00,0x00},{0x49,0x00,0x00},{0xDB,0xDB,0xDB},{0x92,0x92,0x92},{0xFF,0x00,0xFF},{0x00,0x24,0x92},
                {0x00,0x00,0x6D},{0xB6,0xDB,0xFF},{0xFF,0xB6,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0xFF},{0x00,0x49,0x49},{0x00,0xB6,0x6D},{0xB6,0x00,0xFF},{0x00,0x00,0x00},{0x92,0x49,0x00},{0xFF,0x92,0xFF},{0xB6,0x24,0x00},{0x92,0x00,0xFF},{0x00,0x00,0xDB},{0xFF,0x92,0x00},{0x00,0x00,0x00},
                {0x00,0x00,0x00},{0x24,0x92,0x00},{0xB6,0xB6,0xB6},{0x00,0x6D,0x24},{0xB6,0xFF,0x49},{0x6D,0x49,0xDB},{0xFF,0xFF,0x00},{0xDB,0x6D,0x00},{0x00,0x49,0x00},{0x00,0x6D,0xDB},{0x00,0x92,0x00},{0x24,0x24,0x24},{0xFF,0xFF,0x6D},{0xFF,0x6D,0xFF},{0x92,0x6D,0x00},{0x92,0xFF,0x6D}
        },
        {       // red
                {0xFF,0xB6,0xB6},{0xFF,0x6D,0xFF},{0xFF,0x00,0x00},{0xFF,0x92,0xFF},{0xFF,0x92,0x92},{0xFF,0x49,0x00},{0xFF,0x49,0x49},{0xFF,0x00,0x92},{0xFF,0xFF,0xFF},{0xFF,0x6D,0x6D},{0xFF,0xB6,0x00},{0xFF,0x00,0x6D},{0xFF,0x00,0x6D},{0xFF,0xDB,0x00},{0xFF,0x49,0x00},{0xFF,0xFF,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0xB6,0x6D},{0xFF,0x24,0x00},{0xFF,0xDB,0x00},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0x92},{0xFF,0x49,0xFF},{0xFF,0xDB,0x00},{0xFF,0xFF,0xDB},{0xFF,0x00,0x00},{0xFF,0x00,0x00},{0xFF,0xDB,0xDB},{0xFF,0x92,0x92},{0xFF,0x00,0xFF},{0xFF,0x24,0x92},
                {0xFF,0x00,0x6D},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0x49,0x49},{0xFF,0xB6,0x6D},{0xFF,0x00,0xFF},{0xFF,0x00,0x00},{0xFF,0x49,0x00},{0xFF,0x92,0xFF},{0xFF,0x24,0x00},{0xFF,0x00,0xFF},{0xFF,0x00,0xDB},{0xFF,0x92,0x00},{0xFF,0x00,0x00},
                {0xFF,0x00,0x00},{0xFF,0x92,0x00},{0xFF,0xB6,0xB6},{0xFF,0x6D,0x24},{0xFF,0xFF,0x49},{0xFF,0x49,0xDB},{0xFF,0xFF,0x00},{0xFF,0x6D,0x00},{0xFF,0x49,0x00},{0xFF,0x6D,0xDB},{0xFF,0x92,0x00},{0xFF,0x24,0x24},{0xFF,0xFF,0x6D},{0xFF,0x6D,0xFF},{0xFF,0x6D,0x00},{0xFF,0xFF,0x6D}
        },
        {       // green
                {0xFF,0xFF,0xB6},{0xDB,0xFF,0xFF},{0xFF,0xFF,0x00},{0x92,0xFF,0xFF},{0x00,0xFF,0x92},{0x24,0xFF,0x00},{0x49,0xFF,0x49},{0xFF,0xFF,0x92},{0xFF,0xFF,0xFF},{0x6D,0xFF,0x6D},{0xFF,0xFF,0x00},{0xB6,0xFF,0x6D},{0x92,0xFF,0x6D},{0xDB,0xFF,0x00},{0x6D,0xFF,0x00},{0xFF,0xFF,0xFF},
                {0x6D,0xFF,0xFF},{0xDB,0xFF,0x6D},{0x6D,0xFF,0x00},{0x6D,0xFF,0x00},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0x92},{0x00,0xFF,0xFF},{0xFF,0xFF,0x00},{0x49,0xFF,0xDB},{0x00,0xFF,0x00},{0x49,0xFF,0x00},{0xDB,0xFF,0xDB},{0x92,0xFF,0x92},{0xFF,0xFF,0xFF},{0x00,0xFF,0x92},
                {0x00,0xFF,0x6D},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0xFF},{0x00,0xFF,0x49},{0x00,0xFF,0x6D},{0xB6,0xFF,0xFF},{0x00,0xFF,0x00},{0x92,0xFF,0x00},{0xFF,0xFF,0xFF},{0xB6,0xFF,0x00},{0x92,0xFF,0xFF},{0x00,0xFF,0xDB},{0xFF,0xFF,0x00},{0x00,0xFF,0x00},
                {0x00,0xFF,0x00},{0x24,0xFF,0x00},{0xB6,0xFF,0xB6},{0x00,0xFF,0x24},{0xB6,0xFF,0x49},{0x6D,0xFF,0xDB},{0xFF,0xFF,0x00},{0xDB,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0xDB},{0x00,0xFF,0x00},{0x24,0xFF,0x24},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0x92,0xFF,0x00},{0x92,0xFF,0x6D}
        },
        {       // yellow
                {0xFF,0xFF,0xB6},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x92},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x92},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},
                {0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x24},{0xFF,0xFF,0x49},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x24},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D}
        },
        {       // blue
                {0xFF,0xB6,0xFF},{0xDB,0x6D,0xFF},{0xFF,0x00,0xFF},{0x92,0x92,0xFF},{0x00,0x92,0xFF},{0x24,0x49,0xFF},{0x49,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0x6D,0x6D,0xFF},{0xFF,0xB6,0xFF},{0xB6,0x00,0xFF},{0x92,0x00,0xFF},{0xDB,0xDB,0xFF},{0x6D,0x49,0xFF},{0xFF,0xFF,0xFF},
                {0x6D,0xB6,0xFF},{0xDB,0xB6,0xFF},{0x6D,0x24,0xFF},{0x6D,0xDB,0xFF},{0x92,0xDB,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xDB,0xFF},{0x00,0x49,0xFF},{0xFF,0xDB,0xFF},{0x49,0xFF,0xFF},{0x00,0x00,0xFF},{0x49,0x00,0xFF},{0xDB,0xDB,0xFF},{0x92,0x92,0xFF},{0xFF,0x00,0xFF},{0x00,0x24,0xFF},
                {0x00,0x00,0xFF},{0xB6,0xDB,0xFF},{0xFF,0xB6,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0x49,0xFF},{0x00,0xB6,0xFF},{0xB6,0x00,0xFF},{0x00,0x00,0xFF},{0x92,0x49,0xFF},{0xFF,0x92,0xFF},{0xB6,0x24,0xFF},{0x92,0x00,0xFF},{0x00,0x00,0xFF},{0xFF,0x92,0xFF},{0x00,0x00,0xFF},
                {0x00,0x00,0xFF},{0x24,0x92,0xFF},{0xB6,0xB6,0xFF},{0x00,0x6D,0xFF},{0xB6,0xFF,0xFF},{0x6D,0x49,0xFF},{0xFF,0xFF,0xFF},{0xDB,0x6D,0xFF},{0x00,0x49,0xFF},{0x00,0x6D,0xFF},{0x00,0x92,0xFF},{0x24,0x24,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0x92,0x6D,0xFF},{0x92,0xFF,0xFF}
        },
        {       // magenta
                {0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x92,0xFF},{0xFF,0x92,0xFF},{0xFF,0x49,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x24,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},
                {0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0x92,0xFF},{0xFF,0x24,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0x00,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0x24,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xFF,0xFF}
        },
        {       // cyan
                {0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0x24,0xFF,0xFF},{0x49,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0x6D,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0x00,0xFF,0xFF},{0x24,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x24,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0x92,0xFF,0xFF}
        },
        {       // white
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF}
        }
};
// RP2C04-0002
const unsigned char Palette_VS_0002[8][64][3] =
{
        {       // none
                {0x00,0x00,0x00},{0xFF,0xB6,0x00},{0x92,0x6D,0x00},{0xB6,0xFF,0x49},{0x92,0xFF,0x6D},{0xFF,0x6D,0xFF},{0x00,0x92,0x92},{0xB6,0xDB,0xFF},{0xFF,0x00,0x00},{0x92,0x00,0xFF},{0xFF,0xFF,0x6D},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xDB,0x6D,0xFF},{0x92,0xDB,0xFF},{0x00,0x92,0x00},
                {0x00,0x49,0x00},{0x6D,0xB6,0xFF},{0xB6,0x24,0x00},{0xDB,0xDB,0xDB},{0x00,0xB6,0x6D},{0x6D,0xDB,0x00},{0x49,0x00,0x00},{0x92,0x92,0xFF},{0x49,0x49,0x49},{0xFF,0x00,0xFF},{0x00,0x00,0x6D},{0x49,0xFF,0xDB},{0xDB,0xB6,0xFF},{0x6D,0x49,0x00},{0x00,0x00,0x00},{0x6D,0x49,0xDB},
                {0x92,0x00,0x6D},{0xFF,0xDB,0x92},{0xFF,0x92,0x00},{0xFF,0xB6,0xFF},{0x00,0x6D,0xDB},{0x6D,0x24,0x00},{0xB6,0xB6,0xB6},{0x00,0x00,0xDB},{0xB6,0x00,0xFF},{0xFF,0xDB,0x00},{0x6D,0x6D,0x6D},{0x24,0x49,0x00},{0x00,0x49,0xFF},{0x00,0x00,0x00},{0xDB,0xDB,0x00},{0xFF,0xFF,0xFF},
                {0xDB,0xB6,0x6D},{0x24,0x24,0x24},{0x00,0xFF,0x00},{0xDB,0x6D,0x00},{0x00,0x49,0x49},{0x00,0x24,0x92},{0xFF,0x00,0x92},{0x24,0x92,0x00},{0x00,0x00,0x00},{0x00,0xFF,0xFF},{0x92,0x49,0x00},{0xFF,0xFF,0x00},{0xFF,0xB6,0xB6},{0xB6,0x00,0x6D},{0x00,0x6D,0x24},{0x92,0x92,0x92}
        },
        {       // red
                {0xFF,0x00,0x00},{0xFF,0xB6,0x00},{0xFF,0x6D,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0x6D,0xFF},{0xFF,0x92,0x92},{0xFF,0xDB,0xFF},{0xFF,0x00,0x00},{0xFF,0x00,0xFF},{0xFF,0xFF,0x6D},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x92,0x00},
                {0xFF,0x49,0x00},{0xFF,0xB6,0xFF},{0xFF,0x24,0x00},{0xFF,0xDB,0xDB},{0xFF,0xB6,0x6D},{0xFF,0xDB,0x00},{0xFF,0x00,0x00},{0xFF,0x92,0xFF},{0xFF,0x49,0x49},{0xFF,0x00,0xFF},{0xFF,0x00,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xB6,0xFF},{0xFF,0x49,0x00},{0xFF,0x00,0x00},{0xFF,0x49,0xDB},
                {0xFF,0x00,0x6D},{0xFF,0xDB,0x92},{0xFF,0x92,0x00},{0xFF,0xB6,0xFF},{0xFF,0x6D,0xDB},{0xFF,0x24,0x00},{0xFF,0xB6,0xB6},{0xFF,0x00,0xDB},{0xFF,0x00,0xFF},{0xFF,0xDB,0x00},{0xFF,0x6D,0x6D},{0xFF,0x49,0x00},{0xFF,0x49,0xFF},{0xFF,0x00,0x00},{0xFF,0xDB,0x00},{0xFF,0xFF,0xFF},
                {0xFF,0xB6,0x6D},{0xFF,0x24,0x24},{0xFF,0xFF,0x00},{0xFF,0x6D,0x00},{0xFF,0x49,0x49},{0xFF,0x24,0x92},{0xFF,0x00,0x92},{0xFF,0x92,0x00},{0xFF,0x00,0x00},{0xFF,0xFF,0xFF},{0xFF,0x49,0x00},{0xFF,0xFF,0x00},{0xFF,0xB6,0xB6},{0xFF,0x00,0x6D},{0xFF,0x6D,0x24},{0xFF,0x92,0x92}
        },
        {       // green
                {0x00,0xFF,0x00},{0xFF,0xFF,0x00},{0x92,0xFF,0x00},{0xB6,0xFF,0x49},{0x92,0xFF,0x6D},{0xFF,0xFF,0xFF},{0x00,0xFF,0x92},{0xB6,0xFF,0xFF},{0xFF,0xFF,0x00},{0x92,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0x00},
                {0x00,0xFF,0x00},{0x6D,0xFF,0xFF},{0xB6,0xFF,0x00},{0xDB,0xFF,0xDB},{0x00,0xFF,0x6D},{0x6D,0xFF,0x00},{0x49,0xFF,0x00},{0x92,0xFF,0xFF},{0x49,0xFF,0x49},{0xFF,0xFF,0xFF},{0x00,0xFF,0x6D},{0x49,0xFF,0xDB},{0xDB,0xFF,0xFF},{0x6D,0xFF,0x00},{0x00,0xFF,0x00},{0x6D,0xFF,0xDB},
                {0x92,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0x00,0xFF,0xDB},{0x6D,0xFF,0x00},{0xB6,0xFF,0xB6},{0x00,0xFF,0xDB},{0xB6,0xFF,0xFF},{0xFF,0xFF,0x00},{0x6D,0xFF,0x6D},{0x24,0xFF,0x00},{0x00,0xFF,0xFF},{0x00,0xFF,0x00},{0xDB,0xFF,0x00},{0xFF,0xFF,0xFF},
                {0xDB,0xFF,0x6D},{0x24,0xFF,0x24},{0x00,0xFF,0x00},{0xDB,0xFF,0x00},{0x00,0xFF,0x49},{0x00,0xFF,0x92},{0xFF,0xFF,0x92},{0x24,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0xFF},{0x92,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xB6},{0xB6,0xFF,0x6D},{0x00,0xFF,0x24},{0x92,0xFF,0x92}
        },
        {       // yellow
                {0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x49},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},
                {0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0xB6},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0x6D},{0xFF,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x92},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x24},{0xFF,0xFF,0x92}
        },
        {       // blue
                {0x00,0x00,0xFF},{0xFF,0xB6,0xFF},{0x92,0x6D,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0x6D,0xFF},{0x00,0x92,0xFF},{0xB6,0xDB,0xFF},{0xFF,0x00,0xFF},{0x92,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xDB,0x6D,0xFF},{0x92,0xDB,0xFF},{0x00,0x92,0xFF},
                {0x00,0x49,0xFF},{0x6D,0xB6,0xFF},{0xB6,0x24,0xFF},{0xDB,0xDB,0xFF},{0x00,0xB6,0xFF},{0x6D,0xDB,0xFF},{0x49,0x00,0xFF},{0x92,0x92,0xFF},{0x49,0x49,0xFF},{0xFF,0x00,0xFF},{0x00,0x00,0xFF},{0x49,0xFF,0xFF},{0xDB,0xB6,0xFF},{0x6D,0x49,0xFF},{0x00,0x00,0xFF},{0x6D,0x49,0xFF},
                {0x92,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0x00,0x6D,0xFF},{0x6D,0x24,0xFF},{0xB6,0xB6,0xFF},{0x00,0x00,0xFF},{0xB6,0x00,0xFF},{0xFF,0xDB,0xFF},{0x6D,0x6D,0xFF},{0x24,0x49,0xFF},{0x00,0x49,0xFF},{0x00,0x00,0xFF},{0xDB,0xDB,0xFF},{0xFF,0xFF,0xFF},
                {0xDB,0xB6,0xFF},{0x24,0x24,0xFF},{0x00,0xFF,0xFF},{0xDB,0x6D,0xFF},{0x00,0x49,0xFF},{0x00,0x24,0xFF},{0xFF,0x00,0xFF},{0x24,0x92,0xFF},{0x00,0x00,0xFF},{0x00,0xFF,0xFF},{0x92,0x49,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xB6,0x00,0xFF},{0x00,0x6D,0xFF},{0x92,0x92,0xFF}
        },
        {       // magenta
                {0xFF,0x00,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x92,0xFF},
                {0xFF,0x49,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x24,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0x92,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},
                {0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x24,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0x24,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x24,0xFF},{0xFF,0x00,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF}
        },
        {       // cyan
                {0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},
                {0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x49,0xFF,0xFF},{0x92,0xFF,0xFF},{0x49,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},
                {0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xDB,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF}
        },
        {       // white
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF}
        }
};
// RP2C04-0003
const unsigned char Palette_VS_0003[8][64][3] =
{
        {       // none
                {0xB6,0x00,0xFF},{0xFF,0x6D,0xFF},{0x92,0xFF,0x6D},{0xB6,0xB6,0xB6},{0x00,0x92,0x00},{0xFF,0xFF,0xFF},{0xB6,0xDB,0xFF},{0x24,0x49,0x00},{0x00,0x24,0x92},{0x00,0x00,0x00},{0xFF,0xDB,0x92},{0x6D,0x49,0x00},{0xFF,0x00,0x92},{0xDB,0xDB,0xDB},{0xDB,0xB6,0x6D},{0x92,0xDB,0xFF},
                {0x92,0x92,0xFF},{0x00,0x92,0x92},{0xB6,0x00,0x6D},{0x00,0x49,0xFF},{0x24,0x92,0x00},{0x92,0x6D,0x00},{0xDB,0x6D,0x00},{0x00,0xB6,0x6D},{0x6D,0x6D,0x6D},{0x6D,0x49,0xDB},{0x00,0x00,0x00},{0x00,0x00,0xDB},{0xFF,0x00,0x00},{0xB6,0x24,0x00},{0xFF,0x92,0xFF},{0xFF,0xB6,0xB6},
                {0xDB,0x6D,0xFF},{0x00,0x49,0x00},{0x00,0x00,0x6D},{0xFF,0xFF,0x00},{0x24,0x24,0x24},{0xFF,0xB6,0x00},{0xFF,0x92,0x00},{0xFF,0xFF,0xFF},{0x6D,0xDB,0x00},{0x92,0x00,0x6D},{0x6D,0xB6,0xFF},{0xFF,0x00,0xFF},{0x00,0x6D,0xDB},{0x92,0x92,0x92},{0x00,0x00,0x00},{0x6D,0x24,0x00},
                {0x00,0xFF,0xFF},{0x49,0x00,0x00},{0xB6,0xFF,0x49},{0xFF,0xB6,0xFF},{0x92,0x49,0x00},{0x00,0xFF,0x00},{0xDB,0xDB,0x00},{0x49,0x49,0x49},{0x00,0x6D,0x24},{0x00,0x00,0x00},{0xDB,0xB6,0xFF},{0xFF,0xFF,0x6D},{0x92,0x00,0xFF},{0x49,0xFF,0xDB},{0xFF,0xDB,0x00},{0x00,0x49,0x49}
        },
        {       // red
                {0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xB6,0xB6},{0xFF,0x92,0x00},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0x00},{0xFF,0x24,0x92},{0xFF,0x00,0x00},{0xFF,0xDB,0x92},{0xFF,0x49,0x00},{0xFF,0x00,0x92},{0xFF,0xDB,0xDB},{0xFF,0xB6,0x6D},{0xFF,0xDB,0xFF},
                {0xFF,0x92,0xFF},{0xFF,0x92,0x92},{0xFF,0x00,0x6D},{0xFF,0x49,0xFF},{0xFF,0x92,0x00},{0xFF,0x6D,0x00},{0xFF,0x6D,0x00},{0xFF,0xB6,0x6D},{0xFF,0x6D,0x6D},{0xFF,0x49,0xDB},{0xFF,0x00,0x00},{0xFF,0x00,0xDB},{0xFF,0x00,0x00},{0xFF,0x24,0x00},{0xFF,0x92,0xFF},{0xFF,0xB6,0xB6},
                {0xFF,0x6D,0xFF},{0xFF,0x49,0x00},{0xFF,0x00,0x6D},{0xFF,0xFF,0x00},{0xFF,0x24,0x24},{0xFF,0xB6,0x00},{0xFF,0x92,0x00},{0xFF,0xFF,0xFF},{0xFF,0xDB,0x00},{0xFF,0x00,0x6D},{0xFF,0xB6,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xDB},{0xFF,0x92,0x92},{0xFF,0x00,0x00},{0xFF,0x24,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0x00,0x00},{0xFF,0xFF,0x49},{0xFF,0xB6,0xFF},{0xFF,0x49,0x00},{0xFF,0xFF,0x00},{0xFF,0xDB,0x00},{0xFF,0x49,0x49},{0xFF,0x6D,0x24},{0xFF,0x00,0x00},{0xFF,0xB6,0xFF},{0xFF,0xFF,0x6D},{0xFF,0x00,0xFF},{0xFF,0xFF,0xDB},{0xFF,0xDB,0x00},{0xFF,0x49,0x49}
        },
        {       // green
                {0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0x6D},{0xB6,0xFF,0xB6},{0x00,0xFF,0x00},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x24,0xFF,0x00},{0x00,0xFF,0x92},{0x00,0xFF,0x00},{0xFF,0xFF,0x92},{0x6D,0xFF,0x00},{0xFF,0xFF,0x92},{0xDB,0xFF,0xDB},{0xDB,0xFF,0x6D},{0x92,0xFF,0xFF},
                {0x92,0xFF,0xFF},{0x00,0xFF,0x92},{0xB6,0xFF,0x6D},{0x00,0xFF,0xFF},{0x24,0xFF,0x00},{0x92,0xFF,0x00},{0xDB,0xFF,0x00},{0x00,0xFF,0x6D},{0x6D,0xFF,0x6D},{0x6D,0xFF,0xDB},{0x00,0xFF,0x00},{0x00,0xFF,0xDB},{0xFF,0xFF,0x00},{0xB6,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xB6},
                {0xDB,0xFF,0xFF},{0x00,0xFF,0x00},{0x00,0xFF,0x6D},{0xFF,0xFF,0x00},{0x24,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0x6D,0xFF,0x00},{0x92,0xFF,0x6D},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xDB},{0x92,0xFF,0x92},{0x00,0xFF,0x00},{0x6D,0xFF,0x00},
                {0x00,0xFF,0xFF},{0x49,0xFF,0x00},{0xB6,0xFF,0x49},{0xFF,0xFF,0xFF},{0x92,0xFF,0x00},{0x00,0xFF,0x00},{0xDB,0xFF,0x00},{0x49,0xFF,0x49},{0x00,0xFF,0x24},{0x00,0xFF,0x00},{0xDB,0xFF,0xFF},{0xFF,0xFF,0x6D},{0x92,0xFF,0xFF},{0x49,0xFF,0xDB},{0xFF,0xFF,0x00},{0x00,0xFF,0x49}
        },
        {       // yellow
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x92},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0x92},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xB6},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49}
        },
        {       // blue
                {0xB6,0x00,0xFF},{0xFF,0x6D,0xFF},{0x92,0xFF,0xFF},{0xB6,0xB6,0xFF},{0x00,0x92,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xDB,0xFF},{0x24,0x49,0xFF},{0x00,0x24,0xFF},{0x00,0x00,0xFF},{0xFF,0xDB,0xFF},{0x6D,0x49,0xFF},{0xFF,0x00,0xFF},{0xDB,0xDB,0xFF},{0xDB,0xB6,0xFF},{0x92,0xDB,0xFF},
                {0x92,0x92,0xFF},{0x00,0x92,0xFF},{0xB6,0x00,0xFF},{0x00,0x49,0xFF},{0x24,0x92,0xFF},{0x92,0x6D,0xFF},{0xDB,0x6D,0xFF},{0x00,0xB6,0xFF},{0x6D,0x6D,0xFF},{0x6D,0x49,0xFF},{0x00,0x00,0xFF},{0x00,0x00,0xFF},{0xFF,0x00,0xFF},{0xB6,0x24,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},
                {0xDB,0x6D,0xFF},{0x00,0x49,0xFF},{0x00,0x00,0xFF},{0xFF,0xFF,0xFF},{0x24,0x24,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xDB,0xFF},{0x92,0x00,0xFF},{0x6D,0xB6,0xFF},{0xFF,0x00,0xFF},{0x00,0x6D,0xFF},{0x92,0x92,0xFF},{0x00,0x00,0xFF},{0x6D,0x24,0xFF},
                {0x00,0xFF,0xFF},{0x49,0x00,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xB6,0xFF},{0x92,0x49,0xFF},{0x00,0xFF,0xFF},{0xDB,0xDB,0xFF},{0x49,0x49,0xFF},{0x00,0x6D,0xFF},{0x00,0x00,0xFF},{0xDB,0xB6,0xFF},{0xFF,0xFF,0xFF},{0x92,0x00,0xFF},{0x49,0xFF,0xFF},{0xFF,0xDB,0xFF},{0x00,0x49,0xFF}
        },
        {       // magenta
                {0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0xFF},{0xFF,0x24,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xDB,0xFF},
                {0xFF,0x92,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0x92,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},
                {0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x24,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x24,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x49,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x49,0xFF}
        },
        {       // cyan
                {0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x24,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x92,0xFF,0xFF},
                {0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0x24,0xFF,0xFF},{0x92,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x24,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x92,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0x6D,0xFF,0xFF},
                {0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0x49,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF}
        },
        {       // white
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF}
        }
};
// RP2C04-0004
const unsigned char Palette_VS_0004[8][64][3] =
{
        {       // none
                {0x92,0x6D,0x00},{0x6D,0x49,0xDB},{0x00,0x92,0x92},{0xDB,0xDB,0x00},{0x00,0x00,0x00},{0xFF,0xB6,0xB6},{0x00,0x24,0x92},{0xDB,0x6D,0x00},{0xB6,0xB6,0xB6},{0x6D,0x24,0x00},{0x00,0xFF,0x00},{0x00,0x00,0x6D},{0xFF,0xDB,0x92},{0xFF,0xFF,0x00},{0x00,0x92,0x00},{0xB6,0xFF,0x49},
                {0xFF,0x6D,0xFF},{0x49,0x00,0x00},{0x00,0x49,0xFF},{0xFF,0x92,0xFF},{0x00,0x00,0x00},{0x49,0x49,0x49},{0xB6,0x24,0x00},{0xFF,0x92,0x00},{0xDB,0xB6,0x6D},{0x00,0xB6,0x6D},{0x92,0x92,0xFF},{0x24,0x92,0x00},{0x92,0x00,0x6D},{0x00,0x00,0x00},{0x92,0xFF,0x6D},{0x6D,0xB6,0xFF},
                {0xB6,0x00,0x6D},{0x00,0x6D,0x24},{0x92,0x49,0x00},{0x00,0x00,0xDB},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0x6D,0x6D,0x6D},{0xFF,0x00,0x92},{0x00,0x49,0x49},{0xDB,0xDB,0xDB},{0x00,0x6D,0xDB},{0x00,0x49,0x00},{0x24,0x24,0x24},{0xFF,0xFF,0x6D},{0x92,0x92,0x92},{0xFF,0x00,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0x6D,0x49,0x00},{0xFF,0x00,0x00},{0xFF,0xDB,0x00},{0x49,0xFF,0xDB},{0xFF,0xFF,0xFF},{0x92,0xDB,0xFF},{0x00,0x00,0x00},{0xFF,0xB6,0x00},{0xDB,0x6D,0xFF},{0xB6,0xDB,0xFF},{0x6D,0xDB,0x00},{0xDB,0xB6,0xFF},{0x00,0xFF,0xFF},{0x24,0x49,0x00}
        },
        {       // red
                {0xFF,0x6D,0x00},{0xFF,0x49,0xDB},{0xFF,0x92,0x92},{0xFF,0xDB,0x00},{0xFF,0x00,0x00},{0xFF,0xB6,0xB6},{0xFF,0x24,0x92},{0xFF,0x6D,0x00},{0xFF,0xB6,0xB6},{0xFF,0x24,0x00},{0xFF,0xFF,0x00},{0xFF,0x00,0x6D},{0xFF,0xDB,0x92},{0xFF,0xFF,0x00},{0xFF,0x92,0x00},{0xFF,0xFF,0x49},
                {0xFF,0x6D,0xFF},{0xFF,0x00,0x00},{0xFF,0x49,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0x00},{0xFF,0x49,0x49},{0xFF,0x24,0x00},{0xFF,0x92,0x00},{0xFF,0xB6,0x6D},{0xFF,0xB6,0x6D},{0xFF,0x92,0xFF},{0xFF,0x92,0x00},{0xFF,0x00,0x6D},{0xFF,0x00,0x00},{0xFF,0xFF,0x6D},{0xFF,0xB6,0xFF},
                {0xFF,0x00,0x6D},{0xFF,0x6D,0x24},{0xFF,0x49,0x00},{0xFF,0x00,0xDB},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0x6D},{0xFF,0x00,0x92},{0xFF,0x49,0x49},{0xFF,0xDB,0xDB},{0xFF,0x6D,0xDB},{0xFF,0x49,0x00},{0xFF,0x24,0x24},{0xFF,0xFF,0x6D},{0xFF,0x92,0x92},{0xFF,0x00,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0x00},{0xFF,0x00,0x00},{0xFF,0xDB,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0x00},{0xFF,0xB6,0x00},{0xFF,0x6D,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xDB,0x00},{0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0x00}
        },
        {       // green
                {0x92,0xFF,0x00},{0x6D,0xFF,0xDB},{0x00,0xFF,0x92},{0xDB,0xFF,0x00},{0x00,0xFF,0x00},{0xFF,0xFF,0xB6},{0x00,0xFF,0x92},{0xDB,0xFF,0x00},{0xB6,0xFF,0xB6},{0x6D,0xFF,0x00},{0x00,0xFF,0x00},{0x00,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0x00,0xFF,0x00},{0xB6,0xFF,0x49},
                {0xFF,0xFF,0xFF},{0x49,0xFF,0x00},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0x00},{0x49,0xFF,0x49},{0xB6,0xFF,0x00},{0xFF,0xFF,0x00},{0xDB,0xFF,0x6D},{0x00,0xFF,0x6D},{0x92,0xFF,0xFF},{0x24,0xFF,0x00},{0x92,0xFF,0x6D},{0x00,0xFF,0x00},{0x92,0xFF,0x6D},{0x6D,0xFF,0xFF},
                {0xB6,0xFF,0x6D},{0x00,0xFF,0x24},{0x92,0xFF,0x00},{0x00,0xFF,0xDB},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x6D,0xFF,0x6D},{0xFF,0xFF,0x92},{0x00,0xFF,0x49},{0xDB,0xFF,0xDB},{0x00,0xFF,0xDB},{0x00,0xFF,0x00},{0x24,0xFF,0x24},{0xFF,0xFF,0x6D},{0x92,0xFF,0x92},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0x49,0xFF,0xDB},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0x00},{0xFF,0xFF,0x00},{0xDB,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x6D,0xFF,0x00},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x24,0xFF,0x00}
        },
        {       // yellow
                {0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0xB6},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x49},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x00},{0xFF,0xFF,0x6D},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0x6D},{0xFF,0xFF,0x24},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0x49},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xDB},{0xFF,0xFF,0x00},{0xFF,0xFF,0x24},{0xFF,0xFF,0x6D},{0xFF,0xFF,0x92},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xDB},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0x00}
        },
        {       // blue
                {0x92,0x6D,0xFF},{0x6D,0x49,0xFF},{0x00,0x92,0xFF},{0xDB,0xDB,0xFF},{0x00,0x00,0xFF},{0xFF,0xB6,0xFF},{0x00,0x24,0xFF},{0xDB,0x6D,0xFF},{0xB6,0xB6,0xFF},{0x6D,0x24,0xFF},{0x00,0xFF,0xFF},{0x00,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0x00,0x92,0xFF},{0xB6,0xFF,0xFF},
                {0xFF,0x6D,0xFF},{0x49,0x00,0xFF},{0x00,0x49,0xFF},{0xFF,0x92,0xFF},{0x00,0x00,0xFF},{0x49,0x49,0xFF},{0xB6,0x24,0xFF},{0xFF,0x92,0xFF},{0xDB,0xB6,0xFF},{0x00,0xB6,0xFF},{0x92,0x92,0xFF},{0x24,0x92,0xFF},{0x92,0x00,0xFF},{0x00,0x00,0xFF},{0x92,0xFF,0xFF},{0x6D,0xB6,0xFF},
                {0xB6,0x00,0xFF},{0x00,0x6D,0xFF},{0x92,0x49,0xFF},{0x00,0x00,0xFF},{0x92,0x00,0xFF},{0xB6,0x00,0xFF},{0x6D,0x6D,0xFF},{0xFF,0x00,0xFF},{0x00,0x49,0xFF},{0xDB,0xDB,0xFF},{0x00,0x6D,0xFF},{0x00,0x49,0xFF},{0x24,0x24,0xFF},{0xFF,0xFF,0xFF},{0x92,0x92,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0x6D,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0x49,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xDB,0xFF},{0x00,0x00,0xFF},{0xFF,0xB6,0xFF},{0xDB,0x6D,0xFF},{0xB6,0xDB,0xFF},{0x6D,0xDB,0xFF},{0xDB,0xB6,0xFF},{0x00,0xFF,0xFF},{0x24,0x49,0xFF}
        },
        {       // magenta
                {0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x92,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x24,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x24,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x92,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0x24,0xFF},{0xFF,0x92,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x92,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xB6,0xFF},
                {0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x00,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x00,0xFF},{0xFF,0x49,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x6D,0xFF},{0xFF,0x49,0xFF},{0xFF,0x24,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x92,0xFF},{0xFF,0x00,0xFF},
                {0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0xFF},{0xFF,0x00,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xDB,0xFF},{0xFF,0x00,0xFF},{0xFF,0xB6,0xFF},{0xFF,0x6D,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xDB,0xFF},{0xFF,0xB6,0xFF},{0xFF,0xFF,0xFF},{0xFF,0x49,0xFF}
        },
        {       // cyan
                {0x92,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x6D,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0xB6,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0x49,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0x49,0xFF,0xFF},{0xB6,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0x24,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0x6D,0xFF,0xFF},
                {0xB6,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0x92,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x00,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x00,0xFF,0xFF},{0x24,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x49,0xFF,0xFF},{0xFF,0xFF,0xFF},{0x92,0xFF,0xFF},{0x00,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xDB,0xFF,0xFF},{0xB6,0xFF,0xFF},{0x6D,0xFF,0xFF},{0xDB,0xFF,0xFF},{0x00,0xFF,0xFF},{0x24,0xFF,0xFF}
        },
        {       // white
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},
                {0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF}
        }
};

// Colour Emphasis coefficients for imported palettes
const double Emphasis[8][3] =
{
        {1.00,1.00,1.00},       // black
        {1.00,0.85,0.85},       // red
        {0.85,1.00,0.85},       // green
        {0.85,0.85,0.70},       // yellow
        {0.85,0.85,1.00},       // blue
        {0.85,0.70,0.85},       // magenta
        {0.70,0.85,0.85},       // cyan
        {0.70,0.70,0.70}        // white
};

const int EmphasisOrder[NES::REGION_MAX][8] =
{
        {0,0,0,0,0,0,0,0},      // NONE
        {0,1,2,3,4,5,6,7},      // NTSC
        {0,2,1,3,4,6,5,7},      // PAL
        {0,2,1,3,4,6,5,7}       // Dendy
};

#define CLAMP(x) (((x) > 255) ? 255 : (((x) < 0) ? 0 : (x)))

void    GenerateNTSC (int hue, int sat)
{
        const double black = 0.312;
        const double white = 1.100;
        static const double voltage[4][4] = {
                {0.616,0.840,1.100,1.100}, // #0
                {0.228,0.312,0.552,0.880}, // #D
                {0.500,0.676,0.896,0.896}, // #0 Emph
                {0.192,0.256,0.448,0.712}  // #D Emph
        };

        static const char phases[12][12] = {
                {0,0,0,1,1,1,1,1,1,0,0,0},
                {0,0,1,1,1,1,1,1,0,0,0,0},      // blue
                {0,1,1,1,1,1,1,0,0,0,0,0},
                {1,1,1,1,1,1,0,0,0,0,0,0},      // magenta
                {1,1,1,1,1,0,0,0,0,0,0,1},
                {1,1,1,1,0,0,0,0,0,0,1,1},      // red
                {1,1,1,0,0,0,0,0,0,1,1,1},
                {1,1,0,0,0,0,0,0,1,1,1,1},      // yellow
                {1,0,0,0,0,0,0,1,1,1,1,1},
                {0,0,0,0,0,0,1,1,1,1,1,1},      // green
                {0,0,0,0,0,1,1,1,1,1,1,0},
                {0,0,0,0,1,1,1,1,1,1,0,0},      // cyan
        };
        static const char emphasis[8][12] = {
                {0,0,0,0,0,0,0,0,0,0,0,0},      // none
                {0,0,0,0,1,1,1,1,1,1,0,0},      // red
                {1,1,1,1,1,1,0,0,0,0,0,0},      // green
                {1,1,1,1,1,1,1,1,1,1,0,0},      // yellow
                {1,1,0,0,0,0,0,0,1,1,1,1},      // blue
                {1,1,0,0,1,1,1,1,1,1,1,1},      // magenta
                {1,1,1,1,1,1,0,0,1,1,1,1},      // cyan
                {1,1,1,1,1,1,1,1,1,1,1,1}       // all
        };

        int i, x, y, z;
        for (x = 0; x < 8; x++) 
        {
                int _x = EmphasisOrder[NES::CurRegion][x];
                for (y = 0; y < 4; y++)
                {
                        for (z = 0; z < 16; z++)
                        {
                                double wave[12];
                                for (i = 0; i < 12; i++)
                                {
                                        bool emph = ((emphasis[_x][i]) && (z < 14));
                                        if (z == 0)
                                                wave[i] = voltage[emph ? 2 : 0][y];
                                        else if (z < 13)
                                                wave[i] = phases[z-1][i] ? voltage[emph ? 2 : 0][y] : voltage[emph ? 3 : 1][y];
                                        else if (z == 13)
                                                wave[i] = voltage[emph ? 3 : 1][y];
                                        else    wave[i] = black;
                                        wave[i] = (wave[i] - black) / (white - black);
                                }

                                double Y = 0, I = 0, Q = 0;
                                double phase = hue / 30.0;
                                double S = 1.865 * (sat / 50.0);
                                for (i = 0; i < 12; i++)
                                {
                                        double L = wave[i] / 12.0;
                                        Y += L;
                                        I += L * cos(M_PI * (phase + i) / 6.0) * S;
                                        Q += L * sin(M_PI * (phase + i) / 6.0) * S;
                                }

                                double R, G, B;
                                R = Y + 0.947 * I + 0.624 * Q;
                                G = Y - 0.275 * I - 0.636 * Q;
                                B = Y - 1.109 * I + 1.709 * Q;

                                RawPalette[x][(y << 4) | z][0] = (unsigned char)CLAMP(R * 256);
                                RawPalette[x][(y << 4) | z][1] = (unsigned char)CLAMP(G * 256);
                                RawPalette[x][(y << 4) | z][2] = (unsigned char)CLAMP(B * 256);
                        }
                }
        }
}

void    LoadStaticPalette (const unsigned char input[][64][3])
{
        for (int i = 0; i < 8; i++)
                memcpy(RawPalette[i], *input[EmphasisOrder[NES::CurRegion][i]], sizeof(RawPalette[i]));
}

void    GeneratePAL (int sat)
{
        LoadStaticPalette(Palette_PAL);
        // TODO - implement
}

void    GenerateRGB (int pal, BOOL compat)
{
        if ((pal == PALETTE_PC10) || (pal == PALETTE_PC10_ALT))
        {
                if (pal == PALETTE_PC10)
                        LoadStaticPalette(Palette_PC10);
                if (pal == PALETTE_PC10_ALT)
                        LoadStaticPalette(Palette_PC10_Alt);
                if (compat)
                {
                        // insert grays in column D, allowing for emphasis
                        for (int i = 0; i < 8; i++)
                        {
                                if (!(i & 1))
                                {
                                        RawPalette[i][0x1D][0] = 0x24;
                                        RawPalette[i][0x2D][0] = 0x49;
                                        RawPalette[i][0x3D][0] = 0x92;
                                }
                                if (!(i & 2))
                                {
                                        RawPalette[i][0x1D][1] = 0x24;
                                        RawPalette[i][0x2D][1] = 0x49;
                                        RawPalette[i][0x3D][1] = 0x92;
                                }
                                if (!(i & 4))
                                {
                                        RawPalette[i][0x1D][2] = 0x24;
                                        RawPalette[i][0x2D][2] = 0x49;
                                        RawPalette[i][0x3D][2] = 0x92;
                                }
                        }
                }
        }
        else if (pal == PALETTE_VS1)
                LoadStaticPalette(Palette_VS_0001);
        else if (pal == PALETTE_VS2)
                LoadStaticPalette(Palette_VS_0002);
        else if (pal == PALETTE_VS3)
                LoadStaticPalette(Palette_VS_0003);
        else if (pal == PALETTE_VS4)
                LoadStaticPalette(Palette_VS_0004);
        else    MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_ILLEGAL_PALETTE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
}

BOOL    ImportPalette (const TCHAR *filename, BOOL load)
{
        int i, j;
        FILE *pal;
        // If no filename specified, use configured custom palette for current region
        if (filename == NULL)
                filename = CustPalette[NES::CurRegion];
        if (!_tcslen(filename))
                return FALSE;
        pal = _tfopen(filename, _T("rb"));
        if (!pal)
                return FALSE;
        fseek(pal, 0, SEEK_END);
        if (ftell(pal) < 0xC0)
        {       // too small
                fclose(pal);
                return FALSE;
        }
        if (!load)
        {
                fclose(pal);
                return TRUE;
        }
        if (ftell(pal) >= 0x600)
        {
                fseek(pal, 0, SEEK_SET);
                for (j = 0; j < 8; j++)
                {
                        for (i = 0; i < 64; i++)
                        {
                                fread(&RawPalette[j][i][0], 1, 1, pal);
                                fread(&RawPalette[j][i][1], 1, 1, pal);
                                fread(&RawPalette[j][i][2], 1, 1, pal);
                        }
                }
        }
        else
        {
                fseek(pal, 0, SEEK_SET);
                for (i = 0; i < 64; i++)
                {
                        fread(&RawPalette[0][i][0], 1, 1, pal);
                        fread(&RawPalette[0][i][1], 1, 1, pal);
                        fread(&RawPalette[0][i][2], 1, 1, pal);
                        for (j = 1; j < 8; j++)
                        {
                                RawPalette[j][i][0] = (unsigned char)CLAMP(RawPalette[0][i][0] * Emphasis[j][0]);
                                RawPalette[j][i][1] = (unsigned char)CLAMP(RawPalette[0][i][1] * Emphasis[j][1]);
                                RawPalette[j][i][2] = (unsigned char)CLAMP(RawPalette[0][i][2] * Emphasis[j][2]);
                        }
                }
        }
        fclose(pal);
        return TRUE;
}

void    LoadPalette (PALETTE PalNum)
{
        unsigned int RV, GV, BV;
        int i;
        // If no palette number specified, use configured palette for current region
        if (PalNum == PALETTE_MAX)
                PalNum = Palette[NES::CurRegion];
        if (PalNum == PALETTE_NTSC)
                GenerateNTSC(NTSChue, NTSCsat);
        else if (PalNum == PALETTE_PAL)
                GeneratePAL(PALsat);
        else if (PalNum == PALETTE_EXT)
        {
                if (!ImportPalette(NULL, TRUE))
                {
                        MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_LOAD_PALETTE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        LoadPalette(DefaultPalette[NES::CurRegion]);
                        return;
                }
        }
        else    GenerateRGB(PalNum, PC10compat);

        for (i = 0; i < 0x200; i++)
        {
                RV = RawPalette[i >> 6][i & 0x3F][0];
                GV = RawPalette[i >> 6][i & 0x3F][1];
                BV = RawPalette[i >> 6][i & 0x3F][2];

                Palette15[i] = (unsigned short)(((RV << 7) & 0x7C00) | ((GV << 2) & 0x03E0) | (BV >> 3));
                Palette16[i] = (unsigned short)(((RV << 8) & 0xF800) | ((GV << 3) & 0x07E0) | (BV >> 3));
                Palette32[i] = (RV << 16) | (GV << 8) | BV;
        }
        // redraw the screen with the new palette, but only if emulation isn't active
        if (!NES::Running)
                Update();
}
#undef CLIP

int hue, nsat, psat;
TCHAR extfn[MAX_PATH];

void    UpdatePalette (HWND hDlg, int pal)
{
        if (pal == PALETTE_NTSC)
        {
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_HUESLIDER), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_HUE), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_SATSLIDER), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_SAT), TRUE);

                SendDlgItemMessage(hDlg, IDC_PAL_HUESLIDER, TBM_SETPOS, TRUE, hue);
                SetDlgItemInt(hDlg, IDC_PAL_HUE, hue, TRUE);
                SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_SETPOS, TRUE, nsat);
                SetDlgItemInt(hDlg, IDC_PAL_SAT, nsat, FALSE);
        }
        else if (pal == PALETTE_PAL)
        {
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_HUESLIDER), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_HUE), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_SATSLIDER), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_SAT), TRUE);

                SendDlgItemMessage(hDlg, IDC_PAL_HUESLIDER, TBM_SETPOS, TRUE, -15);
                SetDlgItemInt(hDlg, IDC_PAL_HUE, (UINT)-15, TRUE);
                SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_SETPOS, TRUE, psat);
                SetDlgItemInt(hDlg, IDC_PAL_SAT, psat, FALSE);
        }
        else
        {
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_HUESLIDER), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_HUE), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_SATSLIDER), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_SAT), FALSE);

                SendDlgItemMessage(hDlg, IDC_PAL_HUESLIDER, TBM_SETPOS, TRUE, 0);
                SetDlgItemText(hDlg, IDC_PAL_HUE, _T("N/A"));
                SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_SETPOS, TRUE, 50);
                SetDlgItemText(hDlg, IDC_PAL_SAT, _T("N/A"));
        }
        if ((pal == PALETTE_PC10) || (pal == PALETTE_PC10_ALT))
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_PC10_COMPAT), TRUE);
        else    EnableWindow(GetDlgItem(hDlg, IDC_PAL_PC10_COMPAT), FALSE);

        if (ImportPalette(extfn, FALSE))
        {
                EnableWindow(GetDlgItem(hDlg, IDC_PAL_EXT), TRUE);
                if (pal == PALETTE_EXT)
                        ImportPalette(extfn, TRUE);
        }
        else    EnableWindow(GetDlgItem(hDlg, IDC_PAL_EXT), FALSE);

        RedrawWindow(hDlg, NULL, NULL, RDW_INVALIDATE);
}

BOOL inUpdate = FALSE;
PALETTE pal;
INT_PTR CALLBACK        PaletteConfigProc (HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
        static const int paltable[PALETTE_MAX] = {IDC_PAL_NTSC,IDC_PAL_PAL,IDC_PAL_PC10,IDC_PAL_VS1,IDC_PAL_VS2,IDC_PAL_VS3,IDC_PAL_VS4,IDC_PAL_EXT,IDC_PAL_PC10_ALT};
        static const int PalEntries[64] = {
                IDC_PAL_00,IDC_PAL_01,IDC_PAL_02,IDC_PAL_03,IDC_PAL_04,IDC_PAL_05,IDC_PAL_06,IDC_PAL_07,IDC_PAL_08,IDC_PAL_09,IDC_PAL_0A,IDC_PAL_0B,IDC_PAL_0C,IDC_PAL_0D,IDC_PAL_0E,IDC_PAL_0F,
                IDC_PAL_10,IDC_PAL_11,IDC_PAL_12,IDC_PAL_13,IDC_PAL_14,IDC_PAL_15,IDC_PAL_16,IDC_PAL_17,IDC_PAL_18,IDC_PAL_19,IDC_PAL_1A,IDC_PAL_1B,IDC_PAL_1C,IDC_PAL_1D,IDC_PAL_1E,IDC_PAL_1F,
                IDC_PAL_20,IDC_PAL_21,IDC_PAL_22,IDC_PAL_23,IDC_PAL_24,IDC_PAL_25,IDC_PAL_26,IDC_PAL_27,IDC_PAL_28,IDC_PAL_29,IDC_PAL_2A,IDC_PAL_2B,IDC_PAL_2C,IDC_PAL_2D,IDC_PAL_2E,IDC_PAL_2F,
                IDC_PAL_30,IDC_PAL_31,IDC_PAL_32,IDC_PAL_33,IDC_PAL_34,IDC_PAL_35,IDC_PAL_36,IDC_PAL_37,IDC_PAL_38,IDC_PAL_39,IDC_PAL_3A,IDC_PAL_3B,IDC_PAL_3C,IDC_PAL_3D,IDC_PAL_3E,IDC_PAL_3F
        };

        int wmId, wmEvent;
        OPENFILENAME ofn;
        PAINTSTRUCT ps;
        HDC hdc;

        int i;

        switch (uMsg)
        {
        case WM_INITDIALOG:
                SetWindowText(hDlg, Lang::GetString(LANG_DLG_PAL_TITLE));
                SetDlgItemText(hDlg, IDOK,               Lang::GetString(LANG_DLG_OK));
                SetDlgItemText(hDlg, IDCANCEL,           Lang::GetString(LANG_DLG_CANCEL));
                SetDlgItemText(hDlg, IDC_PAL_BROWSE,     Lang::GetString(LANG_DLG_BROWSE));
                SetDlgItemText(hDlg, IDC_PAL_EXT,        Lang::GetString(LANG_DLG_PAL_CUSTOM));
                SetDlgItemText(hDlg, IDC_PAL_PC10_COMPAT, Lang::GetString(LANG_DLG_PAL_EXTRAGRAY));
                {
                        HWND hChild = GetWindow(hDlg, GW_CHILD);
                        while (hChild) {
                                TCHAR txt[128] = {0};
                                GetWindowText(hChild, txt, 128);
                                struct { const TCHAR *orig; LangStringID id; } labels[] = {
                                        { _T("Palette"),          LANG_DLG_PAL_PALETTE  },
                                        { _T("&Hue:"),            LANG_DLG_PAL_HUE      },
                                        { _T("&Sat:"),            LANG_DLG_PAL_SAT      },
                                        { _T("Preview"),          LANG_DLG_PAL_PREVIEW  },
                                        { _T("Emphasis:"),        LANG_DLG_PAL_EMPHASIS },
                                        { _T("Note: Red and Green emphasis swapped for PAL/Dendy"), LANG_DLG_PAL_NOTE },
                                        { NULL, LANG_STRING_COUNT }
                                };
                                for (int k = 0; labels[k].orig != NULL; k++)
                                        if (_tcscmp(txt, labels[k].orig) == 0)
                                                { SetWindowText(hChild, Lang::GetString(labels[k].id)); break; }
                                hChild = GetWindow(hChild, GW_HWNDNEXT);
                        }
                }
                inUpdate = TRUE;
                hue = NTSChue;
                nsat = NTSCsat;
                psat = PALsat;
                pal = Palette[NES::CurRegion];
                _tcscpy(extfn, CustPalette[NES::CurRegion]);

                if (pal == PALETTE_NTSC)
                        GenerateNTSC(hue, nsat);
                else if (pal == PALETTE_PAL)
                        GeneratePAL(psat);
                else if (pal == PALETTE_EXT)
                {
                        if (!ImportPalette(extfn, TRUE))
                        {
                                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_GFX_LOAD_PALETTE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                                pal = DefaultPalette[NES::CurRegion];
                        }
                }
                else    GenerateRGB(pal, PC10compat);

                SendDlgItemMessage(hDlg, IDC_PAL_HUESLIDER, TBM_SETRANGE, FALSE, MAKELONG(-30, 30));
                SendDlgItemMessage(hDlg, IDC_PAL_HUESLIDER, TBM_SETTICFREQ, 5, 0);
                SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_SETRANGE, FALSE, MAKELONG(0, 100));
                SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_SETTICFREQ, 5, 0);
                SetDlgItemText(hDlg, IDC_PAL_EXTFILE, extfn);
                CheckRadioButton(hDlg, paltable[0], paltable[PALETTE_MAX-1], paltable[pal]);
                CheckDlgButton(hDlg, IDC_PAL_PC10_COMPAT, PC10compat ? BST_CHECKED : BST_UNCHECKED);
                UpdatePalette(hDlg, pal);
                inUpdate = FALSE;
                Theme::ApplyToDialog(hDlg);
                return TRUE;
        case WM_COMMAND:
                if (inUpdate)
                        break;
                wmId    = LOWORD(wParam);
                wmEvent = HIWORD(wParam);
                switch (wmId)
                {
                case IDC_PAL_NTSC:
                        pal = PALETTE_NTSC;
                        GenerateNTSC(hue, nsat);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_PAL:
                        pal = PALETTE_PAL;
                        GeneratePAL(psat);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_PC10:
                        pal = PALETTE_PC10;
                        GenerateRGB(pal, (IsDlgButtonChecked(hDlg, IDC_PAL_PC10_COMPAT) == BST_CHECKED));
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_PC10_ALT:
                        pal = PALETTE_PC10_ALT;
                        GenerateRGB(pal, (IsDlgButtonChecked(hDlg, IDC_PAL_PC10_COMPAT) == BST_CHECKED));
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_PC10_COMPAT:
                        GenerateRGB(pal, (IsDlgButtonChecked(hDlg, IDC_PAL_PC10_COMPAT) == BST_CHECKED));
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_EXTFILE:
                        GetDlgItemText(hDlg, IDC_PAL_EXTFILE, extfn, MAX_PATH);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_EXT:
                        if (ImportPalette(extfn, TRUE))
                        {
                                pal = PALETTE_EXT;
                                UpdatePalette(hDlg, pal);
                                return TRUE;
                        }
                        break;
                case IDC_PAL_VS1:
                        pal = PALETTE_VS1;
                        GenerateRGB(pal, FALSE);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_VS2:
                        pal = PALETTE_VS2;
                        GenerateRGB(pal, FALSE);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_VS3:
                        pal = PALETTE_VS3;
                        GenerateRGB(pal, FALSE);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_VS4:
                        pal = PALETTE_VS4;
                        GenerateRGB(pal, FALSE);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDC_PAL_BROWSE: {
                        ZeroMemory(&ofn, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hDlg;
                        ofn.hInstance = hInst;
                        TCHAR PalFilter[256];
                        TCHAR *pPF = PalFilter;
                        _tcscpy(pPF, Lang::GetString(LANG_FILTER_PALETTE)); pPF += _tcslen(pPF) + 1;
                        _tcscpy(pPF, _T("*.PAL")); pPF += _tcslen(pPF) + 1;
                        *pPF = 0;
                        ofn.lpstrFilter = PalFilter;
                        ofn.lpstrCustomFilter = NULL;
                        ofn.nFilterIndex = 1;
                        ofn.lpstrFile = extfn;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrFileTitle = NULL;
                        ofn.nMaxFileTitle = 0;
                        ofn.lpstrInitialDir = Path_PAL;
                        ofn.Flags = OFN_FILEMUSTEXIST;
                        ofn.lpstrDefExt = NULL;
                        ofn.lCustData = 0;
                        ofn.lpfnHook = NULL;
                        ofn.lpTemplateName = NULL;
                        if (GetOpenFileName(&ofn))
                        {
                                _tcscpy(Path_PAL, extfn);
                                Path_PAL[ofn.nFileOffset-1] = 0;
                                if (ImportPalette(extfn, TRUE))
                                {
                                        pal = PALETTE_EXT;
                                        CheckRadioButton(hDlg, paltable[0], paltable[PALETTE_MAX-1], paltable[pal]);
                                        SetDlgItemText(hDlg, IDC_PAL_EXTFILE, extfn);
                                        UpdatePalette(hDlg, pal);
                                }
                                else    MessageBox(hDlg, Lang::GetString(LANG_ERR_GFX_INVALID_PALETTE), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);
                        }
                        // Re-apply theme - GetOpenFileName can reset dark mode state
                        if (Theme::IsDark())
                                Theme::Reapply();
                        return TRUE;
                }
                case IDC_PAL_ER:
                case IDC_PAL_EG:
                case IDC_PAL_EB:
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                case IDOK:
                        if (pal == PALETTE_NTSC)
                        {
                                NTSChue = hue;
                                NTSCsat = nsat;
                        }
                        if (pal == PALETTE_PAL)
                                PALsat = psat;
                        if (pal == PALETTE_EXT)
                                _tcscpy(CustPalette[NES::CurRegion], extfn);
                        Palette[NES::CurRegion] = pal;
                        PC10compat = (IsDlgButtonChecked(hDlg, IDC_PAL_PC10_COMPAT) == BST_CHECKED);
                        LoadPalette(pal);
                        EndDialog(hDlg, 0);
                        return TRUE;
                case IDCANCEL:
                        LoadPalette(PALETTE_MAX);
                        EndDialog(hDlg, 0);
                        return TRUE;
                }
                break;
        case WM_HSCROLL:
                if (lParam == (LPARAM)GetDlgItem(hDlg, IDC_PAL_HUESLIDER))
                {
                        hue = SendDlgItemMessage(hDlg, IDC_PAL_HUESLIDER, TBM_GETPOS, 0, 0);
                        SetDlgItemInt(hDlg, IDC_PAL_HUE, hue, TRUE);
                        GenerateNTSC(hue, nsat);
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                }
                if (lParam == (LPARAM)GetDlgItem(hDlg, IDC_PAL_SATSLIDER))
                {
                        if (pal == PALETTE_NTSC)
                        {
                                nsat = SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_GETPOS, 0, 0);
                                SetDlgItemInt(hDlg, IDC_PAL_SAT, nsat, FALSE);
                                GenerateNTSC(hue, nsat);
                        }
                        else if (pal == PALETTE_PAL)
                        {
                                psat = SendDlgItemMessage(hDlg, IDC_PAL_SATSLIDER, TBM_GETPOS, 0, 0);
                                SetDlgItemInt(hDlg, IDC_PAL_SAT, psat, FALSE);
                                GeneratePAL(psat);
                        }
                        UpdatePalette(hDlg, pal);
                        return TRUE;
                }
                break;
        case WM_PAINT:
                hdc = BeginPaint(hDlg, &ps);
                {
                        HDC compdc = CreateCompatibleDC(hdc);
                        HBITMAP bmp;
                        POINT wcl = {0, 0};
                        RECT wrect, rect;
                        ClientToScreen(hDlg, &wcl);
                        GetWindowRect(GetDlgItem(hDlg, PalEntries[0]), &rect);
                        wrect.top = rect.top - wcl.y;
                        wrect.left = rect.left - wcl.x;
                        GetWindowRect(GetDlgItem(hDlg, PalEntries[63]), &rect);
                        wrect.bottom = rect.bottom - wcl.y;
                        wrect.right = rect.right - wcl.x;
                        bmp = CreateCompatibleBitmap(hdc, wrect.right - wrect.left, wrect.bottom - wrect.top);
                        SelectObject(compdc, bmp);
                        for (i = 0; i < 64; i++)
                        {
                                HWND dlgitem = GetDlgItem(hDlg, PalEntries[i]);
                                HBRUSH brush;
                                unsigned char emp =
                                        ((IsDlgButtonChecked(hDlg, IDC_PAL_ER) == BST_CHECKED) ? 0x1 : 0x0) |
                                        ((IsDlgButtonChecked(hDlg, IDC_PAL_EG) == BST_CHECKED) ? 0x2 : 0x0) |
                                        ((IsDlgButtonChecked(hDlg, IDC_PAL_EB) == BST_CHECKED) ? 0x4 : 0x0);
                                unsigned int R = RawPalette[emp][i][0], G = RawPalette[emp][i][1], B = RawPalette[emp][i][2];
                                brush = CreateSolidBrush(RGB(R, G, B));
                                GetWindowRect(dlgitem, &rect);
                                rect.top -= wcl.y + wrect.top;
                                rect.bottom -= wcl.y + wrect.top;
                                rect.left -= wcl.x + wrect.left;
                                rect.right -= wcl.x + wrect.left;
                                FillRect(compdc, &rect, brush);
                                DeleteObject(brush);
                        }
                        BitBlt(hdc, wrect.left, wrect.top, wrect.right - wrect.left, wrect.bottom - wrect.top, compdc, 0, 0, SRCCOPY);
                        DeleteDC(compdc);
                        DeleteObject(bmp);
                }
                EndPaint(hDlg, &ps);
                return TRUE;
        }
        return FALSE;
}
void    PaletteConfig (void)
{
        DialogBox(hInst, MAKEINTRESOURCE(IDD_PALETTE), hMainWnd, PaletteConfigProc);
}
} // namespace GFX
