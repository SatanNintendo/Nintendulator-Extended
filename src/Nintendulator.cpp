/* Nintendulator - Win32 NES emulator written in C++
 * Copyright (C) 2002-2022 QMT Productions
 *
 * Based on NinthStar, a portable Win32 NES Emulator written in C++
 * Copyright (C) 2000 David de Regt
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "stdafx.h"
#include "Nintendulator.h"
#include "resource.h"
#include "Lang.h"
#include "MapperInterface.h"
#include "NES.h"
#include "CPU.h"
#include "PPU.h"
#include "APU.h"
#include "GFX.h"
#include "AVI.h"
#include "Debugger.h"
#include "Movie.h"
#include "Controllers.h"
#include "States.h"
#include "HeaderEdit.h"
#include "Theme.h"
#include "MonitorSync.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")

#define MAX_LOADSTRING 100

// Global Variables:
int ConfigVersion; // current version of configuration data
HINSTANCE hInst; // current instance
HWND hMainWnd; // main window
HMENU hMenu; // main window menu
HACCEL hAccelTable; // accelerators
int SizeMult; // size multiplier
BOOL FixAspect; // fix aspect ratio for NTSC/PAL
TCHAR ProgPath[MAX_PATH]; // program path
TCHAR Path_ROM[MAX_PATH]; // current ROM directory
TCHAR Path_NMV[MAX_PATH]; // current movie directory
TCHAR Path_AVI[MAX_PATH]; // current AVI directory
TCHAR Path_PAL[MAX_PATH]; // current palette directory
TCHAR DataPath[MAX_PATH]; // user data path
BOOL MaskKeyboard = FALSE; // mask keyboard accelerators (for when Family Basic Keyboard is active)
BOOL MaskMouse = FALSE; // hide mouse cursor (for Arkanoid paddle and SNES Mouse)
HWND hDebug; // Debug Info window
BOOL dbgVisible; // whether or not the Debug window is open

TCHAR szTitle[MAX_LOADSTRING]; // The title bar text
TCHAR szWindowClass[MAX_LOADSTRING]; // The title bar text

// Forward declarations of functions included in this code module:
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK DebugWnd(HWND, UINT, WPARAM, LPARAM);

TCHAR TitlebarBuffer[256];
int TitlebarDelay;

// Theme subclass for the main window WndProc to handle dark mode colors
static LRESULT CALLBACK ThemeMainSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
        // Handle background erase for both themes to prevent flicker on ROM load.
        // hbrBackground is NULL in WNDCLASSEX, so this is the only place it's painted.
        if (uMsg == WM_ERASEBKGND)
        {
                HDC hdc = (HDC)wParam;
                RECT rc;
                GetClientRect(hWnd, &rc);
                FillRect(hdc, &rc, Theme::GetBackgroundBrush());
                return TRUE;
        }

        if (Theme::IsDark())
        {
                switch (uMsg)
                {
                case WM_CTLCOLORDLG:
                case WM_CTLCOLORSTATIC:
                {
                        HDC hdc = (HDC)wParam;
                        SetTextColor(hdc, Theme::GetTextColor());
                        SetBkColor(hdc, Theme::GetBgColor());
                        SetBkMode(hdc, TRANSPARENT);
                        return (LRESULT)Theme::GetBackgroundBrush();
                }
                case WM_CTLCOLORBTN:
                {
                        HDC hdc = (HDC)wParam;
                        SetTextColor(hdc, Theme::GetTextColor());
                        SetBkColor(hdc, Theme::GetBgColor());
                        SetBkMode(hdc, TRANSPARENT);
                        return (LRESULT)Theme::GetBackgroundBrush();
                }
                case WM_CTLCOLOREDIT:
                case WM_CTLCOLORLISTBOX:
                {
                        HDC hdc = (HDC)wParam;
                        SetTextColor(hdc, Theme::GetControlTextColor());
                        SetBkColor(hdc, Theme::GetControlBgColor());
                        return (LRESULT)Theme::GetControlBrush();
                }
                }
        }
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void BuildLanguageMenu(HMENU hMainMenu)
{
        int cnt = GetMenuItemCount(hMainMenu);
        HMENU hLangMenu = GetSubMenu(hMainMenu, cnt - 2); // Language before Help
        if (!hLangMenu) return;

        while (GetMenuItemCount(hLangMenu) > 0)
                DeleteMenu(hLangMenu, 0, MF_BYPOSITION);

        int langCount = Lang::GetLanguageCount();
        if (langCount == 0)
        {
                AppendMenu(hLangMenu, MF_STRING | MF_GRAYED, IDM_LANGUAGE_BASE,
                        Lang::GetString(LANG_NO_LANGUAGES));
                return;
        }

        for (int i = 0; i < langCount && i < 100; i++)
        {
                UINT flags = MF_STRING;
                if (_tcscmp(Lang::GetLanguageFileName(i), Lang::GetCurrentLanguage()) == 0)
                        flags |= MF_CHECKED;
                AppendMenu(hLangMenu, flags, IDM_LANGUAGE_BASE + i, Lang::GetLanguageDisplayName(i));
        }
}

int APIENTRY _tWinMain (HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
        size_t i;
        MSG msg;

        // Initialize global strings
        LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
        LoadString(hInstance, IDS_NINTENDULATOR, szWindowClass, MAX_LOADSTRING);
        MyRegisterClass(hInstance);

        GetModuleFileName(NULL, ProgPath, MAX_PATH);
        for (i = _tcslen(ProgPath); (i > 0) && (ProgPath[i] != _T('\\')); i--)
                ProgPath[i] = 0;

        // find our folder in Application Data, if it exists
        if (!SUCCEEDED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, DataPath)))
        {
                // if we can't even find that, then there's a much bigger problem...
                MessageBox(NULL, _T("FATAL: unable to locate Application Data folder"), _T("Nintendulator"), MB_OK | MB_ICONERROR);
                return FALSE;
        }
        PathAppend(DataPath, _T("Nintendulator"));
        if (GetFileAttributes(DataPath) == INVALID_FILE_ATTRIBUTES)
                CreateDirectory(DataPath, NULL);

        // Perform application initialization:
        if (!InitInstance (hInstance, nCmdShow))
                return FALSE;

        // Initialize the MonitorSync module now that the main window exists.
        // It loads DWM/WGL function pointers and queries the current monitor
        // refresh rate. Does not enable anything yet — that happens only when
        // the user toggles "Match Monitor Rate" on.
        MonitorSync::Init(hMainWnd);

        hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_NINTENDULATOR));

        // Initialize language system
        Lang::Init(hInstance);
        BuildLanguageMenu(GetMenu(hMainWnd));
        Lang::UpdateMenu(GetMenu(hMainWnd));
        // Lang::UpdateMenu uses ModifyMenu(MF_STRING) which resets checkmarks.
        // Restore them based on current variable values.
        GFX::SyncMenuChecks();
        Theme::SyncMenuChecks();
        // Re-apply controller mappings after language strings are loaded,
        // since Controllers::Init() was called before Lang::Init()
        Controllers::StdPort_SetMappings();
        Controllers::ExpPort_SetMappings();

        // Re-apply menu enable/disable states after Lang::UpdateMenu,
        // which resets MF_GRAYED on all modified items
        NES::SyncMenuStates();

        UpdateTitlebar();

        // Update debug dialog title now that language strings are loaded
        // (CreateDialog was called before Lang::Init, so WM_INITDIALOG used default English strings)
        if (hDebug)
                SetWindowText(hDebug, Lang::GetString(LANG_DLG_DEBUG_TITLE));

        // Apply the loaded theme to main window and debug dialog
        Theme::ApplyToMainWindow(hMainWnd);
        if (hDebug)
                Theme::ApplyToDialog(hDebug);

        // Show the main window AFTER all initialization is complete
        // to prevent flickering caused by menu updates on a visible window
        ShowWindow(hMainWnd, nCmdShow);

        timeBeginPeriod(1);

        if (lpCmdLine[0])
        {
                // grab a copy of the command line parms - we're gonna need to extract a single filename from it
                TCHAR *cmdline = _tcsdup(lpCmdLine);
                // and remember the pointer so we can safely free it at the end
                TCHAR *bkptr = cmdline;

                // if the filename is in quotes, strip them off (and ignore everything past it)
                if (cmdline[0] == _T('"'))
                {
                        cmdline++;
                        // yes, it IS possible for the second quote to not exist!
                        if (_tcschr(cmdline, '"'))
                                *_tcschr(cmdline, '"') = 0;
                }
                // otherwise just kill everything past the first space
                else if (_tcschr(cmdline, ' '))
                        *_tcschr(cmdline, ' ') = 0;

                NES::OpenFile(cmdline);
                // Allocated using _tcsdup()
                free(bkptr); // free up the memory from its original pointer
        }

        // Main message loop:
        while (GetMessage(&msg, NULL, 0, 0))
        {
                // There are several places where the emulation thread can stop itself
                // and those need to unacquire the controllers from the main thread
                if (NES::DoStop & STOPMODE_BREAK)
                {
                        NES::DoStop &= ~STOPMODE_BREAK;
                        Controllers::UnAcquire();
                }

                // If WM_CLOSE came in during fullscreen emulation, shut down Graphics
                // here to revert to Windowed mode, then re-post WM_CLOSE and let the
                // program exit. Without this, we'd be destroying the window out from
                // underneath DirectDraw, which causes it to crash.
                if (NES::DoStop & STOPMODE_QUIT)
                {
                        NES::DoStop &= ~STOPMODE_QUIT;
                        GFX::Stop();
                        PostMessage(hMainWnd, WM_CLOSE, 0, 0);
                }

                // The proper way to do this is to maintain a list of modeless dialog handles and check each of them here
                // or to keep one handle and have each modeless dialog set that handle whenever they get focus (KB 71450)
                // The problem is that mapper DLLs have no way of (un)registering any modeless dialogs they create
                // or notifying me that they've acquired focus and should be the ones passed to IsDialogMessage
                // Thus, this (probably incorrect) method is being used instead, based on the (currently) valid
                // assumption that all windows other than the main one (and modal dialogs) will be modeless dialogs
                // ProcessMessages() doesn't have this because it's only used in 2 places which don't matter
                // (namely, controller configuration and the brief delay when stopping emulation)
                HWND focus = GetActiveWindow();
                if ((focus != NULL) && (focus != hMainWnd) && IsDialogMessage(focus, &msg))
                        continue;
                if (MaskKeyboard || !TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
                {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                }
        }
        timeEndPeriod(1);

        return (int)msg.wParam;
}

//
// FUNCTION: MyRegisterClass()
//
// PURPOSE: Registers the window class.
//
// COMMENTS:
//
// This function and its usage is only necessary if you want this code
// to be compatible with Win32 systems prior to the 'RegisterClassEx'
// function that was added to Windows 95. It is important to call this function
// so that the application will get 'well formed' small icons associated
// with it.
//
ATOM MyRegisterClass (HINSTANCE hInstance)
{
        WNDCLASSEX wcex;

        wcex.cbSize = sizeof(WNDCLASSEX);

        wcex.style = 0;  // No CS_HREDRAW|CS_VREDRAW — GFX manages repaints; avoids intermediate erase/paint states on resize
        wcex.lpfnWndProc = WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hInstance;
        wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_NINTENDULATOR));
        wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = NULL;  // WM_ERASEBKGND handled manually in WndProc/Theme subclass
        wcex.lpszMenuName = MAKEINTRESOURCE(IDC_NINTENDULATOR);
        wcex.lpszClassName = szWindowClass;
        wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));

        return RegisterClassEx(&wcex);
}

//
// FUNCTION: InitInstance(HANDLE, int)
//
// PURPOSE: Saves instance handle and creates main window
//
// COMMENTS:
//
// In this function, we save the instance handle in a global variable and
// create and display the main program window.
//
BOOL InitInstance (HINSTANCE hInstance, int nCmdShow)
{
        GFX::DirectDraw = NULL; // gotta do this so we don't paint from nothing
        hInst = hInstance;
        hMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_NINTENDULATOR));
        int nesW = 256 * 3;
        int nesH = 240 * 3;
        RECT rc = { 0, 0, nesW, nesH };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;
        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);
        int posX = (scrW - winW) / 2;
        int posY = (scrH - winH) / 2;
        hMainWnd = CreateWindow(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW, posX, posY, winW, winH, NULL, hMenu, hInstance, NULL);
        if (!hMainWnd)
                return FALSE;
        // Defer ShowWindow until after all initialization (Lang, menu updates)
        // to prevent window flickering on startup
        DragAcceptFiles(hMainWnd, TRUE);

        // Apply theme subclass to main window for dark mode support
        SetWindowSubclass(hMainWnd, ThemeMainSubclass, 0, 0);

        hDebug = CreateDialog(hInst, MAKEINTRESOURCE(IDD_DEBUG), hMainWnd, DebugWnd);

        NES::Init();
        return TRUE;
}

//
// FUNCTION: WndProc(HWND, unsigned, WORD, LONG)
//
// PURPOSE: Processes messages for the main window.
//
// WM_COMMAND - process the application menu
// WM_PAINT - Paint the main window
// WM_DESTROY - post a quit message and return
//
//
LRESULT CALLBACK WndProc (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
        HDC hdc;
        PAINTSTRUCT ps;
        int wmId, wmEvent;
        TCHAR FileName[MAX_PATH];
        OPENFILENAME ofn;
        BOOL running = NES::Running;

        switch (message)
        {
        case WM_COMMAND:
                wmId = LOWORD(wParam);
                wmEvent = HIWORD(wParam);
                // Parse the menu selections:
                switch (wmId)
                {
                case ID_FILE_EXIT:
                        SendMessage(hWnd, WM_CLOSE, 0, 0);
                        break;
                case ID_FILE_OPEN: {
                        FileName[0] = 0;
                        ZeroMemory(&ofn, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hMainWnd;
                        ofn.hInstance = hInst;
                        // Build file filter using localized display names
                        TCHAR ROMFilter[1024];
                        TCHAR *pRF = ROMFilter;
                        // Helper: append a filter entry (display name + pattern)
                        #define APPEND_FILTER_ENTRY(name, pattern) do { \
                                _tcscpy(pRF, name); pRF += _tcslen(pRF) + 1; \
                                _tcscpy(pRF, pattern); pRF += _tcslen(pRF) + 1; \
                        } while(0)
                        APPEND_FILTER_ENTRY(Lang::GetString(LANG_FILTER_ROM_ALL), _T("*.NES;*.UNIF;*.UNF;*.FDS;*.NSF"));
                        APPEND_FILTER_ENTRY(Lang::GetString(LANG_FILTER_ROM_INES), _T("*.NES"));
                        APPEND_FILTER_ENTRY(Lang::GetString(LANG_FILTER_ROM_UNIF), _T("*.UNF;*.UNIF"));
                        APPEND_FILTER_ENTRY(Lang::GetString(LANG_FILTER_ROM_FDS), _T("*.FDS"));
                        APPEND_FILTER_ENTRY(Lang::GetString(LANG_FILTER_ROM_NSF), _T("*.NSF"));
                        *pRF = 0; // double null terminator
                        #undef APPEND_FILTER_ENTRY

                        ofn.lpstrFilter = ROMFilter;
                        ofn.lpstrCustomFilter = NULL;
                        ofn.nFilterIndex = 1;
                        ofn.lpstrFile = FileName;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrFileTitle = NULL;
                        ofn.nMaxFileTitle = 0;
                        ofn.lpstrInitialDir = Path_ROM;
                        ofn.lpstrTitle = Lang::GetString(LANG_OPEN_ROM_TITLE);
                        ofn.Flags = OFN_FILEMUSTEXIST;
                        ofn.lpstrDefExt = NULL;
                        ofn.lCustData = 0;
                        ofn.lpfnHook = NULL;
                        ofn.lpTemplateName = NULL;
                        if (GetOpenFileName(&ofn))
                        {
                                _tcscpy(Path_ROM, FileName);
                                Path_ROM[ofn.nFileOffset-1] = 0;
                                Theme::Reapply();
                                NES::Stop();
                                NES::OpenFile(FileName);
                        }
                        // Re-apply theme - GetOpenFileName can reset dark/light mode state
                        Theme::Reapply();
                        break;
                }
                case ID_FILE_CLOSE:
                        NES::Stop();
                        NES::CloseFile();
                        break;
                case ID_FILE_HEADER: {
                        FileName[0] = 0;
                        ZeroMemory(&ofn, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hMainWnd;
                        ofn.hInstance = hInst;
                        // Build header editor file filter using localized display name
                        TCHAR HdrFilter[256];
                        TCHAR *pHF = HdrFilter;
                        _tcscpy(pHF, Lang::GetString(LANG_FILTER_ROM_INES_SHORT)); pHF += _tcslen(pHF) + 1;
                        _tcscpy(pHF, _T("*.NES")); pHF += _tcslen(pHF) + 1;
                        *pHF = 0; // double null terminator

                        ofn.lpstrFilter = HdrFilter;
                        ofn.lpstrCustomFilter = NULL;
                        ofn.nFilterIndex = 1;
                        ofn.lpstrFile = FileName;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrFileTitle = NULL;
                        ofn.nMaxFileTitle = 0;
                        ofn.lpstrInitialDir = Path_ROM;
                        ofn.lpstrTitle = Lang::GetString(LANG_OPEN_HEADER_TITLE);
                        ofn.Flags = OFN_FILEMUSTEXIST;
                        ofn.lpstrDefExt = NULL;
                        ofn.lCustData = 0;
                        ofn.lpfnHook = NULL;
                        ofn.lpTemplateName = NULL;

                        if (GetOpenFileName(&ofn))
                        {
                                _tcscpy(Path_ROM, FileName);
                                Path_ROM[ofn.nFileOffset-1] = 0;
                                HeaderEdit::Open(FileName);
                        }
                        // Re-apply theme - GetOpenFileName can reset dark/light mode state
                        Theme::Reapply();
                        break;
                }
                case ID_FILE_AUTORUN:
                        NES::AutoRun = !NES::AutoRun;
                        if (NES::AutoRun)
                                CheckMenuItem(hMenu, ID_FILE_AUTORUN, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_FILE_AUTORUN, MF_UNCHECKED);
                        break;
                case ID_FILE_BROWSESAVES:
                        BrowseFolder(DataPath);
                        break;

                case ID_CPU_RUN:
                        NES::Start(FALSE);
                        break;
                case ID_CPU_STEP:
                        NES::Stop();    // need to stop first
                        NES::Start(TRUE);       // so the 'start' makes it through
                        break;
                case ID_CPU_STOP:
                        NES::Stop();
                        break;
                case ID_CPU_SOFTRESET:
                        NES::Pause(FALSE);
                        if (Movie::Mode)
                                Movie::Stop();
                        NES::Reset(RESET_SOFT);
                        if (running)
                                NES::Resume();
                        break;
                case ID_CPU_HARDRESET:
                        NES::Pause(FALSE);
                        if (Movie::Mode)
                                Movie::Stop();
                        NES::Reset(RESET_HARD);
                        if (running)
                                NES::Resume();
                        break;
                case ID_CPU_SAVESTATE:
                        if (running)
                                NES::Pause(TRUE);
                        else    NES::SkipToVBlank();
                        States::SaveState();
                        if (running)    NES::Resume();
                        break;
                case ID_CPU_LOADSTATE:
                        NES::Pause(FALSE);
                        States::LoadState();
                        if (running)    NES::Resume();
#ifdef ENABLE_DEBUGGER
                        else if (Debugger::Enabled)
                                Debugger::Update(Debugger::Mode);
#endif /* ENABLE_DEBUGGER */
                        break;
                case ID_CPU_PREVSTATE:
                        States::SelSlot += 9;
                        States::SelSlot %= 10;
                        States::SetSlot(States::SelSlot);
                        break;
                case ID_CPU_NEXTSTATE:
                        States::SelSlot += 1;
                        States::SelSlot %= 10;
                        States::SetSlot(States::SelSlot);
                        break;
                case ID_CPU_GAMEGENIE:
                        NES::GameGenie = !NES::GameGenie;
                        if (NES::GameGenie)
                                CheckMenuItem(hMenu, ID_CPU_GAMEGENIE, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_CPU_GAMEGENIE, MF_UNCHECKED);
                        break;
                case ID_CPU_BADOPS:
                        CPU::LogBadOps = !CPU::LogBadOps;
                        if (CPU::LogBadOps)
                                CheckMenuItem(hMenu, ID_CPU_BADOPS, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_CPU_BADOPS, MF_UNCHECKED);
                        break;
                case ID_CPU_FRAMESTEP_ENABLED:
                        NES::FrameStep = !NES::FrameStep;
                        if (NES::FrameStep)
                                CheckMenuItem(hMenu, ID_CPU_FRAMESTEP_ENABLED, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_CPU_FRAMESTEP_ENABLED, MF_UNCHECKED);
                        break;
                case ID_CPU_FRAMESTEP_STEP:
                        NES::GotStep = TRUE;
                        break;
                case ID_PPU_FRAMESKIP_AUTO:
                        GFX::aFSkip = !GFX::aFSkip;
                        GFX::SetFrameskip(-1);
                        break;
                case ID_PPU_FRAMESKIP_0:
                        GFX::SetFrameskip(0);
                        break;
                case ID_PPU_FRAMESKIP_1:
                        GFX::SetFrameskip(1);
                        break;
                case ID_PPU_FRAMESKIP_2:
                        GFX::SetFrameskip(2);
                        break;
                case ID_PPU_FRAMESKIP_3:
                        GFX::SetFrameskip(3);
                        break;
                case ID_PPU_FRAMESKIP_4:
                        GFX::SetFrameskip(4);
                        break;
                case ID_PPU_FRAMESKIP_5:
                        GFX::SetFrameskip(5);
                        break;
                case ID_PPU_FRAMESKIP_6:
                        GFX::SetFrameskip(6);
                        break;
                case ID_PPU_FRAMESKIP_7:
                        GFX::SetFrameskip(7);
                        break;
                case ID_PPU_FRAMESKIP_8:
                        GFX::SetFrameskip(8);
                        break;
                case ID_PPU_FRAMESKIP_9:
                        GFX::SetFrameskip(9);
                        break;
                case ID_PPU_SIZE_1X:
                        SizeMult = 1;
                        NES::UpdateInterface();
                        CheckMenuRadioItem(hMenu, ID_PPU_SIZE_1X, ID_PPU_SIZE_4X, ID_PPU_SIZE_1X, MF_BYCOMMAND);
                        break;
                case ID_PPU_SIZE_2X:
                        SizeMult = 2;
                        NES::UpdateInterface();
                        CheckMenuRadioItem(hMenu, ID_PPU_SIZE_1X, ID_PPU_SIZE_4X, ID_PPU_SIZE_2X, MF_BYCOMMAND);
                        break;
                case ID_PPU_SIZE_3X:
                        SizeMult = 3;
                        NES::UpdateInterface();
                        CheckMenuRadioItem(hMenu, ID_PPU_SIZE_1X, ID_PPU_SIZE_4X, ID_PPU_SIZE_3X, MF_BYCOMMAND);
                        break;
                case ID_PPU_SIZE_4X:
                        SizeMult = 4;
                        NES::UpdateInterface();
                        CheckMenuRadioItem(hMenu, ID_PPU_SIZE_1X, ID_PPU_SIZE_4X, ID_PPU_SIZE_4X, MF_BYCOMMAND);
                        break;
                case ID_PPU_SIZE_ASPECT:
                        FixAspect = !FixAspect;
                        NES::UpdateInterface();
                        if (FixAspect)
                                CheckMenuItem(hMenu, ID_PPU_SIZE_ASPECT, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_PPU_SIZE_ASPECT, MF_UNCHECKED);
                        break;
                case ID_PPU_MODE_NTSC:
                        NES::Stop();
                        NES::SetRegion(NES::REGION_NTSC);
                        if (running)    NES::Start(FALSE);
                        break;
                case ID_PPU_MODE_PAL:
                        NES::Stop();
                        NES::SetRegion(NES::REGION_PAL);
                        if (running)    NES::Start(FALSE);
                        break;
                case ID_PPU_MODE_DENDY:
                        NES::Stop();
                        NES::SetRegion(NES::REGION_DENDY);
                        if (running)    NES::Start(FALSE);
                        break;
                case ID_PPU_PALETTE:
                        GFX::PaletteConfig();
                        break;
                case ID_PPU_SLOWDOWN_ENABLED:
                        GFX::SlowDown = !GFX::SlowDown;
                        if (GFX::SlowDown)
                                CheckMenuItem(hMenu, ID_PPU_SLOWDOWN_ENABLED, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_PPU_SLOWDOWN_ENABLED, MF_UNCHECKED);
                        break;
                case ID_PPU_SLOWDOWN_2:
                        GFX::SlowRate = 2;
                        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_2, MF_BYCOMMAND);
                        break;
                case ID_PPU_SLOWDOWN_3:
                        GFX::SlowRate = 3;
                        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_3, MF_BYCOMMAND);
                        break;
                case ID_PPU_SLOWDOWN_4:
                        GFX::SlowRate = 4;
                        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_4, MF_BYCOMMAND);
                        break;
                case ID_PPU_SLOWDOWN_5:
                        GFX::SlowRate = 5;
                        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_5, MF_BYCOMMAND);
                        break;
                case ID_PPU_SLOWDOWN_10:
                        GFX::SlowRate = 10;
                        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_10, MF_BYCOMMAND);
                        break;
                case ID_PPU_SLOWDOWN_20:
                        GFX::SlowRate = 20;
                        CheckMenuRadioItem(hMenu, ID_PPU_SLOWDOWN_2, ID_PPU_SLOWDOWN_20, ID_PPU_SLOWDOWN_20, MF_BYCOMMAND);
                        break;
                case ID_PPU_FULLSCREEN:
    NES::Stop();
    GFX::Stop();
    GFX::Fullscreen = !GFX::Fullscreen;
    GFX::Start();
    APU::UpdateDRC();
    if (running)
        NES::Start(FALSE);
    break;

case ID_PPU_SCANLINES:
    NES::Stop();
    GFX::Stop();
    GFX::Scanlines = !GFX::Scanlines;
    GFX::Start();
    if (running)
        NES::Start(FALSE);
    if (GFX::Scanlines)
        CheckMenuItem(hMenu, ID_PPU_SCANLINES, MF_CHECKED);
    else
        CheckMenuItem(hMenu, ID_PPU_SCANLINES, MF_UNCHECKED);
    break;

case ID_PPU_BILINEAR:
    GFX::Bilinear = !GFX::Bilinear;
    GFX::ApplyGLFilter();
    if (GFX::Bilinear)
        CheckMenuItem(hMenu, ID_PPU_BILINEAR, MF_CHECKED);
    else
        CheckMenuItem(hMenu, ID_PPU_BILINEAR, MF_UNCHECKED);
    break;
case ID_PPU_MATCHRATE:
                        GFX::MatchMonitorRate = !GFX::MatchMonitorRate;
        // Hand off to the MonitorSync module: this is where vsync is enabled,
        // the monitor rate is (re)measured, and DRC state is reset.
        MonitorSync::Enable(GFX::MatchMonitorRate ? TRUE : FALSE);
        if (GFX::MatchMonitorRate)
                                CheckMenuItem(hMenu, ID_PPU_MATCHRATE, MF_CHECKED);
        else    CheckMenuItem(hMenu, ID_PPU_MATCHRATE, MF_UNCHECKED);
        break;

case ID_PPU_INTSCALE:
    NES::Stop();
    GFX::Stop();
    GFX::IntegerScale = !GFX::IntegerScale;
    GFX::Start();
    if (running)
        NES::Start(FALSE);
    if (GFX::IntegerScale)
        CheckMenuItem(hMenu, ID_PPU_INTSCALE, MF_CHECKED);
    else
        CheckMenuItem(hMenu, ID_PPU_INTSCALE, MF_UNCHECKED);
    break;

case ID_PPU_ALWAYSONTOP:
    GFX::AlwaysOnTop = !GFX::AlwaysOnTop;
    if (GFX::AlwaysOnTop)
        SetWindowPos(hMainWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    else
        SetWindowPos(hMainWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (GFX::AlwaysOnTop)
        CheckMenuItem(hMenu, ID_PPU_ALWAYSONTOP, MF_CHECKED);
    else
        CheckMenuItem(hMenu, ID_PPU_ALWAYSONTOP, MF_UNCHECKED);
    break;

case ID_PPU_EXCLUSIVEFS:
    GFX::ExclusiveFullscreen = !GFX::ExclusiveFullscreen;
    if (GFX::ExclusiveFullscreen)
        CheckMenuItem(hMenu, ID_PPU_EXCLUSIVEFS, MF_CHECKED);
    else
        CheckMenuItem(hMenu, ID_PPU_EXCLUSIVEFS, MF_UNCHECKED);
    if (GFX::Fullscreen)
    {
        NES::Stop();
        GFX::Stop();
        GFX::Start();
        APU::UpdateDRC();
        if (running) NES::Start(FALSE);
    }
    break;

case ID_PPU_THEME_LIGHT:
    Theme::SetMode(Theme::MODE_LIGHT);
    Theme::SyncMenuChecks();
    Theme::Reapply();
    break;

case ID_PPU_THEME_DARK:
    Theme::SetMode(Theme::MODE_DARK);
    Theme::SyncMenuChecks();
    Theme::Reapply();
    break;

case ID_SOUND_ENABLED:
    NES::SoundEnabled = !NES::SoundEnabled;
    if (NES::SoundEnabled)
    {
        if (running)
            APU::SoundON();
        CheckMenuItem(hMenu, ID_SOUND_ENABLED, MF_CHECKED);
    }
    else
    {
        if (running)
            APU::SoundOFF();
        CheckMenuItem(hMenu, ID_SOUND_ENABLED, MF_UNCHECKED);
    }
    break;
                        
                case ID_SOUND_VOLUME:
                        APU::Config();
                        break;
                case ID_INPUT_SETUP:
                        NES::Stop();
                        Controllers::OpenConfig();
                        if (running)    NES::Start(FALSE);
                        break;
#ifdef ENABLE_DEBUGGER
                case ID_DEBUG_CPU:
                        Debugger::SetMode(Debugger::Mode ^ DEBUG_MODE_CPU);
                        break;
                case ID_DEBUG_PPU:
                        Debugger::SetMode(Debugger::Mode ^ DEBUG_MODE_PPU);
                        break;
#endif /* ENABLE_DEBUGGER */
                case ID_DEBUG_STATWND:
                        dbgVisible = !dbgVisible;
                        if (dbgVisible)
                                CheckMenuItem(hMenu, ID_DEBUG_STATWND, MF_CHECKED);
                        else    CheckMenuItem(hMenu, ID_DEBUG_STATWND, MF_UNCHECKED);
                        ShowWindow(hDebug, dbgVisible ? SW_SHOW : SW_HIDE);
                        break;
                case ID_GAME:
                        NES::MapperConfig();
                        break;
                case ID_MISC_STARTAVICAPTURE:
                        AVI::Start();
                        break;
                case ID_MISC_STOPAVICAPTURE:
                        AVI::End();
                        break;
                case ID_MISC_PLAYMOVIE:
                        Movie::Play();
                        break;
                case ID_MISC_RECORDMOVIE:
                        Movie::Record();
                        break;
                case ID_MISC_STOPMOVIE:
                        Movie::Stop();
                        break;
                case ID_HELP_ABOUT:
                        DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                        break;
                default:
                        if (LOWORD(wParam) >= IDM_LANGUAGE_BASE &&
                            LOWORD(wParam) <= IDM_LANGUAGE_MAX)
                        {
                                int idx = LOWORD(wParam) - IDM_LANGUAGE_BASE;
                                if (idx < Lang::GetLanguageCount())
                                {
                                        Lang::Load(Lang::GetLanguageFileName(idx));
                                        Lang::SaveToRegistry(Lang::GetLanguageFileName(idx));
                                        HMENU hM = GetMenu(hWnd);
                                        Lang::UpdateMenu(hM);
                                        // Lang::UpdateMenu resets checkmarks - restore them
                                        GFX::SyncMenuChecks();
                                        Theme::SyncMenuChecks();
                                        // Lang::UpdateMenu also resets MF_GRAYED — restore menu states
                                        NES::SyncMenuStates();
                                        BuildLanguageMenu(hM);
                                        // Update controller mappings after language change
                                        Controllers::StdPort_SetMappings();
                                        Controllers::ExpPort_SetMappings();
                                        // Update debug dialog title after language change
                                        if (hDebug)
                                                SetWindowText(hDebug, Lang::GetString(LANG_DLG_DEBUG_TITLE));
                                        DrawMenuBar(hWnd);
                                        MessageBox(hWnd,
                                                Lang::GetString(LANG_LANG_CHANGED_MSG),
                                                Lang::GetString(LANG_LANG_CHANGED),
                                                MB_ICONINFORMATION | MB_OK);
                                }
                        }
                        else
                                return DefWindowProc(hWnd, message, wParam, lParam);
                        break;
                }
                break;
        case WM_DROPFILES:
                DragQueryFile((HDROP)wParam, 0, FileName, MAX_PATH);
                DragFinish((HDROP)wParam);
                NES::Stop();
                NES::OpenFile(FileName);
                break;
        case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        if (!NES::Running)
                GFX::Repaint();
        EndPaint(hWnd, &ps);
        break;

case WM_KEYDOWN:
        if (wParam == VK_F11)
        {
                BOOL wasRunning = NES::Running;

                NES::Stop();
                GFX::Stop();

                GFX::Fullscreen = !GFX::Fullscreen;

                GFX::Start();

                if (wasRunning)
                        NES::Start(FALSE);

                return 0;
        }
        break;

case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (lParam & (1 << 29)))
        {
                BOOL wasRunning = NES::Running;

                NES::Stop();
                GFX::Stop();

                GFX::Fullscreen = !GFX::Fullscreen;

                GFX::Start();

                if (wasRunning)
                        NES::Start(FALSE);

                return 0;
        }
        break;

case WM_SIZE:
{
        RECT rc;
        GetClientRect(hWnd, &rc);
        // GL_Resize calls wglMakeCurrent from the UI thread. If the NES thread
        // is currently inside GL_DrawFrame (between its own wglMakeCurrent and
        // SwapBuffers), this races and can corrupt the in-flight GL state.
        // Post a deferred resize request that GL_DrawFrame picks up safely at
        // the start of the next frame, the same way vsync changes are deferred.
        GFX::PostGLResize(rc.right - rc.left, rc.bottom - rc.top);
}
break;

case WM_APP_SETTITLE:
{
        // Async title-bar update posted by UpdateTitlebar() when called from
        // the NES emulation thread. lParam owns the heap string; free it here.
        TCHAR *title = reinterpret_cast<TCHAR*>(lParam);
        if (title)
        {
                SetWindowText(hWnd, title);
                delete[] title;
        }
}
break;

case WM_DISPLAYCHANGE:
        // The display resolution or refresh rate has changed (user
        // adjusted settings, plugged in a new monitor, switched monitor
        // while the emulator was running, etc.). Have MonitorSync
        // re-measure the refresh rate so DRC keeps tracking the correct
        // value. This is safe to call from the main thread; it only
        // updates cached measurements, not the volatile enable flag.
        MonitorSync::OnDisplayChange();
        break;

case WM_CLOSE:
                NES::Stop();
                // Cannot safely shutdown DirectDraw while in fullscreen mode,
                // so defer it to the message loop
                if (GFX::DirectDraw && GFX::Fullscreen)
                        NES::DoStop |= STOPMODE_QUIT;
                else    NES::Destroy();
                break;
        case WM_DESTROY:
                PostQuitMessage(0);
                break;
        case WM_SYSCOMMAND:
                // disallow screen saver while emulating (doesn't work if password protected)
                if (running && (((wParam & 0xFFF0) == SC_SCREENSAVE) || ((wParam & 0xFFF0) == SC_MONITORPOWER)))
                        return 0;
                // else fall through
        default:
                return DefWindowProc(hWnd, message, wParam, lParam);
                break;
        }
        return 0;
}

INT_PTR CALLBACK About (HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
        switch (message)
        {
                case WM_INITDIALOG:
                SetWindowText(hDlg, Lang::GetString(LANG_ABOUT_TITLE));
                SetDlgItemText(hDlg, IDOK, Lang::GetString(LANG_DLG_OK));
                Theme::ApplyToDialog(hDlg);
                return TRUE;
        case WM_COMMAND:
                if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
                {
                        EndDialog(hDlg, LOWORD(wParam));
                        return TRUE;
                }
                break;
        }
        return FALSE;
}

INT_PTR CALLBACK DebugWnd (HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
        switch (message)
        {
        case WM_INITDIALOG:
                SetWindowText(hDlg, Lang::GetString(LANG_DLG_DEBUG_TITLE));
                SendDlgItemMessage(hDlg, IDC_DEBUGTEXT, EM_SETLIMITTEXT, 0, 0);
                SetWindowPos(hDlg, hMainWnd, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOOWNERZORDER | SWP_NOSIZE);
                Theme::ApplyToDialog(hDlg);
                return FALSE;
        case WM_COMMAND:
                if (LOWORD(wParam) == IDCANCEL)
                {
                        dbgVisible = FALSE;
                        ShowWindow(hDebug, SW_HIDE);
                        CheckMenuItem(hMenu, ID_DEBUG_STATWND, MF_UNCHECKED);
                        return TRUE;
                }
                break;
        case WM_SIZE:
                MoveWindow(GetDlgItem(hDlg, IDC_DEBUGTEXT), 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
                return TRUE;
        }
        return FALSE;
}
void AddDebug (const TCHAR *txt)
{
        int dbglen = GetWindowTextLength(GetDlgItem(hDebug, IDC_DEBUGTEXT));
//      if (!dbgVisible)
//              return;
        SendDlgItemMessage(hDebug, IDC_DEBUGTEXT, EM_SETSEL, dbglen, dbglen);
        SendDlgItemMessage(hDebug, IDC_DEBUGTEXT, EM_REPLACESEL, FALSE, (LPARAM)txt);
        dbglen += (int)_tcslen(txt);
        SendDlgItemMessage(hDebug, IDC_DEBUGTEXT, EM_SETSEL, dbglen, dbglen);
        SendDlgItemMessage(hDebug, IDC_DEBUGTEXT, EM_REPLACESEL, FALSE, (LPARAM)_T("\r\n"));
}

// Shortcut for browsing to folders (though it could be used to run anything else, it's only ever called with folder names)
void BrowseFolder (const TCHAR *dir)
{
        ShellExecute(hMainWnd, NULL, dir, NULL, NULL, SW_SHOWNORMAL);
}

BOOL ProcessMessages (void)
{
        MSG msg;
        BOOL gotMessage = PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
                // See message loop in WinMain for explanation on why this is here
                if (NES::DoStop & STOPMODE_BREAK)
                {
                        NES::DoStop &= ~STOPMODE_BREAK;
                        Controllers::UnAcquire();
                }
                if (MaskKeyboard || !TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
                {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                }
                // If a Stop was requested and it's actually stopped, then break out
                // Otherwise, it sometimes gets in an infinite loop of WM_PAINT processing,
                // especially when holding Shift+F2 to repeatedly single-step in the debugger
                if ((NES::DoStop & STOPMODE_NOW) && !NES::Running)
                        break;
        }
        return gotMessage;
}

void UpdateTitlebar (void)
{
        TCHAR titlebar[256];
        if (NES::Running)
        {
                if (GFX::forceNoSkip)
                        _stprintf(titlebar, Lang::GetString(LANG_WINDOW_TITLE_FPS_NOSKIP), GFX::FPSnum);
                else    _stprintf(titlebar, Lang::GetString(LANG_WINDOW_TITLE_FPS_FSKIP), GFX::FPSnum, GFX::FSkip, GFX::aFSkip ? Lang::GetString(LANG_TITLEBAR_AUTO) : _T(""));
        }
        else    _tcscpy(titlebar, Lang::GetString(LANG_WINDOW_TITLE_STOPPED));
        if (TitlebarDelay > 0)
        {
                TitlebarDelay--;
                _tcscat(titlebar, _T(" - "));
                _tcscat(titlebar, TitlebarBuffer);
        }

        // CRITICAL: never call SetWindowText from the NES emulation thread.
        // SetWindowText on a window owned by another thread is implemented as
        // SendMessage(WM_SETTEXT), which BLOCKS the caller until the UI thread's
        // message pump processes the message. If the UI thread is busy (DWM
        // composition cycle, window manager repaint, antivirus hook) this stall
        // can be 5-15ms — easily long enough to miss the next vblank and produce
        // the ~30-second periodic glitch where both video and audio drop out
        // simultaneously (audio drops because the NES thread also drives the APU
        // write loop, so any stall here starves the DirectSound buffer too).
        //
        // PostMessage(WM_SETTEXT) is fully asynchronous: the NES thread posts
        // the string and returns immediately. The UI thread applies it whenever
        // it next processes messages — at most one frame later, imperceptible.
        // We copy the string to a heap buffer because the stack allocation here
        // will be gone by the time the UI thread runs; the UI thread frees it
        // via a custom WM_APP_SETTITLE handler in WndProc.
        //
        // When NOT running (called from UI thread directly), use SetWindowText
        // as before — no cross-thread issue in that case.
        if (NES::Running && GetCurrentThreadId() != GetWindowThreadProcessId(hMainWnd, NULL))
        {
                // Allocate a persistent copy for the async message.
                TCHAR *asyncTitle = new TCHAR[256];
                _tcscpy(asyncTitle, titlebar);
                // WM_APP_SETTITLE = WM_APP+1. Defined in Nintendulator.h.
                // WndProc handles it: SetWindowText((TCHAR*)lParam); delete[] (TCHAR*)lParam;
                if (!PostMessage(hMainWnd, WM_APP_SETTITLE, 0, (LPARAM)asyncTitle))
                        delete[] asyncTitle; // PostMessage failed (queue full) — just drop it
        }
        else
        {
                SetWindowText(hMainWnd, titlebar);
        }
}
void __cdecl PrintTitlebar (const TCHAR *Text, ...)
{
        va_list marker;
        va_start(marker, Text);
        _vstprintf(TitlebarBuffer, Text, marker);
        va_end(marker);
        TitlebarDelay = 15;
        UpdateTitlebar();
}
