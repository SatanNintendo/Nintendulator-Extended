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

// P102: throttled ring-phase feedback state (see UpdateDRC).
// drc_base_freq is the playback rate established by the last SoundON();
// the feedback controller is only allowed to trim the current rate by
// a small, inaudible amount around this base, so the deterministic P95
// rate selection remains authoritative.
static DWORD            drc_base_freq   = FREQ;
static long             drc_feedback_check_count = 0;

// P94: no deferred DirectSound frequency request is kept here.  Rate changes
// happen only during the explicit SoundOFF/SoundON transition requested by
// RestartForMonitorSync(), on the NES thread at a safe frame boundary.

// Cached DirectSound position for the legacy non-MMR path.
//
// The original Nintendulator buffer-management path still uses these cached
// cursors when MMR is disabled. The P90 MMR path deliberately does not read
// them and does not call GetCurrentPosition(); it uses a QPC-predicted play
// slot instead. Keeping the legacy cache intact preserves the normal audio
// path outside Match Monitor Rate.
static volatile LONG    g_DSCacheRpos   = -1L;
static volatile LONG    g_DSCacheWpos   = -1L;
static volatile LONG    g_DSCacheAge    = 99L;

// ------------------------------------------------------------------
// P93 — no background DirectSound control thread.
//
// The previous P30/P90/P91/P92 worker architecture is deliberately removed
// from the active path.  It could block in SetFrequency/GetCurrentPosition
// inside audiodg.exe, and P92 waited indefinitely for that worker during MMR
// shutdown.  MMR does not need a consumer cursor at all: producer cadence and
// the DirectSound sample clock are deterministic once the ring is primed.
// Frequency changes are therefore performed only during an explicit
// SoundOFF/SoundON transition when Match Monitor Rate is toggled.
// ------------------------------------------------------------------
static volatile LONG     g_AudioWorkerPolls = 0L;
static volatile LONG     g_AudioSetFreqCalls = 0L;
static volatile LONG     g_AudioPlayStarts = 0L;
static volatile LONG     g_AudioSafetyWaits = 0L;
static volatile LONG     g_AudioCurrentFreq = FREQ;

// P99: remember the DirectSound playback slot across an explicit audio
// restart. SoundOFF/SoundON is used for fullscreen/MMR transitions, so
// preserving the playback phase avoids forcing the driver back to byte zero.
static volatile LONG     g_AudioResumePosition = 0L;
static volatile LONG     g_AudioResumeValid = 0L;

// P93: prime four complete slots (~66.7 ms at 60 Hz) before Play().
#define AUDIO_PRIME_SLOTS 4
static volatile LONG     g_AudioPrimeSlots = 0L;
static volatile LONG     g_AudioPlayPending = 0L;
static volatile LONG     g_AudioRestartPending = 0L;

// ------------------------------------------------------------------
// P102: throttled ring-phase feedback parameters (see UpdateDRC).
// One fill check every 32 frames (~0.53 s at 60 Hz, ~0.64 s at 50 Hz)
// is enough to keep the write cursor centered in the 6-slot ring while
// adding at most one GetCurrentPosition call per interval -- executed
// at the post-SwapBuffers point the P90 notes explicitly blessed for
// audio maintenance (never inside CPU/PPU execution).
//
// The controller targets a 3-slot lead and only trims the playback
// frequency when the measured lead leaves the 1.5..4.5 slot band. The
// proportional gain (0.05% per slot of error, capped at +-0.3% total)
// corresponds to a pitch shift of at most ~5 cents -- inaudible in game
// audio -- while correcting drift at up to ~130 samples/second. If the
// lead ever reaches the danger zone (< 0.7 or > 5.3 slots: the producer
// stalled hard or a transition scrambled the phase), the write cursor
// is snap-re-anchored 4 slots ahead of the play cursor instead.
// ------------------------------------------------------------------
#define DRC_FEEDBACK_INTERVAL   32
static const double     DRC_FILL_TARGET  = 3.0;   // desired lead, in slots
static const double     DRC_FILL_MIN     = 1.5;   // trim when lead drops below
static const double     DRC_FILL_MAX     = 4.5;   // trim when lead rises above
static const double     DRC_FILL_SNAP_LO = 0.7;   // snap-reanchor below this
static const double     DRC_FILL_SNAP_HI = (double)FRAMEBUF - 0.7; // or above this
static const double     DRC_FILL_KP      = 0.0005; // freq trim per slot of error
static const double     DRC_FREQ_TRIM_MAX = 0.003; // +-0.3% around SoundON base

// Forward declaration: SoundON() appears before the definition below.
static double GetEffectiveProducerSampleRate();

// SoundON() is defined before these audio-only sample accumulators.
extern int sampcycles;
extern int samppos;


void StartAudioCtrlThread() {}
void StopAudioCtrlThread() {}

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
long GetAudioPrimeSlots(void)
{
        return (long)InterlockedExchangeAdd(&g_AudioPrimeSlots, 0L);
}
long GetAudioNotifyActive(void)
{
        return 0;
}
long GetAudioNotifySignals(void)
{
        return 0;
}
long GetAudioNotifyPlaySlot(void)
{
        return 0;
}
long GetAudioNotifyPeriodUs(void)
{
        return 0;
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
        InterlockedExchange(&g_AudioPrimeSlots, 0L);
        InterlockedExchange(&g_AudioRestartPending, 0L);
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

        // A newly-created secondary buffer always starts at the original
        // format rate. Clear the cached rate so a later SoundON() cannot
        // accidentally skip SetFrequency() based on a stale previous buffer.
        InterlockedExchange(&g_AudioCurrentFreq, FREQ);
        InterlockedExchange(&g_AudioResumePosition, 0L);
        InterlockedExchange(&g_AudioResumeValid, 0L);

        EI.DbgOut(Lang::GetString(LANG_MSG_APU_STARTED));
#endif  /* !NSFPLAYER */
}

void    Stop (void)
{
#ifndef NSFPLAYER
        if (Buffer)
        {
                SoundOFF();
                LPDIRECTSOUNDBUFFER tmpBuffer = Buffer;
                Buffer = NULL;
                if (tmpBuffer)
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

        // IDirectSoundBuffer::Stop() does not reset the secondary-buffer
        // playback cursor. Stop first, then read the cursor while the buffer
        // is no longer advancing; this removes a small race at slot boundaries.
        LONG resumePos = 0L;
        BOOL resumeValid = FALSE;
        isEnabled = FALSE;
        InterlockedExchange(&g_AudioPlayPending, 0L);
        InterlockedExchange(&g_AudioPrimeSlots, 0L);

        if (Buffer)
        {
                Buffer->Stop();
                if (LockSize > 0)
                {
                        DWORD playPos = 0;
                        if (SUCCEEDED(Buffer->GetCurrentPosition(&playPos, NULL)))
                        {
                                DWORD bufferBytes = (DWORD)(LockSize * FRAMEBUF);
                                if (playPos < bufferBytes)
                                {
                                        resumePos = (LONG)((playPos / (DWORD)LockSize) * (DWORD)LockSize);
                                        resumeValid = TRUE;
                                }
                        }
                }
        }

        InterlockedExchange(&g_AudioResumePosition, resumePos);
        InterlockedExchange(&g_AudioResumeValid, resumeValid ? 1L : 0L);
}

// ------------------------------------------------------------------
// P102: soft-pause audio lifecycle (fullscreen / savestate / reset).
//
// Those flows stop the NES producer thread through STOPMODE_SOFT
// *without* stopping DirectSound. While the producer is gone the
// secondary buffer keeps consuming the ring at drc_play_freq Hz:
// once the buffered lead plays out, the play cursor laps the frozen
// write cursor and replays stale ring content (audible crackling), and
// after the producer resumes, the ring phase (write-ahead) is left
// wherever the pause duration happened to put it -- potentially right
// on top of the play cursor. Because the MMR producer writes slots in
// bursts (the 17/17/16 ms pacing beat) and the P90+ design has no
// consumer feedback at all, that phase error never self-corrects and
// turns into a sustained crackle.
//
// SoftPause() mutes the ring when the producer stops, so the pause is
// heard as a brief fade to silence instead of stale audio.
// SoftResume() re-anchors the write cursor AUDIO_PRIME_SLOTS ahead of
// the current play position (one cheap GetCurrentPosition, issued at a
// moment when the NES thread is provably not running) and restarts the
// P98 fade-in so playback resumes with a clean lead.
//
// Neither function touches Stop/Play/SetFrequency/SetCurrentPosition:
// the DirectSound driver state is never manipulated here.
// ------------------------------------------------------------------
static void AudioReanchorRing (void)
{
        DWORD playPos = 0;
        if (FAILED(Buffer->GetCurrentPosition(&playPos, NULL)))
                return;

        DWORD slotBytes = (DWORD)LockSize;
        if (slotBytes == 0)
                return;

        DWORD ringBytes = slotBytes * FRAMEBUF;
        if (ringBytes == 0 || playPos >= ringBytes)
                return;

        DWORD playSlot = playPos / slotBytes;
        if (playSlot >= FRAMEBUF)
                playSlot = 0;

        // Next written slot goes AUDIO_PRIME_SLOTS ahead of the play
        // cursor. Everything the consumer will cross before reaching it
        // is silenced so no stale sample can leak out first.
        DWORD anchorSlot = (playSlot + AUDIO_PRIME_SLOTS) % FRAMEBUF;
        DWORD writeStart = anchorSlot * slotBytes;

        next_pos = (unsigned long)anchorSlot;

        DWORD dist = (writeStart + ringBytes - playPos) % ringBytes;
        if (dist > 0)
        {
                LPVOID p1 = NULL, p2 = NULL;
                DWORD n1 = 0, n2 = 0;
                if (SUCCEEDED(Buffer->Lock(playPos, dist, &p1, &n1, &p2, &n2, 0)))
                {
                        if (p1 && n1)
                                ZeroMemory(p1, n1);
                        if (p2 && n2)
                                ZeroMemory(p2, n2);
                        Buffer->Unlock(p1, n1, p2, n2);
                }
        }

        // Restart the fade-in counter so the first slot written after
        // the re-anchor ramps in from silence instead of clicking.
        InterlockedExchange(&g_AudioPrimeSlots, 0L);
}

void    SoftPause (void)
{
        LPVOID bufPtr;
        DWORD bufBytes;
        if (!isEnabled || !Buffer)
                return;

        // Zero the whole ring. The consumer keeps looping over silence
        // for the rest of the pause, so no stale audio can ever replay,
        // whatever the pause duration. Pure shared-memory writes -- the
        // same operation a normal slot write performs.
        if (FAILED(Buffer->Lock(0, 0, &bufPtr, &bufBytes, NULL, 0, DSBLOCK_ENTIREBUFFER)))
                return;
        ZeroMemory(bufPtr, bufBytes);
        Buffer->Unlock(bufPtr, bufBytes, NULL, 0);
}

void    SoftResume (void)
{
        if (!isEnabled || !Buffer || LockSize == 0)
                return;
        AudioReanchorRing();
}

void    SoundON (void)
{
        LPVOID bufPtr;
        DWORD bufBytes;
        if (isEnabled)
        {
                // P102: SoundON while audio is already running means the
                // emulation thread is (re)starting after a soft pause
                // (NES::Start following a STOPMODE_SOFT exit). NES::Resume()
                // re-anchors the ring directly; this branch covers the
                // Start()-style entry points. No driver state is touched.
                SoftResume();
                return;
        }
        if (!Buffer)
        {
                Start();
                if (!Buffer)
                        return;
        }

        LONG resumePos = InterlockedExchange(&g_AudioResumePosition, 0L);
        BOOL resumeValid = (InterlockedExchange(&g_AudioResumeValid, 0L) != 0);

        // Reset only the software sample accumulators. This does not reset any
        // emulated APU channel state; it simply prevents a partial sample
        // averaging window from straddling a fullscreen/MMR transition.
        Cycles = 0;
        BufPos = 0;
        sampcycles = 0;
        samppos = 0;

        Try(Buffer->Lock(0, 0, &bufPtr, &bufBytes, NULL, 0, DSBLOCK_ENTIREBUFFER), Lang::GetString(LANG_ERR_APU_BUFFER));
        ZeroMemory(bufPtr, bufBytes);
        Try(Buffer->Unlock(bufPtr, bufBytes, NULL, 0), Lang::GetString(LANG_ERR_APU_BUFFER));

        // DirectSound keeps the secondary-buffer cursor when it is stopped.
        // Do not call SetCurrentPosition(0) here: moving the cursor during a
        // mode transition creates an abrupt phase jump that can become audible
        // as a click/crackle. The slot containing the saved cursor is left as
        // one silent slot; new audio is then primed starting at the following
        // slot, with the existing fade-in protecting the restart edge.
        next_pos = 0;
        if (resumeValid && LockSize > 0)
        {
                DWORD resumeSlot = (DWORD)resumePos / (DWORD)LockSize;
                if (resumeSlot < (DWORD)FRAMEBUF)
                        next_pos = (resumeSlot + 1) % FRAMEBUF;
        }

        // Keep the buffer stopped until the initial prime slots have been
        // copied. The DirectSound playback rate is established while stopped,
        // then playback starts only after enough audio lead exists.
        isEnabled = TRUE;
        InterlockedExchange(&g_AudioPlayPending, 1L);
        InterlockedExchange(&g_AudioPrimeSlots, 0L);
        // Establish the playback rate while the buffer is stopped.
        // With MMR the whole emulator clock is slowed by targetHz/NESHz, so
        // the nominal 44100-Hz APU stream must be consumed at the same ratio.
        // This value is applied only at a SoundOFF/SoundON transition; there
        // is no runtime SetFrequency worker anymore.
        // P95: when MMR is active, playback must follow the *actual sample
        // producer rate* of the slot generator while the emulation clock is
        // slowed to the monitor cadence.  The slot generator produces 735
        // samples per ~29830 CPU cycles, so its native-rate stream is slightly
        // below 44100 Hz; scale that exact producer rate by target/NES-native
        // clock ratio.  Using raw 44100 Hz here drains the ring while MMR is
        // running at 60.000 Hz and reproduces the user's delayed crackle.
        double targetHz = MonitorSync::GetTargetHz();
        double nesHz = MonitorSync::GetNESHz();
        DWORD startFreq = FREQ;
        if (GFX::MatchMonitorRate && targetHz > 0.0 && nesHz > 0.0)
        {
                double nativeProducerHz = GetEffectiveProducerSampleRate();
                double mmrProducerHz = nativeProducerHz * (targetHz / nesHz);
                startFreq = (DWORD)(mmrProducerHz + 0.5);
        }
        if (startFreq < 100) startFreq = 100;
        if (startFreq > 100000) startFreq = 100000;
        if (InterlockedExchangeAdd(&g_AudioCurrentFreq, 0L) != (LONG)startFreq)
        {
                Try(Buffer->SetFrequency(startFreq), Lang::GetString(LANG_ERR_APU_BUFFER));
                InterlockedIncrement(&g_AudioSetFreqCalls);
                InterlockedExchange(&g_AudioCurrentFreq, (LONG)startFreq);
        }
        drc_play_freq = startFreq;
        // P102: this is the playback rate the throttled feedback
        // controller in UpdateDRC() trims around; also restart its
        // check cadence so the first fill check happens a full interval
        // after the fresh prime.
        drc_base_freq = startFreq;
        drc_feedback_check_count = 0;
        // If the MMR restart request was posted before the DirectSound buffer
        // existed (the cold-start case), SoundON itself has now satisfied that
        // request. Do not perform an unnecessary second SoundOFF/SoundON on
        // the first rendered frame.
        InterlockedExchange(&g_AudioRestartPending, 0L);
        // Invalidate the legacy DS-position cache used only by the non-MMR
        // path. The MMR path does not consult the consumer cursor.
        InterlockedExchange(&g_DSCacheRpos, -1L);
        InterlockedExchange(&g_DSCacheWpos, -1L);
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

// The APU slot generator is integer-cycle based:
//   NewBufPos = FREQ * Cycles / MHz
// and the slot closes when NewBufPos >= buflen.
// Since Cycles is reset at each slot boundary, the exact average producer
// sample rate is determined by the integer number of CPU/master cycles per
// slot. Using that rate for MMR playback removes a small long-term drift
// without changing any emulated APU timing.
static double GetEffectiveProducerSampleRate()
{
        if (MHz == 0 || buflen <= 0)
                return (double)FREQ;

        unsigned long long numerator =
                (unsigned long long)buflen * (unsigned long long)MHz;
        unsigned long long slotCycles =
                (numerator + (unsigned long long)FREQ - 1ULL) / (unsigned long long)FREQ;
        if (slotCycles == 0)
                slotCycles = 1;

        return ((double)buflen * (double)MHz) / (double)slotCycles;
}

// P93: playback-rate changes are applied only by RestartForMonitorSync().
//
void    RestartForMonitorSync (void)
{
#ifndef NSFPLAYER
        // UI thread: only post an atomic request. The actual SoundOFF/SoundON
        // transition is performed by UpdateDRC() on the NES thread after the
        // current frame's SwapBuffers, so DirectSound is never manipulated
        // concurrently with APU::Run from the menu thread.
        InterlockedExchange(&g_AudioRestartPending, 1L);
#endif /* !NSFPLAYER */
}

void    UpdateDRC (void)
{
#ifndef NSFPLAYER
        // P93: no runtime DirectSound frequency feedback/control. The only
        // allowed rate transition is the explicit MMR toggle request posted
        // by RestartForMonitorSync(). UpdateDRC already runs after the frame's
        // SwapBuffers, so the one-time SoundOFF/SoundON transition cannot race
        // the APU producer and does not sit inside CPU/PPU execution.
        LONG restart = InterlockedExchange(&g_AudioRestartPending, 0L);
        if (restart && Buffer && isEnabled)
        {
                SoundOFF();
                SoundON();
                return;
        }
        if (!Buffer || !isEnabled)
                return;
        if (!MonitorSync::IsEnabled())
        {
                drc_play_freq = FREQ;
                return;
        }

        // ============================================================
        // P102: throttled ring-phase feedback.
        //
        // The P90+ MMR audio path is fully deterministic and fully
        // open-loop: the producer writes slots blind and nothing ever
        // verifies that the write cursor keeps a safe distance from the
        // DirectSound play cursor. Two consequences were measured on the
        // user's machine (Log 2026-09-18):
        //
        //   1. PaceSlot() re-anchors its target sequence to live DWM
        //      composition timestamps every frame, so the producer
        //      effectively runs at the REAL composition rate (measured
        //      60.0108 fps, a 17/17/16 ms 3-frame beat), while the
        //      consumer rate is fixed at SoundON time from the NOMINAL
        //      reported target (44100 * 60.000/60.0988 = 44027 Hz).
        //      That +9 samples/s mismatch makes the write cursor lap
        //      the play cursor roughly every 8 minutes -> the periodic
        //      "random" crackle.
        //
        //   2. Any producer stall (display transition, scheduler hiccup,
        //      DWM maintenance) permanently shifts the ring phase, and
        //      nothing ever pulls it back.
        //
        // This controller restores the pre-P90 self-healing property
        // WITHOUT reintroducing per-frame audiodg IPC: one
        // GetCurrentPosition per 32 frames (~0.5 s), executed here in
        // UpdateDRC -- the post-SwapBuffers point the P90 notes
        // explicitly identified as safe for audio maintenance. A
        // periodic SetFrequency trim of at most +-0.3% (about 5 cents,
        // inaudible) is applied only when the measured lead leaves the
        // 1.5..4.5 slot band; in the 0.7..5.3 slot danger zone the write
        // cursor is snap-re-anchored 4 slots ahead of the play cursor
        // instead (see AudioReanchorRing). Steady state costs at most a
        // couple of SetFrequency calls per minute and typically none
        // once converged.
        // ============================================================
        if (!GFX::MatchMonitorRate || LockSize == 0)
                return;
        if (InterlockedExchangeAdd(&g_AudioPlayPending, 0L) != 0L)
                return;         // still priming; the consumer is not running yet

        if (++drc_feedback_check_count < DRC_FEEDBACK_INTERVAL)
                return;
        drc_feedback_check_count = 0;

        DWORD playPos = 0;
        if (FAILED(Buffer->GetCurrentPosition(&playPos, NULL)))
                return;

        DWORD slotBytes = (DWORD)LockSize;
        DWORD ringBytes = slotBytes * FRAMEBUF;
        if (ringBytes == 0 || playPos >= ringBytes)
                return;

        DWORD writePos = (DWORD)next_pos * slotBytes;
        LONG fillBytes = (LONG)((writePos + ringBytes - playPos) % ringBytes);
        double fillSlots = (double)fillBytes / (double)slotBytes;

        if (fillSlots < DRC_FILL_SNAP_LO || fillSlots > DRC_FILL_SNAP_HI)
        {
                // Danger zone: the cursors are about to collide (or the
                // phase was scrambled by a stall). Repair immediately by
                // re-anchoring the write cursor with a fresh lead.
                AudioReanchorRing();
                return;
        }

        if (fillSlots < DRC_FILL_MIN || fillSlots > DRC_FILL_MAX)
        {
                double base = (drc_base_freq > 0) ? (double)drc_base_freq : (double)FREQ;
                double want = base * (1.0 + DRC_FILL_KP * (fillSlots - DRC_FILL_TARGET));
                double fmin = base * (1.0 - DRC_FREQ_TRIM_MAX);
                double fmax = base * (1.0 + DRC_FREQ_TRIM_MAX);
                if (want < fmin) want = fmin;
                if (want > fmax) want = fmax;
                DWORD newFreq = (DWORD)(want + 0.5);
                if (newFreq < 100)   newFreq = 100;
                if (newFreq > 100000) newFreq = 100000;
                if ((LONG)newFreq != InterlockedExchangeAdd(&g_AudioCurrentFreq, 0L))
                {
                        if (SUCCEEDED(Buffer->SetFrequency(newFreq)))
                        {
                                InterlockedIncrement(&g_AudioSetFreqCalls);
                                InterlockedExchange(&g_AudioCurrentFreq, (LONG)newFreq);
                        }
                }
                drc_play_freq = newFreq;
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
        drc_play_freq = FREQ;
        // A disabled MMR mode must not leave a stale restart request waiting
        // for a future frame.  The next SoundON() will select the normal
        // 44100-Hz path directly.
        InterlockedExchange(&g_AudioRestartPending, 0L);
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
                // P90: the MMR path does not call GetCurrentPosition at all.
                // The write-ahead check uses a QPC-predicted consumer slot,
                // keeping audiodg.exe out of the steady-state NES thread.
                // ============================================================
                if (isEnabled && Buffer && GFX::MatchMonitorRate)
                {
                        LARGE_INTEGER p73Run = {0};
                        QueryPerformanceCounter(&p73Run);
                        // P93: producer cadence is already owned by
                        // GFX::DrawScreen()/MonitorSync::PaceFrame().  The
                        // DirectSound ring is deliberately written without
                        // polling, notifications, QPC cursor prediction, or
                        // Sleep-based safety waits.  Four primed slots provide
                        // the initial headroom; equal producer/consumer clocks
                        // preserve the ring phase thereafter.
                        GFX::SetMMRProducerTrace(
                                p73Run.QuadPart, p73Run.QuadPart, p73Run.QuadPart,
                                0, 0, 0, 0, 0, 0);
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
                        // P98: fade in the very first primed slot after a SoundOFF()/
                        // SoundON() restart (fullscreen toggle, MMR audio-rate restart,
                        // etc). SoundON() hard-stops and re-Play()s the DirectSound
                        // buffer; the waveform is essentially never sitting on a zero
                        // crossing at that instant, so jumping straight from silence to
                        // full-amplitude audio produces an audible click/crackle right
                        // at the transition. Ramp only this one slot's samples from 0 to
                        // full amplitude -- every other slot (including this one once
                        // primed > 0) is copied unchanged, so normal playback is
                        // completely untouched.
                        LONG primedBefore = InterlockedExchangeAdd(&g_AudioPrimeSlots, 0L);
                        if (primedBefore == 0 && buflen > 0)
                        {
                                static short s_FadeInBuf[4096];
                                int n = buflen;
                                if (n > 4096) n = 4096;
                                for (int fi = 0; fi < n; fi++)
                                {
                                        double gain = (double)fi / (double)buflen;
                                        s_FadeInBuf[fi] = (short)((double)buffer[fi] * gain);
                                }
                                Try(Buffer->Lock(next_pos * LockSize, LockSize, &bufPtr, &bufBytes, NULL, 0, 0), Lang::GetString(LANG_ERR_APU_BUFFER));
                                memcpy(bufPtr, s_FadeInBuf, bufBytes);
                                Try(Buffer->Unlock(bufPtr, bufBytes, NULL, 0), Lang::GetString(LANG_ERR_APU_BUFFER));
                        }
                        else
                        {
                                Try(Buffer->Lock(next_pos * LockSize, LockSize, &bufPtr, &bufBytes, NULL, 0, 0), Lang::GetString(LANG_ERR_APU_BUFFER));
                                memcpy(bufPtr, buffer, bufBytes);
                                Try(Buffer->Unlock(bufPtr, bufBytes, NULL, 0), Lang::GetString(LANG_ERR_APU_BUFFER));
                        }

                        // P90: prime four complete slots before starting
                        // playback. The buffer is stopped during this phase,
                        // so the initial writes are independent of the hardware
                        // play cursor and leave ~66.7 ms of lead after Play().
                        LONG primed = InterlockedIncrement(&g_AudioPrimeSlots);
                        LONG pendingPlayNow = InterlockedExchangeAdd(&g_AudioPlayPending, 0L);
                        if (pendingPlayNow && primed >= AUDIO_PRIME_SLOTS)
                        {
                                HRESULT playHr = Buffer->Play(0, 0, DSBPLAY_LOOPING);
                                if (SUCCEEDED(playHr))
                                {
                                        InterlockedExchange(&g_AudioPlayPending, 0L);
                                        InterlockedIncrement(&g_AudioPlayStarts);
                                }
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
