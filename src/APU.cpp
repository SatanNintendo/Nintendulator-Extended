/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) QMT Productions
 */

#ifdef  NSFPLAYER
# include "in_nintendulator.h"
# include "MapperInterface.h"
# include "APU.h"
# include "CPU.h"
#else   /* !NSFPLAYER */
# include "stdafx.h"
# include "Nintendulator.h"
# include "resource.h"
# include "MapperInterface.h"
# include "NES.h"
# include "APU.h"
# include "CPU.h"
# include "PPU.h"
# include "AVI.h"
# include "Controllers.h"
# include "GFX.h"
# include "Lang.h"
# include "Theme.h"
# include "MonitorSync.h"

# pragma comment(lib, "dsound.lib")
# pragma comment(lib, "dxguid.lib")
#endif  /* NSFPLAYER */

#define SOUND_FILTERING

namespace APU
{
int                     Cycles;
int                     BufPos;
int                     InternalClock = 0;

#ifndef NSFPLAYER
unsigned long           next_pos;
unsigned long           LockSize = 0;
BOOL                    isEnabled;

LPDIRECTSOUND           DirectSound;
LPDIRECTSOUNDBUFFER     PrimaryBuffer;
LPDIRECTSOUNDBUFFER     Buffer;

short                   *buffer;
int                     buflen;
int                     volumes[7];
BYTE                    Regs[0x18];
#endif  /* !NSFPLAYER */

unsigned long           MHz;
#ifndef NSFPLAYER
// Dynamic Rate Control state.
// With FRAMEBUF=6 (100ms total buffer), we target the write cursor at
// ~2.5 slots ahead of the play cursor = 2.5/6 = 0.417, rounded to 0.42.
// Previously 0.55 with FRAMEBUF=4 (2.2/4 slots ahead). Keeping the same
// 5% margin above the midpoint means we stay near the centre of the ring
// buffer and have equal headroom in both directions before starving or
// overflowing. The extra buffer capacity absorbs longer scheduler hiccups
// (up to ~50ms) without the wait-loop ever being entered.
static const double     drc_max_adjust  = 0.05;

#define FREQ            44100
#define BITS            16
// FRAMEBUF: number of DirectSound slots in the ring buffer.
// 4 slots = 66.67ms total buffer (4 x 16.67ms at 60 Hz NTSC).
// Raised to 6 slots = 100ms. This gives UpdateDRC and the pre-check
// more room to absorb DWM scheduling hiccups without the buffer running
// dry. The extra latency (~33ms) is below perception threshold for
// retro gaming and is the difference between a slot being free on the
// very first pre-check vs. entering the wait-loop on a loaded system.
#define FRAMEBUF        6
const unsigned int      LOCK_SIZE = FREQ * (BITS / 8);

static DWORD            drc_play_freq   = FREQ;  // current DirectSound playback frequency

// Deferred DirectSound SetFrequency reset, used by ResetDRC().
// IDirectSoundBuffer::SetFrequency is an IPC call into audiodg.exe (the
// Windows Audio Engine). The Audio Engine runs its own periodic service
// cycle; if SetFrequency arrives at the wrong moment in that cycle the
// call can block. Calling SetFrequency directly from ResetDRC would be
// unsafe because ResetDRC can be invoked from the UI thread (via
// MonitorSync::Enable(FALSE) on MMR toggle-off). Instead, ResetDRC posts
// the reset frequency here.
//
// P30/P87: frequency requests are consumed by the dedicated audio-control
// background thread (AudioCtrlTick), never by the NES thread. P87 further
// removes the per-frame buffer-fill feedback loop; g_DRCApplyFreq now carries
// only the deterministic MMR target-rate changes.
// -1 = no pending reset.
static volatile LONG    g_PendingFreq   = -1L;

// Cached DirectSound buffer position for pre-check in APU::Run.
//
// PROBLEM (P27): APU::Run is called ~735 times per frame, interleaved with
// CPU::ExecOp. Each time NewBufPos >= buflen (roughly once per frame), the
// pre-check calls Buffer->GetCurrentPosition — an IPC call into audiodg.exe.
// If this IPC hits the bad phase of audiodg's 10ms service cycle, it blocks
// for 0-10ms inside the CPU::ExecOp loop. This shifts the remaining PPU
// emulation, pushes DrawScreen/SwapBuffers later, and causes a missed vblank
// — exactly the periodic ~30s video+audio dropout symptom.
//
// FIX: cache the DS position here so APU::Run's pre-check can use the
// cached value instead of issuing a new IPC call. Originally (P27) this
// cache was populated once per frame by UpdateDRC itself; as of P30 it is
// populated by a dedicated background thread roughly every 8ms (see the
// P30 block comment a few lines down), so it is if anything fresher than
// before, and UpdateDRC no longer performs the IPC call at all. Either
// way, the cached position is at most a few ms stale when APU::Run reads
// it, sufficient for a pre-check heuristic: if the cache says the slot is
// free, we try to write (the Lock call will confirm); if the slot turned
// out busy since the cache was taken, the wait-loop issues its own real
// GetCurrentPosition and recovers in one iteration.
//
// No lock needed for THIS pair of variables specifically: a torn read of
// a 32-bit value on x86/x64 is atomic, and the worst case is a stale
// pre-check that falls through to the wait-loop -- exactly the pre-cache
// behavior. (The Buffer pointer itself, which the background thread also
// touches, is a different story and IS protected -- see g_BufferCS.)
//
// g_DSCacheAge tracks how many ticks have elapsed since the cache was
// updated. If the worker thread hasn't run yet (first frame, or MMR just
// enabled), the age will be high and APU::Run/UpdateDRC fall back to
// treating the cache as not-yet-valid, just as before.
static volatile LONG    g_DSCacheRpos   = -1L;  // cached play cursor / LockSize
static volatile LONG    g_DSCacheWpos   = -1L;  // cached write cursor / LockSize
static volatile LONG    g_DSCacheAge    = 99L;  // frames since last cache update
                                                 // 99 = "invalid, use real IPC"

// ------------------------------------------------------------------
// P30 — Audio control background thread.
//
// ROOT CAUSE (found in session 9 review, after P26-P29 already shipped):
//
//   UpdateDRC() still called Buffer->GetCurrentPosition() directly, once
//   EVERY frame, unconditionally, from the NES thread -- the only
//   remaining unconditional per-frame IPC call into audiodg.exe left in
//   the whole pipeline. Every other stall source found so far (DwmFlush,
//   wglMakeCurrent races, SetWindowText, glTexSubImage2D) was either
//   removed or made asynchronous/bounded (P26, P28, P29). This one was
//   not: it runs post-vblank "because that's safe", but that reasoning
//   only holds if the IPC call itself is fast. It is not guaranteed to
//   be: IDirectSoundBuffer::GetCurrentPosition/SetFrequency are calls
//   into audiodg.exe, the same class of cross-process call that DwmFlush
//   (a call into dwm.exe) was shown to occasionally block on for 20-40ms
//   during that process's own periodic housekeeping (P26). audiodg.exe
//   has an analogous periodic service cycle. A single unlucky frame in
//   that cycle stalls the NES thread post-vblank, which delays the
//   START of the next frame's CPU/PPU loop -- pushing the FOLLOWING
//   SwapBuffers past ITS vblank deadline and starving the DirectSound
//   buffer at the same time. That is exactly the reported symptom: a
//   simultaneous video micro-stutter + audio click, roughly periodic,
//   surviving every fix that did not touch this specific call.
//
// FIX:
//   Move GetCurrentPosition and SetFrequency off the NES thread
//   entirely, onto a dedicated low-priority worker. The worker:
//     - Wakes roughly every 8ms (finer-grained than the old once-per-
//       frame refresh, so the caches used by UpdateDRC/APU::Run are, if
//       anything, fresher than before).
//     - Calls GetCurrentPosition and republishes both the slot-index
//       cache (g_DSCacheRpos/Wpos, used by APU::Run's pre-check, P27)
//       and a byte-precision cache (g_DSCacheRposBytes/WposBytes, used
//       by UpdateDRC's fill-ratio math, which needs sub-slot resolution
//       to compute a smooth +/-0.5%/frame correction).
//     - Applies any pending SetFrequency request: either a full reset
//       (g_PendingFreq, posted by ResetDRC) or a DRC-computed target
//       (g_DRCApplyFreq, posted by UpdateDRC).
//   UpdateDRC() is pure CPU-bound target calculation and posts a deterministic
//   target-rate change; it never calls into audiodg.exe itself, so a stall on
//   the worker thread has zero effect on frame pacing.
//
// THREAD SAFETY:
//   The worker touches the COM `Buffer` pointer, which the NES thread
//   can set to NULL and Release() at any time (APU::Stop(), region
//   switches, ROM close). g_BufferCS + an AddRef-while-locked pattern
//   (see AudioCtrlTick) makes this safe: the worker either observes a
//   live, ref-counted Buffer for the duration of its call, or observes
//   NULL and skips the tick. APU::Stop() only nulls the pointer and
//   calls Release() outside the critical section, so a slow worker call
//   cannot be starved by holding the lock, and Stop() cannot free the
//   object while the worker is using it. This lock is only ever
//   contended around Stop()/region-switch (rare, not part of the
//   steady 60fps path), never during normal gameplay.
// ------------------------------------------------------------------
static volatile LONG    g_DSCacheRposBytes = -1L; // byte-precision play cursor
static volatile LONG    g_DSCacheWposBytes = -1L; // byte-precision write cursor
static volatile LONG    g_DRCApplyFreq     = -1L; // DRC-computed target freq, -1 = none pending

static CRITICAL_SECTION g_BufferCS;
static bool              g_BufferCSInit    = false;

static HANDLE            g_AudioCtrlThread = NULL;
static volatile LONG     g_AudioCtrlStop   = 0L;
static volatile LONG     g_AudioCtrlReady  = 0L;
static volatile LONG     g_AudioWorkerPolls = 0L;
static volatile LONG     g_AudioSetFreqCalls = 0L;
static volatile LONG     g_AudioPlayStarts = 0L;
static volatile LONG     g_AudioSafetyWaits = 0L;
static volatile LONG     g_AudioCurrentFreq = FREQ;

// P88: do not start DirectSound playback until the first complete audio slot
// has been written. Starting the secondary buffer immediately after zeroing it
// lets the play cursor race the first Lock/Unlock on a cold start. On machines
// where DirectSound/WASAPI starts with a different scheduling phase, that race
// can manifest as a persistent crackle which disappears after a stop/start or
// fullscreen toggle. The buffer is therefore primed while stopped, then Play()
// is issued exactly once after slot 0 has been filled.
static volatile LONG     g_AudioPlayPending = 0L;

// Runs on the worker thread. Grabs a ref-counted snapshot of Buffer under
// g_BufferCS so it can never race with APU::Stop()'s Release(). See the
// P30 header comment above for the full thread-safety argument.
static void AudioCtrlTick()
{
        EnterCriticalSection(&g_BufferCS);
        LPDIRECTSOUNDBUFFER localBuf = Buffer;
        if (localBuf)
                localBuf->AddRef();
        unsigned long localLockSize = LockSize;
        LeaveCriticalSection(&g_BufferCS);

        if (!localBuf || !isEnabled || localLockSize == 0)
        {
                if (localBuf) localBuf->Release();
                return;
        }

        InterlockedIncrement(&g_AudioWorkerPolls);

        // Apply a pending full-frequency reset first (matches the original
        // ordering in UpdateDRC: reset takes priority over a stale in-flight
        // DRC target computed against the pre-reset frequency).
        LONG pendingReset = InterlockedExchange(&g_PendingFreq, -1L);
        if (pendingReset > 0)
        {
                if (SUCCEEDED(localBuf->SetFrequency((DWORD)pendingReset)))
                {
                        InterlockedIncrement(&g_AudioSetFreqCalls);
                        InterlockedExchange(&g_AudioCurrentFreq, pendingReset);
                }
        }

        // Apply the most recent DRC-computed target, if any.
        LONG pendingDRC = InterlockedExchange(&g_DRCApplyFreq, -1L);
        if (pendingDRC > 0)
        {
                if (SUCCEEDED(localBuf->SetFrequency((DWORD)pendingDRC)))
                {
                        InterlockedIncrement(&g_AudioSetFreqCalls);
                        InterlockedExchange(&g_AudioCurrentFreq, pendingDRC);
                }
        }

        // Refresh the position caches. This is the one GetCurrentPosition
        // call in the whole pipeline now -- entirely off the NES thread.
        unsigned long rpos, wpos;
        if (SUCCEEDED(localBuf->GetCurrentPosition(&rpos, &wpos)))
        {
                InterlockedExchange(&g_DSCacheRposBytes, (LONG)rpos);
                InterlockedExchange(&g_DSCacheWposBytes, (LONG)wpos);
                InterlockedExchange(&g_DSCacheRpos, (LONG)(rpos / localLockSize));
                InterlockedExchange(&g_DSCacheWpos, (LONG)(wpos / localLockSize));
                InterlockedExchange(&g_DSCacheAge,  0L);
        }

        localBuf->Release();
}

static DWORD WINAPI AudioCtrlThreadProc(void*)
{
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        InterlockedExchange(&g_AudioCtrlReady, 1L);

        while (!InterlockedExchangeAdd(&g_AudioCtrlStop, 0L))
        {
                AudioCtrlTick();
                // P88: this worker is diagnostic/safety support, not the
                // pacing clock. Polling audiodg every 8ms at ABOVE_NORMAL
                // priority could itself disturb the Windows 7 audio engine.
                // A 32ms low-priority cadence still leaves plenty of headroom
                // inside the 100ms (6-slot) ring buffer.
                Sleep(32);
        }
        return 0;
}

void StartAudioCtrlThread()
{
        if (g_AudioCtrlThread) return;

        InterlockedExchange(&g_AudioCtrlStop,  0L);
        InterlockedExchange(&g_AudioCtrlReady, 0L);

        g_AudioCtrlThread = CreateThread(NULL, 0, AudioCtrlThreadProc, NULL, 0, NULL);
        if (!g_AudioCtrlThread) return;

        for (int i = 0; i < 100 && !InterlockedExchangeAdd(&g_AudioCtrlReady, 0L); i++)
                Sleep(1);
}

void StopAudioCtrlThread()
{
        if (!g_AudioCtrlThread) return;
        InterlockedExchange(&g_AudioCtrlStop, 1L);
        // GetCurrentPosition/SetFrequency have no cancellation; bound the
        // wait the same way P28's StopVBlankThread does, and move on --
        // the thread will exit on its own once its current IPC call
        // returns, it just won't be joined here.
        WaitForSingleObject(g_AudioCtrlThread, 200);
        CloseHandle(g_AudioCtrlThread);
        g_AudioCtrlThread = NULL;
        InterlockedExchange(&g_AudioCtrlReady, 0L);
}
#endif

const   unsigned char   LengthCounts[32] = {
        0x09,0xFD,
        0x13,0x01,
        0x27,0x03,
        0x4F,0x05,
        0x9F,0x07,
        0x3B,0x09,
        0x0D,0x0B,
        0x19,0x0D,

        0x0B,0x0F,
        0x17,0x11,
        0x2F,0x13,
        0x5F,0x15,
        0xBF,0x17,
        0x47,0x19,
        0x0F,0x1B,
        0x1F,0x1D
};
const   signed char     SquareDuty[4][8] = {
        {-4,-4,-4,-4,-4,-4,-4,+4},
        {-4,-4,-4,-4,-4,-4,+4,+4},
        {-4,-4,-4,-4,+4,+4,+4,+4},
        {+4,+4,+4,+4,+4,+4,-4,-4},
};
// Unipolar duty table for NonlinearMixing (0 or 1, multiplied by Vol to get 0-15)
const   unsigned char   SquareDutyNL[4][8] = {
        {0,0,0,0,0,0,0,1},
        {0,0,0,0,0,0,1,1},
        {0,0,0,0,1,1,1,1},
        {1,1,1,1,1,1,0,0},
};
// NonlinearMixing: emulate real 2A03 DAC transfer function (NesDev formula).
// false = legacy linear mixing; true = accurate nonlinear mixing.
bool NonlinearMixing = false;
// BootWithDisabledFrameIRQ: some Famiclones start with $4017 = 0x40 (IRQ disabled, 4-step mode)
// Set to true to emulate those clones.
bool BootWithDisabledFrameIRQ = false;
const   signed char     TriangleDuty[32] = {
        +7,+6,+5,+4,+3,+2,+1,+0,
        -1,-2,-3,-4,-5,-6,-7,-8,
        -8,-7,-6,-5,-4,-3,-2,-1,
        +0,+1,+2,+3,+4,+5,+6,+7,
};
const   unsigned long   NoiseFreqNTSC[16] = {
        0x002,0x004,0x008,0x010,0x020,0x030,0x040,0x050,
        0x065,0x07F,0x0BE,0x0FE,0x17D,0x1FC,0x3F9,0x7F2,
};
const   unsigned long   NoiseFreqPAL[16] = {
        0x002,0x004,0x007,0x00F,0x01E,0x02C,0x03B,0x04A,
        0x05E,0x076,0x0B1,0x0EC,0x162,0x1D8,0x3B1,0x761,
};
const   unsigned char   DPCMFreqNTSC[16] = {
        0xD6,0xBE,0xAA,0xA0,0x8F,0x7F,0x71,0x6B,
        0x5F,0x50,0x47,0x40,0x35,0x2A,0x24,0x1B,
};
const   unsigned char   DPCMFreqPAL[16] = {
        0xC7,0xB1,0x9E,0x95,0x8A,0x76,0x69,0x63,
        0x58,0x4A,0x42,0x3B,0x31,0x27,0x21,0x19,
};
const   int     FrameCyclesNTSC[5] = { 3728,7456,11185,14914,18640 };
const   int     FrameCyclesPAL[5] = { 4156,8313,12469,16626,20782 };

namespace Race
{
        unsigned char Square0_wavehold, Square0_LengthCtr1, Square0_LengthCtr2;
        unsigned char Square1_wavehold, Square1_LengthCtr1, Square1_LengthCtr2;
        unsigned char Triangle_wavehold, Triangle_LengthCtr1, Triangle_LengthCtr2;
        unsigned char Noise_wavehold, Noise_LengthCtr1, Noise_LengthCtr2;
        void Run (void);

void    PowerOn (void)
{
        Reset();
        Triangle_wavehold = Triangle_LengthCtr1 = Triangle_LengthCtr2 = 0;
}
void    Reset (void)
{
        Square0_wavehold = Square0_LengthCtr1 = Square0_LengthCtr2 = 0;
        Square1_wavehold = Square1_LengthCtr1 = Square1_LengthCtr2 = 0;
        Noise_wavehold = Noise_LengthCtr1 = Noise_LengthCtr2 = 0;
}
} // namespace race

namespace Square0
{
        unsigned char volume, envelope, wavehold, duty, swpspeed, swpdir, swpstep, swpenab;
        unsigned long freq;     // short
        unsigned char Vol;
        unsigned char CurD;
        unsigned char LengthCtr;
        unsigned char EnvCtr, Envelope, BendCtr;
        BOOL Enabled, ValidFreq, Active;
        BOOL EnvClk, SwpClk;
        unsigned long Cycles;   // short
        signed long Pos;
        unsigned char NL_Pos;   // unipolar position for NonlinearMixing (0-15)

void    PowerOn (void)
{
        Reset();
}
void    Reset (void)
{
        volume = envelope = wavehold = duty = swpspeed = swpdir = swpstep = swpenab = 0;
        freq = 0;
        Vol = 0;
        CurD = 0;
        LengthCtr = 0;
        Envelope = 0;
        Enabled = ValidFreq = Active = FALSE;
        EnvClk = SwpClk = FALSE;
        Pos = 0;
        NL_Pos = 0;
        Cycles = 1;
        EnvCtr = 1;
        BendCtr = 1;
}
inline void     CheckActive (void)
{
        ValidFreq = (freq >= 0x8) && ((swpdir) || !((freq + (freq >> swpstep)) & 0x800));
        Active = LengthCtr && ValidFreq;
        Pos = Active ? (SquareDuty[duty][CurD] * Vol) : 0;
        NL_Pos = Active ? (SquareDutyNL[duty][CurD] * Vol) : 0;
}
inline void     Write (int Reg, unsigned char Val)
{
        switch (Reg)
        {
        case 0: volume = Val & 0xF;
                envelope = Val & 0x10;
                Race::Square0_wavehold = Val & 0x20;
                duty = (Val >> 6) & 0x3;
                Vol = envelope ? volume : Envelope;
                break;
        case 1: swpstep = Val & 0x07;
                swpdir = Val & 0x08;
                swpspeed = (Val >> 4) & 0x7;
                swpenab = Val & 0x80;
                SwpClk = TRUE;
                break;
        case 2: freq &= 0x700;
                freq |= Val;
                break;
        case 3: freq &= 0xFF;
                freq |= (Val & 0x7) << 8;
                if (Enabled)
                {
                        Race::Square0_LengthCtr1 = LengthCounts[(Val >> 3) & 0x1F] + 1;
                        Race::Square0_LengthCtr2 = LengthCtr;
                }
                CurD = 0;
                EnvClk = TRUE;
                break;
        case 4: Enabled = Val ? TRUE : FALSE;
                if (!Enabled)
                        LengthCtr = 0;
                break;
        }
        CheckActive();
}

inline void     Run (void)
{
        // Only run on odd clocks
        if (!(InternalClock & 1))
                return;
        if (!Cycles--)
        {
                Cycles = freq;
                CurD = (CurD - 1) & 0x7;
                if (Active)
                {
                        Pos = SquareDuty[duty][CurD] * Vol;
                        NL_Pos = SquareDutyNL[duty][CurD] * Vol;
                }
        }
}
inline void     QuarterFrame (void)
{
        if (EnvClk)
        {
                EnvClk = FALSE;
                Envelope = 0xF;
                EnvCtr = volume;
        }
        else if (!EnvCtr--)
        {
                EnvCtr = volume;
                if (Envelope)
                        Envelope--;
                else    Envelope = wavehold ? 0xF : 0x0;
        }
        Vol = envelope ? volume : Envelope;
        CheckActive();
}
inline void     HalfFrame (void)
{
        if (!BendCtr--)
        {
                BendCtr = swpspeed;
                if (swpenab && swpstep && ValidFreq)
                {
                        int sweep = freq >> swpstep;
                        freq += swpdir ? ~sweep : sweep;
                }
        }
        if (SwpClk)
        {
                SwpClk = FALSE;
                BendCtr = swpspeed;
        }
        if (LengthCtr && !wavehold)
                LengthCtr--;
        CheckActive();
}
} // namespace Square0

namespace Square1
{
        unsigned char volume, envelope, wavehold, duty, swpspeed, swpdir, swpstep, swpenab;
        unsigned long freq;     // short
        unsigned char Vol;
        unsigned char CurD;
        unsigned char LengthCtr;
        unsigned char EnvCtr, Envelope, BendCtr;
        BOOL Enabled, ValidFreq, Active;
        BOOL EnvClk, SwpClk;
        unsigned long Cycles;   // short
        signed long Pos;
        unsigned char NL_Pos;   // unipolar position for NonlinearMixing (0-15)

void    PowerOn (void)
{
        Reset();
}
void    Reset (void)
{
        volume = envelope = wavehold = duty = swpspeed = swpdir = swpstep = swpenab = 0;
        freq = 0;
        Vol = 0;
        CurD = 0;
        LengthCtr = 0;
        Envelope = 0;
        Enabled = ValidFreq = Active = FALSE;
        EnvClk = SwpClk = FALSE;
        Pos = 0;
        NL_Pos = 0;
        Cycles = 1;
        EnvCtr = 1;
        BendCtr = 1;
}
inline void     CheckActive (void)
{
        ValidFreq = (freq >= 0x8) && ((swpdir) || !((freq + (freq >> swpstep)) & 0x800));
        Active = LengthCtr && ValidFreq;
        Pos = Active ? (SquareDuty[duty][CurD] * Vol) : 0;
        NL_Pos = Active ? (SquareDutyNL[duty][CurD] * Vol) : 0;
}
inline void     Write (int Reg, unsigned char Val)
{
        switch (Reg)
        {
        case 0: volume = Val & 0xF;
                envelope = Val & 0x10;
                Race::Square1_wavehold = Val & 0x20;
                duty = (Val >> 6) & 0x3;
                Vol = envelope ? volume : Envelope;
                break;
        case 1: swpstep = Val & 0x07;
                swpdir = Val & 0x08;
                swpspeed = (Val >> 4) & 0x7;
                swpenab = Val & 0x80;
                SwpClk = TRUE;
                break;
        case 2: freq &= 0x700;
                freq |= Val;
                break;
        case 3: freq &= 0xFF;
                freq |= (Val & 0x7) << 8;
                if (Enabled)
                {
                        Race::Square1_LengthCtr1 = LengthCounts[(Val >> 3) & 0x1F] + 1;
                        Race::Square1_LengthCtr2 = LengthCtr;
                }
                CurD = 0;
                EnvClk = TRUE;
                break;
        case 4: Enabled = Val ? TRUE : FALSE;
                if (!Enabled)
                        LengthCtr = 0;
                break;
        }
        CheckActive();
}
inline void     Run (void)
{
        // Only run on odd clocks
        if (!(InternalClock & 1))
                return;
        if (!Cycles--)
        {
                Cycles = freq;
                CurD = (CurD - 1) & 0x7;
                if (Active)
                {
                        Pos = SquareDuty[duty][CurD] * Vol;
                        NL_Pos = SquareDutyNL[duty][CurD] * Vol;
                }
        }
}
inline void     QuarterFrame (void)
{
        if (EnvClk)
        {
                EnvClk = FALSE;
                Envelope = 0xF;
                EnvCtr = volume;
        }
        else if (!EnvCtr--)
        {
                EnvCtr = volume;
                if (Envelope)
                        Envelope--;
                else    Envelope = wavehold ? 0xF : 0x0;
        }
        Vol = envelope ? volume : Envelope;
        CheckActive();
}
inline void     HalfFrame (void)
{
        if (!BendCtr--)
        {
                BendCtr = swpspeed;
                if (swpenab && swpstep && ValidFreq)
                {
                        int sweep = freq >> swpstep;
                        freq += swpdir ? -sweep : sweep;
                }
        }
        if (SwpClk)
        {
                SwpClk = FALSE;
                BendCtr = swpspeed;
        }
        if (LengthCtr && !wavehold)
                LengthCtr--;
        CheckActive();
}
} // namespace Square1

namespace Triangle
{
        unsigned char linear, wavehold;
        unsigned long freq;     // short
        unsigned char CurD;
        unsigned char LengthCtr, LinCtr;
        BOOL Enabled, Active;
        BOOL LinClk;
        unsigned long Cycles;   // short
        signed long Pos;

void    PowerOn (void)
{
        Reset();
}
void    Reset (void)
{
        linear = wavehold = 0;
        freq = 0;
        CurD = 0;
        LengthCtr = LinCtr = 0;
        Enabled = Active = FALSE;
        LinClk = FALSE;
        Pos = 0;
        Cycles = 1;
}
inline void     CheckActive (void)
{
        Active = LengthCtr && LinCtr;
        if (freq < 4)
                Pos = 0;        // beyond hearing range
        else    Pos = TriangleDuty[CurD] * 8;
}
inline void     Write (int Reg, unsigned char Val)
{
        switch (Reg)
        {
        case 0: linear = Val & 0x7F;
                Race::Triangle_wavehold = (Val >> 7) & 0x1;
                break;
        case 2: freq &= 0x700;
                freq |= Val;
                break;
        case 3: freq &= 0xFF;
                freq |= (Val & 0x7) << 8;
                if (Enabled)
                {
                        Race::Triangle_LengthCtr1 = LengthCounts[(Val >> 3) & 0x1F] + 1;
                        Race::Triangle_LengthCtr2 = LengthCtr;
                }
                LinClk = TRUE;
                break;
        case 4: Enabled = Val ? TRUE : FALSE;
                if (!Enabled)
                        LengthCtr = 0;
                break;
        }
        CheckActive();
}
inline void     Run (void)
{
        if (!Cycles--)
        {
                Cycles = freq;
                if (Active)
                {
                        CurD++;
                        CurD &= 0x1F;
                        if (freq < 4)
                                Pos = 0;        // beyond hearing range
                        else    Pos = TriangleDuty[CurD] * 8;
                }
        }
}
inline void     QuarterFrame (void)
{
        if (LinClk)
                LinCtr = linear;
        else    if (LinCtr)
                LinCtr--;
        if (!wavehold)
                LinClk = FALSE;
        CheckActive();
}
inline void     HalfFrame (void)
{
        if (LengthCtr && !wavehold)
                LengthCtr--;
        CheckActive();
}
} // namespace Triangle

namespace Noise
{
        unsigned char volume, envelope, wavehold, datatype;
        unsigned long freq;     // short
        unsigned long CurD;     // short
        unsigned char Vol;
        unsigned char LengthCtr;
        unsigned char EnvCtr, Envelope;
        BOOL Enabled;
        BOOL EnvClk;
        unsigned long Cycles;   // short
        signed long Pos;

const unsigned long     *FreqTable;
void    PowerOn (void)
{
        Reset();
}
void    Reset (void)
{
        volume = envelope = wavehold = datatype = 0;
        freq = 0;
        Vol = 0;
        LengthCtr = 0;
        Envelope = 0;
        Enabled = FALSE;
        EnvClk = FALSE;
        Pos = 0;
        CurD = 1;
        Cycles = 1;
        EnvCtr = 1;
}
inline void     Write (int Reg, unsigned char Val)
{
        switch (Reg)
        {
        case 0: volume = Val & 0x0F;
                envelope = Val & 0x10;
                Race::Noise_wavehold = Val & 0x20;
                Vol = envelope ? volume : Envelope;
                if (LengthCtr)
                        Pos = ((CurD & 0x4000) ? -2 : 2) * Vol;
                break;
        case 2: freq = Val & 0xF;
                datatype = Val & 0x80;
                break;
        case 3: if (Enabled)
                {
                        Race::Noise_LengthCtr1 = LengthCounts[(Val >> 3) & 0x1F] + 1;
                        Race::Noise_LengthCtr2 = LengthCtr;
                }
                EnvClk = TRUE;
                break;
        case 4: Enabled = Val ? TRUE : FALSE;
                if (!Enabled)
                        LengthCtr = 0;
                break;
        }
}
inline void     Run (void)
{
        // Only run on odd clocks
        if (!(InternalClock & 1))
                return;
        // this uses pre-decrement due to the lookup table
        if (!--Cycles)
        {
                Cycles = FreqTable[freq];
                if (datatype)
                        CurD = (CurD << 1) | (((CurD >> 14) ^ (CurD >> 8)) & 0x1);
                else    CurD = (CurD << 1) | (((CurD >> 14) ^ (CurD >> 13)) & 0x1);
                if (LengthCtr)
                        Pos = ((CurD & 0x4000) ? -2 : 2) * Vol;
        }
}
inline void     QuarterFrame (void)
{
        if (EnvClk)
        {
                EnvClk = FALSE;
                Envelope = 0xF;
                EnvCtr = volume;
        }
        else if (!EnvCtr--)
        {
                EnvCtr = volume;
                if (Envelope)
                        Envelope--;
                else    Envelope = wavehold ? 0xF : 0x0;
        }
        Vol = envelope ? volume : Envelope;
        if (LengthCtr)
                Pos = ((CurD & 0x4000) ? -2 : 2) * Vol;
}
inline void     HalfFrame (void)
{
        if (LengthCtr && !wavehold)
                LengthCtr--;
}
} // namespace Noise

namespace DPCM
{
        unsigned char freq, wavehold, doirq, pcmdata, addr, len;
        unsigned long CurAddr, SampleLen;       // short
        BOOL silenced, bufempty, fetching;
        unsigned char shiftreg, outbits, buffer;
        unsigned long LengthCtr;        // short
        unsigned long Cycles;   // short
        signed long Pos;
        int DoStart, DoInc;

const   unsigned char   *FreqTable;
void    PowerOn (void)
{
        Reset();
}
void    Reset (void)
{
        freq = wavehold = doirq = pcmdata = addr = len = 0;
        CurAddr = SampleLen = 0;
        silenced = TRUE;
        shiftreg = buffer = 0;
        LengthCtr = 0;
        Pos = 0;

        Cycles = 511;
        bufempty = TRUE;
        fetching = FALSE;
        outbits = 8;
        DoStart = DoInc = 0;
}
inline void     Write (int Reg, unsigned char Val)
{
        switch (Reg)
        {
        case 0: freq = Val & 0xF;
                wavehold = (Val >> 6) & 0x1;
                doirq = Val >> 7;
                if (!doirq)
                        CPU::WantIRQ &= ~IRQ_DPCM;
                break;
        case 1: pcmdata = Val & 0x7F;
                Pos = (pcmdata - 0x40) * 3;
                break;
        case 2: addr = Val;
                break;
        case 3: len = Val;
                break;
        case 4: if (Val)
                {
                        // If channel is silent, schedule a reload
                        if (!LengthCtr)
                                DoStart = 1;
                }
                else
                {
                        DoStart = 0;
                        DoInc = 0;
                        LengthCtr = 0;
                }
                CPU::WantIRQ &= ~IRQ_DPCM;
                break;
        }
}
inline void     Run (void)
{
        // On odd clock, trigger reload on $4015 write
        if (InternalClock & 1)
        {
                if (DoInc && !--DoInc)
                {
                        if (++CurAddr == 0x10000)
                                CurAddr = 0x8000;
                        if (LengthCtr && !--LengthCtr)
                        {
                                if (wavehold)
                                        DoStart = 1;
                                else if (doirq)
                                        CPU::WantIRQ |= IRQ_DPCM;
                        }
                }
                if (DoStart && !--DoStart)
                {
                        CurAddr = 0xC000 | (addr << 6);
                        LengthCtr = (len << 4) + 1;
                }
        }

        // Do everything else on even clocks
        if (!(InternalClock & 1))
        {
                // This uses pre-decrement due to the lookup table
                if (!--Cycles)
                {
                        Cycles = FreqTable[freq];
                        if (!silenced)
                        {
                                if (shiftreg & 1)
                                {
                                        if (pcmdata <= 0x7D)
                                                pcmdata += 2;
                                }
                                else
                                {
                                        if (pcmdata >= 0x02)
                                                pcmdata -= 2;
                                }
                                shiftreg >>= 1;
                                Pos = (pcmdata - 0x40) * 3;
                        }
                        if (!--outbits)
                        {
                                outbits = 8;
                                if (!bufempty)
                                {
                                        shiftreg = buffer;
                                        bufempty = TRUE;
                                        silenced = FALSE;
                                }
                                else    silenced = TRUE;
                        }
                }
        }
        // If the buffer is empty and there's a sample to play, schedule DMA
        if (bufempty && !fetching && LengthCtr)
        {
                fetching = TRUE;
                CPU::EnableDMA |= DMA_PCM;
        }
}

void    Fetch (void)
{
        buffer = CPU::MemGetDMA(CurAddr);
        bufempty = FALSE;
        fetching = FALSE;
        DoInc = 1;
}
} // namespace DPCM

namespace Frame
{
        unsigned char Bits;
        int Cycles;
        BOOL Quarter, Half, IRQ, Zero;
        BOOL ClearBit6;     // NRS: delayed frame IRQ clear after $4015 read

const   int     *CycleTable;
void    PowerOn (void)
{
        Bits = 0;
        Cycles = 0;
        Quarter = Half = IRQ = Zero = ClearBit6 = FALSE;
        // NRS: some Famiclones boot with frame IRQ already disabled ($4017 = 0x40)
        if (BootWithDisabledFrameIRQ)
        {
                Bits = 0x40;
                CPU::WantIRQ &= ~IRQ_FRAME;
        }
}
void    Reset (void)
{
        Cycles = 0;
        Quarter = Half = IRQ = Zero = ClearBit6 = FALSE;
}
inline void     Write (unsigned char Val)
{
        Bits = Val & 0xC0;
        Zero = TRUE;
        if (Bits & 0x40)
                CPU::WantIRQ &= ~IRQ_FRAME;
}
inline void     Run (void)
{
        // Only run on odd clocks
        if (!(InternalClock & 1))
        {
                if (Quarter)
                {
                        Square0::QuarterFrame();
                        Square1::QuarterFrame();
                        Triangle::QuarterFrame();
                        Noise::QuarterFrame();
                        Quarter = FALSE;
                }
                if (Half)
                {
                        Square0::HalfFrame();
                        Square1::HalfFrame();
                        Triangle::HalfFrame();
                        Noise::HalfFrame();
                        Half = FALSE;
                }
                return;
        }

        if (IRQ)
        {
                if (!Bits)
                        CPU::WantIRQ |= IRQ_FRAME;
                IRQ = FALSE;
        }

        // NRS: delayed frame IRQ flag clear (set on $4015 read, applied next APU cycle)
        if (ClearBit6)
        {
                CPU::WantIRQ &= ~IRQ_FRAME;
                ClearBit6 = FALSE;
        }

        if (Zero)
        {
                if (Bits & 0x80)
                {
                        Quarter = TRUE;
                        Half = TRUE;
                }
                Cycles = -1;
                Zero = FALSE;
        }
        // step A
        else if (Cycles == CycleTable[0])
        {
                Quarter = TRUE;
        }
        // step B
        else if (Cycles == CycleTable[1])
        {
                Quarter = TRUE;
                Half = TRUE;
        }
        // step C
        else if (Cycles == CycleTable[2])
        {
                Quarter = TRUE;
        }
        // step D
        else if (Cycles == CycleTable[3])
        {
                if (!(Bits & 0x80))
                {
                        Quarter = TRUE;
                        Half = TRUE;
                        IRQ = TRUE;
                        Cycles = -1;
                        if (!Bits)
                                CPU::WantIRQ |= IRQ_FRAME;
                }
        }
        // step E
        else if (Cycles == CycleTable[4])
        {
                Quarter = TRUE;
                Half = TRUE;
                Cycles = -1;
        }

        Cycles++;
}
} // namespace Frame

void    Race::Run (void)
{
        Square0::wavehold = Square0_wavehold;
        if (Square0_LengthCtr1)
        {
                if (Square0::LengthCtr == Square0_LengthCtr2)
                        Square0::LengthCtr = Square0_LengthCtr1;
                Square0_LengthCtr1 = 0;
        }

        Square1::wavehold = Square1_wavehold;
        if (Square1_LengthCtr1)
        {
                if (Square1::LengthCtr == Square1_LengthCtr2)
                        Square1::LengthCtr = Square1_LengthCtr1;
                Square1_LengthCtr1 = 0;
        }

        Triangle::wavehold = Triangle_wavehold;
        if (Triangle_LengthCtr1)
        {
                if (Triangle::LengthCtr == Triangle_LengthCtr2)
                        Triangle::LengthCtr = Triangle_LengthCtr1;
                Triangle_LengthCtr1 = 0;
        }

        Noise::wavehold = Noise_wavehold;
        if (Noise_LengthCtr1)
        {
                if (Noise::LengthCtr == Noise_LengthCtr2)
                        Noise::LengthCtr = Noise_LengthCtr1;
                Noise_LengthCtr1 = 0;
        }
}

void    MAPINT  IntWrite (int Bank, int Addr, int Val)
{
#ifndef NSFPLAYER
        if (Addr < 0x018)
                Regs[Addr] = Val;
#endif  /* !NSFPLAYER */
        switch (Addr)
        {
        case 0x000:     Square0::Write(0, Val);         break;
        case 0x001:     Square0::Write(1, Val);         break;
        case 0x002:     Square0::Write(2, Val);         break;
        case 0x003:     Square0::Write(3, Val);         break;
        case 0x004:     Square1::Write(0, Val);         break;
        case 0x005:     Square1::Write(1, Val);         break;
        case 0x006:     Square1::Write(2, Val);         break;
        case 0x007:     Square1::Write(3, Val);         break;
        case 0x008:     Triangle::Write(0, Val);        break;
        case 0x00A:     Triangle::Write(2, Val);        break;
        case 0x00B:     Triangle::Write(3, Val);        break;
        case 0x00C:     Noise::Write(0, Val);           break;
        case 0x00E:     Noise::Write(2, Val);           break;
        case 0x00F:     Noise::Write(3, Val);           break;
        case 0x010:     DPCM::Write(0, Val);            break;
        case 0x011:     DPCM::Write(1, Val);            break;
        case 0x012:     DPCM::Write(2, Val);            break;
        case 0x013:     DPCM::Write(3, Val);            break;
        case 0x014:     CPU::EnableDMA |= DMA_SPR;
                        CPU::DMAPage = Val;             break;
        case 0x015:     Square0::Write(4, Val & 0x1);
                        Square1::Write(4, Val & 0x2);
                        Triangle::Write(4, Val & 0x4);
                        Noise::Write(4, Val & 0x8);
                        DPCM::Write(4, Val & 0x10);     break;
#ifndef NSFPLAYER
        case 0x016:     Controllers::Write(Val);        break;
#else   /* NSFPLAYER */
#endif  /* !NSFPLAYER */
        case 0x017:     Frame::Write(Val);              break;
        }
}

int     MAPINT  IntRead (int Bank, int Addr)
{
        int result = -1;
        switch (Addr)
        {
        case 0x015:
                result =
                        ((      Square0::LengthCtr) ? 0x01 : 0) |
                        ((      Square1::LengthCtr) ? 0x02 : 0) |
                        ((     Triangle::LengthCtr) ? 0x04 : 0) |
                        ((        Noise::LengthCtr) ? 0x08 : 0) |
                        ((         DPCM::LengthCtr) ? 0x10 : 0) |
                        ((CPU::WantIRQ & IRQ_FRAME) ? 0x40 : 0) |
                        ((CPU::WantIRQ &  IRQ_DPCM) ? 0x80 : 0);
                // NRS: frame IRQ clear is delayed by 1 APU cycle (ClearBit6 mechanism)
                Frame::ClearBit6 = TRUE;    // DPCM flag doesn't get reset
                break;
#ifndef NSFPLAYER
        case 0x016:
                result = CPU::LastRead & 0xC0;
                result |= Controllers::Port1->Read() & 0x19;
                result |= Controllers::PortExp->Read1() & 0x1F;
                break;
        case 0x017:
                result = CPU::LastRead & 0xC0;
                result |= Controllers::Port2->Read() & 0x19;
                result |= Controllers::PortExp->Read2() & 0x1F;
                break;
#endif  /* !NSFPLAYER */
        }
        return result;
}

#ifndef NSFPLAYER
#define Try(action, errormsg) do {\
        if (FAILED(action))\
        {\
                Stop();\
                Start();\
                if (FAILED(action))\
                {\
                        SoundOFF();\
                        MessageBox(hMainWnd, errormsg, Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK | MB_ICONERROR);\
                        return;\
                }\
        }\
} while (false)
#endif  /* !NSFPLAYER */

void    SetRegion (void)
{
#ifndef NSFPLAYER
        BOOL Enabled = isEnabled;
        BOOL Started = (Buffer != NULL);
        if (Enabled)
                SoundOFF();
        if (Started)
                Stop();
#endif  /* !NSFPLAYER */
        int WantFPS = 60;
        switch (NES::CurRegion)
        {
        case NES::REGION_NTSC:
                WantFPS = 60;
                MHz = 1789773;
                Noise::FreqTable = NoiseFreqNTSC;
                DPCM::FreqTable = DPCMFreqNTSC;
                Frame::CycleTable = FrameCyclesNTSC;
                break;
        case NES::REGION_PAL:
                WantFPS = 50;
                MHz = 1662607;
                Noise::FreqTable = NoiseFreqPAL;
                DPCM::FreqTable = DPCMFreqPAL;
                Frame::CycleTable = FrameCyclesPAL;
                break;
        case NES::REGION_DENDY:
                WantFPS = 50;
                MHz = 1773447;
                Noise::FreqTable = NoiseFreqNTSC;
                DPCM::FreqTable = DPCMFreqNTSC;
                Frame::CycleTable = FrameCyclesNTSC;
                break;
        default:
                EI.DbgOut(Lang::GetString(LANG_ERR_APU_REGION));
                break;
        }
#ifndef NSFPLAYER
        LockSize = LOCK_SIZE / WantFPS;
        buflen = LockSize / (BITS / 8);
        if (buffer)
                delete[] buffer;
        buffer = new short[buflen];
        if (Started)
                Start();
        if (Enabled)
                SoundON();
#endif  /* !NSFPLAYER */

        // Keep MonitorSync aware of the active NES region so its
        // GetNESHz()/GetFrameHz() return the correct region timing for DRC.
#ifndef NSFPLAYER
        NotifyMonitorSyncRegion();
#endif  /* !NSFPLAYER */
}

#ifndef NSFPLAYER
long GetAudioWorkerPolls(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioWorkerPolls, 0L);
}
long GetAudioSetFreqCalls(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioSetFreqCalls, 0L);
}
long GetAudioPlayStarts(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioPlayStarts, 0L);
}
long GetAudioSafetyWaits(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioSafetyWaits, 0L);
}
long GetAudioCurrentFreq(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioCurrentFreq, 0L);
}
long GetAudioPlayPending(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioPlayPending, 0L);
}
#endif

// Forward the current NES region to the MonitorSync module.
// Implemented here (rather than in NES.cpp) so the call site does not
// have to include MonitorSync.h, and so the dependency on
// MonitorSync::SetNESRegion stays inside the APU translation unit.
#ifndef NSFPLAYER
void    NotifyMonitorSyncRegion (void)
{
        MonitorSync::SetNESRegion((int)NES::CurRegion);
}
#endif  /* !NSFPLAYER */

void    Init (void)
{
#ifndef NSFPLAYER
        DirectSound     = NULL;
        PrimaryBuffer   = NULL;
        Buffer          = NULL;
        buffer          = nullptr;
        isEnabled       = FALSE;
        InterlockedExchange(&g_AudioPlayPending, 0L);

        // P30: critical section guarding the Buffer pointer against the
        // audio-control worker thread. Initialised once here; deleted in
        // Destroy(). See the P30 block comment above g_DSCacheRposBytes
        // for the full thread-safety rationale.
        if (!g_BufferCSInit)
        {
                InitializeCriticalSection(&g_BufferCS);
                g_BufferCSInit = true;
        }
#endif  /* !NSFPLAYER */
        MHz             = 1;
#ifndef NSFPLAYER
        LockSize        = 1;
        buflen          = 0;
#endif  /* !NSFPLAYER */
        BufPos          = 0;
#ifndef NSFPLAYER
        next_pos        = 0;


        if (FAILED(DirectSoundCreate(&DSDEVID_DefaultPlayback, &DirectSound, NULL)))
        {
                Destroy();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_APU_DIRECTSOUND), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK);
                return;
        }
        if (FAILED(DirectSound->SetCooperativeLevel(hMainWnd, DSSCL_PRIORITY)))
        {
                Destroy();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_APU_DIRECTSOUND), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK);
                return;
        }
#endif  /* !NSFPLAYER */
}

void    Destroy (void)
{
        Stop();
#ifndef NSFPLAYER
        // Defensive: normally already stopped via MonitorSync::Enable(FALSE),
        // but make sure the worker is never left running past the point
        // where g_BufferCS is torn down.
        StopAudioCtrlThread();
        if (g_BufferCSInit)
        {
                DeleteCriticalSection(&g_BufferCS);
                g_BufferCSInit = false;
        }
        if (DirectSound)
        {
                DirectSound->Release();
                DirectSound = NULL;
        }
#endif  /* !NSFPLAYER */
}

void    Start (void)
{
#ifndef NSFPLAYER
        WAVEFORMATEX WFX;
        DSBUFFERDESC DSBD;
        if (!DirectSound)
                return;

        ZeroMemory(&DSBD, sizeof(DSBUFFERDESC));
        DSBD.dwSize = sizeof(DSBUFFERDESC);
        DSBD.dwFlags = DSBCAPS_PRIMARYBUFFER;
        DSBD.dwBufferBytes = 0;
        DSBD.lpwfxFormat = NULL;
        if (FAILED(DirectSound->CreateSoundBuffer(&DSBD, &PrimaryBuffer, NULL)))
        {
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_APU_BUFFER), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK);
                return;
        }

        ZeroMemory(&WFX, sizeof(WAVEFORMATEX));
        WFX.wFormatTag = WAVE_FORMAT_PCM;
        WFX.nChannels = 1;
        WFX.nSamplesPerSec = FREQ;
        WFX.wBitsPerSample = BITS;
        WFX.nBlockAlign = WFX.wBitsPerSample / 8 * WFX.nChannels;
        WFX.nAvgBytesPerSec = WFX.nSamplesPerSec * WFX.nBlockAlign;
        if (FAILED(PrimaryBuffer->SetFormat(&WFX)))
        {
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_APU_FORMAT), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK);
                return;
        }
        if (FAILED(PrimaryBuffer->Play(0, 0, DSBPLAY_LOOPING)))
        {
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_APU_BUFFER), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK);
                return;
        }

        // DSBCAPS_LOCSOFTWARE deliberately omitted: on Windows Vista and later,
        // DirectSound always routes through WASAPI shared mode regardless of
        // this flag (hardware mixing was removed). Specifying LOCSOFTWARE forces
        // the legacy software mixer code path which adds latency and slightly
        // increases the probability of IPC stalls when audiodg.exe is busy.
        // Omitting it lets the driver choose the optimal path.
        DSBD.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLFREQUENCY;
        DSBD.dwBufferBytes = LockSize * FRAMEBUF;
        DSBD.lpwfxFormat = &WFX;

        if (FAILED(DirectSound->CreateSoundBuffer(&DSBD, &Buffer, NULL)))
        {
                Stop();
                MessageBox(hMainWnd, Lang::GetString(LANG_ERR_APU_BUFFER), Lang::GetString(LANG_DLG_NINTENDULATOR), MB_OK);
                return;
        }
        EI.DbgOut(Lang::GetString(LANG_MSG_APU_STARTED));
#endif  /* !NSFPLAYER */
}

void    Stop (void)
{
#ifndef NSFPLAYER
        if (Buffer)
        {
                SoundOFF();
                // P30: null the pointer under g_BufferCS, then Release()
                // outside the lock. The audio-control worker AddRefs Buffer
                // while holding the same lock before using it (AudioCtrlTick),
                // so it either sees the live pointer here (and its AddRef
                // keeps the object alive until it Release()s its own
                // reference) or sees NULL and skips the tick -- never a
                // dangling pointer. Releasing outside the lock keeps this
                // call from blocking on a slow in-flight worker IPC call.
                LPDIRECTSOUNDBUFFER tmpBuffer = NULL;
                if (g_BufferCSInit)
                {
                        EnterCriticalSection(&g_BufferCS);
                        tmpBuffer = Buffer;
                        Buffer = NULL;
                        LeaveCriticalSection(&g_BufferCS);
                }
                else
                {
                        tmpBuffer = Buffer;
                        Buffer = NULL;
                }
                tmpBuffer->Release();
        }
        if (PrimaryBuffer)
        {
                PrimaryBuffer->Stop();
                PrimaryBuffer->Release();
                PrimaryBuffer = NULL;
        }
        if (buffer)
        {
                delete[] buffer;
                buffer = NULL;
        }
#endif  /* !NSFPLAYER */
}

void    PowerOn  (void)
{
#ifndef NSFPLAYER
        ZeroMemory(Regs, 0x18);
#endif  /* !NSFPLAYER */
        Frame::PowerOn();
        Square0::PowerOn();
        Square1::PowerOn();
        Triangle::PowerOn();
        Noise::PowerOn();
        DPCM::PowerOn();
        Race::PowerOn();
        Cycles = 1;
        CPU::WantIRQ &= ~(IRQ_FRAME | IRQ_DPCM);
        InternalClock = 0;
}
void    Reset  (void)
{
#ifndef NSFPLAYER
        ZeroMemory(Regs, 0x18);
#endif  /* !NSFPLAYER */
        Frame::Reset();
        Square0::Reset();
        Square1::Reset();
        Triangle::Reset();
        Noise::Reset();
        DPCM::Reset();
        Race::Reset();
        Cycles = 1;
        CPU::WantIRQ &= ~(IRQ_FRAME | IRQ_DPCM);
        InternalClock = 0;
}

#ifndef NSFPLAYER
void    SoundOFF (void)
{
        if (!isEnabled)
                return;
        isEnabled = FALSE;
        InterlockedExchange(&g_AudioPlayPending, 0L);
        if (Buffer)
                Buffer->Stop();
}

void    SoundON (void)
{
        LPVOID bufPtr;
        DWORD bufBytes;
        if (isEnabled)
                return;
        if (!Buffer)
        {
                Start();
                if (!Buffer)
                        return;
        }
        Try(Buffer->Lock(0, 0, &bufPtr, &bufBytes, NULL, 0, DSBLOCK_ENTIREBUFFER), Lang::GetString(LANG_ERR_APU_BUFFER));
        ZeroMemory(bufPtr, bufBytes);
        Try(Buffer->Unlock(bufPtr, bufBytes, NULL, 0), Lang::GetString(LANG_ERR_APU_BUFFER));
        // Explicitly align both playback phase and sample rate before the
        // buffer starts looping. After SoundOFF(), DirectSound does not need
        // to resume at slot zero; zeroing the buffer alone therefore does not
        // guarantee producer/consumer phase alignment. Fullscreen toggling
        // calls SoundOFF/SoundON and was observed to cure the crackle, so make
        // that recovery deterministic rather than driver-position dependent.
        Try(Buffer->SetCurrentPosition(0), Lang::GetString(LANG_ERR_APU_BUFFER));
        // P88: keep the buffer stopped until the first complete audio slot
        // has been copied into slot 0. This removes the cold-start race
        // between DirectSound's play cursor and the first producer Lock().
        // The control worker is responsible for the MMR frequency target;
        // non-MMR operation remains at the native 44100 Hz rate.
        isEnabled = TRUE;
        InterlockedExchange(&g_AudioPlayPending, 1L);
        next_pos = 0;
        // P88: establish the correct playback rate while the buffer is still
        // stopped. For MMR this is targetHz/NESHz; for normal operation it is
        // the native 44100 Hz. Doing this before Play() avoids changing the
        // DirectSound rate underneath active playback on the cold-start path.
        DWORD startFreq = FREQ;
        if (MonitorSync::IsEnabled())
        {
                double targetHz = MonitorSync::GetTargetHz();
                double nesHz = MonitorSync::GetNESHz();
                if (targetHz > 0.0 && nesHz > 0.0)
                        startFreq = (DWORD)((double)FREQ * (targetHz / nesHz) + 0.5);
        }
        if (startFreq < 100) startFreq = 100;
        if (startFreq > 100000) startFreq = 100000;
        Try(Buffer->SetFrequency(startFreq), Lang::GetString(LANG_ERR_APU_BUFFER));
        InterlockedIncrement(&g_AudioSetFreqCalls);
        InterlockedExchange(&g_AudioCurrentFreq, (LONG)startFreq);
        drc_play_freq = startFreq;
        // Invalidate the DS position cache so the first APU::Run pre-check
        // after start uses a live GetCurrentPosition call rather than stale
        // data from a previous session. UpdateDRC will populate the cache
        // on the first post-vblank tick.
        InterlockedExchange(&g_DSCacheRpos, -1L);
        InterlockedExchange(&g_DSCacheWpos, -1L);
        InterlockedExchange(&g_DSCacheRposBytes, -1L);
        InterlockedExchange(&g_DSCacheWposBytes, -1L);
        InterlockedExchange(&g_DSCacheAge,  99L);
}

INT_PTR CALLBACK        VolumeConfigProc (HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
        static const int vol_sliders[7] = {IDC_AUDIO_VOL_MASTER, IDC_AUDIO_VOL_SQ0, IDC_AUDIO_VOL_SQ1, IDC_AUDIO_VOL_TRI, IDC_AUDIO_VOL_NOI, IDC_AUDIO_VOL_PCM, IDC_AUDIO_VOL_EXT};
        static const int vol_mutes[7] = {IDC_AUDIO_MUTE_MASTER, IDC_AUDIO_MUTE_SQ0, IDC_AUDIO_MUTE_SQ1, IDC_AUDIO_MUTE_TRI, IDC_AUDIO_MUTE_NOI, IDC_AUDIO_MUTE_PCM, IDC_AUDIO_MUTE_EXT};
        int wmId, wmEvent;
        int i;

        switch (uMsg)
        {
        case WM_INITDIALOG:
                {
                SetWindowText(hDlg, Lang::GetString(LANG_DLG_VOL_TITLE));
                SetDlgItemText(hDlg, IDOK, Lang::GetString(LANG_DLG_VOL_CLOSE));
                // Localize channel labels and Mute checkboxes by matching window text
                static const LangStringID vol_labels[7] = {LANG_DLG_VOL_MASTER, LANG_DLG_VOL_SQ0, LANG_DLG_VOL_SQ1, LANG_DLG_VOL_TRI, LANG_DLG_VOL_NOI, LANG_DLG_VOL_PCM, LANG_DLG_VOL_EXT};
                HWND hChild = GetWindow(hDlg, GW_CHILD);
                while (hChild) {
                        TCHAR txt[64] = {0};
                        GetWindowText(hChild, txt, 64);
                        if (_tcscmp(txt, _T("Mute")) == 0)
                                SetWindowText(hChild, Lang::GetString(LANG_DLG_VOL_MUTE));
                        else {
                                struct { const TCHAR *orig; LangStringID id; } groups[] = {
                                        { _T("&Master"), LANG_DLG_VOL_MASTER },
                                        { _T("SQ&0"),    LANG_DLG_VOL_SQ0   },
                                        { _T("SQ&1"),    LANG_DLG_VOL_SQ1   },
                                        { _T("&TRI"),    LANG_DLG_VOL_TRI   },
                                        { _T("&NOI"),    LANG_DLG_VOL_NOI   },
                                        { _T("&PCM"),    LANG_DLG_VOL_PCM   },
                                        { _T("&EXT"),    LANG_DLG_VOL_EXT   },
                                        { NULL, LANG_STRING_COUNT }
                                };
                                for (int k = 0; groups[k].orig != NULL; k++)
                                        if (_tcscmp(txt, groups[k].orig) == 0)
                                                { SetWindowText(hChild, Lang::GetString(groups[k].id)); break; }
                        }
                        hChild = GetWindow(hChild, GW_HWNDNEXT);
                }
                for (i = 0; i < 7; i++)
                {
                        SendDlgItemMessage(hDlg, vol_sliders[i], TBM_SETRANGE, FALSE, MAKELONG(0, 100));
                        SendDlgItemMessage(hDlg, vol_sliders[i], TBM_SETTICFREQ, 10, 0);
                        if (volumes[i] >= 0)
                        {
                                SendDlgItemMessage(hDlg, vol_sliders[i], TBM_SETPOS, TRUE, 100 - volumes[i]);
                                CheckDlgButton(hDlg, vol_mutes[i], BST_UNCHECKED);
                        }
                        else
                        {
                                SendDlgItemMessage(hDlg, vol_sliders[i], TBM_SETPOS, TRUE, 100 + volumes[i]);
                                CheckDlgButton(hDlg, vol_mutes[i], BST_CHECKED);
                        }
                }
                Theme::ApplyToDialog(hDlg);
                return TRUE;
                }  // end WM_INITDIALOG
        case WM_COMMAND:
                wmId    = LOWORD(wParam);
                wmEvent = HIWORD(wParam);
                for (i = 0; i < 7; i++)
                {
                        if (wmId == vol_mutes[i])
                        {
                                int vol = 100 - SendDlgItemMessage(hDlg, vol_sliders[i], TBM_GETPOS, 0, 0);
                                if (IsDlgButtonChecked(hDlg, vol_mutes[i]) == BST_CHECKED)
                                        volumes[i] = -vol;
                                else    volumes[i] = vol;
                                return TRUE;
                        }
                }
                if (wmId == IDOK)
                {
                        EndDialog(hDlg, 0);
                        return TRUE;
                }
                break;
        case WM_VSCROLL:
                for (i = 0; i < 7; i++)
                {
                        if (lParam == (LPARAM)GetDlgItem(hDlg, vol_sliders[i]))
                        {
                                int vol = 100 - SendDlgItemMessage(hDlg, vol_sliders[i], TBM_GETPOS, 0, 0);
                                if (IsDlgButtonChecked(hDlg, vol_mutes[i]) == BST_CHECKED)
                                        volumes[i] = -vol;
                                else    volumes[i] = vol;
                                return TRUE;
                        }
                }
                break;
        }
        return FALSE;
}

void    Config (void)
{
        DialogBox(hInst, MAKEINTRESOURCE(IDD_VOLUME), hMainWnd, VolumeConfigProc);
}

int     Save (FILE *out)
{
        int clen = 0;
        unsigned char tpc;
        tpc = Regs[0x15] & 0xF;
        writeByte(tpc);                 //      uint8           Last value written to $4015, lower 4 bits

        writeByte(Regs[0x01]);          //      uint8           Last value written to $4001
        writeWord(Square0::freq);       //      uint16          Square0 frequency
        writeByte(Square0::LengthCtr);  //      uint8           Square0 timer
        writeByte(Square0::CurD);       //      uint8           Square0 duty cycle pointer
        tpc = (Square0::EnvClk ? 0x2 : 0x0) | (Square0::SwpClk ? 0x1 : 0x0);
        writeByte(tpc);                 //      uint8           Boolean flags for whether Square0 envelope(2)/sweep(1) needs a reload
        writeByte(Square0::EnvCtr);     //      uint8           Square0 envelope counter
        writeByte(Square0::Envelope);   //      uint8           Square0 envelope value
        writeByte(Square0::BendCtr);    //      uint8           Square0 bend counter
        writeWord(Square0::Cycles);     //      uint16          Square0 cycles
        writeByte(Regs[0x00]);          //      uint8           Last value written to $4000

        writeByte(Regs[0x05]);          //      uint8           Last value written to $4005
        writeWord(Square1::freq);       //      uint16          Square1 frequency
        writeByte(Square1::LengthCtr);  //      uint8           Square1 timer
        writeByte(Square1::CurD);       //      uint8           Square1 duty cycle pointer
        tpc = (Square1::EnvClk ? 0x2 : 0x0) | (Square1::SwpClk ? 0x1 : 0x0);
        writeByte(tpc);                 //      uint8           Boolean flags for whether Square1 envelope(2)/sweep(1) needs a reload
        writeByte(Square1::EnvCtr);     //      uint8           Square1 envelope counter
        writeByte(Square1::Envelope);   //      uint8           Square1 envelope value
        writeByte(Square1::BendCtr);    //      uint8           Square1 bend counter
        writeWord(Square1::Cycles);     //      uint16          Square1 cycles
        writeByte(Regs[0x04]);          //      uint8           Last value written to $4004

        writeWord(Triangle::freq);      //      uint16          Triangle frequency
        writeByte(Triangle::LengthCtr); //      uint8           Triangle timer
        writeByte(Triangle::CurD);      //      uint8           Triangle duty cycle pointer
        writeByte(Triangle::LinClk);    //      uint8           Boolean flag for whether linear counter needs reload
        writeByte(Triangle::LinCtr);    //      uint8           Triangle linear counter
        writeByte(Triangle::Cycles);    //      uint16          Triangle cycles
        writeByte(Regs[0x08]);          //      uint8           Last value written to $4008

        writeByte(Regs[0x0E]);          //      uint8           Last value written to $400E
        writeByte(Noise::LengthCtr);    //      uint8           Noise timer
        writeWord(Noise::CurD);         //      uint16          Noise duty cycle pointer
        writeByte(Noise::EnvClk);       //      uint8           Boolean flag for whether Noise envelope needs a reload
        writeByte(Noise::EnvCtr);       //      uint8           Noise envelope counter
        writeByte(Noise::Envelope);     //      uint8           Noise  envelope value
        writeWord(Noise::Cycles);       //      uint16          Noise cycles
        writeByte(Regs[0x0C]);          //      uint8           Last value written to $400C

        writeByte(Regs[0x10]);          //      uint8           Last value written to $4010
        writeByte(Regs[0x11]);          //      uint8           Last value written to $4011
        writeByte(Regs[0x12]);          //      uint8           Last value written to $4012
        writeByte(Regs[0x13]);          //      uint8           Last value written to $4013
        writeWord(DPCM::CurAddr);       //      uint16          DPCM current address
        writeWord(DPCM::SampleLen);     //      uint16          DPCM current length
        writeByte(DPCM::shiftreg);      //      uint8           DPCM shift register
        tpc = (DPCM::DoInc ? 0x10 : 0) | (DPCM::DoStart ? 0x8 : 0) | (DPCM::fetching ? 0x4 : 0x0) | (DPCM::silenced ? 0x0 : 0x2) | (DPCM::bufempty ? 0x0 : 0x1);        // variables were renamed and inverted
        writeByte(tpc);                 //      uint8           DPCM incrementing(D4)/resetting(D3)/fetching(D2)/!silenced(D1)/!empty(D0)
        writeByte(DPCM::outbits);       //      uint8           DPCM shift count
        writeByte(DPCM::buffer);        //      uint8           DPCM read buffer
        writeWord(DPCM::Cycles);        //      uint16          DPCM cycles
        writeWord(DPCM::LengthCtr);     //      uint16          DPCM length counter

        writeByte(Regs[0x17]);          //      uint8           Last value written to $4017
        writeWord(Frame::Cycles);       //      uint16          Frame counter cycles
        tpc = (Frame::Zero ? 0x8 : 0) | (Frame::IRQ ? 0x4 : 0) | (Frame::Half ? 0x2 : 0) | (Frame::Quarter ? 0x1 : 0);
        writeByte(tpc);                 //      uint8           Frame counter Zero(D3)/IRQ(D2)/Half(D1)/Quarter(D0) pending

        tpc = CPU::WantIRQ & (IRQ_DPCM | IRQ_FRAME);
        writeByte(tpc);                 //      uint8           APU-related IRQs (PCM and FRAME, as-is)
        tpc = InternalClock & 0xFF;
        writeByte(tpc);                 //      uint8           APU clock, lower 8 bits (for phase)

        return clen;
}

int     Load (FILE *in, int version_id)
{
        int clen = 0;
        unsigned char tpc;

        readByte(tpc);                  //      uint8           Last value written to $4015, lower 4 bits
        IntWrite(0x4, 0x015, tpc);      // this will ACK any DPCM IRQ

        readByte(tpc);                  //      uint8           Last value written to $4001
        IntWrite(0x4, 0x001, tpc);
        readWord(Square0::freq);        //      uint16          Square0 frequency
        readByte(Square0::LengthCtr);   //      uint8           Square0 timer
        readByte(Square0::CurD);        //      uint8           Square0 duty cycle pointer
        readByte(tpc);                  //      uint8           Boolean flags for whether Square0 envelope(2)/sweep(1) needs a reload
        Square0::EnvClk = (tpc & 0x2);
        Square0::SwpClk = (tpc & 0x1);
        readByte(Square0::EnvCtr);      //      uint8           Square0 envelope counter
        readByte(Square0::Envelope);    //      uint8           Square0 envelope value
        readByte(Square0::BendCtr);     //      uint8           Square0 bend counter
        readWord(Square0::Cycles);      //      uint16          Square0 cycles
        if (version_id < 1004)
                Square0::Cycles >>= 1;
        readByte(tpc);                  //      uint8           Last value written to $4000
        IntWrite(0x4, 0x000, tpc);

        readByte(tpc);                  //      uint8           Last value written to $4005
        IntWrite(0x4, 0x005, tpc);
        readWord(Square1::freq);        //      uint16          Square1 frequency
        readByte(Square1::LengthCtr);   //      uint8           Square1 timer
        readByte(Square1::CurD);        //      uint8           Square1 duty cycle pointer
        readByte(tpc);                  //      uint8           Boolean flags for whether Square1 envelope(2)/sweep(1) needs a reload
        Square1::EnvClk = (tpc & 0x2);
        Square1::SwpClk = (tpc & 0x1);
        readByte(Square1::EnvCtr);      //      uint8           Square1 envelope counter
        readByte(Square1::Envelope);    //      uint8           Square1 envelope value
        readByte(Square1::BendCtr);     //      uint8           Square1 bend counter
        readWord(Square1::Cycles);      //      uint16          Square1 cycles
        if (version_id < 1004)
                Square1::Cycles >>= 1;
        readByte(tpc);                  //      uint8           Last value written to $4004
        IntWrite(0x4, 0x004, tpc);

        readWord(Triangle::freq);       //      uint16          Triangle frequency
        readByte(Triangle::LengthCtr);  //      uint8           Triangle timer
        readByte(Triangle::CurD);       //      uint8           Triangle duty cycle pointer
        readByte(Triangle::LinClk);     //      uint8           Boolean flag for whether linear counter needs reload
        readByte(Triangle::LinCtr);     //      uint8           Triangle linear counter
        readByte(Triangle::Cycles);     //      uint16          Triangle cycles
        readByte(tpc);                  //      uint8           Last value written to $4008
        IntWrite(0x4, 0x008, tpc);

        readByte(tpc);                  //      uint8           Last value written to $400E
        IntWrite(0x4, 0x00E, tpc);
        readByte(Noise::LengthCtr);     //      uint8           Noise timer
        readWord(Noise::CurD);          //      uint16          Noise duty cycle pointer
        readByte(Noise::EnvClk);        //      uint8           Boolean flag for whether Noise envelope needs a reload
        readByte(Noise::EnvCtr);        //      uint8           Noise envelope counter
        readByte(Noise::Envelope);      //      uint8           Noise  envelope value
        readWord(Noise::Cycles);        //      uint16          Noise cycles
        if (version_id < 1004)
                Noise::Cycles >>= 1;
        readByte(tpc);                  //      uint8           Last value written to $400C
        IntWrite(0x4, 0x00C, tpc);

        readByte(tpc);                  //      uint8           Last value written to $4010
        IntWrite(0x4, 0x010, tpc);
        readByte(tpc);                  //      uint8           Last value written to $4011
        IntWrite(0x4, 0x011, tpc);
        readByte(tpc);                  //      uint8           Last value written to $4012
        IntWrite(0x4, 0x012, tpc);
        readByte(tpc);                  //      uint8           Last value written to $4013
        IntWrite(0x4, 0x013, tpc);
        readWord(DPCM::CurAddr);        //      uint16          DPCM current address
        readWord(DPCM::SampleLen);      //      uint16          DPCM current length
        readByte(DPCM::shiftreg);       //      uint8           DPCM shift register
        readByte(tpc);                  //      uint8           DPCM incrementing(D4)/resetting(D3)/fetching(D2)/!silenced(D1)/!empty(D0)
        if (version_id >= 1004)
        {
                DPCM::DoInc = !!(tpc & 0x10);
                DPCM::DoStart = !!(tpc & 0x8);
        }
        else    DPCM::DoInc = DPCM::DoStart = FALSE;
        DPCM::fetching = !!(tpc & 0x4);
        DPCM::silenced = !(tpc & 0x2);  // variable was renamed and inverted
        DPCM::bufempty = !(tpc & 0x1);  // variable was renamed and inverted
        readByte(DPCM::outbits);        //      uint8           DPCM shift count
        readByte(DPCM::buffer);         //      uint8           DPCM read buffer
        readWord(DPCM::Cycles);         //      uint16          DPCM cycles
        if (version_id < 1004)
                DPCM::Cycles >>= 1;
        readWord(DPCM::LengthCtr);      //      uint16          DPCM length counter

        readByte(tpc);                  //      uint8           Frame counter bits (last write to $4017)
        IntWrite(0x4, 0x017, tpc);      // and this will ACK any frame IRQ
        readWord(Frame::Cycles);        //      uint16          Frame counter cycles
        if (version_id < 1004)
                Frame::Cycles >>= 1;
        if (version_id < 1001)
                readByte(_val);         //      uint8           Frame counter phase
        if (version_id >= 1004)
        {
                readByte(tpc);          //      uint8           Frame counter Zero(D3)/IRQ(D2)/Half(D1)/Quarter(D0) pending
                Frame::Zero = !!(tpc & 0x8);
                Frame::IRQ = !!(tpc & 0x4);
                Frame::Half = !!(tpc & 0x2);
                Frame::Quarter = !!(tpc & 0x1);
        }
        else    Frame::Zero = Frame::IRQ = Frame::Half = Frame::Quarter = FALSE;

        readByte(tpc);                  //      uint8           APU-related IRQs (PCM and FRAME, as-is)
        CPU::WantIRQ |= tpc;    // so we can reload them here

        if (version_id >= 1004)
        {
                readByte(tpc);                  //      uint8           APU clock, lower 8 bits (for phase)
                InternalClock = tpc;
        }
        else    InternalClock = 0;

        return clen;
}

void    SaveSettings (HKEY SettingsBase)
{
        RegSetValueEx(SettingsBase, _T("VolMaster"), 0, REG_DWORD, (LPBYTE)&volumes[0], sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("VolSq0")   , 0, REG_DWORD, (LPBYTE)&volumes[1], sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("VolSq1")   , 0, REG_DWORD, (LPBYTE)&volumes[2], sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("VolTri")   , 0, REG_DWORD, (LPBYTE)&volumes[3], sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("VolNoi")   , 0, REG_DWORD, (LPBYTE)&volumes[4], sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("VolPCM")   , 0, REG_DWORD, (LPBYTE)&volumes[5], sizeof(DWORD));
        RegSetValueEx(SettingsBase, _T("VolExt")   , 0, REG_DWORD, (LPBYTE)&volumes[6], sizeof(DWORD));
}

void    LoadSettings (HKEY SettingsBase)
{
        unsigned long Size;

        // Defaults
        for (int i = 0; i < 7; i++)
                volumes[i] = 100;

        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolMaster"), 0, NULL, (LPBYTE)&volumes[0], &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolSq0")   , 0, NULL, (LPBYTE)&volumes[1], &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolSq1")   , 0, NULL, (LPBYTE)&volumes[2], &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolTri")   , 0, NULL, (LPBYTE)&volumes[3], &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolNoi")   , 0, NULL, (LPBYTE)&volumes[4], &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolPCM")   , 0, NULL, (LPBYTE)&volumes[5], &Size);
        Size = sizeof(DWORD);   RegQueryValueEx(SettingsBase, _T("VolExt")   , 0, NULL, (LPBYTE)&volumes[6], &Size);
}

#else   /* NSFPLAYER */
short   sample_pos = 0;
BOOL    sample_ok = FALSE;
#endif  /* !NSFPLAYER */
int sampcycles = 0, samppos = 0;

// Dynamic Rate Control.
// Called once per rendered frame while Match Monitor Rate is enabled.
//
// P87: MMR owns the slot cadence, so the old Layer-2 buffer-fill feedback
// controller has been removed. The DirectSound hardware write cursor is not
// the emulator's queued-audio length; using it for feedback could repeatedly
// move the playback frequency and create audible artifacts. The remaining
// adjustment is deterministic: FREQ * (monitor-target Hz / NES frame Hz).
//
// Total deviation from FREQ remains capped at +/-5%.
//
void    UpdateDRC (void)
{
#ifndef NSFPLAYER
        if (!Buffer || !isEnabled)
                return;

        // P88: MMR owns the cadence of NES video frames. Audio slots are
        // produced from the same NES master clock, so when the frame rate is
        // slowed from 60.0988 Hz to a 60.000 Hz display, the generated audio
        // sample rate slows by the same ratio. The correct DirectSound target
        // is therefore deterministic:
        //
        //     audio_hz = FREQ * (target_hz / NES_native_hz)
        //
        // The old Layer-2 DRC inferred emulator queue fill from the DirectSound
        // hardware write cursor minus play cursor. That is not the amount of
        // audio queued by the emulator. The feedback loop could keep posting
        // SetFrequency() changes even while the software producer/consumer
        // cadence was already correct. Because SetFrequency crosses into the
        // Windows audio engine, those repeated rate changes were a plausible
        // source of audible crackle.
        //
        // P87 removes that fill-feedback loop. MMR now determines the audio
        // rate from the same target clock that determines PaceSlot(). The
        // actual SetFrequency call remains deferred to AudioCtrlTick(), so
        // the NES thread still never enters audiodg.exe.
        double newFreqD = (double)FREQ;
        if (MonitorSync::IsEnabled())
        {
                double targetHz = MonitorSync::GetTargetHz();
                double nesHz    = MonitorSync::GetNESHz();
                // P88: audio data is generated from the NES master clock.
                // Once frame pacing is slowed from 60.0988 -> 60.000 Hz,
                // the generated sample rate falls by the same ratio. The
                // DirectSound playback frequency must therefore be target/NES,
                // not target/nominalFrame(60.0).
                if (nesHz > 0.0 && targetHz > 0.0)
                        newFreqD = (double)FREQ * (targetHz / nesHz);
        }

        // Keep the existing hard safety envelope.
        double lo = (double)FREQ * (1.0 - drc_max_adjust);
        double hi = (double)FREQ * (1.0 + drc_max_adjust);
        if (newFreqD < lo) newFreqD = lo;
        if (newFreqD > hi) newFreqD = hi;

        DWORD newFreq = (DWORD)(newFreqD + 0.5);

        // Only post a change when the deterministic MMR target actually moved.
        // The ±5 Hz dead zone avoids needless IPC calls for insignificant
        // rounding/noise in the measured monitor rate.
        if (newFreq != drc_play_freq &&
            (newFreq > drc_play_freq + 5 || newFreq + 5 < drc_play_freq))
        {
                drc_play_freq = newFreq;
                InterlockedExchange(&g_DRCApplyFreq, (LONG)newFreq);
        }
#endif /* !NSFPLAYER */
}

// Reset DRC: force playback frequency back to the standard 44100 Hz.
// Called when Match Monitor Rate is disabled (toggled off by the user),
// so that any accumulated DRC adjustment is cleared and audio plays at
// the original sample rate. This is intentionally NOT the same as calling
// UpdateDRC(), because UpdateDRC measures the current buffer fill and may
// keep the frequency shifted if the buffer happens to be off-center at
// the moment of disable.
void    ResetDRC (void)
{
#ifndef NSFPLAYER
        if (!Buffer || !isEnabled)
                return;
        drc_play_freq = FREQ;
        // Post the reset frequency for deferred application by AudioCtrlTick.
        // ResetDRC itself may be called from MonitorSync::Enable(FALSE) on the
        // UI thread; calling SetFrequency directly there would be an IPC call
        // into audiodg.exe from a thread that has no business stalling there.
        InterlockedExchange(&g_PendingFreq, (LONG)FREQ);
#endif /* !NSFPLAYER */
}

void    Run (void)
{
#ifndef NSFPLAYER
        LARGE_INTEGER p73RunEnter = {0};
        QueryPerformanceCounter(&p73RunEnter);
#endif

#ifndef NSFPLAYER
        int NewBufPos = FREQ * ++Cycles / MHz;
        if (NewBufPos >= buflen)
        {
                LPVOID bufPtr;
                DWORD bufBytes;
                unsigned long rpos, wpos;

                Cycles = NewBufPos = 0;
                if (AVI::IsActive())
                        AVI::AddAudio();

                // ============================================================
                // MMR PACING PATH
                //
                // The emulator thread is the producer of game frames and
                // DirectSound slots. When Match Monitor Rate is enabled, the
                // slot cadence is paced directly from the measured monitor
                // rate (within the supported near-rate window). This removes
                // the old dependency on audiodg's ~10ms GetCurrentPosition
                // quantization and, more importantly, removes the native
                // 60.0988Hz-vs-monitor beat that forced the render queue to
                // periodically drop/duplicate a frame.
                //
                // GetCurrentPosition remains a SAFETY VALVE only: if the
                // audio buffer is critically overfull after a pause/stall,
                // refreshes from the worker cache can detect it without using
                // the IPC call as the normal pacing clock.
                // ============================================================
                if (isEnabled && Buffer && GFX::MatchMonitorRate)
                {
                        LARGE_INTEGER p73PaceEnter = {0}, p73PaceWake = {0};
                        LARGE_INTEGER p73SafetyBegin = {0}, p73SafetyEnd = {0};
                        ULONGLONG p80PaceWakeCycles = 0;
                        // P88: MMR is now frame-driven. The display cadence is
                        // paced once per NES frame in GFX::DrawScreen, not when
                        // the APU happens to cross its 735-sample slot boundary.
                        // The audio slot writer therefore performs NO display
                        // pacing here. This is the key separation between
                        // video timing and DirectSound buffering.
                        QueryPerformanceCounter(&p73PaceEnter);
                        p73PaceWake = p73PaceEnter;
                        QueryThreadCycleTime(GetCurrentThread(), &p80PaceWakeCycles);
                        LONG p73SafetyLoops = 0;

                        // Keep the cached cursor only as a safety check. If the
                        // buffer is ever reported critically full, use a short
                        // real-time sleep rather than consuming another monitor
                        // cadence slot. Under normal matched-rate operation this
                        // path is not entered (and the current logs show
                        // safetyLoops=0 throughout the steady-state window).
                        LONG cacheAge = InterlockedExchangeAdd(&g_DSCacheAge, 1L);
                        if (cacheAge <= 4)
                        {
                                unsigned long sr = (unsigned long)InterlockedExchangeAdd(&g_DSCacheRpos, 0L);
                                unsigned long sw = (unsigned long)InterlockedExchangeAdd(&g_DSCacheWpos, 0L);
                                if (sw < sr) sw += FRAMEBUF;

                                int safetyLoops = 0;
                                QueryPerformanceCounter(&p73SafetyBegin);
                                while ((sr <= next_pos) && (next_pos <= sw) && safetyLoops < 2)
                                {
                                        Sleep(1);
                                        sr = (unsigned long)InterlockedExchangeAdd(&g_DSCacheRpos, 0L);
                                        sw = (unsigned long)InterlockedExchangeAdd(&g_DSCacheWpos, 0L);
                                        if (sw < sr) sw += FRAMEBUF;
                                        ++safetyLoops;
                                }
                                p73SafetyLoops = safetyLoops;
                                if (safetyLoops > 0)
                                        InterlockedExchangeAdd(&g_AudioSafetyWaits, safetyLoops);
                                QueryPerformanceCounter(&p73SafetyEnd);
                        }

                        {
                                FILETIME c={0},e={0},k={0},u={0};
                                LONGLONG cpuWake100=0;
                                if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
                                {
                                        ULARGE_INTEGER a,b;
                                        a.LowPart=k.dwLowDateTime; a.HighPart=k.dwHighDateTime;
                                        b.LowPart=u.dwLowDateTime; b.HighPart=u.dwHighDateTime;
                                        cpuWake100=(LONGLONG)(a.QuadPart+b.QuadPart);
                                }
                                GFX::SetMMRProducerTrace(
                                        p73RunEnter.QuadPart, p73PaceEnter.QuadPart, p73PaceWake.QuadPart,
                                        cpuWake100, p80PaceWakeCycles, p73SafetyBegin.QuadPart, p73SafetyEnd.QuadPart,
                                        p73SafetyLoops, 0);
                        }

                        goto write_slot;
                }

                // ============================================================
                // ORIGINAL PATH (MMR disabled)
                // ============================================================
                if (isEnabled && Buffer)
                {
                        LONG cacheAge = InterlockedExchangeAdd(&g_DSCacheAge, 1L);
                        if (cacheAge <= 2)
                        {
                                unsigned long sr = (unsigned long)InterlockedExchangeAdd(&g_DSCacheRpos, 0L);
                                unsigned long sw = (unsigned long)InterlockedExchangeAdd(&g_DSCacheWpos, 0L);
                                if (sw < sr) sw += FRAMEBUF;
                                if (!((sr <= next_pos) && (next_pos <= sw)))
                                        goto write_slot;
                        }
                        else
                        {
                                unsigned long pr, pw;
                                if (SUCCEEDED(Buffer->GetCurrentPosition(&pr, &pw)))
                                {
                                        unsigned long sr = pr / LockSize;
                                        unsigned long sw = pw / LockSize;
                                        if (sw < sr) sw += FRAMEBUF;
                                        if (!((sr <= next_pos) && (next_pos <= sw)))
                                                goto write_slot;
                                }
                        }
                }

                do
                {
                        if (!isEnabled)
                                break;
                        Sleep(1);
                        Try(Buffer->GetCurrentPosition(&rpos, &wpos), Lang::GetString(LANG_ERR_APU_BUFFER));
                        rpos /= LockSize;
                        wpos /= LockSize;
                        if (wpos < rpos)
                                wpos += FRAMEBUF;
                } while ((rpos <= next_pos) && (next_pos <= wpos));
                write_slot:
                if (isEnabled)
                {
                        Try(Buffer->Lock(next_pos * LockSize, LockSize, &bufPtr, &bufBytes, NULL, 0, 0), Lang::GetString(LANG_ERR_APU_BUFFER));
                        memcpy(bufPtr, buffer, bufBytes);
                        Try(Buffer->Unlock(bufPtr, bufBytes, NULL, 0), Lang::GetString(LANG_ERR_APU_BUFFER));

                        // P88: start playback only after a complete slot has
                        // been written. Until this point the DirectSound play
                        // cursor is stationary at zero, so slot 0 can never be
                        // overwritten underneath active playback.
                        if (InterlockedExchange(&g_AudioPlayPending, 0L) != 0)
                        {
                                Try(Buffer->Play(0, 0, DSBPLAY_LOOPING), Lang::GetString(LANG_ERR_APU_BUFFER));
                                InterlockedIncrement(&g_AudioPlayStarts);
                        }

                        next_pos = (next_pos + 1) % FRAMEBUF;

                        // NOTE: SetFrequency is NO LONGER called from here.
                        // It was moved to UpdateDRC() (called from GFX::DrawScreen
                        // AFTER SwapBuffers unblocks). The previous reasoning that
                        // "APU::Run is outside the vblank-critical window" was
                        // wrong: APU::Run is interleaved with CPU::ExecOp across
                        // the whole frame, and a 0-10ms audiodg IPC stall here
                        // could shift the remaining CPU/PPU emulation enough to
                        // miss the next vblank — producing the periodic video+audio
                        // dropout symptom. UpdateDRC runs after SwapBuffers, when
                        // the CPU/PPU work for this frame is already complete, so
                        // the same stall costs nothing visible.
                        // g_PendingFreq is now consumed at the start of UpdateDRC
                        // (used only for the ResetDRC → FREQ reset path).
                }
        }
#define VolAdjust(pos, vol) ((volumes[vol] > 0) ? (((pos) * volumes[vol]) / 100) : 0)
#else   /* NSFPLAYER */
#define VolAdjust(pos, vol) (pos)
        int NewBufPos = SAMPLERATE * ++Cycles / MHz;
        if (NewBufPos == SAMPLERATE)    // we've generated 1 second, so we can reset our counters now
                Cycles = NewBufPos = 0;
#endif  /* !NSFPLAYER */
        Frame::Run();
        Race::Run();
        Square0::Run();
        Square1::Run();
        Triangle::Run();
        Noise::Run();
        DPCM::Run();

#ifdef  SOUND_FILTERING
        if (NonlinearMixing)
        {
                // NonlinearMixing: accumulate linear sample values for averaging.
                // We use NL_Pos (0-15) for squares and derive TND values below.
                // During filtering we accumulate as-is; final NL formula applied at flush.
                samppos += (long)Square0::NL_Pos + (long)Square1::NL_Pos;
        }
        else
        {
                samppos += VolAdjust(Square0::Pos, 1) + VolAdjust(Square1::Pos, 2) + VolAdjust(Triangle::Pos, 3) + VolAdjust(Noise::Pos, 4) + VolAdjust(DPCM::Pos, 5);
        }
#endif  /* SOUND_FILTERING */
        sampcycles++;
        
        if (NewBufPos != BufPos)
        {
                BufPos = NewBufPos;
                if (NonlinearMixing)
                {
                        // Apply NesDev nonlinear DAC formula.
                        // Pulse: 0-30 range (sum of two 0-15 channels).
                        // TND channels need 0-based values:
                        //   Triangle 0-15: normalize from TriangleDuty*8 range [-64,+56]
                        //   Noise 0-15: abs(Noise::Pos) / max_noise_vol (max=30 for Vol=15)
                        //   DPCM 0-127: pcmdata directly
                        double s0 = (double)Square0::NL_Pos;
                        double s1 = (double)Square1::NL_Pos;
#ifdef  SOUND_FILTERING
                        // Use averaged pulse for filtering mode
                        double pulse_avg = ((double)samppos / sampcycles);
                        s0 = pulse_avg * 0.5;
                        s1 = 0.0;
#endif
                        double pulse = (s0 + s1 > 0.0) ?
                                95.88 / ((8128.0 / (s0 + s1)) + 100.0) : 0.0;

                        // Triangle: Pos in [-64,+56], map to 0-15
                        double tri_v = (Triangle::Pos + 64.0) * (15.0 / 120.0);
                        if (tri_v < 0.0) tri_v = 0.0;
                        if (tri_v > 15.0) tri_v = 15.0;
                        // Noise: |Pos| / 2 gives Vol (0-15)
                        double nse_v = (double)(Noise::Pos < 0 ? -Noise::Pos : Noise::Pos) / 2.0;
                        if (nse_v > 15.0) nse_v = 15.0;
                        // DPCM: pcmdata 0-127
                        double dmc_v = (double)DPCM::pcmdata;
                        double tnd_denom = (tri_v > 0.0 || nse_v > 0.0 || dmc_v > 0.0) ?
                                (tri_v / 8227.0 + nse_v / 12241.0 + dmc_v / 22638.0) : 0.0;
                        double tnd = (tnd_denom > 0.0) ?
                                159.79 / (1.0 / tnd_denom + 100.0) : 0.0;

                        samppos = (long)((pulse + tnd) * 32767.0);
                        if ((MI) && (MI->GenSound))
                                samppos += VolAdjust(MI->GenSound(sampcycles), 6);
                        samppos = VolAdjust(samppos, 0);
                }
                else
                {
#ifdef  SOUND_FILTERING
                        samppos = (samppos << 6) / sampcycles;
#else   /* !SOUND_FILTERING */
                        samppos = (VolAdjust(Square0::Pos, 1) + VolAdjust(Square1::Pos, 2) + VolAdjust(Triangle::Pos, 3) + VolAdjust(Noise::Pos, 4) + VolAdjust(DPCM::Pos, 5)) << 6;
#endif  /* SOUND_FILTERING */
                        if ((MI) && (MI->GenSound))
                                samppos += VolAdjust(MI->GenSound(sampcycles), 6);
                        samppos = VolAdjust(samppos, 0);
                }
                if (samppos < -0x8000)
                        samppos = -0x8000;
                if (samppos > 0x7FFF)
                        samppos = 0x7FFF;
#ifndef NSFPLAYER
                buffer[BufPos] = (short)samppos;
#else   /* NSFPLAYER */
                sample_pos = (short)samppos;
                sample_ok = TRUE;
#endif  /* !NSFPLAYER */
                samppos = sampcycles = 0;
        }
}

} // namespace APU
