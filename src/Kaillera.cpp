/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) QMT Productions
 *
 * Kaillera netplay support - implementation
 *
 * Integration model (modeled after the proven patterns used by Nestopia
 * and FinalBurn Neo, adapted to Nintendulator's threading model):
 *
 *  - kailleraclient.dll is loaded dynamically at startup.  When the DLL is
 *    missing (or has the wrong bitness) the emulator behaves exactly as
 *    before - netplay is simply unavailable.
 *
 *  - Netplay > Connect... runs the Kaillera server browser (the DLL's own
 *    window) on a dedicated thread, so the emulator UI stays responsive.
 *
 *  - When a game starts, the DLL invokes our game callback on its own
 *    thread.  The callback only stores the parameters and posts
 *    WM_APP_KAILLERA_STARTGAME to the main window; the UI thread then
 *    validates the controller configuration, hard-resets the NES (so that
 *    every client starts from an identical power-on state) and starts the
 *    emulation.  The callback itself stays alive until the session ends,
 *    which is what most client builds (including the original 0.9) expect.
 *
 *  - Once per frame (scanline 241, from the NES emulation thread),
 *    Controllers::UpdateInput() hands control to Kaillera::FrameInput():
 *    the local controller byte is sent through kailleraModifyPlayValues()
 *    (which blocks until every player has delivered their input for that
 *    frame - this is what keeps all clients in lockstep), and the combined
 *    result is injected back into the controller ports.
 *
 *  - The first 60 frames after a game start are run with neutral input
 *    (the same "toss the first N frames" approach FinalBurn Neo uses) -
 *    the client DLLs need a few frames to stabilize the input pipeline
 *    after connecting, and this guarantees every client runs identical
 *    frames.
 *
 *  - Savestates, movies, resets, controller reconfiguration and Game
 *    Genie toggling are blocked while a session is active, because any
 *    local-only state change would instantly desynchronize the game.
 *    The master's PPU mode (NTSC/PAL/Hybrid) and Game Genie state are
 *    announced in the first exchange and verified by every client.
 */

#include "stdafx.h"
#include "Nintendulator.h"
#include "resource.h"
#include "MapperInterface.h"
#include "NES.h"
#include "Controllers.h"
#include "Movie.h"
#include "Lang.h"
#include "Theme.h"
#include "Kaillera.h"
#include "kailleraclient.h"

namespace Kaillera
{

/* ──────────────────────────────────────────────────────────────────────────
 * Session state (visible to the rest of the program)
 * ────────────────────────────────────────────────────────────────────────── */

volatile BOOL   Active = FALSE;         // TRUE while input is being exchanged
int             PlayerIndex = 0;                        // our player number, 1-based (0 = spectator)
int             NumPlayers = 0;                         // players in the current session

/* ──────────────────────────────────────────────────────────────────────────
 * Client DLL
 * ────────────────────────────────────────────────────────────────────────── */

static HMODULE                          hClientDLL = NULL;
static kailleraGetVersionFunc           p_GetVersion = NULL;
static kailleraInitFunc                 p_Init = NULL;
static kailleraShutdownFunc             p_Shutdown = NULL;
static kailleraSetInfosFunc             p_SetInfos = NULL;
static kailleraSelectServerDialogFunc   p_SelectServerDialog = NULL;
static kailleraModifyPlayValuesFunc     p_ModifyPlayValues = NULL;
static kailleraChatSendFunc             p_ChatSend = NULL;
static kailleraEndGameFunc              p_EndGame = NULL;

/* Resolve an export, taking stdcall name decoration into account.
 * 32-bit MSVC-built DLLs (the original client 0.9, SupraClient, ...) export
 * decorated names ("_kailleraInit@0"), while MinGW-built and 64-bit DLLs use
 * undecorated names - try both so that every existing client build works.
 * NOTE: GetProcAddress always takes an ANSI string, never TCHAR. */
static FARPROC Resolve (HMODULE hDLL, const char *name, int argBytes)
{
#ifdef _M_IX86
        char decorated[64];
        _snprintf(decorated, sizeof(decorated), "_%s@%d", name, argBytes);
        FARPROC proc = GetProcAddress(hDLL, decorated);
        if (proc != NULL)
                return proc;
#else
        (void)argBytes; // no stdcall decoration on x64
#endif
        return GetProcAddress(hDLL, name);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Session state (internal)
 * ────────────────────────────────────────────────────────────────────────── */

// wParam values used with WM_APP_KAILLERA_ENDED
enum EndReason
{
        END_USER = 0,           // the local user disconnected
        END_LOST = 1,           // network error / another player dropped
        END_SETTINGS = 2,       // master's emulator settings differ from ours
        END_REJECTED = 3        // the game start was rejected (message already shown)
};

// Command bits carried in the second byte of the per-player packet
#define KAILLERA_CMD_STARTUP    0x01    // frame-1 packet from player 1
#define KAILLERA_CMD_GENIE      0x08    // master has Game Genie enabled

// Number of neutral-input frames to run after a game starts, while the
// client DLL stabilizes its input pipeline (same approach as FBNeo).
#define KAILLERA_SYNC_FRAMES    60

// Number of frames exchanged per second (NTSC); used only for the timeout
// in the game callback, so an approximation is fine.
#define KAILLERA_POLL_MS        250

static volatile BOOL    userDisconnecting = FALSE;      // TRUE: don't report "connection lost"
static volatile BOOL    startPending = FALSE;           // game callback is waiting for the UI
static volatile int     pendingPlayer = 0;                      // player number from the game callback
static volatile int     pendingPlayers = 0;                     // player count from the game callback
static HANDLE           hDialogThread = NULL;           // server browser thread
static volatile BOOL    dialogRunning = FALSE;          // TRUE while the browser window is open

// Controller slots captured at connect time.
// 2-player mode:  slot 0 = Port 1,  slot 1 = Port 2  (Standard Controllers)
// 4-player mode:  slots 0-3 = Four Score sub-controllers 1-4
static Controllers::StdPort *slotPort[4] = { NULL, NULL, NULL, NULL };
static int                      slotCount = 0;

// Per-frame state (emulation thread only, except syncFrames which is
// initialized by the UI thread before Active is set)
static volatile int     syncFrames = 0;
static int                      frameCounter = 0;
static BOOL                     startupChecked = FALSE;

// Buffers passed to kailleraSetInfos(); kept static because the DLL is free
// to hold on to the pointers.
static char                     appName[128];
static char                     gameList[512];

/* ──────────────────────────────────────────────────────────────────────────
 * Helpers
 * ────────────────────────────────────────────────────────────────────────── */

static void KailleraMsg (LangStringID id)
{
        MessageBox(hMainWnd, Lang::GetString(id), Lang::GetString(LANG_NETPLAY_TITLE), MB_OK | MB_ICONWARNING);
}

/* ──────────────────────────────────────────────────────────────────────────
 * DLL callbacks (invoked on DLL-owned threads!)
 * ────────────────────────────────────────────────────────────────────────── */

static int WINAPI GameCallback (char * /*game*/, int player, int numplayers)
{
        // The kaillera API passes the player number 1-based, with 0 meaning
        // spectator.  Anything outside that range is rejected (same as Nestopia).
        if ((numplayers < 1) || (numplayers > 8) || (player < 0) || (player > numplayers))
                return 0;

        pendingPlayer = player;
        pendingPlayers = numplayers;
        userDisconnecting = FALSE;
        startPending = TRUE;
        PostMessage(hMainWnd, WM_APP_KAILLERA_STARTGAME, 0, 0);

        // Wait for the UI thread to launch the game (it clears startPending).
        // The timeout only guards against the window already being gone.
        for (int i = 0; (i < 3000) && startPending; i++)
                Sleep(10);

        if (!Active)
                return 0;               // rejected or shut down - nothing to play

        // Keep the callback alive for the duration of the game.  The original
        // client 0.9 expects the emulation to run inside this callback and to
        // return only when the game is over; modern builds (SupraClient etc.)
        // do not care, so staying here is the maximally compatible behavior
        // (FinalBurn Neo does exactly the same).
        while (Active)
                Sleep(KAILLERA_POLL_MS);

        return 0;
}

/* Convert an ANSI string pair "nick"/"text" into a heap-allocated TCHAR
 * buffer "<nick> <text>" and post it to the UI thread. */
static void PostFormatted (UINT msg, const char *prefix, const char *text)
{
        if ((prefix == NULL) || (hMainWnd == NULL))
                return;
        TCHAR *buf;
        int plen = (int)strlen(prefix);
        int tlen = (text != NULL) ? (int)strlen(text) : 0;
#ifdef UNICODE
        int wplen = MultiByteToWideChar(CP_ACP, 0, prefix, plen, NULL, 0);
        int wtlen = (text != NULL) ? MultiByteToWideChar(CP_ACP, 0, text, tlen, NULL, 0) : 0;
        buf = new TCHAR[wplen + wtlen + 2];     // + space + NUL
        MultiByteToWideChar(CP_ACP, 0, prefix, plen, buf, wplen);
        buf[wplen] = _T(' ');
        if (text != NULL)
                MultiByteToWideChar(CP_ACP, 0, text, tlen, buf + wplen + 1, wtlen);
        buf[wplen + 1 + wtlen] = 0;
#else
        buf = new TCHAR[plen + tlen + 2];
        memcpy(buf, prefix, plen);
        buf[plen] = _T(' ');
        if (text != NULL)
                memcpy(buf + plen + 1, text, tlen);
        buf[plen + 1 + tlen] = 0;
#endif
        if (!PostMessage(hMainWnd, msg, 0, (LPARAM)buf))
                delete[] buf;   // window is gone - drop it
}

static void WINAPI ChatCallback (char *nick, char *text)
{
        if ((nick == NULL) || (nick[0] == 0) || (text == NULL))
                return;
        PostFormatted(WM_APP_KAILLERA_CHAT, nick, text);
}

static void WINAPI DropCallback (char *nick, int playernb)
{
        if ((nick == NULL) || (nick[0] == 0))
                return;
        // suffix the player number so it is clear who left
        char caption[320];
        _snprintf(caption, sizeof(caption), "%s (player %i)", nick, playernb);
        PostFormatted(WM_APP_KAILLERA_DROPPED, caption, "left the game");
}

/* ──────────────────────────────────────────────────────────────────────────
 * Server browser thread
 * ────────────────────────────────────────────────────────────────────────── */

static DWORD WINAPI DialogThread (LPVOID /*lpParameter*/)
{
        // The DLL creates and runs its own window + message pump here.  Passing
        // NULL as the parent keeps our own window visible and usable, and a
        // problem inside the browser can never freeze the emulator UI.
        dialogRunning = TRUE;
        p_SelectServerDialog(NULL);
        dialogRunning = FALSE;
        // Refresh the Netplay menu (re-enable Connect) once the window is
        // closed.  If a game is still active at this point the connection
        // will drop on its own, which ends the session through the regular
        // "connection lost" path.
        PostMessage(hMainWnd, WM_APP_KAILLERA_ENDED, END_USER, 0);
        return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Controller-slot management
 * ────────────────────────────────────────────────────────────────────────── */

/* Validate the current controller configuration for netplay and capture the
 * controller slots.  Returns FALSE (with the caller showing a message) if
 * the configuration is not usable.
 * NOTE: the configuration cannot change while a session is active because
 * Input > Setup is blocked, so the captured pointers stay valid. */
static BOOL CapturePorts (void)
{
        using namespace Controllers;

        slotCount = 0;
        slotPort[0] = slotPort[1] = slotPort[2] = slotPort[3] = NULL;

        // Devices on the expansion port run on purely local input (mice,
        // keyboards, ...) and cannot be synchronized - reject them outright.
        if (PortExp->Type != EXP_UNCONNECTED)
                return FALSE;

        // 2-player setup: standard controllers on both ports
        if ((Port1->Type == STD_STDCONTROLLER) && (Port2->Type == STD_STDCONTROLLER))
        {
                slotPort[0] = Port1;
                slotPort[1] = Port2;
                slotCount = 2;
                return TRUE;
        }

        // 4-player setup: Four Score (controllers 1+3) on port 1 and
        // Four Score 2 (controllers 2+4) on port 2, all standard controllers
        if ((Port1->Type == STD_FOURSCORE) && (Port2->Type == STD_FOURSCORE2)
         && (FSPort1->Type == STD_STDCONTROLLER) && (FSPort2->Type == STD_STDCONTROLLER)
         && (FSPort3->Type == STD_STDCONTROLLER) && (FSPort4->Type == STD_STDCONTROLLER))
        {
                slotPort[0] = FSPort1;
                slotPort[1] = FSPort2;
                slotPort[2] = FSPort3;
                slotPort[3] = FSPort4;
                slotCount = 4;
                return TRUE;
        }

        return FALSE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Setup / teardown
 * ────────────────────────────────────────────────────────────────────────── */

void Init (void)
{
        TCHAR dllPath[MAX_PATH];
        HMODULE hDLL;

        // Always load from the emulator's own directory (ProgPath has a trailing
        // backslash), independent of the current working directory.
        _sntprintf(dllPath, MAX_PATH, _T("%skailleraclient.dll"), ProgPath);
        hDLL = LoadLibrary(dllPath);
        if (hDLL == NULL)
        {
                // Not found or wrong architecture (e.g. a 32-bit DLL in the 64-bit
                // build) - try the 64-bit client name, like FBNeo does.
                _sntprintf(dllPath, MAX_PATH, _T("%skailleraclient64.dll"), ProgPath);
                hDLL = LoadLibrary(dllPath);
        }
        if (hDLL == NULL)
        {
                AddDebug(_T("Kaillera: kailleraclient.dll not found - netplay disabled."));
                return;
        }

        p_GetVersion             = (kailleraGetVersionFunc)        Resolve(hDLL, "kailleraGetVersion", 4);
        p_Init                   = (kailleraInitFunc)              Resolve(hDLL, "kailleraInit", 0);
        p_Shutdown               = (kailleraShutdownFunc)          Resolve(hDLL, "kailleraShutdown", 0);
        p_SetInfos               = (kailleraSetInfosFunc)          Resolve(hDLL, "kailleraSetInfos", 4);
        p_SelectServerDialog     = (kailleraSelectServerDialogFunc)Resolve(hDLL, "kailleraSelectServerDialog", 4);
        p_ModifyPlayValues       = (kailleraModifyPlayValuesFunc)  Resolve(hDLL, "kailleraModifyPlayValues", 8);
        p_ChatSend               = (kailleraChatSendFunc)          Resolve(hDLL, "kailleraChatSend", 4);
        p_EndGame                = (kailleraEndGameFunc)           Resolve(hDLL, "kailleraEndGame", 0);

        // All core entry points must exist - otherwise this is not a kaillera
        // client and we run without netplay.
        if ((p_Init == NULL) || (p_Shutdown == NULL) || (p_SetInfos == NULL)
         || (p_SelectServerDialog == NULL) || (p_ModifyPlayValues == NULL)
         || (p_ChatSend == NULL) || (p_EndGame == NULL))
        {
                AddDebug(_T("Kaillera: kailleraclient.dll is not a valid Kaillera client - netplay disabled."));
                FreeLibrary(hDLL);
                p_GetVersion = NULL;
                p_Init = NULL;
                p_Shutdown = NULL;
                p_SetInfos = NULL;
                p_SelectServerDialog = NULL;
                p_ModifyPlayValues = NULL;
                p_ChatSend = NULL;
                p_EndGame = NULL;
                return;
        }

        hClientDLL = hDLL;
        p_Init();

        if (p_GetVersion != NULL)
        {
                char ver[17];
                TCHAR wver[17];
                TCHAR msg[80];
                int i;
                ZeroMemory(ver, sizeof(ver));
                p_GetVersion(ver);
                for (i = 0; (i < 16) && (ver[i] != 0); i++)
                        wver[i] = (TCHAR)ver[i];
                wver[i] = 0;
                _sntprintf(msg, 80, _T("Kaillera: client DLL v%s loaded - netplay available."), wver);
                AddDebug(msg);
        }
        else
                AddDebug(_T("Kaillera: client DLL loaded - netplay available."));
}

void Destroy (void)
{
        // End any active session first.  p_EndGame also unblocks a potentially
        // stuck kailleraModifyPlayValues call on the emulation thread.
        if (Active)
        {
                userDisconnecting = TRUE;
                Active = FALSE;
                if (p_EndGame != NULL)
                        p_EndGame();
        }
        startPending = FALSE;

        if (hClientDLL == NULL)
                return;

        if (p_Shutdown != NULL)
                p_Shutdown();

        // Freeing the library while a DLL-owned thread still runs inside it
        // would crash the process.  Our browser thread (and, after EndGame +
        // Shutdown, the DLL's game thread) exits on its own - wait briefly,
        // and if it does not make it in time simply skip FreeLibrary: the
        // process is terminating and Windows reclaims everything anyway.
        BOOL canFree = TRUE;
        if (hDialogThread != NULL)
        {
                if (WaitForSingleObject(hDialogThread, 1000) == WAIT_OBJECT_0)
                {
                        CloseHandle(hDialogThread);
                        hDialogThread = NULL;
                }
                else
                        canFree = FALSE;        // thread still inside the DLL - do not free
        }
        if (canFree)
                FreeLibrary(hClientDLL);
        hClientDLL = NULL;

        p_GetVersion = NULL;
        p_Init = NULL;
        p_Shutdown = NULL;
        p_SetInfos = NULL;
        p_SelectServerDialog = NULL;
        p_ModifyPlayValues = NULL;
        p_ChatSend = NULL;
        p_EndGame = NULL;
}

BOOL Available (void)
{
        return (hClientDLL != NULL) ? TRUE : FALSE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Session control (UI thread)
 * ────────────────────────────────────────────────────────────────────────── */

BOOL Connect (void)
{
        if (Active)
                return FALSE;

        // The server browser is already open (or a game is being set up) -
        // never start a second one.
        if (dialogRunning)
                return FALSE;

        if (!Available())
        {
                KailleraMsg(LANG_MSG_NETPLAY_DLL_MISSING);
                return FALSE;
        }
        if (!NES::ROMLoaded)
        {
                KailleraMsg(LANG_MSG_NETPLAY_NEED_ROM);
                return FALSE;
        }
        if (RI.ROMType == ROM_NSF)
        {
                KailleraMsg(LANG_MSG_NETPLAY_NSF);
                return FALSE;
        }

        // Movies and netplay are mutually exclusive - a movie would record
        // local input instead of the synchronized network input.
        if (Movie::Mode)
                Movie::Stop();
        if (NES::Running)
                NES::Stop();

        if (!CapturePorts())
        {
                KailleraMsg(LANG_MSG_NETPLAY_CONTROLLERS);
                return FALSE;
        }

        // Game list offered in the Kaillera "create game" dialog: the base name
        // of the currently loaded ROM.  Every player must have the same ROM
        // loaded locally; the game name itself is only the room title.
        {
                const TCHAR *filename = (RI.Filename != NULL) ? RI.Filename : _T("");
                const TCHAR *base = filename + _tcslen(filename);
                TCHAR tname[256];
                TCHAR *dot;
                while ((base > filename) && (base[-1] != _T('\\')) && (base[-1] != _T('/')))
                        base--;
                _tcsncpy(tname, base, 255);
                tname[255] = 0;
                dot = _tcsrchr(tname, _T('.'));
                if ((dot != NULL) && (dot != tname))
                        *dot = 0;
                if (tname[0] == 0)
                        _tcscpy(tname, _T("NES Game"));
#ifdef UNICODE
                if (WideCharToMultiByte(CP_ACP, 0, tname, -1, gameList, (int)sizeof(gameList) - 2, NULL, NULL) == 0)
                        strcpy(gameList, "NES Game");
#else
                strncpy(gameList, tname, sizeof(gameList) - 2);
                gameList[sizeof(gameList) - 2] = 0;
#endif
                // the game list must end with two consecutive NUL bytes
                gameList[strlen(gameList) + 1] = 0;
        }

        strcpy(appName, "Nintendulator Extended 0.985 (Kaillera)");

        {
                kailleraInfos info;
                ZeroMemory(&info, sizeof(info));
                info.appName                    = appName;
                info.gameList                   = gameList;
                info.gameCallback               = GameCallback;
                info.chatReceivedCallback       = ChatCallback;
                info.clientDroppedCallback      = DropCallback;
                info.moreInfosCallback          = NULL;
                p_SetInfos(&info);
        }

        // Run the Kaillera server browser on its own thread (FBNeo pattern).
        // Any earlier browser thread has already exited (dialogRunning is
        // FALSE) - its handle can be released safely.
        userDisconnecting = FALSE;
        if (hDialogThread != NULL)
        {
                CloseHandle(hDialogThread);
                hDialogThread = NULL;
        }
        hDialogThread = CreateThread(NULL, 0, DialogThread, NULL, 0, NULL);
        if (hDialogThread == NULL)
        {
                AddDebug(_T("Kaillera: unable to start the server browser thread!"));
                return FALSE;
        }
        AddDebug(_T("Kaillera: server browser opened."));
        return TRUE;
}

void Disconnect (void)
{
        if (!Active)
                return;
        userDisconnecting = TRUE;
        Active = FALSE;
        if (p_EndGame != NULL)
                p_EndGame();    // also unblocks a pending input exchange
        PostMessage(hMainWnd, WM_APP_KAILLERA_ENDED, END_USER, 0);
}

/* ──────────────────────────────────────────────────────────────────────────
 * In-game chat (UI thread)
 * ────────────────────────────────────────────────────────────────────────── */

static INT_PTR CALLBACK ChatProc (HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
        switch (message)
        {
        case WM_INITDIALOG:
                SetWindowText(hDlg, Lang::GetString(LANG_NETPLAY_CHAT_TITLE));
                SetDlgItemText(hDlg, IDOK, Lang::GetString(LANG_DLG_OK));
                SetDlgItemText(hDlg, IDCANCEL, Lang::GetString(LANG_DLG_CANCEL));
                Theme::ApplyToDialog(hDlg);
                SetFocus(GetDlgItem(hDlg, IDC_NETPLAY_CHAT_EDIT));
                return FALSE;   // focus was set explicitly
        case WM_COMMAND:
                if (LOWORD(wParam) == IDOK)
                {
                        TCHAR text[256];
                        GetDlgItemText(hDlg, IDC_NETPLAY_CHAT_EDIT, text, 256);
                        if (text[0] != 0)
                        {
                                char ansi[256];
                                BOOL converted = FALSE;
#ifdef UNICODE
                                converted = (WideCharToMultiByte(CP_ACP, 0, text, -1, ansi, sizeof(ansi), NULL, NULL) != 0) ? TRUE : FALSE;
#else
                                strncpy(ansi, text, sizeof(ansi) - 1);
                                ansi[sizeof(ansi) - 1] = 0;
                                converted = TRUE;
#endif
                                if (converted && (p_ChatSend != NULL))
                                        p_ChatSend(ansi);
                        }
                        EndDialog(hDlg, IDOK);
                        return TRUE;
                }
                if (LOWORD(wParam) == IDCANCEL)
                {
                        EndDialog(hDlg, IDCANCEL);
                        return TRUE;
                }
                break;
        }
        return FALSE;
}

BOOL Chat (HWND hWnd)
{
        if (!Active)
                return FALSE;
        DialogBox(hInst, MAKEINTRESOURCE(IDD_NETPLAY_CHAT), hWnd, ChatProc);
        return TRUE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * UI-thread message handlers (WM_APP_KAILLERA_*)
 * ────────────────────────────────────────────────────────────────────────── */

static void RejectStart (LangStringID msg)
{
        // Release the game callback thread first - it must not wait for the
        // user to click "OK" in the message box below.
        startPending = FALSE;
        if (p_EndGame != NULL)
                p_EndGame();
        KailleraMsg(msg);
        UpdateMenus();
}

void OnStartGame (void)
{
        int me = pendingPlayer;
        int num = pendingPlayers;

        // The player number must fit into the captured controller slots
        // (spectators have no slot of their own and are always fine).
        if (me > slotCount)
        {
                RejectStart(LANG_MSG_NETPLAY_NO_SLOT);
                return;
        }
        if (num > slotCount)
        {
                RejectStart(LANG_MSG_NETPLAY_PLAYERS_LIMIT);
                return;
        }

        PlayerIndex = me;
        NumPlayers = num;
        syncFrames = KAILLERA_SYNC_FRAMES;
        frameCounter = 0;
        startupChecked = FALSE;

        // Hard-reset the NES so that every client begins the game from an
        // identical power-on state.  The emulation is stopped at this point
        // (Connect stops it), so this is exactly the ID_CPU_HARDRESET path.
        NES::Reset(RESET_HARD);

        Active = TRUE;
        startPending = FALSE;   // release the game callback thread

        NES::Start(FALSE);
        UpdateMenus();

        {
                TCHAR msg[128];
                if (me == 0)
                        _sntprintf(msg, 128, _T("Kaillera: game started - spectating %d player(s)."), num);
                else
                        _sntprintf(msg, 128, _T("Kaillera: game started - you are player %d of %d."), me, num);
                AddDebug(msg);
                PrintTitlebar(_T("%s"), msg);
        }
}

void OnEnded (WPARAM reason)
{
        BOOL wasActive = Active;

        Active = FALSE;
        startPending = FALSE;

        if (wasActive)
        {
                // Same pattern as ID_CPU_STOP: pumps messages while waiting for the
                // emulation thread to exit.  A blocked kailleraModifyPlayValues call
                // has already returned -1 by the time we get here (connection lost),
                // or was unblocked by kailleraEndGame (user disconnect), so this
                // cannot deadlock.
                if (NES::Running)
                        NES::Stop();
        }

        UpdateMenus();

        switch (reason)
        {
        case END_LOST:
                KailleraMsg(LANG_MSG_NETPLAY_CONN_LOST);
                break;
        case END_SETTINGS:
                KailleraMsg(LANG_MSG_NETPLAY_SETTINGS);
                break;
        }
        // END_USER and END_REJECTED are silent (rejected already showed its own
        // message, and a manual disconnect needs no further confirmation).
}

void OnChat (TCHAR *text)
{
        if (text != NULL)
        {
                AddDebug(text);
                delete[] text;
        }
}

void OnDropped (TCHAR *text)
{
        if (text != NULL)
        {
                AddDebug(text);
                PrintTitlebar(_T("%s"), text);
                delete[] text;
        }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Helpers (UI thread)
 * ────────────────────────────────────────────────────────────────────────── */

void UpdateMenus (void)
{
        EnableMenuItem(hMenu, ID_NETPLAY_CONNECT,     ((Active) || (dialogRunning)) ? MF_GRAYED : MF_ENABLED);
        EnableMenuItem(hMenu, ID_NETPLAY_DISCONNECT,  Active ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(hMenu, ID_NETPLAY_CHAT,        Active ? MF_ENABLED : MF_GRAYED);
}

BOOL Guard (void)
{
        if (!Active)
                return FALSE;
        KailleraMsg(LANG_MSG_NETPLAY_BLOCKED);
        return TRUE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Per-frame input exchange (NES emulation thread)
 * ────────────────────────────────────────────────────────────────────────── */

void FrameInput (void)
{
        unsigned char packet[8][2];     // 8 players max, 2 bytes each
        int me = PlayerIndex;   // 1-based, 0 = spectator
        int mySlot = ((me >= 1) && (me <= 4)) ? (me - 1) : -1;
        int ret, players, j;
        BOOL haveData;

        ZeroMemory(packet, sizeof(packet));

        // 1. Poll the local input into our own controller slot.  MOV_RECORD
        //    makes a Standard Controller store its freshly-read buttons into
        //    MovData[0] without touching anything else.
        if ((mySlot >= 0) && (mySlot < slotCount) && (slotPort[mySlot] != NULL))
        {
                slotPort[mySlot]->Frame(MOV_RECORD);
                if (slotPort[mySlot]->MovData != NULL)
                        packet[0][0] = slotPort[mySlot]->MovData[0];    // our input goes at offset 0
        }

        // 2. Player 1 announces its emulator settings (PPU mode, Game Genie)
        //    with the very first exchange, so every client can verify it is
        //    configured identically - any difference would desync instantly.
        if ((frameCounter == 0) && (me == 1))
                packet[0][1] = (unsigned char)(KAILLERA_CMD_STARTUP | ((NES::CurRegion & 3) << 1) | (NES::GameGenie ? KAILLERA_CMD_GENIE : 0));
        frameCounter++;

        // 3. Exchange input with the other players.  This call blocks until
        //    every player has delivered their input for this frame - it is the
        //    lockstep barrier that keeps all clients synchronized.
        ret = p_ModifyPlayValues(packet, 2);

        if (ret == -1)
        {
                // Network error, or another player left the game.
                Active = FALSE;
                if (!userDisconnecting)
                        PostMessage(hMainWnd, WM_APP_KAILLERA_ENDED, END_LOST, 0);
                return;
        }

        // 4. During the initial delay stage the DLL returns 0 (no data yet),
        //    and the first KAILLERA_SYNC_FRAMES frames are deliberately run
        //    with neutral input while the connections stabilize (FBNeo
        //    approach) - identical on every client, so no divergence.
        haveData = ((ret >= 2) && (syncFrames == 0)) ? TRUE : FALSE;
        if (syncFrames > 0)
                syncFrames--;
        players = ret / 2;
        if (players > 8)
                players = 8;

        // 5. One-time verification of the master's settings.
        if (haveData && !startupChecked)
        {
                startupChecked = TRUE;
                if (me != 1)
                {
                        unsigned char cmd = packet[0][1];       // player 1's settings byte
                        if ((cmd & KAILLERA_CMD_STARTUP) != 0)
                        {
                                int masterRegion = (cmd >> 1) & 3;
                                BOOL masterGenie = ((cmd & KAILLERA_CMD_GENIE) != 0) ? TRUE : FALSE;
                                if ((masterRegion != (int)NES::CurRegion) || (masterGenie != NES::GameGenie))
                                {
                                        Active = FALSE;
                                        PostMessage(hMainWnd, WM_APP_KAILLERA_ENDED, END_SETTINGS, 0);
                                        return;
                                }
                        }
                }
        }

        // 6. Inject the combined network input into all controller slots.
        //    MOV_PLAY makes a Standard Controller load its buttons from
        //    MovData[0], replacing the local input (this is the same mechanism
        //    movie playback uses, so it is well-tested emulator territory).
        //    Slots beyond the player count (and all slots during the sync
        //    stage) receive neutral zero input.
        for (j = 0; j < slotCount; j++)
        {
                unsigned char byte = 0;
                if (haveData && (j < players))
                        byte = packet[j][0];
                if ((slotPort[j] != NULL) && (slotPort[j]->MovData != NULL))
                {
                        slotPort[j]->MovData[0] = byte;
                        slotPort[j]->Frame(MOV_PLAY);
                }
        }
}

} // namespace Kaillera
