/* NintendulatorML - Theme engine implementation
 * Dark/Light theme for Win32 dialogs using subclassing + Win10 dark mode APIs.
 *
 * Technique:
 * 1. On Windows 10 1809+: Uses SetPreferredAppMode(APPMODE_ALLOW_DARK) from
 *    uxtheme.dll to enable dark system controls (menus, scrollbars, etc.)
 * 2. On Windows 10 1809+: Uses DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)
 *    for dark title bars.
 * 3. All Windows versions: Subclass every dialog with a custom WndProc that
 *    handles WM_CTLCOLOR*, WM_ERASEBKGND to paint backgrounds and text.
 * 4. GDI brushes are maintained and recreated on theme change.
 */

#include "stdafx.h"
#include "Theme.h"
#include "Nintendulator.h"
#include "resource.h"
#include "Lang.h"
#include "Debugger.h"
#include <commctrl.h>

// Try to import DWM functions (available on Vista+)
#pragma comment(lib, "dwmapi.lib")

namespace Theme
{
    // ============================================================
    // Current state
    // ============================================================
    static Mode currentMode = MODE_LIGHT;

    // ============================================================
    // Undocumented Windows 10+ dark mode API types and pointers
    // ============================================================
    typedef enum _PREFERRED_APP_MODE
    {
        APPMODE_DEFAULT    = 0,
        APPMODE_ALLOW_DARK = 1,
        APPMODE_FORCE_DARK = 2,
        APPMODE_FORCE_LIGHT = 3,
        APPMODE_MAX        = 4
    } PREFERRED_APP_MODE;

    typedef BOOL (WINAPI *pfnAllowDarkModeForWindow)(HWND, BOOL);
    typedef PREFERRED_APP_MODE (WINAPI *pfnSetPreferredAppMode)(PREFERRED_APP_MODE);
    typedef void (WINAPI *pfnRefreshImmersiveColorPolicyState)(void);

    static pfnAllowDarkModeForWindow  _AllowDarkModeForWindow = NULL;
    static pfnSetPreferredAppMode     _SetPreferredAppMode = NULL;
    static pfnRefreshImmersiveColorPolicyState _RefreshImmersiveColorPolicyState = NULL;
    static HMODULE hUxTheme = NULL;
    static BOOL darkModeAPIsAvailable = FALSE;

    // DWM attribute for dark title bar
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1+)
    // Before 20H1, the value was 19
    static const DWORD DWMWA_DARK_MODE_BEFORE_20H1 = 19;
    static const DWORD DWMWA_DARK_MODE = 20;

    // ============================================================
    // Color palettes
    // ============================================================
    struct ThemeColors
    {
        COLORREF bg;            // Window/dialog background
        COLORREF text;          // Normal text color
        COLORREF controlBg;     // Control background (edit, listbox, combobox)
        COLORREF controlText;   // Control text color
        COLORREF buttonBg;      // Button face color
        COLORREF buttonText;    // Button text color
        COLORREF menuBg;        // Menu background
        COLORREF menuText;      // Menu text
        COLORREF highlight;     // Selection highlight
        COLORREF highlightText; // Selection text
        COLORREF windowFrame;   // Border color
        COLORREF grayText;      // Disabled text
        COLORREF hotLight;      // Hot tracked item
        COLORREF inactiveBorder;// Inactive window border
    };

    static const ThemeColors LightColors = {
        RGB(240, 240, 240),     // bg
        RGB(0, 0, 0),           // text
        RGB(255, 255, 255),     // controlBg
        RGB(0, 0, 0),           // controlText
        RGB(240, 240, 240),     // buttonBg
        RGB(0, 0, 0),           // buttonText
        RGB(240, 240, 240),     // menuBg
        RGB(0, 0, 0),           // menuText
        RGB(0, 120, 215),       // highlight
        RGB(255, 255, 255),     // highlightText
        RGB(173, 173, 173),     // windowFrame
        RGB(109, 109, 109),     // grayText
        RGB(0, 102, 204),       // hotLight
        RGB(245, 245, 245)      // inactiveBorder
    };

    static const ThemeColors DarkColors = {
        RGB(30, 30, 30),        // bg - dark gray
        RGB(212, 212, 212),     // text - light gray
        RGB(45, 45, 45),        // controlBg
        RGB(212, 212, 212),     // controlText
        RGB(45, 45, 48),        // buttonBg
        RGB(212, 212, 212),     // buttonText
        RGB(30, 30, 30),        // menuBg
        RGB(212, 212, 212),     // menuText
        RGB(0, 122, 204),       // highlight - blue accent
        RGB(255, 255, 255),     // highlightText
        RGB(63, 63, 70),        // windowFrame
        RGB(112, 112, 112),     // grayText
        RGB(0, 122, 204),       // hotLight
        RGB(45, 45, 48)         // inactiveBorder
    };

    // ============================================================
    // GDI brushes (created once, reused)
    // ============================================================
    static HBRUSH hBgBrush       = NULL;
    static HBRUSH hControlBgBrush = NULL;
    static HBRUSH hButtonBgBrush = NULL;
    static HBRUSH hMenuBgBrush   = NULL;

    static void CreateBrushes(void)
    {
        const ThemeColors &c = (currentMode == MODE_DARK) ? DarkColors : LightColors;
        if (hBgBrush)        DeleteObject(hBgBrush);
        if (hControlBgBrush) DeleteObject(hControlBgBrush);
        if (hButtonBgBrush)  DeleteObject(hButtonBgBrush);
        if (hMenuBgBrush)    DeleteObject(hMenuBgBrush);

        hBgBrush        = CreateSolidBrush(c.bg);
        hControlBgBrush = CreateSolidBrush(c.controlBg);
        hButtonBgBrush  = CreateSolidBrush(c.buttonBg);
        hMenuBgBrush    = CreateSolidBrush(c.menuBg);
    }

    // ============================================================
    // Helper: get current colors
    // ============================================================
    static const ThemeColors& Colors(void)
    {
        return (currentMode == MODE_DARK) ? DarkColors : LightColors;
    }

    // ============================================================
    // Load dark mode APIs from uxtheme.dll (Windows 10+ only)
    // ============================================================
    static void LoadDarkModeAPIs(void)
    {
        hUxTheme = LoadLibrary(_T("uxtheme.dll"));
        if (!hUxTheme)
            return;

        // AllowDarkModeForWindow = ordinal 133
        _AllowDarkModeForWindow = (pfnAllowDarkModeForWindow)
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133));

        // SetPreferredAppMode = ordinal 135
        _SetPreferredAppMode = (pfnSetPreferredAppMode)
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135));

        // RefreshImmersiveColorPolicyState = ordinal 104
        _RefreshImmersiveColorPolicyState = (pfnRefreshImmersiveColorPolicyState)
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(104));

        if (_SetPreferredAppMode && _AllowDarkModeForWindow)
        {
            darkModeAPIsAvailable = TRUE;
            // Allow dark mode for the entire process
            _SetPreferredAppMode(APPMODE_ALLOW_DARK);
            if (_RefreshImmersiveColorPolicyState)
                _RefreshImmersiveColorPolicyState();
        }
    }

    // ============================================================
    // Apply dark title bar using DWM (Windows 10+ only)
    // ============================================================
    static void ApplyTitleBarDark(HWND hWnd, BOOL dark)
    {
        // DwmSetWindowAttribute is available on Vista+, but the dark mode
        // attribute only works on Windows 10 1809+
        typedef HRESULT (WINAPI *pfnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

        static pfnDwmSetWindowAttribute pDwmSetWindowAttribute = (pfnDwmSetWindowAttribute)-1;
        if (pDwmSetWindowAttribute == (pfnDwmSetWindowAttribute)-1)
        {
            HMODULE hDwm = LoadLibrary(_T("dwmapi.dll"));
            if (hDwm)
                pDwmSetWindowAttribute = (pfnDwmSetWindowAttribute)
                    GetProcAddress(hDwm, "DwmSetWindowAttribute");
            else
                pDwmSetWindowAttribute = NULL;
        }

        if (!pDwmSetWindowAttribute)
            return;

        BOOL value = dark ? TRUE : FALSE;

        // Try the newer attribute value first (20), then fall back to the older one (19)
        HRESULT hr = pDwmSetWindowAttribute(hWnd, DWMWA_DARK_MODE, &value, sizeof(value));
        if (FAILED(hr))
            pDwmSetWindowAttribute(hWnd, DWMWA_DARK_MODE_BEFORE_20H1, &value, sizeof(value));
    }

    // ============================================================
    // Subclass window procedure for themed dialogs
    // ============================================================
    static LRESULT CALLBACK ThemeDialogProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        switch (uMsg)
        {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        {
            const ThemeColors &c = Colors();
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, c.text);
            SetBkColor(hdc, c.bg);
            return (LRESULT)hBgBrush;
        }

        case WM_CTLCOLORLISTBOX:
        {
            const ThemeColors &c = Colors();
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, c.controlText);
            SetBkColor(hdc, c.controlBg);
            return (LRESULT)hControlBgBrush;
        }

        case WM_CTLCOLORSCROLLBAR:
        {
            const ThemeColors &c = Colors();
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, c.controlText);
            SetBkColor(hdc, c.controlBg);
            return (LRESULT)hControlBgBrush;
        }

        case WM_CTLCOLOREDIT:
        {
            const ThemeColors &c = Colors();
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, c.controlText);
            SetBkColor(hdc, c.controlBg);
            return (LRESULT)hControlBgBrush;
        }

        case WM_CTLCOLORBTN:
        {
            const ThemeColors &c = Colors();
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, c.buttonText);
            SetBkColor(hdc, c.buttonBg);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)hButtonBgBrush;
        }

        case WM_ERASEBKGND:
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, hBgBrush);
            return TRUE;
        }

        case WM_DESTROY:
            // Remove subclass when window is destroyed
            RemoveWindowSubclass(hWnd, ThemeDialogProc, 0);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    // ============================================================
    // Helper: force a window and its children to repaint
    // ============================================================
    static void ForceRepaint(HWND hWnd)
    {
        InvalidateRect(hWnd, NULL, TRUE);
        UpdateWindow(hWnd);

        // Repaint all child windows
        HWND hChild = FindWindowEx(hWnd, NULL, NULL, NULL);
        while (hChild)
        {
            InvalidateRect(hChild, NULL, TRUE);
            UpdateWindow(hChild);
            hChild = FindWindowEx(hWnd, hChild, NULL, NULL);
        }
    }

    // ============================================================
    // Helper: Apply dark mode to a window and all its child controls
    // (calls AllowDarkModeForWindow if available)
    // ============================================================
    static void ApplyDarkModeToWindowTree(HWND hWnd, BOOL enable)
    {
        if (darkModeAPIsAvailable)
            _AllowDarkModeForWindow(hWnd, enable);

        // Also apply to child windows (ComboBoxes, etc.)
        HWND hChild = FindWindowEx(hWnd, NULL, NULL, NULL);
        while (hChild)
        {
            if (darkModeAPIsAvailable)
                _AllowDarkModeForWindow(hChild, enable);
            hChild = FindWindowEx(hWnd, hChild, NULL, NULL);
        }
    }

    // ============================================================
    // Public API
    // ============================================================

    void Init(void)
    {
        currentMode = MODE_LIGHT;
        CreateBrushes();
        LoadDarkModeAPIs();
    }

    void Destroy(void)
    {
        if (hBgBrush)        { DeleteObject(hBgBrush);        hBgBrush = NULL; }
        if (hControlBgBrush) { DeleteObject(hControlBgBrush); hControlBgBrush = NULL; }
        if (hButtonBgBrush)  { DeleteObject(hButtonBgBrush);  hButtonBgBrush = NULL; }
        if (hMenuBgBrush)    { DeleteObject(hMenuBgBrush);    hMenuBgBrush = NULL; }

        // Note: Don't free hUxTheme here since other code may still use uxtheme
    }

    Mode GetMode(void)
    {
        return currentMode;
    }

    void SetMode(Mode mode)
    {
        if (currentMode == mode)
            return;
        currentMode = mode;
        CreateBrushes();
    }

    void Toggle(void)
    {
        SetMode(currentMode == MODE_LIGHT ? MODE_DARK : MODE_LIGHT);
    }

    void ApplyToDialog(HWND hDlg)
    {
        if (currentMode == MODE_DARK)
        {
            // Enable dark mode for this window and its children (Win10+)
            ApplyDarkModeToWindowTree(hDlg, TRUE);

            // Set dark title bar (Win10+)
            ApplyTitleBarDark(hDlg, TRUE);

            // Subclass the dialog to handle background/text colors
            SetWindowSubclass(hDlg, ThemeDialogProc, 0, 0);

            ForceRepaint(hDlg);
        }
        else
        {
            // Remove subclassing when switching to light mode
            RemoveWindowSubclass(hDlg, ThemeDialogProc, 0);

            // Disable dark mode for this window tree
            ApplyDarkModeToWindowTree(hDlg, FALSE);

            // Revert title bar to light
            ApplyTitleBarDark(hDlg, FALSE);

            ForceRepaint(hDlg);
        }
    }

    void RemoveFromDialog(HWND hDlg)
    {
        RemoveWindowSubclass(hDlg, ThemeDialogProc, 0);
    }

    void ApplyToMainWindow(HWND hWnd)
    {
        if (currentMode == MODE_DARK)
        {
            // Update the window class background brush
            SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)hBgBrush);

            // Enable dark mode for the main window (Win10+)
            if (darkModeAPIsAvailable)
                _AllowDarkModeForWindow(hWnd, TRUE);

            // Set dark title bar (Win10+)
            ApplyTitleBarDark(hWnd, TRUE);

            // Enable dark mode for menu bar (Win10+)
            if (darkModeAPIsAvailable && hMenu)
            {
                // Apply dark mode to menu window
                MENUBARINFO mbi = {0};
                mbi.cbSize = sizeof(MENUBARINFO);
                if (GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi))
                {
                    if (mbi.hwndMenu)
                        _AllowDarkModeForWindow(mbi.hwndMenu, TRUE);
                }
            }
        }
        else
        {
            SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)((HBRUSH)(COLOR_APPWORKSPACE + 1)));

            // Disable dark mode for the main window
            if (darkModeAPIsAvailable)
                _AllowDarkModeForWindow(hWnd, FALSE);

            // Revert title bar to light
            ApplyTitleBarDark(hWnd, FALSE);
        }

        InvalidateRect(hWnd, NULL, TRUE);
        UpdateWindow(hWnd);
    }

    void Reapply(void)
    {
        ApplyToMainWindow(hMainWnd);

        // Repaint the debug window
        if (hDebug)
            ApplyToDialog(hDebug);

#ifdef ENABLE_DEBUGGER
        // Repaint the debugger windows if they exist
        if (Debugger::CPUWnd)
            ApplyToDialog(Debugger::CPUWnd);
        if (Debugger::PPUWnd)
            ApplyToDialog(Debugger::PPUWnd);
#endif

        // Force a menu bar redraw
        DrawMenuBar(hMainWnd);

        // Refresh immersive color policy if available
        if (darkModeAPIsAvailable && _RefreshImmersiveColorPolicyState)
            _RefreshImmersiveColorPolicyState();
    }

    void SaveSettings(HKEY SettingsBase)
    {
        DWORD val = (DWORD)currentMode;
        RegSetValueEx(SettingsBase, _T("ThemeMode"), 0, REG_DWORD, (LPBYTE)&val, sizeof(DWORD));
    }

    void LoadSettings(HKEY SettingsBase)
    {
        unsigned long Size = sizeof(DWORD);
        DWORD val = 0;
        RegQueryValueEx(SettingsBase, _T("ThemeMode"), 0, NULL, (LPBYTE)&val, &Size);
        currentMode = (Mode)val;
        CreateBrushes();
    }

    void SyncMenuChecks(void)
    {
        if (currentMode == MODE_DARK)
        {
            CheckMenuItem(hMenu, ID_PPU_THEME_DARK, MF_CHECKED);
            CheckMenuItem(hMenu, ID_PPU_THEME_LIGHT, MF_UNCHECKED);
        }
        else
        {
            CheckMenuItem(hMenu, ID_PPU_THEME_DARK, MF_UNCHECKED);
            CheckMenuItem(hMenu, ID_PPU_THEME_LIGHT, MF_CHECKED);
        }
    }

    HBRUSH GetBackgroundBrush(void)
    {
        return hBgBrush;
    }

    HBRUSH GetControlBrush(void)
    {
        return hControlBgBrush;
    }

    COLORREF GetBgColor(void)
    {
        return Colors().bg;
    }

    COLORREF GetTextColor(void)
    {
        return Colors().text;
    }

    COLORREF GetControlBgColor(void)
    {
        return Colors().controlBg;
    }

    COLORREF GetControlTextColor(void)
    {
        return Colors().controlText;
    }

    bool IsDark(void)
    {
        return currentMode == MODE_DARK;
    }

    void EnableForWindow(HWND hWnd, BOOL enable)
    {
        if (darkModeAPIsAvailable)
            _AllowDarkModeForWindow(hWnd, enable);
    }

    void SetTitleBarDark(HWND hWnd, BOOL dark)
    {
        ApplyTitleBarDark(hWnd, dark);
    }

    void RefreshMenuBar(void)
    {
        if (darkModeAPIsAvailable && _RefreshImmersiveColorPolicyState)
            _RefreshImmersiveColorPolicyState();
        DrawMenuBar(hMainWnd);
    }

} // namespace Theme
