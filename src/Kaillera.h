/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) QMT Productions
 *
 * Kaillera netplay support - public interface
 *
 * All functions marked (UI) must be called from the main/UI thread.
 * FrameInput() is called from the NES emulation thread once per frame.
 * Everything else is internal to Kaillera.cpp.
 */

#pragma once
#include "kailleraclient.h"

// Custom window messages used to marshal events from the Kaillera DLL
// threads (game callback / chat / drop notifications) onto the UI thread.
// WM_APP_SETTITLE (WM_APP + 1) is already used by UpdateTitlebar().
#define WM_APP_KAILLERA_STARTGAME       (WM_APP + 2)    // game callback fired -> validate + launch
#define WM_APP_KAILLERA_ENDED           (WM_APP + 3)    // session ended (wParam = Kaillera::EndReason)
#define WM_APP_KAILLERA_CHAT            (WM_APP + 4)    // chat line received (lParam = heap TCHAR[])
#define WM_APP_KAILLERA_DROPPED         (WM_APP + 5)    // player dropped (lParam = heap TCHAR[])

namespace Kaillera
{
// ─── Session state (read-only for the rest of the program) ──────────────────
extern volatile BOOL    Active;         // TRUE while a netplay session is exchanging input
extern int              PlayerIndex;            // our player number (1-based; 0 = spectator)
extern int              NumPlayers;                     // number of players (not counting spectators) in the session

// ─── Setup / teardown (UI thread) ───────────────────────────────────────────
void    Init (void);            // load kailleraclient.dll and resolve exports
void    Destroy (void);         // end any active session and unload the DLL
BOOL    Available (void);               // TRUE if the client DLL was loaded successfully

// ─── Session control (UI thread) ────────────────────────────────────────────
BOOL    Connect (void);         // validate configuration and open the Kaillera server browser
void    Disconnect (void);              // end the active netplay session
BOOL    Chat (HWND hWnd);               // open the in-game chat dialog and send the message

// ─── UI-thread handlers for the WM_APP_KAILLERA_* messages ─────────────────
void    OnStartGame (void);
void    OnEnded (WPARAM reason);
void    OnChat (TCHAR *text);           // takes ownership of the heap string
void    OnDropped (TCHAR *text);                // takes ownership of the heap string

// ─── Helpers (UI thread) ────────────────────────────────────────────────────
void    UpdateMenus (void);             // refresh enable/gray state of the Netplay menu items
BOOL    Guard (void);                   // TRUE (with a message) if an action is blocked by netplay

// ─── Per-frame input exchange (emulation thread) ────────────────────────────
void    FrameInput (void);              // called from Controllers::UpdateInput() when Active

// ─── v4 netplay diagnostics ─────────────────────────────────────────────────
// The v3 test log proved the remaining ~10 s micro-freeze lives between two
// emulation-frame PUBLICATIONS (prodGap/renderGap/present all spike to
// 5 vblanks while every measured pipeline stage stays fast and the audio
// gate never blocks). The only unmeasured code in that window is the
// lockstep wait inside kailleraModifyPlayValues - so v4 measures it:
//
//   - FrameInput() times every kailleraModifyPlayValues() call. Waits over
//     10 ms are appended to %TEMP%\nintendulator_events.log as
//     "NETSTALL exch=N wait=X.XXms" (thread-safe, hard-capped, survives the
//     whole session - unlike the timing log, which is overwritten by a
//     periodic dump every 10 seconds).
//   - The GFX publication path (same emulation thread, later in the same
//     frame) copies GetLockstepWaitLastUs() into each frame's queue packet,
//     so every timing-log row now ends with "lsWait=" - the definitive
//     per-frame discriminator between a network-imported stall and a local
//     one.
//   - GFX also logs "PRESGAP ..." events (presentation-gap stalls) with the
//     matching lsWait, and APU logs AUDIODROP/AUDIOANCHOR/AUDIOTRIM events
//     so the audio ring state is visible across the whole session too.
//
// LogDiagEvent is callable from any thread. ResetDiagCounters() truncates
// the event log and zeroes the lockstep statistics; OnStartGame calls it
// so each test session starts with a clean file.
void    LogDiagEvent (LPCTSTR fmt, ...);
void    ResetDiagCounters (void);
long    GetLockstepWaitLastUs (void);   // last exchange wait, microseconds
void    GetLockstepStats (long *n, double *meanMs, double *maxMs,
                        long *over8ms, long *over20ms, long *over50ms, long *over100ms);
}
