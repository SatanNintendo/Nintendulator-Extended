/* NintendulatorML - Theme engine
 * Provides light/dark theme support for Win32 dialogs and controls.
 * Uses Windows 10+ dark mode APIs (SetPreferredAppMode, AllowDarkModeForWindow)
 * with graceful fallback for Windows 7/8.
 *
 * How it works:
 * - On Windows 10 1809+: Uses undocumented uxtheme APIs for dark menus,
 *   scrollbars, and title bars, plus subclassing for dialog backgrounds.
 * - On Windows 7/8: Uses only subclassing for dialog backgrounds and controls.
 * - Menu dark mode is achieved via SetPreferredAppMode (Win10+).
 * - Title bar dark mode via DwmSetWindowAttribute (Win10+).
 * - Dialog backgrounds via WM_CTLCOLOR* subclassing (all Windows).
 */

#pragma once

#include <windows.h>

namespace Theme
{
    // Theme modes
    enum Mode
    {
        MODE_LIGHT = 0,   // Standard Windows light theme
        MODE_DARK  = 1    // Custom dark theme
    };

    // Initialize the theme system (called once at startup)
    void Init(void);

    // Clean up resources (called at shutdown)
    void Destroy(void);

    // Set current theme mode
    void SetMode(Mode mode);

    // Apply theme to a dialog window and all its child controls
    // This subclasses the dialog and all children to handle color messages
    // Also sets the dark title bar attribute on Windows 10+
    void ApplyToDialog(HWND hDlg);

    // Apply theme to the main window background, title bar, and menu
    void ApplyToMainWindow(HWND hWnd);

    // Re-apply the current theme (useful after language change resets colors)
    // Also re-applies to debugger windows if open
    void Reapply(void);

    // Save/Load theme setting from registry
    void SaveSettings(HKEY SettingsBase);
    void LoadSettings(HKEY SettingsBase);

    // Sync menu checkmark for theme menu item
    void SyncMenuChecks(void);

    // Get brush for WM_CTLCOLOR handlers (returns appropriate brush for control type)
    HBRUSH GetBackgroundBrush(void);
    HBRUSH GetControlBrush(void);

    // Get colors
    COLORREF GetBgColor(void);
    COLORREF GetTextColor(void);
    COLORREF GetControlBgColor(void);
    COLORREF GetControlTextColor(void);

    // Check if dark mode is active (convenience)
    bool IsDark(void);
}
