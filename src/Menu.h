#pragma once

#include <windows.h>

#include "ControlConfig.h"

// ========================================
// Tray icon & popup menu
// ========================================

// Custom message sent by the tray icon (on left/right click)
constexpr UINT WM_TRAYICON = WM_USER + 100;

// Popup-menu item IDs
constexpr UINT IDM_TRAY_SHOW = 2001;
constexpr UINT IDM_TRAY_EXIT = 2002;

// Register the system tray icon.
// Returns true on success.
bool CreateTrayIcon(HWND hwnd, HINSTANCE hInstance);

// Remove the tray icon (and the popup menu window) from the system.
void DestroyTrayIcon(HWND hwnd);

// Show the theme-aware popup menu at the current cursor position.
// Clicking Show / Exit is reported back to `owner` as WM_COMMAND.
// The menu dismisses itself on focus loss, capture loss or Escape.
void ShowTrayMenu(HWND owner, const LauncherConfig& config);

// Show a themed context menu with up to 8 rows at the given screen point.
// The selected row's id is posted to `owner` as WM_COMMAND.
void ShowContextMenu(
    HWND owner,
    const LauncherConfig& config,
    const wchar_t* const* labels,
    const UINT* ids,
    int count,
    int screenX,
    int screenY);

// True while a context menu (ShowContextMenu) is on screen. The launcher
// uses it to avoid hiding itself when focus moves to the popup.
bool IsContextMenuVisible();

// Hide whichever popup menu is currently showing.
void HidePopupMenu();
