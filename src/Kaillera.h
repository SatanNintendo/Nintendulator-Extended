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
#define WM_APP_KAILLERA_STARTGAME	(WM_APP + 2)	// game callback fired -> validate + launch
#define WM_APP_KAILLERA_ENDED		(WM_APP + 3)	// session ended (wParam = Kaillera::EndReason)
#define WM_APP_KAILLERA_CHAT		(WM_APP + 4)	// chat line received (lParam = heap TCHAR[])
#define WM_APP_KAILLERA_DROPPED		(WM_APP + 5)	// player dropped (lParam = heap TCHAR[])

namespace Kaillera
{
// ─── Session state (read-only for the rest of the program) ──────────────────
extern volatile BOOL	Active;		// TRUE while a netplay session is exchanging input
extern int		PlayerIndex;		// our player number (1-based; 0 = spectator)
extern int		NumPlayers;			// number of players (not counting spectators) in the session

// ─── Setup / teardown (UI thread) ───────────────────────────────────────────
void	Init (void);		// load kailleraclient.dll and resolve exports
void	Destroy (void);		// end any active session and unload the DLL
BOOL	Available (void);		// TRUE if the client DLL was loaded successfully

// ─── Session control (UI thread) ────────────────────────────────────────────
BOOL	Connect (void);		// validate configuration and open the Kaillera server browser
void	Disconnect (void);		// end the active netplay session
BOOL	Chat (HWND hWnd);		// open the in-game chat dialog and send the message

// ─── UI-thread handlers for the WM_APP_KAILLERA_* messages ─────────────────
void	OnStartGame (void);
void	OnEnded (WPARAM reason);
void	OnChat (TCHAR *text);		// takes ownership of the heap string
void	OnDropped (TCHAR *text);		// takes ownership of the heap string

// ─── Helpers (UI thread) ────────────────────────────────────────────────────
void	UpdateMenus (void);		// refresh enable/gray state of the Netplay menu items
BOOL	Guard (void);			// TRUE (with a message) if an action is blocked by netplay

// ─── Per-frame input exchange (emulation thread) ────────────────────────────
void	FrameInput (void);		// called from Controllers::UpdateInput() when Active
}
