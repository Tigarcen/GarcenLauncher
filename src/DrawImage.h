#pragma once

#include <windows.h>

#include <string>

#include <gdiplus.h>

// ========================================
// PNG image
// ========================================

// Draw the source rectangle (sourceX, sourceY, sourceWidth, sourceHeight)
// of the given PNG file into the destination rectangle.
// Returns false when the PNG cannot be loaded.
bool DrawPng(
    Gdiplus::Graphics& graphics,
    const wchar_t* pngPath,
    int sourceX,
    int sourceY,
    int sourceWidth,
    int sourceHeight,
    int destinationX,
    int destinationY,
    int destinationWidth,
    int destinationHeight);

// ========================================
// Embedded PNG resources
// ========================================

// Resource ids of the PNGs compiled into the executable (see version.rc)
enum AssetResource : int
{
    ASSET_TRASH     = 101,
    ASSET_TRASH_ON  = 102,
    ASSET_OPACITY   = 103,
    ASSET_THEME     = 104,
    ASSET_DROP      = 105,
    ASSET_BOX       = 106,
    ASSET_WEB       = 107,
    ASSET_APP_ICON  = 108,
};

// Draw a rectangle of an embedded PNG resource into the destination
// rectangle. Same as DrawPng but the image ships inside the exe, so no
// assets folder is needed next to it.
// Returns false when the resource cannot be loaded.
bool DrawPngResource(
    Gdiplus::Graphics& graphics,
    int resourceId,
    int sourceX,
    int sourceY,
    int sourceWidth,
    int sourceHeight,
    int destinationX,
    int destinationY,
    int destinationWidth,
    int destinationHeight);

// Convert an embedded PNG resource into an HICON at the requested square
// size. Used for the taskbar/window and tray icons. The caller owns the
// returned icon and must release it with DestroyIcon.
HICON CreatePngResourceIcon(int resourceId, int size);

// ========================================
// Executable icon
// ========================================

// Extract an icon matching the destination size from an executable and
// draw it with anti-aliased rounded corners (radius=6).
// Returns false when the icon cannot be loaded.
bool DrawExeIcon(
    Gdiplus::Graphics& graphics,
    const wchar_t* exePath,
    int destinationX,
    int destinationY,
    int destinationWidth,
    int destinationHeight);

// ========================================
// Scrollbar
// ========================================

// Draw the scrollbar thumb as a capsule (rounded rect, radius = width/2)
// at the given float position. Color is fixed at argb(0.2, 0, 0, 0).
// No track is drawn. Float coords keep easing animation sub-pixel smooth.
void DrawScrollbar(
    Gdiplus::Graphics& graphics,
    float x,
    float y,
    float width,
    float height);

// ========================================
// Hierarchy name strip
// ========================================

// Draw the name of the current folder level centered in a 20px tall
// strip inset 10px from the window edges on all three sides (left/top/right).
// Levels.empty() → draws nothing (we're on the root page).
// While the strip is being renamed the launcher paints a real EDIT control
// on top of it, so this stays a pure label drawing routine.
//   stripRect: the strip bounds (x=35, y=10, w=windowWidth-70, h=20)
//   displayName: current folder name
//   isDarkTheme: theme colour selection
void DrawHierarchyName(
    Gdiplus::Graphics& graphics,
    const Gdiplus::Rect& stripRect,
    const std::wstring& displayName,
    bool isDarkTheme);

// ========================================
// Back button
// ========================================

// Draw the folder-navigation back arrow (two rotated rounded rectangles)
// at the fixed location (16, 18). Color is white on dark theme, #333333
// on light. Dimensions match the launcher's button grid.
void DrawBackBottom(
    Gdiplus::Graphics& graphics,
    bool isDarkTheme);

// ========================================
// Trash
// ========================================

// Draw the delete zone (bottom-right). Both assets are sheet-style images
// holding two theme halves side by side:
//   trash.png    40×20 → dark uses the left 20×20, light the right 20×20
//   trash_on.png 40×21 → same split, 20×21 per half (1px taller while an
//                          item is being dragged, growing upward)
// The image is anchored to area's bottom-right corner.
void DrawTrash(
    Gdiplus::Graphics& graphics,
    const Gdiplus::Rect& area,
    bool isItemDragging,
    bool isDarkTheme);

// ========================================
// Insertion line
// ========================================

struct ActionArea;

// Which edge of the ActionArea the insertion line is attached to
enum class InsertionSide
{
    Left,
    Right,
};

// Draw the insertion indicator for the given item area: a 16px wide
// vertical bar centred on the area's left or right edge, spanning the
// area's full height. Colour argb(0.2, 46, 175, 240).
// The bar is the 16px rectangle minus two 6px wide side strips whose
// inner corners are rounded with radius 6.
void DrawInsertionLine(
    Gdiplus::Graphics& graphics,
    const ActionArea& area,
    InsertionSide side = InsertionSide::Right);

// ========================================
// Launcher item
// ========================================

struct Item;

// Draw one launcher item into the given area rectangle.
//   - Leaf items (no children): 32x32 exe icon at (x+26, y+23), plus
//     item.name horizontally centered below at y+61
//   - Folder items (has children): only item.name, horizontally centered
//   - text is white when isDarkTheme, otherwise #333333
//   - isDragTarget outlines a folder card with argb(1, 46, 175, 240)
//     while files are dragged over that item
//   - hideName skips the text label (used by the drag-over-item preview,
//     which shows a temporary group card without a title)
void DrawItem(
    Gdiplus::Graphics& graphics,
    const Item& item,
    int areaX,
    float areaY,
    int areaWidth,
    int areaHeight,
    bool isDarkTheme,
    bool isDragTarget,
    bool hideName = false);
