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
// P102: soft-pause lifecycle. SoftPause() mutes the ring when the NES
// producer thread exits through a soft stop (fullscreen / savestate /
// reset) while DirectSound keeps playing; SoftResume() re-anchors the
// write cursor a safe lead ahead of the play cursor when the producer
// is about to start again. Neither touches Stop/Play/SetFrequency or
// SetCurrentPosition, so the DirectSound driver state is preserved.
void    SoftPause       (void);
void    SoftResume      (void);
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
void    RestartForMonitorSync (void); // posts one safe audio restart at frame end
// Kaillera netplay: called by Kaillera::OnEnded() (UI thread, AFTER
// NES::Stop) to end the netplay audio-rate trim and clear its state, so
// single-player audio returns to the normal P103 discipline.
void    ResetNetplayAudio (void);

#ifndef NSFPLAYER
// Tell the APU that the monitor sync module needs to be informed of the
// current NES region. Implemented in APU.cpp; declared here so callers
// can reach it without including MonitorSync.h.
// NSFPLAYER build does not use MonitorSync, so the function is absent there.
void    NotifyMonitorSyncRegion (void);

// MMR/DirectSound diagnostic counters.
long    GetAudioWorkerPolls (void);
long    GetAudioSetFreqCalls (void);
long    GetAudioPlayStarts (void);
long    GetAudioSafetyWaits (void);
long    GetAudioCurrentFreq (void);
long    GetAudioPlayPending (void);
long    GetAudioPrimeSlots (void);
// Kaillera netplay: number of audio slots dropped (never written) by the
// netplay gate in APU::Run because the slot was inside the live mix
// window. Surfaced in the timing log so the netplay rate trim can be
// verified (should stay at or near zero once the trim has converged).
long    GetAudioNetplaySkips (void);
// v4: number of drained-ring re-anchors performed by the netplay gate
// (brief silence + fade-in - the audible recovery event). Surfaced next
// to netplaySkips so the two recovery kinds can be told apart.
long    GetAudioNetplayReanchors (void);
#endif  /* !NSFPLAYER */

int     MAPINT  IntRead (int, int);
void    MAPINT  IntWrite (int, int, int);
} // namespace APU
