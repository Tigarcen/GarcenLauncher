#pragma once

#include <windows.h>

// ========================================
// Launcher configuration
// ========================================

struct LauncherConfig
{
    int width = 508;
    int height = 434;
    int radius = 10;
    wchar_t theme[16] = L"dark";
    double opacity = 1.0;
    // Global hotkey: Win32 MOD_* flags plus a virtual-key code.
    // Default is Alt + ` (MOD_ALT / VK_OEM_3)
    UINT hotkeyModifiers = MOD_ALT;
    UINT hotkeyKey = VK_OEM_3;
};

// Make sure config.gcson exists next to the executable,
// then read and parse it
LauncherConfig InitConfig();

// Background color for the given theme name
COLORREF GetThemeBackgroundColor(const wchar_t* theme);

// Write the current configuration back to config.gcson
bool UpdateSizeLocalize(const LauncherConfig& config);

// Persist the current theme to config.gcson
bool UpdateTheme(const LauncherConfig& config);
