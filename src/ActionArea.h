#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include <gdiplus.h>

#include "List.h"

// ========================================
// Action area
// ========================================

// A clickable rectangular region on the launcher window.
// type: what the area does, key: payload interpreted according to type.
struct ActionArea
{
    std::wstring type;
    std::wstring key;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Open the file or folder at the given path with the default shell
// handler. Returns true when the shell accepted the request.
bool OpenItem(const std::wstring& path);

// ========================================
// Action area
ActionArea CreateArea(
    const wchar_t* type,
    const wchar_t* key,
    int x,
    int y,
    int width,
    int height);

// Return the first area containing the point (x, y),
// or nullptr when no area is hit
const ActionArea* HitTestArea(
    const std::vector<ActionArea>& areas,
    int x,
    int y);

// ========================================
// History item areas
// ========================================

// Snapshot of one navigation level's item areas, kept so the UI can
// restore the previous grid (positions and scroll offset) when going
// back up the folder hierarchy.
struct HistoryItemsAreas
{
    std::vector<ActionArea> areas;
    int scroll_distance = 0;
};

// ========================================
// View area
// ========================================

// A padded rectangular region with rounded corners, typically used as
// a content container inset from the window edges.
struct ViewArea
{
    int top = 10;
    int right = 10;
    int bottom = 40;
    int left = 10;
    int radius = 8;
};

// Fill the view area (inset from the window edges by viewArea's margins,
// with rounded corners) with the given color.
void PaintViewArea(
    Gdiplus::Graphics& graphics,
    int width,
    int height,
    const ViewArea& viewArea,
    Gdiplus::Color color = Gdiplus::Color(255, 255, 0, 0));

// ========================================
// Item size
// ========================================

// Fixed size of each item in the grid; C++ uses static constexpr for
// compile-time constants inside a struct/class.
struct ItemSize
{
    static constexpr int width = 96;
    static constexpr int height = 84;
};

// ========================================
// Item areas
// ========================================

// Build a grid of ActionArea entries (type = "item") from the given
// items array. Each cell is ItemSize::width x ItemSize::height, laid out
// left-to-right top-to-bottom inside the view area. The grid may grow
// past the window height vertically, but every row fits horizontally
// inside viewArea. The key of each area is:
//   - item.path  when the item has no children (leaf)
//   - item.name  when the item has children (folder)
std::vector<ActionArea> GetItemsAreas(
    const std::vector<Item>& items,
    const ViewArea& viewArea,
    int windowWidth);
