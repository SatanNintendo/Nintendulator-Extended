/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) QMT Productions
 */

#pragma once

#ifndef NSFPLAYER
#include <mmsystem.h>
#define DIRECTSOUND_VERSION 0x0800
#include <dsound.h>
#endif  /* !NSFPLAYER */

namespace APU
{
extern short    *buffer;
extern unsigned long    LockSize;
extern int      buflen;
extern int      InternalClock;
// NRS ported settings
extern bool     NonlinearMixing;            // true = use 2A03 DAC nonlinear formula
extern bool     BootWithDisabledFrameIRQ;   // true = start with $4017 bit6 set (Famiclone)

#ifdef  NSFPLAYER
extern  short   sample_pos;
extern  BOOL    sample_ok;
#endif  /* NSFPLAYER */

namespace DPCM
{
        void    Fetch (void);
}

void    Init            (void);
void    Destroy         (void);
void    Start           (void);
void    Stop            (void);
#ifndef NSFPLAYER
int     Save            (FILE *);
int     Load            (FILE *, int ver);
void    SaveSettings    (HKEY);
void    LoadSettings    (HKEY);
void    SoundOFF        (void);
void    SoundON         (void);
#endif  /* !NSFPLAYER */
void    PowerOn         (void);
void    Reset           (void);
#ifndef NSFPLAYER
void    Config          (void);
#endif  /* !NSFPLAYER */
void    Run             (void);
void    SetRegion       (void);
void    UpdateDRC       (void);
void    ResetDRC        (void);

#ifndef NSFPLAYER
// Tell the APU that the monitor sync module needs to be informed of the
// current NES region. Implemented in APU.cpp; declared here so callers
// can reach it without including MonitorSync.h.
// NSFPLAYER build does not use MonitorSync, so the function is absent there.
void    NotifyMonitorSyncRegion (void);

// P30: dedicated audio-control background thread.
//
// Starts/stops a low-priority worker thread that owns 100% of the
// IDirectSoundBuffer::GetCurrentPosition / SetFrequency IPC traffic
// into audiodg.exe. Called from MonitorSync::Enable(TRUE/FALSE), the
// same lifecycle point that starts/stops the P28 vblank-poller thread.
// Safe to call even if the DirectSound buffer does not exist yet (the
// worker simply skips its work for that tick); safe to call StopAudioCtrlThread
// even if the thread was never started.
void    StartAudioCtrlThread (void);
void    StopAudioCtrlThread  (void);
#endif  /* !NSFPLAYER */

int     MAPINT  IntRead (int, int);
void    MAPINT  IntWrite (int, int, int);
} // namespace APU
