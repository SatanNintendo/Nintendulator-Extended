/*
 * NintendulatorML - Localization System
 * src/Lang.cpp
 */

#include "stdafx.h"
#include "Lang.h"
#include "Nintendulator.h"
#include "resource.h"
#include <fstream>
#include <algorithm>

// ============================================================
// DEFAULT STRINGS (English)
// ============================================================
static const TCHAR *DefaultStrings[LANG_STRING_COUNT] =
{
    // Menu
    _T("&File"),
    _T("&Open...\tCtrl+O"),
    _T("&Close"),
    _T("Recent &Files"),
    _T("&Clear Recent Files"),
    _T("E&xit\tAlt+F4"),
    _T("&NES"),
    _T("&Run\tF2"),
    _T("&Step\tShift+F2"),
    _T("So&ft Reset\tF4"),
    _T("H&ard Reset\tShift+F4"),
    _T("S&top\tF3"),
    _T("&Settings"),
    _T("&Video..."),
    _T("&Sound..."),
    _T("&Controllers..."),
    _T("&Game Genie\tCtrl+G"),
    _T("&Paths..."),
    _T("&Clock speed..."),
    _T("&Movie"),
    _T("&Record..."),
    _T("&Play..."),
    _T("&Stop"),
    _T("&CPU"),
    _T("&PPU"),
    _T("&Sound"),
    _T("&Input"),
    _T("&Debug"),
    _T("Dis&assembly\tCtrl+F1"),
    _T("&Video\tCtrl+F2"),
    _T("&Misc"),
    _T("&Language"),
    _T("&Help"),
    _T("&About..."),

    // Window Title
    _T("Nintendulator"),
    _T("Nintendulator [Paused]"),
    _T("Nintendulator - %s - %d FPS"),
    _T("Nintendulator - Stopped"),
    _T("Nintendulator - %i FPS (No FSkip)"),
    _T("Nintendulator - %i FPS (%i %sFSkip)"),
    _T("Auto"),
    _T("Volume"),

    // File Open
    _T("Open ROM"),
    _T("NES ROMs (*.nes;*.unif;*.fds;*.nsf)\0*.nes;*.unif;*.fds;*.nsf\0All Files (*.*)\0*.*\0"),
    _T("Open Movie"),
    _T("Save Movie"),
    _T("Open Palette"),
    _T("Save AVI"),

    // ROM Errors
    _T("Failed to open ROM file"),
    _T("Invalid ROM file format"),
    _T("Unsupported mapper"),
    _T("Invalid UNIF ROM"),
    _T("Invalid FDS header"),
    _T("Invalid FDS disk side"),
    _T("Invalid NSF file"),
    _T("ROM loaded: %s"),
    _T("ROM closed"),
    _T("A ROM is already loaded. Close it first."),

    // Video
    _T("Failed to initialize DirectDraw"),
    _T("Failed to create DirectDraw surface"),
    _T("Failed to set DirectDraw palette"),
    _T("DirectDraw render error"),
    _T("Unsupported video mode"),
    _T("Screenshot saved: %s"),

    // Audio
    _T("Failed to initialize DirectSound"),
    _T("Failed to create DirectSound buffer"),
    _T("Unsupported audio format"),
    _T("Sound disabled"),

    // Controllers device list
    _T("Unconnected"),
    _T("Standard Controller"),
    _T("Zapper"),
    _T("Arkanoid Paddle"),
    _T("Power Pad"),
    _T("Four Score (port 1 only)"),
    _T("SNES Controller"),
    _T("VS Unisystem Zapper"),
    _T("SNES Mouse"),
    _T("Four Score (port 2 only)"),
    _T("Famicom 4-Player Adapter"),
    _T("Famicom Arkanoid Paddle"),
    _T("Family Basic Keyboard"),
    _T("Subor Keyboard"),
    _T("Family Trainer"),
    _T("Oeka Kids Tablet"),

    // Controllers - Additional Messages
    _T("No configuration necessary!"),
    _T("Unable to initialize DirectInput!"),
    _T("Failed to initialize keyboard!"),
    _T("Failed to initialize mouse!"),
    _T("Failed to initialize joysticks!"),
    _T("Error - encountered invalid keyboard key ID!"),
    _T("Error - encountered invalid mouse button ID!"),

    // Controllers dialog
    _T("Controller Port &1:"),
    _T("Controller Port &2:"),
    _T("&Expansion Port:"),
    _T("&Config"),
    _T("&Allow simultaneous Left+Right and Up+Down"),
    _T("Configure &POV triggers as 8-way buttons"),
    
    // Controllers
    _T("Unable to initialize keyboard input device!"),
    _T("Unable to set keyboard input data format!"),
    _T("Unable to set keyboard input cooperative level!"),
    _T("Unable to initialize mouse input device!"),
    _T("Unable to set mouse input cooperative level!"),
    _T("Unable to restore joystick input cooperative level!"),
    _T("Error: selected invalid controller type for standard port!"),
    _T("Error: selected invalid controller type for expansion port!"),

    // States
    _T("State saved to slot %d"),
    _T("State loaded from slot %d"),
    _T("Failed to save state"),
    _T("Failed to load state"),
    _T("State file version mismatch"),
    _T("State file is corrupted"),
    _T("Slot %d"),

    // Recording
    _T("Recording movie..."),
    _T("Playing movie..."),
    _T("Movie stopped"),
    _T("Failed to open movie file"),
    _T("Invalid movie file"),
    _T("Movie version mismatch"),
    _T("Recording AVI..."),
    _T("Failed to open AVI file"),
    _T("AVI recording stopped"),

    // Game Genie
    _T("Invalid Game Genie code"),
    _T("Too many Game Genie codes (maximum 3)"),
    _T("Game Genie code applied"),

    // Mapper
    _T("Failed to load %s - not a valid DLL file!"),
    _T("Mapper not found"),
    _T("Failed to load mapper pack %s - version mismatch!"),

    // Debugger
    _T("Exec: $%04X"),
    _T("Read: $%04X"),
    _T("Write: $%04X"),
    _T("Access: $%04X"),
    _T("Opcode: $%02X"),
    _T("Interrupt: NMI"),
    _T("Interrupt: IRQ"),
    _T("Interrupt: BRK"),
    _T("Scanline: %i"),
    _T("UNDEFINED"),
    _T(" (+)"),
    _T(" (-)"),
    _T("Logging started"),
    _T("Logging stopped"),

    // Dialogs
    _T("OK"),
    _T("Cancel"),
    _T("Yes"),
    _T("No"),
    _T("Apply"),
    _T("Browse..."),
    _T("Nintendulator"),

    // About
    _T("About Nintendulator"),
    _T("Nintendulator\nNES Emulator\n\nCopyright (C) QMT Productions"),

    // File Submenu
    _T("Edit iNES &Header"),
    _T("&Auto-Run"),
    _T("&Browse Save Files"),    

    // CPU Submenu
    _T("Sa&ve State\tF5"),
    _T("&Load State\tF8"),
    _T("&Prev State\tF6"),
    _T("&Next State\tF7"),
    _T("L&og Invalid Opcodes"),
    _T("Fra&me Step"),
    _T("&Enabled\tCtrl+F"),
    _T("&Step\tSpace"),    
    
    // Sound Submenu
    _T("&Enabled\tCtrl+S"),

    // Debug Submenu
    _T("&Status Window"),

    // Misc Submenu
    _T("&Start AVI Capture..."),
    _T("St&op AVI Capture"),    

    // PPU Submenu
    _T("&Fix Aspect"),
    _T("&Frameskip"),
    _T("&Auto"),
    _T("&Enabled\tTab"),
    _T("&Hybrid"),
    _T("&Size"),
    _T("&Mode"),
    _T("P&alette..."),
    _T("Sl&owdown"),
    _T("F&ullscreen\tAlt+Enter / F11"),
    _T("S&canlines"),
    _T("&Bilinear Filter\tCtrl+B"),
    _T("&Integer Scaling"),
    _T("&Match Monitor Rate"),
    _T("Al&ways on Top"),
    _T("&Exclusive Fullscreen"),
    _T("&Game"),

    // Language
    _T("Language Changed"),
    _T("Language has been changed. Some elements will update on restart."),

    // === CPU ===
    _T("Bad opcode, CPU locked"),
    _T("APU started"),
    _T("Unknown region, APU not configured"),
    _T("Unable to reopen file! Discard changes?"),
    _T("Unable to open ROM!"),
    _T("Selected file is not an iNES ROM image!"),
    _T("Selected ROM appears to have been corrupted by \"DiskDude!\" - cleaning..."),
    _T("Unrecognized or inconsistent data detected in ROM header! Do you wish to clean it?"),
    _T("iNES Editor"),
    _T("Unable to modify device input cooperative level!"),
    _T("Unable to initialize DirectInput!"),
    _T("Unable to restore device input cooperative level!"),
    // AVI
    _T("No AVI capture is currently in progress!"),
    _T("An AVI capture is already in progress!"),
    _T("Failed to configure AVI compression!"),
    _T("Error! AVI frame capture attempted while not recording!"),
    _T("Failed to write video to AVI!"),
    _T("Error! AVI audio capture attempted while not recording!"),
    _T("Failed to write audio to AVI!"),
    // GFX
    _T("Failed to determine performance counter frequency!"),
    _T("Fatal error: unable to load DirectDraw DLL!"),
    _T("Fatal error: unable to locate entry point for DirectDrawCreateEx!"),
    _T("Failed to initialize DirectDraw 7"),
    _T("Failed to set fullscreen cooperative level! Returning to Windowed mode..."),
    _T("No fullscreen resolutions are supported on your display device!"),
    _T("No fullscreen resolutions are supported on your display device! Reverting to Windowed mode..."),
    _T("Failed to set DirectDraw cooperative level"),
    _T("Failed to create primary surface"),
    _T("Failed to get secondary surface"),
    _T("Failed to create secondary surface"),
    _T("Failed to create clipper"),
    _T("Failed to set clipper window"),
    _T("Failed to assign clipper to primary surface"),
    _T("Failed to retrieve surface description"),
    _T("Invalid bit depth detected!"),
    _T("Failed to initialize OpenGL! Falling back to DirectDraw."),
    // MapperInterface
    _T("No mapper DLLs found!"),
    _T("Nintendulator was unable to fully relocate its data files to \"%s\".\nPlease delete the folder \"%s\" after relocating its contents."),
    _T("Failed to flip to primary surface! Reverting to Windowed mode..."),
    _T("Illegal palette selected!"),
    _T("Unable to load the specified palette! Reverting to default!"),
    _T("Selected file is not a valid palette!"),
    _T("Standard Controller Config"),
    _T("Buttons"),
    _T("D-pad"),
    _T("&A"),
    _T("&B"),
    _T("&Select"),
    _T("S&tart"),
    _T("&Up"),
    _T("&Down"),
    _T("&Left"),
    _T("&Right"),
    _T("&Trigger"),
    _T("&Axis"),
    _T("Zapper Config"),
    _T("Arkanoid Paddle Config"),
    _T("Power Pad Config"),
    _T("Four-Score Configuration"),
    _T("Famicom 4-Player Adapter"),
    _T("SNES Controller Config"),
    _T("SNES Mouse Config"),
    _T("VS Zapper Config"),
    _T("Fli&p"),
    _T("Port &1:"),
    _T("Port &2:"),
    _T("Port &3:"),
    _T("Port &4:"),
    _T("&Config"),
    _T("Close"),
    _T("Controllers"),
// iNES Header Editor
    _T("iNES Header Editor"),
    _T("ROM Filename:"),
    _T("Standard &iNES"),
    _T("NES &2.0"),
    _T("&Mapper Number:"),
    _T("&PRG ROM Banks:"),
    _T("&CHR ROM Banks:"),
    _T("&Horizontal Mirroring"),
    _T("&Vertical Mirroring"),
    _T("VS Unisyst&em"),
    _T("Pla&ychoice-10"),
    _T("&4-screen VRAM"),
    _T("Battery-backed &SRAM"),
    _T("&Trainer"),
    _T("Su&bmapper:"),
    _T("(battery)"),
    _T("PR&G RAM:"),
    _T("CH&R RAM:"),
    _T("&NTSC"),
    _T("P&AL"),
    _T("&Dual"),
    _T("VS PP&U:"),
    _T("VS &Flags:"),
    _T("More Buttons"),
    _T("Family Trainer Config"),
    _T("Family Basic Keyboard"),
    _T("Oeka Kids Tablet"),
// Sound Volume
    _T("&Master"),
    _T("SQ&0"),
    _T("SQ&1"),
    _T("&TRI"),
    _T("&NOI"),
    _T("&PCM"),
    _T("&EXT"),
    _T("Mute"),
    _T("Close"),
    // Movie dialogs
    _T("Play Movie"),
    _T("Record Movie"),
    _T("&Movie File:"),
    _T("&Description:"),
    _T("Length:"),
    _T("Frames:"),
    _T("Rerecords:"),
    _T("TV Mode:"),
    _T("&Resume recording on savestate load"),
    _T("Controllers"),
    _T("Port 1:"),
    _T("Port 2:"),
    _T("Expansion:"),
    _T("&Configure..."),
    _T("Record from..."),
    _T("&Hard Reset"),
    _T("Current &State"),
    // Palette dialog
    _T("Palette Configuration"),
    _T("Palette"),
    _T("&Hue:"),
    _T("&Sat:"),
    _T("&Custom..."),
    _T("Preview"),
    _T("Emphasis:"),
    _T("Note: Red and Green emphasis swapped for PAL/Dendy"),
    _T("Extra gra&ys (compatibility)"),
    // DLL Select dialog
    _T("Select Mapper DLL"),
    _T("This ROM is supported by multiple mapper DLLs.\nPlease select the one you wish to use."),
    // Breakpoint dialog
    _T("Add/Edit Breakpoint"),
    _T("&Enable Breakpoint"),
    _T("Address"),
    _T("Operation"),
    _T("Time"),
    _T("E&xecution"),
    _T("&Read"),
    _T("&Write"),
    _T("&Access"),
    _T("&Opcode"),
    _T("&NMI"),
    _T("&IRQ"),
    _T("&BRK"),
    _T("&Scanline"),
    _T("&From"),
    _T("&To"),
    _T("Debug Information"),
    // Debugger CPU dialog
    _T("Debugger"),
    _T("Registers"),
    _T("Timing"),
    _T("IRQs"),
    _T("Flags"),
    _T("Banks"),
    _T("Breakpoints"),
    _T("Controls"),
    _T("Memory"),
    _T("Trace"),
    _T("Run"),
    _T("Step"),
    _T("Reset"),
    _T("Power"),
    _T("D&ump CPU"),
    _T("Du&mp PPU"),
    _T("S&tart Log"),
    _T("Stop Lo&g"),
    _T("Seek to PC"),
    _T("Seek to Address:"),
    _T("Sel>"),
    _T("A&dd"),
    _T("&Edit"),
    _T("De&lete"),
    // Debugger PPU dialog
    _T("Debugger - PPU"),
    _T("Nametable"),
    _T("Sprites"),
    _T("Pattern Tables"),
    _T("Palette"),
    _T("Details"),
    _T("Left-click to cycle palettes."),
    _T("Hover cursor over item to view details.\nRight-click to remember selection."),
    _T("Selection Type:"),
    _T("8x1&6 Flip"),
    _T("Incorrect mapper DLL version!"),
    _T("Error loading mapper DLL!"),
    // Theme
    _T("&Theme"),
    _T("&Light"),
    _T("&Dark"),

    // File dialog filter display names
    _T("All supported files (*.NES, *.UNIF, *.UNF, *.FDS, *.NSF)"),
    _T("iNES ROM Images (*.NES)"),
    _T("Universal NES Interchange Format ROM files (*.UNIF, *.UNF)"),
    _T("Famicom Disk System Disk Images (*.FDS)"),
    _T("NES Sound Files (*.NSF)"),
    _T("iNES ROM Images (*.NES)"),
    _T("Nintendulator Movie (*.NMV)"),
    _T("AVI File (*.AVI)"),
    _T("Palette file (*.PAL)"),
    _T("Edit Header"),

    // Language menu
    _T("(No languages found)"),

    // Movie
    _T("Unexpected EOF in movie!"),
    _T("PAL"),
    _T("NTSC"),
    _T("Length: %i:%02i.%02i"),

    // Debugger validation
    _T("Invalid address range specified!"),
    _T("Invalid opcode specified!"),
    _T("Invalid scanline specified!"),

    // MapperInterface compatibility
    _T("Unsupported"),
    _T("Partially supported"),
    _T("Mostly supported"),
    _T("Fully supported!"),

    // Netplay (Kaillera)
    _T("Kaillera Netplay"),
    _T("Kaillera Chat"),
    _T("&Netplay"),
    _T("&Connect...\tCtrl+K"),
    _T("&Disconnect"),
    _T("&Chat...\tCtrl+T"),
    _T("Kaillera client (kailleraclient.dll) not found!\n\nPlace a Kaillera client DLL into the emulator folder, next to the program, and restart it.\nPlease make sure the DLL matches this build: this is the %d-bit emulator, so a %d-bit kailleraclient.dll is required. A 64-bit client can be downloaded from the Open Kaillera project (open-kaillera.github.io, n02 client)."),
    _T("The kailleraclient.dll in the emulator folder cannot be used with this build!\n\nThis emulator is a %d-bit build, so it requires a %d-bit kailleraclient.dll - the DLL you placed was built for a different architecture.\nPlease make sure you really placed a %d-bit version of kailleraclient.dll (for example the n02 client from open-kaillera.github.io) and restart the program."),
    _T("The kailleraclient.dll in the emulator folder is not a valid Kaillera client!\n\nPlease make sure you placed a genuine Kaillera client DLL that matches this build (%d-bit) and restart the program."),
    _T("Load a ROM before connecting!"),
    _T("Netplay is not available for NSF files."),
    _T("Unsupported controller configuration for netplay!\n\nSupported setups:\n- Port 1: Standard Controller, Port 2: Standard Controller (up to 2 players)\n- Port 1: Four Score, Port 2: Four Score 2, all four sub-controllers set to Standard Controller (up to 4 players)\n\nThe expansion port must be left Unconnected.\nSet it up via Input > Setup... and try again."),
    _T("Your player number does not fit the current controller configuration!\nPlayers 3-4 require the Four Score setup (Port 1: Four Score, Port 2: Four Score 2)."),
    _T("Too many players for the current controller configuration!\nPlayers 3-4 require the Four Score setup (Port 1: Four Score, Port 2: Four Score 2)."),
    _T("Kaillera: connection lost - the netplay session has ended."),
    _T("The game host uses different emulator settings (PPU mode or Game Genie)!\nAll players must use the same PPU mode (NTSC/PAL/Hybrid) and the same Game Genie state. Match the host's settings and reconnect."),
    _T("This action is not available during a netplay session!\nDisconnect first (Netplay > Disconnect)."),

};

// ============================================================
// Internal Data
// ============================================================
namespace
{
    // Initializer that fills Strings from DefaultStrings before any Lang::GetString calls.
    // This ensures correct text even if GetString is called before Lang::Init()
    // (e.g. CreateDialog for the Debug window is called before Lang::Init in WinMain).
    struct StringsInit
    {
        std::wstring data[LANG_STRING_COUNT];
        StringsInit()
        {
            for (int i = 0; i < LANG_STRING_COUNT; i++)
                data[i] = DefaultStrings[i];
        }
    } StringsStorage;
    std::wstring (&Strings)[LANG_STRING_COUNT] = StringsStorage.data;

    struct LangEntry
    {
        std::wstring fileName;   // e.g. "Russian" (without .lng)
        std::wstring displayName; // e.g. display name from [Info] Language=
    };
    std::vector<LangEntry> LanguageList;
    std::wstring CurrentLanguage = L"English";
    TCHAR LangDir[MAX_PATH] = { 0 };

    void ResetToDefaults()
    {
        for (int i = 0; i < LANG_STRING_COUNT; i++)
            Strings[i] = DefaultStrings[i];
    }

    std::wstring Trim(const std::wstring &s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return L"";
        size_t b = s.find_last_not_of(L" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Read the Language= field from [Info] section of a .lng file
    std::wstring ReadLangDisplayName(const TCHAR *filePath)
    {
        FILE *f = NULL;
        _tfopen_s(&f, filePath, _T("rt,ccs=UTF-8"));
        if (!f)
        {
            _tfopen_s(&f, filePath, _T("rt"));
            if (!f) return L"";
        }

        bool inInfo = false;
        wchar_t line[LANG_MAX_STRING * 2];
        while (fgetws(line, _countof(line), f))
        {
            std::wstring s = Trim(std::wstring(line));
            if (s.empty() || s[0] == L'#') continue;
            if (s == L"[Info]") { inInfo = true; continue; }
            if (s[0] == L'[') { inInfo = false; continue; }
            if (!inInfo) continue;

            size_t eq = s.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = Trim(s.substr(0, eq));
            if (_wcsicmp(key.c_str(), L"Language") == 0)
            {
                std::wstring val = Trim(s.substr(eq + 1));
                fclose(f);
                return val;
            }
        }
        fclose(f);
        return L"";
    }

    void ScanLangDir()
    {
        LanguageList.clear();
        TCHAR mask[MAX_PATH];
        _tcscpy(mask, LangDir);
        _tcscat(mask, _T("*.lng"));
        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(mask, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            LangEntry entry;
            std::wstring name = fd.cFileName;
            size_t dot = name.rfind(L'.');
            if (dot != std::wstring::npos) name = name.substr(0, dot);
            entry.fileName = name;

            // Read display name from [Info] Language= field
            TCHAR filePath[MAX_PATH];
            _tcscpy(filePath, LangDir);
            _tcscat(filePath, fd.cFileName);
            std::wstring displayName = ReadLangDisplayName(filePath);
            entry.displayName = displayName.empty() ? name : displayName;

            LanguageList.push_back(entry);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
}

// ============================================================
// Key Mapping Table
// ============================================================
struct KeyMapping { const wchar_t *key; LangStringID id; };
static const KeyMapping KeyMap[] =
{
    { L"MENU_FILE",                 LANG_MENU_FILE },
    { L"MENU_FILE_OPEN",            LANG_MENU_FILE_OPEN },
    { L"MENU_FILE_CLOSE",           LANG_MENU_FILE_CLOSE },
    { L"MENU_FILE_RECENT",          LANG_MENU_FILE_RECENT },
    { L"MENU_FILE_RECENT_CLEAR",    LANG_MENU_FILE_RECENT_CLEAR },
    { L"MENU_FILE_EXIT",            LANG_MENU_FILE_EXIT },
    { L"MENU_NES",                  LANG_MENU_NES },
    { L"MENU_NES_RUN",              LANG_MENU_NES_RUN },
    { L"MENU_NES_PAUSE",            LANG_MENU_NES_PAUSE },
    { L"MENU_NES_RESET",            LANG_MENU_NES_RESET },
    { L"MENU_NES_POWER",            LANG_MENU_NES_POWER },
    { L"MENU_NES_STOP",             LANG_MENU_NES_STOP },
    { L"MENU_SETTINGS",             LANG_MENU_SETTINGS },
    { L"MENU_SETTINGS_VIDEO",       LANG_MENU_SETTINGS_VIDEO },
    { L"MENU_SETTINGS_SOUND",       LANG_MENU_SETTINGS_SOUND },
    { L"MENU_SETTINGS_CONTROLLERS", LANG_MENU_SETTINGS_CONTROLLERS },
    { L"MENU_SETTINGS_GAMEGENIE",   LANG_MENU_SETTINGS_GAMEGENIE },
    { L"MENU_SETTINGS_PATHS",       LANG_MENU_SETTINGS_PATHS },
    { L"MENU_SETTINGS_CLOCK",       LANG_MENU_SETTINGS_CLOCK },
    { L"MENU_MOVIE",                LANG_MENU_MOVIE },
    { L"MENU_MOVIE_RECORD",         LANG_MENU_MOVIE_RECORD },
    { L"MENU_MOVIE_PLAY",           LANG_MENU_MOVIE_PLAY },
    { L"MENU_MOVIE_STOP",           LANG_MENU_MOVIE_STOP },
    { L"MENU_CPU",                  LANG_MENU_CPU },
    { L"MENU_PPU",                  LANG_MENU_PPU },
    { L"MENU_SOUND",                LANG_MENU_SOUND },
    { L"MENU_INPUT",                LANG_MENU_INPUT },
    { L"MENU_DEBUG",                LANG_MENU_DEBUG },
    { L"MENU_DEBUG_CPU",            LANG_MENU_DEBUG_CPU },
    { L"MENU_DEBUG_PPU",            LANG_MENU_DEBUG_PPU },
    { L"MENU_MISC",                 LANG_MENU_MISC },
    { L"MENU_LANGUAGE",             LANG_MENU_LANGUAGE },
    { L"MENU_HELP",                 LANG_MENU_HELP },
    { L"MENU_HELP_ABOUT",           LANG_MENU_HELP_ABOUT },
    { L"WINDOW_TITLE",              LANG_WINDOW_TITLE },
    { L"WINDOW_TITLE_PAUSED",       LANG_WINDOW_TITLE_PAUSED },
    { L"WINDOW_TITLE_FPS",          LANG_WINDOW_TITLE_FPS },
    { L"WINDOW_TITLE_STOPPED",      LANG_WINDOW_TITLE_STOPPED },
    { L"WINDOW_TITLE_FPS_NOSKIP",   LANG_WINDOW_TITLE_FPS_NOSKIP },
    { L"WINDOW_TITLE_FPS_FSKIP",    LANG_WINDOW_TITLE_FPS_FSKIP },
    { L"TITLEBAR_AUTO",             LANG_TITLEBAR_AUTO },
    { L"DLG_VOL_TITLE",             LANG_DLG_VOL_TITLE },
    { L"OPEN_ROM_TITLE",            LANG_OPEN_ROM_TITLE },
    { L"OPEN_ROM_FILTER",           LANG_OPEN_ROM_FILTER },
    { L"OPEN_MOVIE_TITLE",          LANG_OPEN_MOVIE_TITLE },
    { L"SAVE_MOVIE_TITLE",          LANG_SAVE_MOVIE_TITLE },
    { L"OPEN_PALETTE_TITLE",        LANG_OPEN_PALETTE_TITLE },
    { L"SAVE_AVI_TITLE",            LANG_SAVE_AVI_TITLE },
    { L"ERR_ROM_OPEN",              LANG_ERR_ROM_OPEN },
    { L"ERR_ROM_INVALID",           LANG_ERR_ROM_INVALID },
    { L"ERR_ROM_MAPPER",            LANG_ERR_ROM_MAPPER },
    { L"ERR_ROM_UNIF",              LANG_ERR_ROM_UNIF },
    { L"ERR_ROM_FDS_HEADER",        LANG_ERR_ROM_FDS_HEADER },
    { L"ERR_ROM_FDS_DISKSIDE",      LANG_ERR_ROM_FDS_DISKSIDE },
    { L"ERR_ROM_NSF",               LANG_ERR_ROM_NSF },
    { L"MSG_ROM_LOADED",            LANG_MSG_ROM_LOADED },
    { L"MSG_ROM_CLOSED",            LANG_MSG_ROM_CLOSED },
    { L"MSG_ROM_ALREADY_LOADED",    LANG_MSG_ROM_ALREADY_LOADED },
    { L"ERR_GFX_DIRECTDRAW",        LANG_ERR_GFX_DIRECTDRAW },
    { L"ERR_GFX_SURFACE",           LANG_ERR_GFX_SURFACE },
    { L"ERR_GFX_PALETTE",           LANG_ERR_GFX_PALETTE },
    { L"ERR_GFX_RENDER",            LANG_ERR_GFX_RENDER },
    { L"ERR_GFX_MODE",              LANG_ERR_GFX_MODE },
    { L"MSG_GFX_SCREENSHOT",        LANG_MSG_GFX_SCREENSHOT },
    { L"ERR_APU_DIRECTSOUND",       LANG_ERR_APU_DIRECTSOUND },
    { L"ERR_APU_BUFFER",            LANG_ERR_APU_BUFFER },
    { L"ERR_APU_FORMAT",            LANG_ERR_APU_FORMAT },
    { L"MSG_APU_DISABLED",          LANG_MSG_APU_DISABLED },
    { L"CONT_UNCONNECTED",          LANG_CONT_UNCONNECTED },
    { L"CONT_STDCONTROLLER",        LANG_CONT_STDCONTROLLER },
    { L"CONT_ZAPPER",               LANG_CONT_ZAPPER },
    { L"CONT_ARKANOIDPADDLE",       LANG_CONT_ARKANOIDPADDLE },
    { L"CONT_POWERPAD",             LANG_CONT_POWERPAD },
    { L"CONT_FOURSCORE1",           LANG_CONT_FOURSCORE1 },
    { L"CONT_SNESCONTROLLER",       LANG_CONT_SNESCONTROLLER },
    { L"CONT_VSZAPPER",             LANG_CONT_VSZAPPER },
    { L"CONT_SNESMOUSE",            LANG_CONT_SNESMOUSE },
    { L"CONT_FOURSCORE2",           LANG_CONT_FOURSCORE2 },
    { L"CONT_FAMI4PLAY",            LANG_CONT_FAMI4PLAY },
    { L"CONT_EXP_ARKANOIDPADDLE",   LANG_CONT_EXP_ARKANOIDPADDLE },
    { L"CONT_FAMILYBASICKEYBOARD",  LANG_CONT_FAMILYBASICKEYBOARD },
    { L"CONT_SUBORKEYBOARD",        LANG_CONT_SUBORKEYBOARD },
    { L"CONT_FAMTRAINER",           LANG_CONT_FAMTRAINER },
    { L"CONT_TABLET",               LANG_CONT_TABLET },
    { L"CONT_LABEL_PORT1",          LANG_CONT_LABEL_PORT1 },
    { L"CONT_LABEL_PORT2",          LANG_CONT_LABEL_PORT2 },
    { L"CONT_LABEL_EXP",            LANG_CONT_LABEL_EXP },
    { L"CONT_CONFIG",               LANG_CONT_CONFIG },
    { L"CONT_UDLR",                 LANG_CONT_UDLR },
    { L"CONT_POV",                  LANG_CONT_POV },    
    { L"ERR_CTRL_KEYBOARD_INIT",    LANG_ERR_CTRL_KEYBOARD_INIT },
    { L"ERR_CTRL_KEYBOARD_FORMAT",  LANG_ERR_CTRL_KEYBOARD_FORMAT },
    { L"ERR_CTRL_KEYBOARD_LEVEL",   LANG_ERR_CTRL_KEYBOARD_LEVEL },
    { L"ERR_CTRL_MOUSE_INIT",       LANG_ERR_CTRL_MOUSE_INIT },
    { L"ERR_CTRL_MOUSE_LEVEL",      LANG_ERR_CTRL_MOUSE_LEVEL },
    { L"ERR_CTRL_JOY_LEVEL",        LANG_ERR_CTRL_JOY_LEVEL },
    { L"ERR_CTRL_INVALID_STD",      LANG_ERR_CTRL_INVALID_STD },
    { L"ERR_CTRL_INVALID_EXP",      LANG_ERR_CTRL_INVALID_EXP },
    { L"MSG_STATE_SAVED",           LANG_MSG_STATE_SAVED },
    { L"MSG_STATE_LOADED",          LANG_MSG_STATE_LOADED },
    { L"ERR_STATE_SAVE",            LANG_ERR_STATE_SAVE },
    { L"ERR_STATE_LOAD",            LANG_ERR_STATE_LOAD },
    { L"ERR_STATE_VERSION",         LANG_ERR_STATE_VERSION },
    { L"ERR_STATE_CORRUPT",         LANG_ERR_STATE_CORRUPT },
    { L"MSG_STATE_SLOT",            LANG_MSG_STATE_SLOT },
    { L"MSG_MOVIE_RECORDING",       LANG_MSG_MOVIE_RECORDING },
    { L"MSG_MOVIE_PLAYING",         LANG_MSG_MOVIE_PLAYING },
    { L"MSG_MOVIE_STOPPED",         LANG_MSG_MOVIE_STOPPED },
    { L"ERR_MOVIE_OPEN",            LANG_ERR_MOVIE_OPEN },
    { L"ERR_MOVIE_INVALID",         LANG_ERR_MOVIE_INVALID },
    { L"ERR_MOVIE_VERSION",         LANG_ERR_MOVIE_VERSION },
    { L"MSG_AVI_RECORDING",         LANG_MSG_AVI_RECORDING },
    { L"ERR_AVI_OPEN",              LANG_ERR_AVI_OPEN },
    { L"MSG_AVI_STOPPED",           LANG_MSG_AVI_STOPPED },
    { L"ERR_GENIE_INVALID",         LANG_ERR_GENIE_INVALID },
    { L"ERR_GENIE_TOO_MANY",        LANG_ERR_GENIE_TOO_MANY },
    { L"MSG_GENIE_APPLIED",         LANG_MSG_GENIE_APPLIED },
    { L"ERR_MAPPER_LOAD",           LANG_ERR_MAPPER_LOAD },
    { L"ERR_MAPPER_NOTFOUND",       LANG_ERR_MAPPER_NOTFOUND },
    { L"ERR_MAPPER_VERSION",        LANG_ERR_MAPPER_VERSION },
    { L"DBG_BREAK_EXEC",            LANG_DBG_BREAK_EXEC },
    { L"DBG_BREAK_READ",            LANG_DBG_BREAK_READ },
    { L"DBG_BREAK_WRITE",           LANG_DBG_BREAK_WRITE },
    { L"DBG_BREAK_ACCESS",          LANG_DBG_BREAK_ACCESS },
    { L"DBG_BREAK_OPCODE",          LANG_DBG_BREAK_OPCODE },
    { L"DBG_BREAK_NMI",             LANG_DBG_BREAK_NMI },
    { L"DBG_BREAK_IRQ",             LANG_DBG_BREAK_IRQ },
    { L"DBG_BREAK_BRK",             LANG_DBG_BREAK_BRK },
    { L"DBG_BREAK_SCANLINE",        LANG_DBG_BREAK_SCANLINE },
    { L"DBG_BREAK_UNDEF",           LANG_DBG_BREAK_UNDEF },
    { L"DBG_BREAK_ENABLED",         LANG_DBG_BREAK_ENABLED },
    { L"DBG_BREAK_DISABLED",        LANG_DBG_BREAK_DISABLED },
    { L"MSG_DBG_LOG_STARTED",       LANG_MSG_DBG_LOG_STARTED },
    { L"MSG_DBG_LOG_STOPPED",       LANG_MSG_DBG_LOG_STOPPED },
    { L"DLG_OK",                    LANG_DLG_OK },
    { L"DLG_CANCEL",                LANG_DLG_CANCEL },
    { L"DLG_YES",                   LANG_DLG_YES },
    { L"DLG_NO",                    LANG_DLG_NO },
    { L"DLG_APPLY",                 LANG_DLG_APPLY },
    { L"DLG_BROWSE",                LANG_DLG_BROWSE },
    { L"DLG_NINTENDULATOR",         LANG_DLG_NINTENDULATOR },
    { L"ABOUT_TITLE",               LANG_ABOUT_TITLE },
    { L"ABOUT_TEXT",                LANG_ABOUT_TEXT },
    { L"MENU_FILE_HEADER",          LANG_MENU_FILE_HEADER },
    { L"MENU_FILE_AUTORUN",         LANG_MENU_FILE_AUTORUN },
    { L"MENU_FILE_BROWSESAVES",     LANG_MENU_FILE_BROWSESAVES },    
    { L"MENU_CPU_SAVESTATE",        LANG_MENU_CPU_SAVESTATE },
    { L"MENU_CPU_LOADSTATE",        LANG_MENU_CPU_LOADSTATE },
    { L"MENU_CPU_PREVSTATE",        LANG_MENU_CPU_PREVSTATE },
    { L"MENU_CPU_NEXTSTATE",        LANG_MENU_CPU_NEXTSTATE },
    { L"MENU_CPU_BADOPS",           LANG_MENU_CPU_BADOPS },
    { L"MENU_CPU_FRAMESTEP",        LANG_MENU_CPU_FRAMESTEP },
    { L"MENU_CPU_FRAMESTEP_ENABLED",LANG_MENU_CPU_FRAMESTEP_ENABLED },
    { L"MENU_CPU_FRAMESTEP_STEP",   LANG_MENU_CPU_FRAMESTEP_STEP },    
    { L"MENU_SOUND_ENABLED",        LANG_MENU_SOUND_ENABLED },
    { L"MENU_DEBUG_STATWND",        LANG_MENU_DEBUG_STATWND },
    { L"MENU_MISC_STARTAVI",        LANG_MENU_MISC_STARTAVI },
    { L"MENU_MISC_STOPAVI",         LANG_MENU_MISC_STOPAVI },    
    { L"MENU_PPU_FIXASPECT",        LANG_MENU_PPU_FIXASPECT },    
    { L"MENU_PPU_FRAMESKIP",        LANG_MENU_PPU_FRAMESKIP },
    { L"MENU_PPU_FRAMESKIP_AUTO",   LANG_MENU_PPU_FRAMESKIP_AUTO },
    { L"MENU_PPU_SLOWDOWN_ENABLED", LANG_MENU_PPU_SLOWDOWN_ENABLED },
    { L"MENU_PPU_MODE_HYBRID",      LANG_MENU_PPU_MODE_HYBRID },
    { L"MENU_PPU_SIZE",             LANG_MENU_PPU_SIZE },
    { L"MENU_PPU_MODE",             LANG_MENU_PPU_MODE },
    { L"MENU_PPU_PALETTE",          LANG_MENU_PPU_PALETTE },
    { L"MENU_PPU_SLOWDOWN",         LANG_MENU_PPU_SLOWDOWN },
    { L"MENU_PPU_FULLSCREEN",       LANG_MENU_PPU_FULLSCREEN },
    { L"MENU_PPU_SCANLINES",        LANG_MENU_PPU_SCANLINES },
    { L"MENU_PPU_BILINEAR",         LANG_MENU_PPU_BILINEAR },
    { L"MENU_PPU_INTSCALE",         LANG_MENU_PPU_INTSCALE },
    { L"MENU_PPU_MATCHRATE",        LANG_MENU_PPU_MATCHRATE },
    { L"MENU_PPU_ALWAYSONTOP",      LANG_MENU_PPU_ALWAYSONTOP },
    { L"MENU_PPU_EXCLUSIVEFS",      LANG_MENU_PPU_EXCLUSIVEFS },
    { L"MENU_GAME",                 LANG_MENU_GAME },
    { L"LANG_CHANGED",              LANG_LANG_CHANGED },
    { L"LANG_CHANGED_MSG",          LANG_LANG_CHANGED_MSG },
    { L"ERR_CPU_BAD_OPCODE",  LANG_ERR_CPU_BAD_OPCODE  },
    { L"MSG_APU_STARTED",     LANG_MSG_APU_STARTED     },
    { L"ERR_APU_REGION",      LANG_ERR_APU_REGION      },
    { L"ERR_HEADER_REOPEN",        LANG_ERR_HEADER_REOPEN        },
    { L"ERR_HEADER_OPEN",          LANG_ERR_HEADER_OPEN          },
    { L"ERR_HEADER_NOT_INES",      LANG_ERR_HEADER_NOT_INES      },
    { L"ERR_HEADER_DISKDUDE",      LANG_ERR_HEADER_DISKDUDE      },
    { L"ERR_HEADER_INCONSISTENT",  LANG_ERR_HEADER_INCONSISTENT  },
    { L"TITLE_INES_EDITOR",        LANG_TITLE_INES_EDITOR        },
    { L"ERR_CTRL_DINPUT_LEVEL",          LANG_ERR_CTRL_DINPUT_LEVEL          },
    { L"ERR_CTRL_DEVICE_LEVEL",          LANG_ERR_CTRL_DEVICE_LEVEL          },
    { L"ERR_CTRL_DEVICE_RESTORE",        LANG_ERR_CTRL_DEVICE_RESTORE        },
    { L"ERR_AVI_NOT_RECORDING",          LANG_ERR_AVI_NOT_RECORDING          },
    { L"ERR_AVI_ALREADY_RECORDING",      LANG_ERR_AVI_ALREADY_RECORDING      },
    { L"ERR_AVI_COMPRESSION",            LANG_ERR_AVI_COMPRESSION            },
    { L"ERR_AVI_FRAME_NOT_RECORDING",    LANG_ERR_AVI_FRAME_NOT_RECORDING    },
    { L"ERR_AVI_WRITE_VIDEO",            LANG_ERR_AVI_WRITE_VIDEO            },
    { L"ERR_AVI_AUDIO_NOT_RECORDING",    LANG_ERR_AVI_AUDIO_NOT_RECORDING    },
    { L"ERR_AVI_WRITE_AUDIO",            LANG_ERR_AVI_WRITE_AUDIO            },
    { L"ERR_GFX_PERF_COUNTER",           LANG_ERR_GFX_PERF_COUNTER           },
    { L"ERR_GFX_DDRAW_DLL",              LANG_ERR_GFX_DDRAW_DLL              },
    { L"ERR_GFX_DDRAW_ENTRY",            LANG_ERR_GFX_DDRAW_ENTRY            },
    { L"ERR_GFX_DDRAW7",                 LANG_ERR_GFX_DDRAW7                 },
    { L"ERR_GFX_FULLSCREEN_LEVEL",       LANG_ERR_GFX_FULLSCREEN_LEVEL       },
    { L"ERR_GFX_NO_FULLSCREEN_RES",      LANG_ERR_GFX_NO_FULLSCREEN_RES      },
    { L"ERR_GFX_NO_FULLSCREEN_RES_REVERT", LANG_ERR_GFX_NO_FULLSCREEN_RES_REVERT },
    { L"ERR_GFX_COOP_LEVEL",             LANG_ERR_GFX_COOP_LEVEL             },
    { L"ERR_GFX_PRIMARY_SURFACE",        LANG_ERR_GFX_PRIMARY_SURFACE        },
    { L"ERR_GFX_SECONDARY_SURFACE",      LANG_ERR_GFX_SECONDARY_SURFACE      },
    { L"ERR_GFX_CREATE_SECONDARY",       LANG_ERR_GFX_CREATE_SECONDARY       },
    { L"ERR_GFX_CLIPPER",                LANG_ERR_GFX_CLIPPER                },
    { L"ERR_GFX_CLIPPER_WINDOW",         LANG_ERR_GFX_CLIPPER_WINDOW         },
    { L"ERR_GFX_CLIPPER_ASSIGN",         LANG_ERR_GFX_CLIPPER_ASSIGN         },
    { L"ERR_GFX_SURF_DESC",              LANG_ERR_GFX_SURF_DESC              },
    { L"ERR_GFX_BIT_DEPTH",              LANG_ERR_GFX_BIT_DEPTH              },
    { L"ERR_GFX_OPENGL",                 LANG_ERR_GFX_OPENGL                 },
    { L"ERR_MAPPER_NO_DLLS",             LANG_ERR_MAPPER_NO_DLLS             },
    { L"ERR_NES_RELOCATE",               LANG_ERR_NES_RELOCATE               },
    { L"ERR_GFX_FLIP",                   LANG_ERR_GFX_FLIP                   },
    { L"ERR_GFX_ILLEGAL_PALETTE",        LANG_ERR_GFX_ILLEGAL_PALETTE        },
    { L"ERR_GFX_LOAD_PALETTE",           LANG_ERR_GFX_LOAD_PALETTE           },
    { L"ERR_GFX_INVALID_PALETTE",        LANG_ERR_GFX_INVALID_PALETTE        },
    { L"DLG_CTRL_STDCONT",           LANG_DLG_CTRL_STDCONT           },
    { L"DLG_CTRL_BUTTONS",           LANG_DLG_CTRL_BUTTONS           },
    { L"DLG_CTRL_DPAD",              LANG_DLG_CTRL_DPAD              },
    { L"DLG_CTRL_A",                 LANG_DLG_CTRL_A                 },
    { L"DLG_CTRL_B",                 LANG_DLG_CTRL_B                 },
    { L"DLG_CTRL_SELECT",            LANG_DLG_CTRL_SELECT            },
    { L"DLG_CTRL_START",             LANG_DLG_CTRL_START             },
    { L"DLG_CTRL_UP",                LANG_DLG_CTRL_UP                },
    { L"DLG_CTRL_DOWN",              LANG_DLG_CTRL_DOWN              },
    { L"DLG_CTRL_LEFT",              LANG_DLG_CTRL_LEFT              },
    { L"DLG_CTRL_RIGHT",             LANG_DLG_CTRL_RIGHT             },
    { L"DLG_CTRL_TRIGGER",           LANG_DLG_CTRL_TRIGGER           },
    { L"DLG_CTRL_AXIS",              LANG_DLG_CTRL_AXIS              },
    { L"DLG_CTRL_ZAPPER",            LANG_DLG_CTRL_ZAPPER            },
    { L"DLG_CTRL_ARKPAD",            LANG_DLG_CTRL_ARKPAD            },
    { L"DLG_CTRL_POWERPAD",          LANG_DLG_CTRL_POWERPAD          },
    { L"DLG_CTRL_FOURSCORE",         LANG_DLG_CTRL_FOURSCORE         },
    { L"DLG_CTRL_FAMI4PLAY",         LANG_DLG_CTRL_FAMI4PLAY         },
    { L"DLG_CTRL_SNESCONT",          LANG_DLG_CTRL_SNESCONT          },
    { L"DLG_CTRL_SNESMOUSE",         LANG_DLG_CTRL_SNESMOUSE         },
    { L"DLG_CTRL_VSZAPPER",          LANG_DLG_CTRL_VSZAPPER          },
    { L"DLG_CTRL_FLIP",              LANG_DLG_CTRL_FLIP              },
    { L"DLG_CTRL_PORT1",             LANG_DLG_CTRL_PORT1             },
    { L"DLG_CTRL_PORT2",             LANG_DLG_CTRL_PORT2             },
    { L"DLG_CTRL_PORT3",             LANG_DLG_CTRL_PORT3             },
    { L"DLG_CTRL_PORT4",             LANG_DLG_CTRL_PORT4             },
    { L"DLG_CTRL_CONFIG",            LANG_DLG_CTRL_CONFIG            },
    { L"DLG_CTRL_CLOSE",             LANG_DLG_CTRL_CLOSE             },
    { L"DLG_CTRL_TITLE",             LANG_DLG_CTRL_TITLE             },
    { L"DLG_CTRL_MORE_BUTTONS",      LANG_DLG_CTRL_MORE_BUTTONS      },
    { L"DLG_CTRL_FAMTRAIN",          LANG_DLG_CTRL_FAMTRAIN          },
    { L"DLG_CTRL_FBKEY",             LANG_DLG_CTRL_FBKEY             },
    { L"DLG_CTRL_TABLET",            LANG_DLG_CTRL_TABLET            },
    { L"DLG_VOL_MASTER",             LANG_DLG_VOL_MASTER             },
    { L"DLG_VOL_SQ0",                LANG_DLG_VOL_SQ0                },
    { L"DLG_VOL_SQ1",                LANG_DLG_VOL_SQ1                },
    { L"DLG_VOL_TRI",                LANG_DLG_VOL_TRI                },
    { L"DLG_VOL_NOI",                LANG_DLG_VOL_NOI                },
    { L"DLG_VOL_PCM",                LANG_DLG_VOL_PCM                },
    { L"DLG_VOL_EXT",                LANG_DLG_VOL_EXT                },
    { L"DLG_VOL_MUTE",               LANG_DLG_VOL_MUTE               },
    { L"DLG_VOL_CLOSE",              LANG_DLG_VOL_CLOSE              },
    { L"DLG_INES_TITLE",             LANG_DLG_INES_TITLE             },
    { L"DLG_INES_FILENAME",          LANG_DLG_INES_FILENAME          },
    { L"DLG_INES_VER1",              LANG_DLG_INES_VER1              },
    { L"DLG_INES_VER2",              LANG_DLG_INES_VER2              },
    { L"DLG_INES_MAPPER",            LANG_DLG_INES_MAPPER            },
    { L"DLG_INES_PRG",               LANG_DLG_INES_PRG               },
    { L"DLG_INES_CHR",               LANG_DLG_INES_CHR               },
    { L"DLG_INES_HORIZ",             LANG_DLG_INES_HORIZ             },
    { L"DLG_INES_VERT",              LANG_DLG_INES_VERT              },
    { L"DLG_INES_VS",                LANG_DLG_INES_VS                },
    { L"DLG_INES_PC10",              LANG_DLG_INES_PC10              },
    { L"DLG_INES_4SCR",              LANG_DLG_INES_4SCR              },
    { L"DLG_INES_BATT",              LANG_DLG_INES_BATT              },
    { L"DLG_INES_TRAIN",             LANG_DLG_INES_TRAIN             },
    { L"DLG_INES_SUBMAP",            LANG_DLG_INES_SUBMAP            },
    { L"DLG_INES_BATTERY",           LANG_DLG_INES_BATTERY           },
    { L"DLG_INES_PRGRAM",            LANG_DLG_INES_PRGRAM            },
    { L"DLG_INES_CHRRAM",            LANG_DLG_INES_CHRRAM            },
    { L"DLG_INES_NTSC",              LANG_DLG_INES_NTSC              },
    { L"DLG_INES_PAL",               LANG_DLG_INES_PAL               },
    { L"DLG_INES_DUAL",              LANG_DLG_INES_DUAL              },
    { L"DLG_INES_VSPPU",             LANG_DLG_INES_VSPPU             },
    { L"DLG_INES_VSFLAGS",           LANG_DLG_INES_VSFLAGS           },
    { L"DLG_MOVIE_PLAY_TITLE",        LANG_DLG_MOVIE_PLAY_TITLE        },
    { L"DLG_MOVIE_RECORD_TITLE",      LANG_DLG_MOVIE_RECORD_TITLE      },
    { L"DLG_MOVIE_FILE",              LANG_DLG_MOVIE_FILE              },
    { L"DLG_MOVIE_DESC",              LANG_DLG_MOVIE_DESC              },
    { L"DLG_MOVIE_LENGTH",            LANG_DLG_MOVIE_LENGTH            },
    { L"DLG_MOVIE_FRAMES",            LANG_DLG_MOVIE_FRAMES            },
    { L"DLG_MOVIE_RERECORDS",         LANG_DLG_MOVIE_RERECORDS         },
    { L"DLG_MOVIE_TVMODE",            LANG_DLG_MOVIE_TVMODE            },
    { L"DLG_MOVIE_RESUME",            LANG_DLG_MOVIE_RESUME            },
    { L"DLG_MOVIE_CONTROLLERS",       LANG_DLG_MOVIE_CONTROLLERS       },
    { L"DLG_MOVIE_PORT1",             LANG_DLG_MOVIE_PORT1             },
    { L"DLG_MOVIE_PORT2",             LANG_DLG_MOVIE_PORT2             },
    { L"DLG_MOVIE_EXPANSION",         LANG_DLG_MOVIE_EXPANSION         },
    { L"DLG_MOVIE_CONFIGURE",         LANG_DLG_MOVIE_CONFIGURE         },
    { L"DLG_MOVIE_RECORD_FROM",       LANG_DLG_MOVIE_RECORD_FROM       },
    { L"DLG_MOVIE_HARD_RESET",        LANG_DLG_MOVIE_HARD_RESET        },
    { L"DLG_MOVIE_CUR_STATE",         LANG_DLG_MOVIE_CUR_STATE         },
    { L"DLG_PAL_TITLE",               LANG_DLG_PAL_TITLE               },
    { L"DLG_PAL_PALETTE",             LANG_DLG_PAL_PALETTE             },
    { L"DLG_PAL_HUE",                 LANG_DLG_PAL_HUE                 },
    { L"DLG_PAL_SAT",                 LANG_DLG_PAL_SAT                 },
    { L"DLG_PAL_CUSTOM",              LANG_DLG_PAL_CUSTOM              },
    { L"DLG_PAL_PREVIEW",             LANG_DLG_PAL_PREVIEW             },
    { L"DLG_PAL_EMPHASIS",            LANG_DLG_PAL_EMPHASIS            },
    { L"DLG_PAL_NOTE",                LANG_DLG_PAL_NOTE                },
    { L"DLG_PAL_EXTRAGRAY",           LANG_DLG_PAL_EXTRAGRAY           },
    { L"DLG_DLL_TITLE",               LANG_DLG_DLL_TITLE               },
    { L"DLG_DLL_TEXT",                LANG_DLG_DLL_TEXT                },
    { L"DLG_BREAK_TITLE",             LANG_DLG_BREAK_TITLE             },
    { L"DLG_BREAK_ENABLED",           LANG_DLG_BREAK_ENABLED           },
    { L"DLG_BREAK_ADDR",              LANG_DLG_BREAK_ADDR              },
    { L"DLG_BREAK_OP",                LANG_DLG_BREAK_OP                },
    { L"DLG_BREAK_TIME",              LANG_DLG_BREAK_TIME              },
    { L"DLG_BREAK_EXEC",              LANG_DLG_BREAK_EXEC              },
    { L"DLG_BREAK_READ",              LANG_DLG_BREAK_READ              },
    { L"DLG_BREAK_WRITE",             LANG_DLG_BREAK_WRITE             },
    { L"DLG_BREAK_ACCESS",            LANG_DLG_BREAK_ACCESS            },
    { L"DLG_BREAK_OPCODE",            LANG_DLG_BREAK_OPCODE            },
    { L"DLG_BREAK_NMI",               LANG_DLG_BREAK_NMI               },
    { L"DLG_BREAK_IRQ",               LANG_DLG_BREAK_IRQ               },
    { L"DLG_BREAK_BRK",               LANG_DLG_BREAK_BRK               },
    { L"DLG_BREAK_SCANLINE",          LANG_DLG_BREAK_SCANLINE          },
    { L"DLG_BREAK_FROM",              LANG_DLG_BREAK_FROM              },
    { L"DLG_BREAK_TO",                LANG_DLG_BREAK_TO                },
    { L"DLG_DEBUG_TITLE",             LANG_DLG_DEBUG_TITLE             },
    { L"DLG_DBGCPU_TITLE",            LANG_DLG_DBGCPU_TITLE            },
    { L"DLG_DBGCPU_REGISTERS",        LANG_DLG_DBGCPU_REGISTERS        },
    { L"DLG_DBGCPU_TIMING",           LANG_DLG_DBGCPU_TIMING           },
    { L"DLG_DBGCPU_IRQS",             LANG_DLG_DBGCPU_IRQS             },
    { L"DLG_DBGCPU_FLAGS",            LANG_DLG_DBGCPU_FLAGS            },
    { L"DLG_DBGCPU_BANKS",            LANG_DLG_DBGCPU_BANKS            },
    { L"DLG_DBGCPU_BREAKPOINTS",      LANG_DLG_DBGCPU_BREAKPOINTS      },
    { L"DLG_DBGCPU_CONTROLS",         LANG_DLG_DBGCPU_CONTROLS         },
    { L"DLG_DBGCPU_MEMORY",           LANG_DLG_DBGCPU_MEMORY           },
    { L"DLG_DBGCPU_TRACE",            LANG_DLG_DBGCPU_TRACE            },
    { L"DLG_DBGCPU_RUN",              LANG_DLG_DBGCPU_RUN              },
    { L"DLG_DBGCPU_STEP",             LANG_DLG_DBGCPU_STEP             },
    { L"DLG_DBGCPU_RESET",            LANG_DLG_DBGCPU_RESET            },
    { L"DLG_DBGCPU_POWER",            LANG_DLG_DBGCPU_POWER            },
    { L"DLG_DBGCPU_DUMPCPU",          LANG_DLG_DBGCPU_DUMPCPU          },
    { L"DLG_DBGCPU_DUMPPPU",          LANG_DLG_DBGCPU_DUMPPPU          },
    { L"DLG_DBGCPU_STARTLOG",         LANG_DLG_DBGCPU_STARTLOG         },
    { L"DLG_DBGCPU_STOPLOG",          LANG_DLG_DBGCPU_STOPLOG          },
    { L"DLG_DBGCPU_SEEKPC",           LANG_DLG_DBGCPU_SEEKPC           },
    { L"DLG_DBGCPU_SEEKTO",           LANG_DLG_DBGCPU_SEEKTO           },
    { L"DLG_DBGCPU_SEEKSEL",          LANG_DLG_DBGCPU_SEEKSEL          },
    { L"DLG_DBGCPU_ADD",              LANG_DLG_DBGCPU_ADD              },
    { L"DLG_DBGCPU_EDIT",             LANG_DLG_DBGCPU_EDIT             },
    { L"DLG_DBGCPU_DELETE",           LANG_DLG_DBGCPU_DELETE           },
    { L"DLG_DBGPPU_TITLE",            LANG_DLG_DBGPPU_TITLE            },
    { L"DLG_DBGPPU_NAMETABLE",        LANG_DLG_DBGPPU_NAMETABLE        },
    { L"DLG_DBGPPU_SPRITES",          LANG_DLG_DBGPPU_SPRITES          },
    { L"DLG_DBGPPU_PATTERNS",         LANG_DLG_DBGPPU_PATTERNS         },
    { L"DLG_DBGPPU_PALETTE",          LANG_DLG_DBGPPU_PALETTE          },
    { L"DLG_DBGPPU_DETAILS",          LANG_DLG_DBGPPU_DETAILS          },
    { L"DLG_DBGPPU_CLICK",            LANG_DLG_DBGPPU_CLICK            },
    { L"DLG_DBGPPU_HOVER",            LANG_DLG_DBGPPU_HOVER            },
    { L"DLG_DBGPPU_SELTYPE",          LANG_DLG_DBGPPU_SELTYPE          },
    { L"DLG_DBGPPU_TILEFLIP",         LANG_DLG_DBGPPU_TILEFLIP         },
    { L"ERR_MAPPER_DLL_VERSION",       LANG_ERR_MAPPER_DLL_VERSION      },
    { L"ERR_MAPPER_DLL_LOAD",          LANG_ERR_MAPPER_DLL_LOAD         },
    { L"MENU_PPU_THEME",              LANG_MENU_PPU_THEME              },
    { L"MENU_PPU_THEME_LIGHT",        LANG_MENU_PPU_THEME_LIGHT        },
    { L"MENU_PPU_THEME_DARK",         LANG_MENU_PPU_THEME_DARK         },
    // File dialog filter display names
    { L"FILTER_ROM_ALL",              LANG_FILTER_ROM_ALL              },
    { L"FILTER_ROM_INES",             LANG_FILTER_ROM_INES             },
    { L"FILTER_ROM_UNIF",             LANG_FILTER_ROM_UNIF             },
    { L"FILTER_ROM_FDS",              LANG_FILTER_ROM_FDS              },
    { L"FILTER_ROM_NSF",              LANG_FILTER_ROM_NSF              },
    { L"FILTER_ROM_INES_SHORT",       LANG_FILTER_ROM_INES_SHORT       },
    { L"FILTER_MOVIE",                LANG_FILTER_MOVIE                },
    { L"FILTER_AVI",                  LANG_FILTER_AVI                  },
    { L"FILTER_PALETTE",              LANG_FILTER_PALETTE              },
    { L"OPEN_HEADER_TITLE",           LANG_OPEN_HEADER_TITLE           },
    // Language menu
    { L"NO_LANGUAGES",                LANG_NO_LANGUAGES                },
    // Movie
    { L"MOVIE_UNEXPECTED_EOF",        LANG_MOVIE_UNEXPECTED_EOF        },
    { L"MOVIE_TV_PAL",               LANG_MOVIE_TV_PAL               },
    { L"MOVIE_TV_NTSC",              LANG_MOVIE_TV_NTSC              },
    { L"MOVIE_LENGTH_FMT",           LANG_MOVIE_LENGTH_FMT           },
    // Debugger validation
    { L"DBG_INVALID_ADDR",            LANG_DBG_INVALID_ADDR            },
    { L"DBG_INVALID_OPCODE",          LANG_DBG_INVALID_OPCODE          },
    { L"DBG_INVALID_SCANLINE",        LANG_DBG_INVALID_SCANLINE        },
    // MapperInterface compatibility
    { L"COMPAT_UNSUPPORTED",          LANG_COMPAT_UNSUPPORTED          },
    { L"COMPAT_PARTIAL",              LANG_COMPAT_PARTIAL              },
    { L"COMPAT_MOSTLY",              LANG_COMPAT_MOSTLY              },
    { L"COMPAT_FULL",                 LANG_COMPAT_FULL                 },

    // Netplay (Kaillera)
    { L"NETPLAY_TITLE",                 LANG_NETPLAY_TITLE              },
    { L"NETPLAY_CHAT_TITLE",            LANG_NETPLAY_CHAT_TITLE         },
    { L"MENU_NETPLAY",                  LANG_MENU_NETPLAY               },
    { L"MENU_NETPLAY_CONNECT",          LANG_MENU_NETPLAY_CONNECT       },
    { L"MENU_NETPLAY_DISCONNECT",       LANG_MENU_NETPLAY_DISCONNECT    },
    { L"MENU_NETPLAY_CHAT",             LANG_MENU_NETPLAY_CHAT          },
    { L"MSG_NETPLAY_DLL_MISSING",       LANG_MSG_NETPLAY_DLL_MISSING    },
    { L"MSG_NETPLAY_DLL_BITNESS",       LANG_MSG_NETPLAY_DLL_BITNESS    },
    { L"MSG_NETPLAY_DLL_INVALID",       LANG_MSG_NETPLAY_DLL_INVALID    },
    { L"MSG_NETPLAY_NEED_ROM",          LANG_MSG_NETPLAY_NEED_ROM       },
    { L"MSG_NETPLAY_NSF",               LANG_MSG_NETPLAY_NSF            },
    { L"MSG_NETPLAY_CONTROLLERS",       LANG_MSG_NETPLAY_CONTROLLERS    },
    { L"MSG_NETPLAY_NO_SLOT",           LANG_MSG_NETPLAY_NO_SLOT        },
    { L"MSG_NETPLAY_PLAYERS_LIMIT",     LANG_MSG_NETPLAY_PLAYERS_LIMIT  },
    { L"MSG_NETPLAY_CONN_LOST",         LANG_MSG_NETPLAY_CONN_LOST      },
    { L"MSG_NETPLAY_SETTINGS",          LANG_MSG_NETPLAY_SETTINGS       },
    { L"MSG_NETPLAY_BLOCKED",           LANG_MSG_NETPLAY_BLOCKED        },

    { NULL, LANG_STRING_COUNT }
};

// ============================================================
// Function Implementation
// ============================================================
void Lang::Init(HINSTANCE hInst)
{
    ResetToDefaults();

    GetModuleFileName(hInst, LangDir, MAX_PATH);
    TCHAR *sl = _tcsrchr(LangDir, _T('\\'));
    if (sl) *(sl + 1) = 0;
    _tcscat(LangDir, _T("Lang\\"));
    CreateDirectory(LangDir, NULL);

    ScanLangDir();

    if (LanguageList.empty())
    {
        TCHAR ep[MAX_PATH];
        _tcscpy(ep, LangDir);
        _tcscat(ep, _T("English.lng"));
        FILE *f = NULL;
        _tfopen_s(&f, ep, _T("wt,ccs=UTF-8"));
        if (f)
        {
            fputs("# NintendulatorML Language File\n# Format: KEY=Value\n\n", f);
            fputs("[Info]\nLanguage=English\nAuthor=\nVersion=1.0\n\n[Strings]\n", f);
            for (int i = 0; KeyMap[i].key != NULL; i++)
            {
                char key8[128];
                WideCharToMultiByte(CP_UTF8, 0, KeyMap[i].key, -1, key8, 128, NULL, NULL);
                char val8[LANG_MAX_STRING * 2];
                WideCharToMultiByte(CP_UTF8, 0, DefaultStrings[KeyMap[i].id], -1, val8, sizeof(val8), NULL, NULL);
                fprintf(f, "%s=%s\n", key8, val8);
            }
            fclose(f);
        }
        ScanLangDir();
    }

    std::wstring saved = LoadFromRegistry();
    if (!saved.empty())
        Load(saved.c_str());
    else if (!LanguageList.empty())
        Load(LanguageList[0].fileName.c_str());
}

bool Lang::Load(const TCHAR *langName)
{
    ResetToDefaults();

    TCHAR path[MAX_PATH];
    _tcscpy(path, LangDir);
    _tcscat(path, langName);
    _tcscat(path, _T(".lng"));

    FILE *f = NULL;
    _tfopen_s(&f, path, _T("rt,ccs=UTF-8"));
    if (!f)
    {
        _tfopen_s(&f, path, _T("rt"));
        if (!f) return false;
    }

    bool inStrings = false;
    wchar_t line[LANG_MAX_STRING * 2];

    while (fgetws(line, _countof(line), f))
    {
        std::wstring s = Trim(std::wstring(line));
        if (s.empty() || s[0] == L'#') continue;
        if (s == L"[Strings]") { inStrings = true; continue; }
        if (s[0] == L'[')     { inStrings = false; continue; }
        if (!inStrings) continue;

        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) continue;

        std::wstring key = Trim(s.substr(0, eq));
        std::wstring val = s.substr(eq + 1);

        std::wstring out;
        for (size_t i = 0; i < val.size(); i++)
        {
            if (val[i] == L'\\' && i + 1 < val.size())
            {
                if (val[i+1] == L'n') { out += L'\n'; i++; }
                else if (val[i+1] == L't') { out += L'\t'; i++; }
                else out += val[i];
            }
            else out += val[i];
        }

        for (int i = 0; KeyMap[i].key != NULL; i++)
        {
            if (_wcsicmp(key.c_str(), KeyMap[i].key) == 0)
            { Strings[KeyMap[i].id] = out; break; }
        }
    }
    fclose(f);
    CurrentLanguage = langName;
    return true;
}

const TCHAR* Lang::GetString(LangStringID id)
{
    if (id < 0 || id >= LANG_STRING_COUNT) return _T("");
    return Strings[id].c_str();
}

int Lang::GetLanguageCount()
{
    return (int)LanguageList.size();
}

const TCHAR* Lang::GetLanguageFileName(int index)
{
    if (index < 0 || index >= (int)LanguageList.size()) return _T("");
    return LanguageList[index].fileName.c_str();
}

const TCHAR* Lang::GetLanguageDisplayName(int index)
{
    if (index < 0 || index >= (int)LanguageList.size()) return _T("");
    return LanguageList[index].displayName.c_str();
}

const TCHAR* Lang::GetCurrentLanguage()
{
    return CurrentLanguage.c_str();
}

void Lang::UpdateMenu(HMENU hMenu)
{
    if (!hMenu) return;

    // Helper: get current MF_GRAYED state for a menu item by command ID
    // ModifyMenu(MF_STRING) resets MF_GRAYED, so we must preserve it
    #define GRAY_STATE(hm, id) (GetMenuState(hm, id, MF_BYCOMMAND) & MF_GRAYED)
    // Helper: get both MF_CHECKED and MF_GRAYED state
    #define CHECK_GRAY_STATE(hm, id) ((GetMenuState(hm, id, MF_BYCOMMAND) & (MF_CHECKED|MF_GRAYED)))

    // === Top-level Menu ===
    ModifyMenu(hMenu, 0, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 0), GetString(LANG_MENU_FILE));
    ModifyMenu(hMenu, 1, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 1), GetString(LANG_MENU_CPU));
    ModifyMenu(hMenu, 2, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 2), GetString(LANG_MENU_PPU));
    ModifyMenu(hMenu, 3, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 3), GetString(LANG_MENU_SOUND));
    ModifyMenu(hMenu, 4, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 4), GetString(LANG_MENU_INPUT));
    ModifyMenu(hMenu, 5, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 5), GetString(LANG_MENU_DEBUG));
    // Position 6 = Game — a single menu item (not a popup), preserve MF_GRAYED
    ModifyMenu(hMenu, ID_GAME, MF_BYCOMMAND | MF_STRING | GRAY_STATE(hMenu, ID_GAME), ID_GAME, GetString(LANG_MENU_GAME));
    ModifyMenu(hMenu, 7, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 7), GetString(LANG_MENU_MISC));
    ModifyMenu(hMenu, 8, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 8), GetString(LANG_MENU_NETPLAY));
    ModifyMenu(hMenu, 9, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 9), GetString(LANG_MENU_LANGUAGE));
    ModifyMenu(hMenu, 10, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)GetSubMenu(hMenu, 10), GetString(LANG_MENU_HELP));

    // === File Submenu ===
    HMENU hFile = GetSubMenu(hMenu, 0);
    ModifyMenu(hFile, ID_FILE_OPEN,        MF_BYCOMMAND | MF_STRING, ID_FILE_OPEN,        GetString(LANG_MENU_FILE_OPEN));
    ModifyMenu(hFile, ID_FILE_CLOSE,       MF_BYCOMMAND | MF_STRING | GRAY_STATE(hFile, ID_FILE_CLOSE), ID_FILE_CLOSE,       GetString(LANG_MENU_FILE_CLOSE));
    ModifyMenu(hFile, ID_FILE_HEADER,      MF_BYCOMMAND | MF_STRING, ID_FILE_HEADER,      GetString(LANG_MENU_FILE_HEADER));
    ModifyMenu(hFile, ID_FILE_AUTORUN,     MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hFile, ID_FILE_AUTORUN), ID_FILE_AUTORUN,     GetString(LANG_MENU_FILE_AUTORUN));
    ModifyMenu(hFile, ID_FILE_BROWSESAVES, MF_BYCOMMAND | MF_STRING, ID_FILE_BROWSESAVES, GetString(LANG_MENU_FILE_BROWSESAVES));
    ModifyMenu(hFile, ID_FILE_EXIT,        MF_BYCOMMAND | MF_STRING, ID_FILE_EXIT,        GetString(LANG_MENU_FILE_EXIT));

    // === CPU Submenu ===
    HMENU hCPU = GetSubMenu(hMenu, 1);
    ModifyMenu(hCPU, ID_CPU_RUN,               MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_RUN),               ID_CPU_RUN,               GetString(LANG_MENU_NES_RUN));
    ModifyMenu(hCPU, ID_CPU_STEP,              MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_STEP),              ID_CPU_STEP,              GetString(LANG_MENU_NES_PAUSE));
    ModifyMenu(hCPU, ID_CPU_STOP,              MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_STOP),              ID_CPU_STOP,              GetString(LANG_MENU_NES_STOP));
    ModifyMenu(hCPU, ID_CPU_SOFTRESET,         MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_SOFTRESET),         ID_CPU_SOFTRESET,         GetString(LANG_MENU_NES_RESET));
    ModifyMenu(hCPU, ID_CPU_HARDRESET,         MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_HARDRESET),         ID_CPU_HARDRESET,         GetString(LANG_MENU_NES_POWER));
    ModifyMenu(hCPU, ID_CPU_SAVESTATE,         MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_SAVESTATE),         ID_CPU_SAVESTATE,         GetString(LANG_MENU_CPU_SAVESTATE));
    ModifyMenu(hCPU, ID_CPU_LOADSTATE,         MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_LOADSTATE),         ID_CPU_LOADSTATE,         GetString(LANG_MENU_CPU_LOADSTATE));
    ModifyMenu(hCPU, ID_CPU_PREVSTATE,         MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_PREVSTATE),         ID_CPU_PREVSTATE,         GetString(LANG_MENU_CPU_PREVSTATE));
    ModifyMenu(hCPU, ID_CPU_NEXTSTATE,         MF_BYCOMMAND | MF_STRING | GRAY_STATE(hCPU, ID_CPU_NEXTSTATE),         ID_CPU_NEXTSTATE,         GetString(LANG_MENU_CPU_NEXTSTATE));
    ModifyMenu(hCPU, ID_CPU_GAMEGENIE,         MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hCPU, ID_CPU_GAMEGENIE),   ID_CPU_GAMEGENIE,         GetString(LANG_MENU_SETTINGS_GAMEGENIE));
    ModifyMenu(hCPU, ID_CPU_BADOPS,            MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hCPU, ID_CPU_BADOPS),      ID_CPU_BADOPS,            GetString(LANG_MENU_CPU_BADOPS));
    // Frame Step - search all positions
    int cntCPU = GetMenuItemCount(hCPU);
    for (int i = 0; i < cntCPU; i++)
    {
        HMENU hFrameStep = GetSubMenu(hCPU, i);
        if (hFrameStep)
        {
            ModifyMenu(hCPU, i, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hFrameStep, GetString(LANG_MENU_CPU_FRAMESTEP));
            UINT chkFS = GetMenuState(hFrameStep, ID_CPU_FRAMESTEP_ENABLED, MF_BYCOMMAND) & MF_CHECKED;
            ModifyMenu(hFrameStep, ID_CPU_FRAMESTEP_ENABLED, MF_BYCOMMAND | MF_STRING | chkFS, ID_CPU_FRAMESTEP_ENABLED, GetString(LANG_MENU_CPU_FRAMESTEP_ENABLED));
            ModifyMenu(hFrameStep, ID_CPU_FRAMESTEP_STEP,    MF_BYCOMMAND | MF_STRING, ID_CPU_FRAMESTEP_STEP,    GetString(LANG_MENU_CPU_FRAMESTEP_STEP));
            break;
        }
    }

    // === PPU Submenu ===
    HMENU hPPU = GetSubMenu(hMenu, 2);
    ModifyMenu(hPPU, ID_PPU_PALETTE,       MF_BYCOMMAND | MF_STRING, ID_PPU_PALETTE,      GetString(LANG_MENU_PPU_PALETTE));
    ModifyMenu(hPPU, ID_PPU_FULLSCREEN,    MF_BYCOMMAND | MF_STRING, ID_PPU_FULLSCREEN,   GetString(LANG_MENU_PPU_FULLSCREEN));
    // Save current checkbox states before ModifyMenu,
    // since ModifyMenu(MF_STRING) resets MF_CHECKED flag
    ModifyMenu(hPPU, ID_PPU_SCANLINES,  MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hPPU, ID_PPU_SCANLINES),  ID_PPU_SCANLINES,  GetString(LANG_MENU_PPU_SCANLINES));
    ModifyMenu(hPPU, ID_PPU_BILINEAR,   MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hPPU, ID_PPU_BILINEAR),   ID_PPU_BILINEAR,   GetString(LANG_MENU_PPU_BILINEAR));
    ModifyMenu(hPPU, ID_PPU_MATCHRATE,  MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hPPU, ID_PPU_MATCHRATE),  ID_PPU_MATCHRATE,  GetString(LANG_MENU_PPU_MATCHRATE));
    ModifyMenu(hPPU, ID_PPU_INTSCALE,   MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hPPU, ID_PPU_INTSCALE),   ID_PPU_INTSCALE,   GetString(LANG_MENU_PPU_INTSCALE));
    ModifyMenu(hPPU, ID_PPU_ALWAYSONTOP, MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hPPU, ID_PPU_ALWAYSONTOP), ID_PPU_ALWAYSONTOP, GetString(LANG_MENU_PPU_ALWAYSONTOP));
    ModifyMenu(hPPU, ID_PPU_EXCLUSIVEFS, MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hPPU, ID_PPU_EXCLUSIVEFS), ID_PPU_EXCLUSIVEFS, GetString(LANG_MENU_PPU_EXCLUSIVEFS));

    // Theme submenu — it's the last popup in PPU menu
    {
        int cnt = GetMenuItemCount(hPPU);
        HMENU hTheme = GetSubMenu(hPPU, cnt - 1);
        if (hTheme)
        {
            ModifyMenu(hPPU, cnt - 1, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hTheme, GetString(LANG_MENU_PPU_THEME));
            ModifyMenu(hTheme, ID_PPU_THEME_LIGHT, MF_BYCOMMAND | MF_STRING, ID_PPU_THEME_LIGHT, GetString(LANG_MENU_PPU_THEME_LIGHT));
            ModifyMenu(hTheme, ID_PPU_THEME_DARK,  MF_BYCOMMAND | MF_STRING, ID_PPU_THEME_DARK,  GetString(LANG_MENU_PPU_THEME_DARK));
        }
    }

    // Sub-items Frameskip, Size, Mode, Slowdown - nested popups, update titles
    HMENU hFrameskip = GetSubMenu(hPPU, 0);
    if (hFrameskip)
    {
        ModifyMenu(hPPU, 0, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hFrameskip, GetString(LANG_MENU_PPU_FRAMESKIP));
        ModifyMenu(hFrameskip, ID_PPU_FRAMESKIP_AUTO, MF_BYCOMMAND | MF_STRING, ID_PPU_FRAMESKIP_AUTO, GetString(LANG_MENU_PPU_FRAMESKIP_AUTO));
    }
    HMENU hSize = GetSubMenu(hPPU, 1);
    if (hSize)
    {
        ModifyMenu(hPPU, 1, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hSize, GetString(LANG_MENU_PPU_SIZE));
        ModifyMenu(hSize, ID_PPU_SIZE_ASPECT, MF_BYCOMMAND | MF_STRING, ID_PPU_SIZE_ASPECT, GetString(LANG_MENU_PPU_FIXASPECT));
    }
    HMENU hMode = GetSubMenu(hPPU, 2);
    if (hMode)
    {
        ModifyMenu(hPPU, 2, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hMode, GetString(LANG_MENU_PPU_MODE));
        ModifyMenu(hMode, ID_PPU_MODE_DENDY, MF_BYCOMMAND | MF_STRING, ID_PPU_MODE_DENDY, GetString(LANG_MENU_PPU_MODE_HYBRID));
    }
    HMENU hSlowdown = GetSubMenu(hPPU, 4);
    if (hSlowdown)
    {
        ModifyMenu(hPPU, 4, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hSlowdown, GetString(LANG_MENU_PPU_SLOWDOWN));
        UINT chkSlow = GetMenuState(hSlowdown, ID_PPU_SLOWDOWN_ENABLED, MF_BYCOMMAND) & MF_CHECKED;
        ModifyMenu(hSlowdown, ID_PPU_SLOWDOWN_ENABLED, MF_BYCOMMAND | MF_STRING | chkSlow, ID_PPU_SLOWDOWN_ENABLED, GetString(LANG_MENU_PPU_SLOWDOWN_ENABLED));
    }

    // === Sound Submenu ===
    HMENU hSound = GetSubMenu(hMenu, 3);
    ModifyMenu(hSound, ID_SOUND_ENABLED,   MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hSound, ID_SOUND_ENABLED), ID_SOUND_ENABLED,    GetString(LANG_MENU_SOUND_ENABLED));
    ModifyMenu(hSound, ID_SOUND_VOLUME,    MF_BYCOMMAND | MF_STRING, ID_SOUND_VOLUME,     GetString(LANG_MENU_SETTINGS_SOUND));

    // === Input Submenu ===
    HMENU hInput = GetSubMenu(hMenu, 4);
    ModifyMenu(hInput, ID_INPUT_SETUP,     MF_BYCOMMAND | MF_STRING, ID_INPUT_SETUP,      GetString(LANG_MENU_SETTINGS_CONTROLLERS));

    // === Debug Submenu ===
    HMENU hDebug = GetSubMenu(hMenu, 5);
    ModifyMenu(hDebug, ID_DEBUG_CPU,       MF_BYCOMMAND | MF_STRING, ID_DEBUG_CPU,        GetString(LANG_MENU_DEBUG_CPU));
    ModifyMenu(hDebug, ID_DEBUG_PPU,       MF_BYCOMMAND | MF_STRING, ID_DEBUG_PPU,        GetString(LANG_MENU_DEBUG_PPU));
    ModifyMenu(hDebug, ID_DEBUG_STATWND,   MF_BYCOMMAND | MF_STRING | CHECK_GRAY_STATE(hDebug, ID_DEBUG_STATWND), ID_DEBUG_STATWND,    GetString(LANG_MENU_DEBUG_STATWND));

    // === Misc Submenu ===
    // GetSubMenu counts ALL items by position (including non-popup items like Game).
    // Position 6 = Game (not a popup, GetSubMenu returns NULL), so Misc is at position 7.
    // Position 8 = Netplay, Position 9 = Language, Position 10 = Help.
    HMENU hMisc = GetSubMenu(hMenu, 7);
    ModifyMenu(hMisc, ID_MISC_STARTAVICAPTURE, MF_BYCOMMAND | MF_STRING | GRAY_STATE(hMisc, ID_MISC_STARTAVICAPTURE), ID_MISC_STARTAVICAPTURE, GetString(LANG_MENU_MISC_STARTAVI));
    ModifyMenu(hMisc, ID_MISC_STOPAVICAPTURE,  MF_BYCOMMAND | MF_STRING | GRAY_STATE(hMisc, ID_MISC_STOPAVICAPTURE),  ID_MISC_STOPAVICAPTURE,  GetString(LANG_MENU_MISC_STOPAVI));
    ModifyMenu(hMisc, ID_MISC_PLAYMOVIE,       MF_BYCOMMAND | MF_STRING | GRAY_STATE(hMisc, ID_MISC_PLAYMOVIE),       ID_MISC_PLAYMOVIE,       GetString(LANG_MENU_MOVIE_PLAY));
    ModifyMenu(hMisc, ID_MISC_RECORDMOVIE,     MF_BYCOMMAND | MF_STRING | GRAY_STATE(hMisc, ID_MISC_RECORDMOVIE),     ID_MISC_RECORDMOVIE,     GetString(LANG_MENU_MOVIE_RECORD));
    ModifyMenu(hMisc, ID_MISC_STOPMOVIE,       MF_BYCOMMAND | MF_STRING | GRAY_STATE(hMisc, ID_MISC_STOPMOVIE),       ID_MISC_STOPMOVIE,       GetString(LANG_MENU_MOVIE_STOP));

    // === Netplay Submenu ===
    HMENU hNetplay = GetSubMenu(hMenu, 8);
    ModifyMenu(hNetplay, ID_NETPLAY_CONNECT,    MF_BYCOMMAND | MF_STRING | GRAY_STATE(hNetplay, ID_NETPLAY_CONNECT),    ID_NETPLAY_CONNECT,    GetString(LANG_MENU_NETPLAY_CONNECT));
    ModifyMenu(hNetplay, ID_NETPLAY_DISCONNECT, MF_BYCOMMAND | MF_STRING | GRAY_STATE(hNetplay, ID_NETPLAY_DISCONNECT), ID_NETPLAY_DISCONNECT, GetString(LANG_MENU_NETPLAY_DISCONNECT));
    ModifyMenu(hNetplay, ID_NETPLAY_CHAT,       MF_BYCOMMAND | MF_STRING | GRAY_STATE(hNetplay, ID_NETPLAY_CHAT),       ID_NETPLAY_CHAT,       GetString(LANG_MENU_NETPLAY_CHAT));

    // === Help Submenu ===
    HMENU hHelp = GetSubMenu(hMenu, 10);
    ModifyMenu(hHelp, ID_HELP_ABOUT,       MF_BYCOMMAND | MF_STRING, ID_HELP_ABOUT,       GetString(LANG_MENU_HELP_ABOUT));

    #undef GRAY_STATE
    #undef CHECK_GRAY_STATE

    DrawMenuBar(hMainWnd);
}

void Lang::SaveToRegistry(const TCHAR *langName)
{
    HKEY hk;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Nintendulator"), 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hk, _T("Language"), 0, REG_SZ, (const BYTE*)langName, (DWORD)((_tcslen(langName)+1)*sizeof(TCHAR)));
        RegCloseKey(hk);
    }
}

std::wstring Lang::LoadFromRegistry()
{
    HKEY hk;
    std::wstring r;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\Nintendulator"), 0, KEY_READ, &hk) == ERROR_SUCCESS)
    {
        TCHAR buf[256] = {0};
        DWORD sz = sizeof(buf), type = REG_SZ;
        if (RegQueryValueEx(hk, _T("Language"), NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
            r = buf;
        RegCloseKey(hk);
    }
    return r;
}
