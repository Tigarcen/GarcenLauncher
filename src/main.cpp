#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include <dwmapi.h>
#include <gdiplus.h>
#include <mmsystem.h>

#include "ActionArea.h"
#include "ControlConfig.h"
#include "DrawImage.h"
#include "List.h"
#include "Menu.h"

// ========================================
// GarcenLauncher
// ========================================

constexpr wchar_t WINDOW_CLASS[] = L"GarcenLauncherWindow";
constexpr wchar_t WINDOW_TITLE[] = L"GarcenLauncher";

// Posted by a second launch to the instance that is already running, asking
// it to surface its window instead of leaving a duplicate in the tray.
constexpr UINT WM_APP_SHOW_LAUNCHER = WM_APP + 1;

// Image assets are embedded in the exe as RCDATA resources — see
// version.rc and the AssetResource enum in DrawImage.h.
//
// opacity.png holds two 25x25 icons side by side: the light theme icon on
// the left, the dark theme icon on the right
//
// theme.png holds two 20x20 icons side by side: the light theme icon on
// the left, the dark theme icon on the right
//
// drop.png is a 10x15 sprite sheet with three 10x5 rows:
//   y=0..5   : light theme, opacity < 1
//   y=5..10  : dark theme, opacity < 1
//   y=10..15 : opacity == 1

constexpr int THEME_ICON_SIZE = 20;

constexpr int THEME_ICON_MARGIN = 10;

constexpr int OPACITY_ICON_SIZE = 20;

constexpr int OPACITY_ICON_MARGIN = 10;

constexpr int HOTKEY_ID = 1001;

// Window class used by the "press a new hotkey" prompt. The hotkey itself
// (modifiers + virtual key) lives in config.gcson and is loaded into
// g_config by InitConfig.

constexpr wchar_t HOTKEY_PROMPT_CLASS[] = L"GarcenLauncherHotkeyPrompt";

HWND g_hwnd = nullptr;

// Window/taskbar icons created from the embedded win.png resource. Kept
// alive for as long as the registered window class may reference them.
HICON g_appIconLarge = nullptr;
HICON g_appIconSmall = nullptr;

// Held for the lifetime of the process; a second launch detecting that it
// already exists forwards a show request instead of starting up.
static HANDLE g_instanceMutex = nullptr;

LauncherConfig g_config;
COLORREF g_backgroundColor = 0;

// Parsed contents of list.json (list of launcher items)
std::vector<Item> g_items;

// Some or all items from g_items, filtered by the current view level
std::vector<Item> ViewItems;

// Hierarchy navigation cursor; empty when viewing the root level
std::vector<int> Levels;

// Name of the page currently being painted
std::wstring paintPageName = L"home";

// Clickable regions of the current page
std::vector<ActionArea> ActionAreas;

// Snapshots of the item areas of every level left behind while drilling
// down the folder hierarchy (one entry per folder entered)
std::vector<HistoryItemsAreas> HistoryItemsAreasCollect;

// ========================================
// Item context menu
// ========================================

constexpr UINT IDM_ITEM_RENAME = 2101;
constexpr UINT IDM_ITEM_LOCATION = 2102;

// Item the open context menu belongs to, plus the Levels stack captured
// when it was opened. Both commands verify the stack so a navigation that
// happened in between cannot rename/locate the wrong entry.
int g_contextItemIndex = -1;
std::vector<int> g_contextLevels;

// What the in-place rename editor is attached to.
enum class RenameTarget
{
    Strip, // hierarchy title strip (current folder name)
    Item,  // item label in the grid
};

RenameTarget g_renameTarget = RenameTarget::Strip;
int g_renameItemIndex = -1;

// Insertion indicator shown while files are dragged over the launcher
bool g_insertionVisible = false;
ActionArea g_insertionArea;
InsertionSide g_insertionSide = InsertionSide::Right;

// Index in ViewItems where a drop should be inserted (-1 = unknown, append)
int g_insertionIndex = -1;

// Folder item currently highlighted as a drop target (-1 = none). When
// set, the drop goes to the end of that folder's children instead of
// being inserted into the current level.
int g_insertionFolderIndex = -1;

// Leaf item under the middle third of an external drag. Dropping here
// replaces the leaf with a boxN group containing the leaf and dropped item.
int g_insertionMergeIndex = -1;

// Index of the ActionArea currently hovered by the mouse, or -1
int g_hoveredAreaIndex = -1;

// True while TrackMouseEvent is armed for WM_MOUSELEAVE
bool g_trackMouseActive = false;

// GDI+ objects used to compose every frame
ULONG_PTR g_gdiplusToken = 0;

// Frame buffer: a 32bpp premultiplied-alpha DIB handed to UpdateLayeredWindow
HDC g_frameDC = nullptr;
HBITMAP g_frameBitmap = nullptr;
HBITMAP g_framePreviousBitmap = nullptr;
void* g_frameBits = nullptr;

// Scroll offset for item grid inside the viewArea. Clamped to [0, maxOffset]:
//   0          = content at top (first row aligns viewArea.top)
//   +maxOffset = content scrolled up (last row aligns viewArea.bottom)
// Applied as a.y -= offset so positive values shift items UP — wheel DOWN
// (negative delta) increases offset and reveals items below.
// g_scrollOffsetY is the smoothed (float) value actually used for drawing;
// g_targetScrollOffsetY is the wheel-set integer destination. The timer
// lerps current toward target with exponential decay for a springy feel.
int g_targetScrollOffsetY = 0;
float g_scrollOffsetY = 0.0f;
int g_frameWidth = 0;
int g_frameHeight = 0;

// Wheel scrolling eases toward g_targetScrollOffsetY. The decay constant is
// defined per 16 ms and rescaled by the real frame delta, so the motion feels
// identical at 60, 120 or 144 Hz. Frames are paced by the monitor refresh
// rate in the message loop — not by WM_TIMER, which is quantised to the
// ~15.6 ms system tick and is the lowest-priority message in the queue.
constexpr float SCROLL_LERP_K = 0.25f;       // decay per 16 ms; higher = snappier
constexpr float SCROLL_SETTLE_EPSILON = 0.1f; // settled when |target-current| < this

constexpr UINT_PTR BACK_FLASH_TIMER_ID = 2003;
constexpr int BACK_FLASH_MS = 130;

// Back-button error flash: shown when a rename is rejected because a
// sibling folder already uses that name. The border blinks twice.
bool g_backFlashVisible = false;
int g_backFlashTogglesLeft = 0;

// Duplicate-drop feedback: the existing item is scrolled into view and
// its background flashes yellow twice.
constexpr UINT_PTR ITEM_FLASH_TIMER_ID = 2005;
constexpr int ITEM_FLASH_MS = 130;

bool g_itemFlashVisible = false;
int g_itemFlashTogglesLeft = 0;
std::wstring g_itemFlashPath;

static void StopItemFlash()
{
    g_itemFlashVisible = false;
    g_itemFlashTogglesLeft = 0;
    g_itemFlashPath.clear();

    if (g_hwnd)
        KillTimer(g_hwnd, ITEM_FLASH_TIMER_ID);
}

// ---- Level transition animation ----
// Entering a folder reveals the new level through a rounded rect that
// grows from the clicked cell's centre (0×0) into the whole viewArea;
// going back up shrinks the old level into the parent cell centre.
constexpr int VIEW_ANIM_DURATION_MS = 150;

// Motion curves in the same family Windows' Fluent design uses: content
// that appears decelerates into place, content that leaves accelerates
// away. The two directions deliberately do not share a curve.
struct AnimCurve
{
    float x1;
    float y1;
    float x2;
    float y2;
};

// Decelerate — fast start, gentle settle. Used when a level is opened.
constexpr AnimCurve VIEW_ANIM_CURVE_ENTER = {0.1f, 0.9f, 0.2f, 1.0f};

// Accelerate — slow start, quick exit. Used when a level is closed.
constexpr AnimCurve VIEW_ANIM_CURVE_EXIT = {0.7f, 0.0f, 1.0f, 0.5f};

bool g_viewAnimActive = false;
bool g_viewAnimExpanding = true;

// QPC timestamp (seconds) of the animation's first visible frame.
double g_viewAnimStartTime = 0.0;

// True while this process holds a 1 ms system timer resolution request.
// Raised only while some animation is being frame-paced, released as soon as
// the last one settles so an idle launcher stays in deep sleep states.
bool g_animTimerResolutionHeld = false;

// The UI thread is boosted only while animation frames are being paced, so
// an all-core workload cannot starve a short transition at normal priority.
bool g_animThreadPriorityHeld = false;
int g_animPreviousThreadPriority = THREAD_PRIORITY_NORMAL;

// Frame captured before changing levels. It keeps the old level visible
// until the transition finishes.
std::vector<unsigned char> g_viewAnimBackdrop;
int g_viewAnimBackdropWidth = 0;
int g_viewAnimBackdropHeight = 0;

// Fully rendered destination level. Animation ticks only composite this
// snapshot with the old one instead of reloading every item icon.
std::vector<unsigned char> g_viewAnimNewFrame;
int g_viewAnimNewFrameWidth = 0;
int g_viewAnimNewFrameHeight = 0;

struct ViewAnimRect
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

ViewAnimRect g_viewAnimFrom;
ViewAnimRect g_viewAnimTo;

// Hit area for the hierarchy name strip (clicking or double-clicking)
// — filled in RenderFrame, consumed by WM_LBUTTONDBLCLK / WM_LBUTTONDOWN
ActionArea g_hierarchyStripArea;

// True while the in-place rename editor covers the hierarchy strip. The
// editor lives in its own popup (see below); the painter skips the strip
// label so it cannot double-render through the popup's alpha.
bool g_renameActive = false;

// Delete zone in the bottom-right corner: 20×20, 40px from the right
// edge and 10px from the bottom edge. Recomputed every frame.
ActionArea trashArea;

// True while an item is being dragged inside the launcher
bool isItemDragging = false;

// --- Internal item drag state ---
// Pressed item (candidate) — the drag only starts once the mouse moves
// more than DRAG_START_THRESHOLD, so a plain click still opens the item.
int g_dragItemIndex = -1;
POINT g_dragStart = {0, 0};
POINT g_dragMouse = {0, 0};

// Hovered group → blue card outline (same visual as an external drop)
int g_dragHoverFolderIndex = -1;

// Leaf item left/right third → insertion line for reordering
bool g_dragInsertVisible = false;
ActionArea g_dragInsertArea;
InsertionSide g_dragInsertSide = InsertionSide::Right;

// Index in ActionAreas of the item the insertion line is attached to
int g_dragInsertAnchor = -1;

// Leaf item middle third → fake group preview (card with both icons)
int g_dragPreviewIndex = -1;

// True while the cursor (and the dragged ghost) is over the trash zone
bool g_dragOverTrash = false;

// True when hovering the back button is allowed to climb one level; it is
// disarmed after climbing so a single hover only moves up once
bool g_dragBackArmed = false;

// Distance the mouse must travel before a press becomes a drag
constexpr int DRAG_START_THRESHOLD = 5;

// Trash zone grows by this amount on every side both visually and for
// the drop hit test
constexpr int TRASH_PLATE_EXPAND = 4;

// Alpha applied to the dragged ghost cell (premultiplied frame pixels)
constexpr int GHOST_ALPHA = 153; // 60%

// Scrollbar drag state (WM_LBUTTONDOWN on the thumb starts a drag)
bool g_draggingScrollbar = false;
int g_dragStartMouseY = 0;
int g_dragStartOffset = 0;

// Set by input handlers that move the grid without easing (thumb drag).
// The frame loop consumes it so many mouse events coalesce into one frame.
bool g_scrollFrameRequested = false;

ViewArea viewArea;

// ========================================
// Scrollbar geometry
// ========================================

struct ScrollbarInfo
{
    bool visible = false;
    float thumbX = 0.0f;
    float thumbY = 0.0f;
    float thumbH = 0.0f;
    float travel = 0.0f;    // pixels the thumb can move inside viewArea
    float maxOffset = 0.0f; // scroll range the thumb travel maps to
};

// Thumb sits flush against the viewArea's right inner edge, 8px wide.
// Height = track * visible/total (standard proportional scrollbar),
// min 20px. Position follows the smoothed float offset, so it eases
// together with the items and never loses sub-pixel precision.
static ScrollbarInfo ComputeScrollbarInfo(int width, int height, int lastBottom)
{
    ScrollbarInfo info;

    const int vaTop = viewArea.top;
    const int vaBottom = height - viewArea.bottom;
    const int vaRight = width - viewArea.right;
    const float trackH = static_cast<float>(vaBottom - vaTop);

    int maxOffset = lastBottom - vaBottom;
    if (maxOffset < 0) maxOffset = 0;
    info.maxOffset = static_cast<float>(maxOffset);

    if (maxOffset <= 0 || trackH <= 0.0f)
        return info; // everything fits — no scrollbar

    const float contentH = static_cast<float>(lastBottom - vaTop);
    float thumbH = trackH * trackH / contentH;
    if (thumbH < 20.0f) thumbH = 20.0f;
    if (thumbH > trackH) thumbH = trackH;

    info.visible = true;
    info.thumbH = thumbH;
    info.travel = trackH - thumbH;
    info.thumbX = static_cast<float>(vaRight - 8);
    info.thumbY = static_cast<float>(vaTop)
        + info.travel * (g_scrollOffsetY / info.maxOffset);
    return info;
}

// ========================================
// Frame rendering
// ========================================

// The frame is composed into a 32bpp DIB and presented with
// UpdateLayeredWindow, so the rounded corners are antialiased through
// per-pixel alpha instead of being clipped by a hard-edged window region.

void ReleaseFrameBuffer()
{
    if (g_frameDC)
    {
        if (g_framePreviousBitmap)
        {
            SelectObject(g_frameDC, g_framePreviousBitmap);
            g_framePreviousBitmap = nullptr;
        }

        DeleteDC(g_frameDC);
        g_frameDC = nullptr;
    }

    if (g_frameBitmap)
    {
        DeleteObject(g_frameBitmap);
        g_frameBitmap = nullptr;
    }

    g_frameBits = nullptr;
    g_frameWidth = 0;
    g_frameHeight = 0;
}

bool EnsureFrameBuffer(int width, int height)
{
    if (g_frameDC && width == g_frameWidth && height == g_frameHeight)
        return true;

    ReleaseFrameBuffer();

    HDC screen = GetDC(nullptr);

    if (!screen)
        return false;

    g_frameDC = CreateCompatibleDC(screen);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    g_frameBitmap = CreateDIBSection(
        screen,
        &info,
        DIB_RGB_COLORS,
        &g_frameBits,
        nullptr,
        0);

    ReleaseDC(nullptr, screen);

    if (!g_frameDC || !g_frameBitmap)
    {
        ReleaseFrameBuffer();
        return false;
    }

    g_framePreviousBitmap = static_cast<HBITMAP>(
        SelectObject(g_frameDC, g_frameBitmap));

    g_frameWidth = width;
    g_frameHeight = height;

    return true;
}

// Client rect of the ghost cell that follows the cursor while an item is
// being dragged. The cell is positioned so the cursor sits on the icon.
static bool GetDragGhostRect(int& x, int& y, int& w, int& h)
{
    if (!isItemDragging || g_dragItemIndex < 0)
        return false;

    w = ItemSize::width;
    h = ItemSize::height;
    x = g_dragMouse.x - w / 2;
    y = g_dragMouse.y - 32;
    return true;
}

// ========================================
// Level transition animation
// ========================================

static ViewAnimRect MakeAnimRect(
    float left, float top, float right, float bottom)
{
    ViewAnimRect rect;
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = bottom;
    return rect;
}

// Append a closed rounded-rect figure. The corner diameter is clamped so
// it never exceeds the rect itself (needed while the reveal rect is still
// only a few pixels wide).
static void AddRoundedRectPath(
    Gdiplus::GraphicsPath& path,
    float x, float y, float width, float height, float radius)
{
    if (width < 0.0f) { x += width; width = -width; }
    if (height < 0.0f) { y += height; height = -height; }

    float diameter = radius * 2.0f;

    if (diameter > width) diameter = width;
    if (diameter > height) diameter = height;

    if (diameter <= 0.0f)
    {
        path.AddRectangle(Gdiplus::RectF(x, y, width, height));
        return;
    }

    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y,
        diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter,
        diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter,
        diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

// High-resolution time source for the transition. GetTickCount only
// advances in ~15.6 ms system ticks, which would quantise the eased
// progress into a dozen visible steps on a 200 ms animation.
static double QuerySecondsNow()
{
    static double secondsPerTick = 0.0;

    if (secondsPerTick <= 0.0)
    {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);

        secondsPerTick = frequency.QuadPart > 0
            ? 1.0 / static_cast<double>(frequency.QuadPart)
            : 0.001;
    }

    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);

    return static_cast<double>(counter.QuadPart) * secondsPerTick;
}

static double ViewAnimElapsedMs()
{
    return (QuerySecondsNow() - g_viewAnimStartTime) * 1000.0;
}

// Evaluate one axis of a cubic Bezier whose end points are (0,0) and (1,1).
// The curve is parameterised by s; progress on the x axis is t, so first
// invert x(s) = t with Newton-Raphson, then return y(s).
static float CubicBezierAxis(float p1, float p2, float s)
{
    const float inv = 1.0f - s;

    return 3.0f * inv * inv * s * p1
        + 3.0f * inv * s * s * p2
        + s * s * s;
}

static float CubicBezierEase(const AnimCurve& curve, float t)
{
    if (t <= 0.0f)
        return 0.0f;

    if (t >= 1.0f)
        return 1.0f;

    float s = t;

    for (int i = 0; i < 8; ++i)
    {
        const float dx = CubicBezierAxis(curve.x1, curve.x2, s) - t;

        if (std::fabs(dx) < 1e-5f)
            break;

        const float inv = 1.0f - s;

        // d/ds of the x axis
        const float slope =
            3.0f * inv * inv * curve.x1
            + 6.0f * inv * s * (curve.x2 - curve.x1)
            + 3.0f * s * s * (1.0f - curve.x2);

        // Too flat to converge — bisection would be needed; the clamped
        // result is visually indistinguishable at this resolution.
        if (std::fabs(slope) < 1e-6f)
            break;

        s -= dx / slope;

        if (s < 0.0f)
            s = 0.0f;
        else if (s > 1.0f)
            s = 1.0f;
    }

    return CubicBezierAxis(curve.y1, curve.y2, s);
}

// Reveal rect for the current frame. Opening a level decelerates into
// place; closing it accelerates away, so the two directions read as
// distinct motions rather than one played backwards.
static ViewAnimRect CurrentViewAnimRect()
{
    if (!g_viewAnimActive)
        return g_viewAnimTo;

    float t = static_cast<float>(
        ViewAnimElapsedMs() / static_cast<double>(VIEW_ANIM_DURATION_MS));

    if (t > 1.0f)
        t = 1.0f;
    else if (t < 0.0f)
        t = 0.0f;

    const float eased = CubicBezierEase(
        g_viewAnimExpanding ? VIEW_ANIM_CURVE_ENTER : VIEW_ANIM_CURVE_EXIT,
        t);

    return MakeAnimRect(
        g_viewAnimFrom.left + (g_viewAnimTo.left - g_viewAnimFrom.left) * eased,
        g_viewAnimFrom.top + (g_viewAnimTo.top - g_viewAnimFrom.top) * eased,
        g_viewAnimFrom.right + (g_viewAnimTo.right - g_viewAnimFrom.right) * eased,
        g_viewAnimFrom.bottom + (g_viewAnimTo.bottom - g_viewAnimFrom.bottom) * eased);
}

// Interpolated transition rect, kept at least one pixel wide/high so both
// the GDI+ path and the GDI region stay valid on the first frame.
static ViewAnimRect CurrentTransitionRect()
{
    ViewAnimRect rect = CurrentViewAnimRect();

    if (rect.right - rect.left < 1.0f)
    {
        const float cx = (rect.left + rect.right) * 0.5f;
        rect.left = cx - 0.5f;
        rect.right = cx + 0.5f;
    }

    if (rect.bottom - rect.top < 1.0f)
    {
        const float cy = (rect.top + rect.bottom) * 0.5f;
        rect.top = cy - 0.5f;
        rect.bottom = cy + 0.5f;
    }

    return rect;
}

// The rounded rect used to clip the viewArea this frame. Entering a level
// clips the new level to the growing rect; returning leaves the parent
// level fully visible and shrinks only the captured old level.
static ViewAnimRect CurrentViewClipRect(int width, int height)
{
    if (g_viewAnimActive && g_viewAnimExpanding)
        return CurrentTransitionRect();

    return MakeAnimRect(
        static_cast<float>(viewArea.left),
        static_cast<float>(viewArea.top),
        static_cast<float>(width - viewArea.right),
        static_cast<float>(height - viewArea.bottom));
}

static void CaptureFrameBits(
    std::vector<unsigned char>& target,
    int& targetWidth,
    int& targetHeight)
{
    targetWidth = g_frameWidth;
    targetHeight = g_frameHeight;

    if (g_frameBits && g_frameWidth > 0 && g_frameHeight > 0)
    {
        const size_t bytes = static_cast<size_t>(g_frameWidth) *
            static_cast<size_t>(g_frameHeight) * 4;
        target.resize(bytes);
        memcpy(target.data(), g_frameBits, bytes);
    }
    else
    {
        target.clear();
    }
}

static void StartViewAnimation(
    HWND hwnd,
    const ViewAnimRect& from,
    const ViewAnimRect& to,
    bool expanding)
{
    (void)hwnd;

    g_viewAnimFrom = from;
    g_viewAnimTo = to;
    g_viewAnimExpanding = expanding;
    g_viewAnimStartTime = QuerySecondsNow();
    g_viewAnimActive = true;

    // g_frameBits still contains the level that is being left. Keep that
    // exact frame so it can remain visible throughout the transition.
    CaptureFrameBits(g_viewAnimBackdrop,
        g_viewAnimBackdropWidth, g_viewAnimBackdropHeight);

    g_viewAnimNewFrame.clear();
    g_viewAnimNewFrameWidth = 0;
    g_viewAnimNewFrameHeight = 0;
}

// Hold or drop the 1 ms system timer resolution request. The frame loop
// sleeps for fractions of a millisecond between animation frames, so a
// coarse Sleep granularity would stall a high-refresh presentation.
// Holding it for the whole process lifetime would keep the CPU out of deep
// idle states while the launcher just sits in the tray.
static void SetAnimationTimerResolution(bool hold)
{
    if (hold == g_animTimerResolutionHeld)
        return;

    if (hold)
        timeBeginPeriod(1);
    else
        timeEndPeriod(1);

    g_animTimerResolutionHeld = hold;
}

static void SetAnimationThreadPriority(bool raise)
{
    if (raise == g_animThreadPriorityHeld)
        return;

    if (raise)
    {
        HANDLE thread = GetCurrentThread();
        const int previous = GetThreadPriority(thread);

        if (!SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL))
            return;

        g_animPreviousThreadPriority = previous;
        g_animThreadPriorityHeld = true;
        return;
    }

    SetThreadPriority(GetCurrentThread(), g_animPreviousThreadPriority);
    g_animThreadPriorityHeld = false;
}

// ========================================
// Animation frame clock
// ========================================

// One display frame in seconds. Frames are paced to the monitor's refresh
// rate so easing is presented at the rate the compositor can actually show.
static double DisplayFrameIntervalSeconds()
{
    static double interval = 0.0;

    if (interval > 0.0)
        return interval;

    int refreshHz = 0;

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);

    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
        refreshHz = static_cast<int>(mode.dmDisplayFrequency);

    // Some virtual / remote display drivers report 0 or 1 — fall back.
    if (refreshHz < 30 || refreshHz > 360)
        refreshHz = 60;

    interval = 1.0 / static_cast<double>(refreshHz);
    return interval;
}

// True while any animation still needs to advance: an in-flight level
// transition, an eased wheel scroll that has not settled, or a scrollbar
// drag that moved the grid since the last presented frame.
bool AnimationFramesNeeded()
{
    if (g_viewAnimActive || g_scrollFrameRequested)
        return true;

    const float diff =
        static_cast<float>(g_targetScrollOffsetY) - g_scrollOffsetY;

    return diff > SCROLL_SETTLE_EPSILON || diff < -SCROLL_SETTLE_EPSILON;
}

// Advance every time-based animation by the real elapsed time. Returns true
// on the tick where the level transition ends, so the caller can present the
// cached destination frame once more and then drop the cache.
bool AdvanceAnimations()
{
    static double lastTick = 0.0;

    const double now = QuerySecondsNow();

    double delta = now - lastTick;
    lastTick = now;

    // First tick, or a stall (window blocked / debugger) — treat as one
    // nominal 60 Hz step instead of teleporting the animation.
    if (delta <= 0.0 || delta > 0.25)
        delta = 1.0 / 60.0;

    bool viewAnimFinished = false;

    if (!g_draggingScrollbar)
    {
        const float target = static_cast<float>(g_targetScrollOffsetY);
        const float diff = target - g_scrollOffsetY;

        if (diff > -SCROLL_SETTLE_EPSILON && diff < SCROLL_SETTLE_EPSILON)
        {
            g_scrollOffsetY = target;
        }
        else
        {
            // Rescale the per-16 ms decay to the actual frame delta so the
            // easing covers the same distance per elapsed time at any Hz.
            const float steps = static_cast<float>(delta / 0.016);
            const float k = 1.0f - std::pow(1.0f - SCROLL_LERP_K, steps);

            g_scrollOffsetY += diff * k;
        }
    }

    if (g_viewAnimActive &&
        ViewAnimElapsedMs() >= static_cast<double>(VIEW_ANIM_DURATION_MS))
    {
        g_viewAnimActive = false;
        g_viewAnimBackdrop.clear();
        g_viewAnimBackdropWidth = 0;
        g_viewAnimBackdropHeight = 0;
        viewAnimFinished = true;
    }

    return viewAnimFinished;
}

// Return the horizontal pixel span of a rounded rect on one scanline.
// Sampling pixel centres keeps the clip aliased like Graphics::SetClip,
// while reducing the backdrop restore to a handful of memcpy calls.
static bool RoundedRectScanlineSpan(
    const ViewAnimRect& rect,
    float radius,
    int y,
    int frameWidth,
    int& spanLeft,
    int& spanRight)
{
    const float sampleY = static_cast<float>(y) + 0.5f;
    const float rectWidth = rect.right - rect.left;
    const float rectHeight = rect.bottom - rect.top;

    if (rectWidth <= 0.0f || rectHeight <= 0.0f ||
        sampleY < rect.top || sampleY >= rect.bottom)
    {
        return false;
    }

    const float maxRadius =
        (rectWidth < rectHeight ? rectWidth : rectHeight) * 0.5f;

    if (radius < 0.0f)
        radius = 0.0f;
    else if (radius > maxRadius)
        radius = maxRadius;

    float inset = 0.0f;

    if (radius > 0.0f)
    {
        float cornerY = 0.0f;

        if (sampleY < rect.top + radius)
            cornerY = rect.top + radius - sampleY;
        else if (sampleY > rect.bottom - radius)
            cornerY = sampleY - (rect.bottom - radius);

        if (cornerY > 0.0f)
        {
            const float horizontal = std::sqrt(
                radius * radius - cornerY * cornerY);
            inset = radius - horizontal;
        }
    }

    spanLeft = static_cast<int>(std::ceil(rect.left + inset - 0.5f));
    spanRight = static_cast<int>(std::ceil(rect.right - inset - 0.5f));

    if (spanLeft < 0) spanLeft = 0;
    if (spanRight > frameWidth) spanRight = frameWidth;

    return spanLeft < spanRight;
}

// Composite the captured old level with the new one and outline the
// transition rect. Entering: old level outside the growing rect, new level
// inside it. Leaving: parent level underneath, old level inside the
// shrinking rect.
static void DrawViewAnimationOverlay(
    Gdiplus::Graphics& graphics,
    int width,
    int height)
{
    if (!g_viewAnimActive)
        return;

    const ViewAnimRect rect = CurrentTransitionRect();
    const float radius = static_cast<float>(viewArea.radius);

    if (!g_viewAnimBackdrop.empty() &&
        g_viewAnimBackdropWidth == width &&
        g_viewAnimBackdropHeight == height)
    {
        unsigned char* frame = static_cast<unsigned char*>(g_frameBits);

        auto copyBackdropSpan = [&](int y, int left, int right) {
            if (left >= right)
                return;

            const size_t offset =
                (static_cast<size_t>(y) * width + left) * 4;
            memcpy(frame + offset, g_viewAnimBackdrop.data() + offset,
                static_cast<size_t>(right - left) * 4);
        };

        const ViewAnimRect viewRect = MakeAnimRect(
            static_cast<float>(viewArea.left),
            static_cast<float>(viewArea.top),
            static_cast<float>(width - viewArea.right),
            static_cast<float>(height - viewArea.bottom));

        for (int y = 0; y < height; ++y)
        {
            int animLeft = 0;
            int animRight = 0;
            const bool animVisible = RoundedRectScanlineSpan(
                rect, radius, y, width, animLeft, animRight);

            if (g_viewAnimExpanding)
            {
                int viewLeft = 0;
                int viewRight = 0;

                if (!RoundedRectScanlineSpan(viewRect, radius,
                        y, width, viewLeft, viewRight))
                {
                    continue;
                }

                if (!animVisible)
                {
                    copyBackdropSpan(y, viewLeft, viewRight);
                    continue;
                }

                const int leftEnd =
                    animLeft < viewRight ? animLeft : viewRight;
                copyBackdropSpan(y, viewLeft, leftEnd);

                const int rightStart =
                    animRight > viewLeft ? animRight : viewLeft;
                copyBackdropSpan(y, rightStart, viewRight);
            }
            else if (animVisible)
            {
                copyBackdropSpan(y, animLeft, animRight);
            }
        }
    }

    Gdiplus::GraphicsPath animPath;
    AddRoundedRectPath(animPath, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top, radius);

    const bool darkTheme = _wcsicmp(g_config.theme, L"dark") == 0;
    const Gdiplus::Color borderColor = darkTheme
        ? Gdiplus::Color(255, 255, 255, 255)
        : Gdiplus::Color(255, 0x33, 0x33, 0x33);
    Gdiplus::Pen borderPen(borderColor, 1.0f);

    graphics.DrawPath(&borderPen, &animPath);
}

void DrawFrameContent(int width, int height)
{
    Gdiplus::Graphics graphics(g_frameDC);

    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    // ClearType, not grayscale AA. Grayscale AA at a 13px CJK size can never
    // look as crisp as the ClearType text every other app renders, which is
    // why grid-fitting alone changed nothing visible. ClearType is safe here
    // because item labels always sit on the opaque window body: the frame DIB
    // keeps alpha = 255 under the glyphs, so DWM presents the subpixel colours
    // exactly instead of blending them against an unknown background.
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // During a transition the destination level has already been rendered
    // once. Reuse those pixels so a 16 ms tick never re-queries shell icons,
    // decodes PNG resources, or lays out every item again.
    if (!g_viewAnimNewFrame.empty() &&
        g_viewAnimNewFrameWidth == width &&
        g_viewAnimNewFrameHeight == height)
    {
        memcpy(g_frameBits, g_viewAnimNewFrame.data(),
            static_cast<size_t>(width) * height * 4);

        if (g_viewAnimActive)
            DrawViewAnimationOverlay(graphics, width, height);

        return;
    }

    // Background: rounded rectangle with antialiased edges
    Gdiplus::SolidBrush backgroundBrush(Gdiplus::Color(
        255,
        GetRValue(g_backgroundColor),
        GetGValue(g_backgroundColor),
        GetBValue(g_backgroundColor)));

    const Gdiplus::RectF clientRect(
        0.0f,
        0.0f,
        static_cast<Gdiplus::REAL>(width),
        static_cast<Gdiplus::REAL>(height));

    if (g_config.radius > 0)
    {
        const Gdiplus::REAL diameter =
            static_cast<Gdiplus::REAL>(g_config.radius * 2);

        Gdiplus::GraphicsPath path;

        path.AddArc(clientRect.X, clientRect.Y,
            diameter, diameter, 180.0f, 90.0f);
        path.AddArc(clientRect.GetRight() - diameter, clientRect.Y,
            diameter, diameter, 270.0f, 90.0f);
        path.AddArc(clientRect.GetRight() - diameter, clientRect.GetBottom() - diameter,
            diameter, diameter, 0.0f, 90.0f);
        path.AddArc(clientRect.X, clientRect.GetBottom() - diameter,
            diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();

        graphics.FillPath(&backgroundBrush, &path);
    }
    else
    {
        graphics.FillRectangle(&backgroundBrush, clientRect);
    }

    // Rebuild the clickable regions for the page being painted
    ActionAreas.clear();

    // Draw the home page
    if (paintPageName == L"home")
    {
        // Build item grid areas from ViewItems
        std::vector<ActionArea> itemAreas = GetItemsAreas(
            ViewItems, viewArea, width);

        // Compute and clamp valid scroll range. offset ∈ [0, maxOffset]:
        //   0          = content at top (first row aligns viewArea.top)
        //   +maxOffset = content scrolled up (last row aligns viewArea.bottom)
        // Split into integer part (written into area.y for HitTest accuracy
        // to the pixel) and fractional part (applied at DrawItem call time
        // for sub-pixel smoothness during easing).
        int vaBottom = height - viewArea.bottom;
        int lastBottom = 0;
        for (const auto& a : itemAreas)
        {
            if (a.y + a.height > lastBottom)
                lastBottom = a.y + a.height;
        }
        int maxOffset = lastBottom - vaBottom;
        if (maxOffset < 0) maxOffset = 0;

        if (g_targetScrollOffsetY < 0)         g_targetScrollOffsetY = 0;
        if (g_targetScrollOffsetY > maxOffset) g_targetScrollOffsetY = maxOffset;
        if (g_scrollOffsetY < 0.0f)            g_scrollOffsetY = 0.0f;
        if (g_scrollOffsetY > static_cast<float>(maxOffset))
            g_scrollOffsetY = static_cast<float>(maxOffset);

        const int intOffset = static_cast<int>(g_scrollOffsetY);
        const float fracOffset =
            g_scrollOffsetY - static_cast<float>(intOffset);

        for (auto& a : itemAreas)
            a.y -= intOffset;

        ActionAreas.insert(
            ActionAreas.end(),
            itemAreas.begin(),
            itemAreas.end());

        // overflow:hidden — clip everything inside the viewArea rounded rect
        // so items that overflow the view area get cut off (both hover and
        // item draws). We restore clip after all item rendering is done.
        // During a level transition the clip follows the growing/shrinking
        // reveal rect instead, which produces the expand/collapse effect.
        {
            const ViewAnimRect clipRect = CurrentViewClipRect(width, height);
            const float vaL = clipRect.left;
            const float vaT = clipRect.top;
            const float vaR = clipRect.right;
            const float vaB = clipRect.bottom;

            Gdiplus::GraphicsPath vaPath;
            AddRoundedRectPath(vaPath, vaL, vaT, vaR - vaL, vaB - vaT,
                static_cast<float>(viewArea.radius));
            graphics.SetClip(&vaPath, Gdiplus::CombineModeReplace);

            // GDI clip on g_frameDC — catches direct BitBlt calls from
            // DrawExeIcon that bypass GDI+ Graphics.SetClip. Create a
            // rounded-rect GDI region identical to the GDI+ clip path.
            HRGN hGdiRgn = CreateRoundRectRgn(
                static_cast<int>(vaL), static_cast<int>(vaT),
                static_cast<int>(vaR) + 1, static_cast<int>(vaB) + 1,
                viewArea.radius, viewArea.radius);
            SelectClipRgn(g_frameDC, hGdiRgn);
            DeleteObject(hGdiRgn); // region is copied by SelectClipRgn
        }

        // Hover highlight: semi-transparent black rounded rect (radius=8)
        // over the currently hovered item area
        if (g_hoveredAreaIndex >= 0 &&
            g_hoveredAreaIndex < static_cast<int>(ActionAreas.size()) &&
            ActionAreas[g_hoveredAreaIndex].type == L"item")
        {
            const ActionArea& hov = ActionAreas[g_hoveredAreaIndex];
            const Gdiplus::REAL radius = 8.0f;
            const Gdiplus::REAL diameter = radius * 2.0f;
            const Gdiplus::RectF hovRect(
                static_cast<Gdiplus::REAL>(hov.x),
                static_cast<Gdiplus::REAL>(hov.y) - fracOffset,
                static_cast<Gdiplus::REAL>(hov.width),
                static_cast<Gdiplus::REAL>(hov.height));

            Gdiplus::GraphicsPath hovPath;
            hovPath.AddArc(hovRect.X, hovRect.Y,
                diameter, diameter, 180.0f, 90.0f);
            hovPath.AddArc(hovRect.GetRight() - diameter, hovRect.Y,
                diameter, diameter, 270.0f, 90.0f);
            hovPath.AddArc(hovRect.GetRight() - diameter,
                hovRect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
            hovPath.AddArc(hovRect.X, hovRect.GetBottom() - diameter,
                diameter, diameter, 90.0f, 90.0f);
            hovPath.CloseFigure();

            Gdiplus::SolidBrush hoverBrush(Gdiplus::Color(25, 0, 0, 0));
            graphics.FillPath(&hoverBrush, &hovPath);
        }

        // Draw each item that intersects the view area
        if (!itemAreas.empty())
        {
            const bool darkTheme =
                _wcsicmp(g_config.theme, L"dark") == 0;

            const int vaLeft = viewArea.left;
            const int vaTop = viewArea.top;
            const int vaRight = width - viewArea.right;
            const int vaBottom = height - viewArea.bottom;

            for (size_t i = 0; i < itemAreas.size(); ++i)
            {
                const ActionArea& area = itemAreas[i];

                const int arLeft = area.x;
                const int arTop = area.y;
                const int arRight = area.x + area.width;
                const int arBottom = area.y + area.height;

                // Skip areas with no overlap on the view area
                if (arLeft >= vaRight || arRight <= vaLeft ||
                    arTop >= vaBottom || arBottom <= vaTop)
                {
                    continue;
                }

                const size_t itemIndex = i < ViewItems.size() ? i : 0;

                // The dragged item leaves its slot blank (the grid does
                // not reflow); the ghost follows the cursor instead.
                if (isItemDragging &&
                    static_cast<int>(i) == g_dragItemIndex)
                {
                    continue;
                }

                // Duplicate drop feedback: yellow background behind the
                // existing item, keeping its icon and name readable.
                if (g_itemFlashVisible && !g_itemFlashPath.empty() &&
                    ViewItems[itemIndex].path == g_itemFlashPath)
                {
                    Gdiplus::GraphicsPath flashPath;
                    AddRoundedRectPath(flashPath,
                        static_cast<float>(area.x),
                        static_cast<float>(area.y) - fracOffset,
                        static_cast<float>(area.width),
                        static_cast<float>(area.height),
                        8.0f);

                    Gdiplus::SolidBrush flashBrush(
                        Gdiplus::Color(255, 252, 195, 31));
                    graphics.FillPath(&flashBrush, &flashPath);
                }

                // External drop over the middle third of a leaf: preview
                // the group card that will be created on drop. The incoming
                // path is not decoded here, keeping DragOver inexpensive.
                if (!isItemDragging &&
                    static_cast<int>(i) == g_insertionMergeIndex)
                {
                    Item preview;
                    preview.children.push_back(ViewItems[itemIndex]);

                    DrawItem(
                        graphics,
                        preview,
                        area.x,
                        static_cast<float>(area.y) - fracOffset,
                        area.width,
                        area.height,
                        darkTheme,
                        true,
                        true);

                    continue;
                }

                // Middle third over a leaf: pretend this leaf already
                // became a group holding it plus the dragged item. Card
                // only — no title.
                if (isItemDragging &&
                    static_cast<int>(i) == g_dragPreviewIndex &&
                    g_dragItemIndex >= 0 &&
                    g_dragItemIndex < static_cast<int>(ViewItems.size()))
                {
                    Item preview;
                    preview.name = ViewItems[itemIndex].name;
                    preview.children.push_back(ViewItems[itemIndex]);
                    preview.children.push_back(ViewItems[g_dragItemIndex]);

                    DrawItem(
                        graphics,
                        preview,
                        area.x,
                        static_cast<float>(area.y) - fracOffset,
                        area.width,
                        area.height,
                        darkTheme,
                        false,
                        true);

                    continue;
                }

                DrawItem(
                    graphics,
                    ViewItems[itemIndex],
                    area.x,
                    static_cast<float>(area.y) - fracOffset,
                    area.width,
                    area.height,
                    darkTheme,
                    static_cast<int>(i) == g_insertionFolderIndex ||
                        static_cast<int>(i) == g_dragHoverFolderIndex,
                    // The in-place rename editor paints this label instead.
                    g_renameActive &&
                        g_renameTarget == RenameTarget::Item &&
                        static_cast<int>(i) == g_renameItemIndex);
            }
        }

        // Insertion indicator anchored to the last item — above the items,
        // below the scrollbar thumb
        if (g_insertionVisible)
            DrawInsertionLine(graphics, g_insertionArea, g_insertionSide);

        // Same indicator for reordering an item by dragging it
        if (isItemDragging && g_dragInsertVisible)
            DrawInsertionLine(graphics, g_dragInsertArea, g_dragInsertSide);

        // Empty level: show a 24x24 box glyph centred in the view area
        if (ViewItems.empty())
        {
            const int centreX = (viewArea.left + (width - viewArea.right)) / 2;
            const int centreY = (viewArea.top + (height - viewArea.bottom)) / 2;

            DrawPngResource(
                graphics,
                ASSET_BOX,
                0,
                0,
                24,
                24,
                centreX - 12,
                centreY - 12,
                24,
                24);
        }

        // Scrollbar thumb — drawn after items so it sits above them,
        // still inside the viewArea clip so no pixel can leak outside.
        // No track is drawn, only the thumb.
        const ScrollbarInfo sb = ComputeScrollbarInfo(width, height, lastBottom);
        if (sb.visible)
        {
            DrawScrollbar(graphics, sb.thumbX, sb.thumbY, 8.0f, sb.thumbH);

            // Registered as a button so HitTestArea gives it priority
            // over any item whose area extends beneath the thumb.
            ActionAreas.push_back(CreateArea(
                L"button",
                L"scrollbar",
                static_cast<int>(sb.thumbX),
                static_cast<int>(sb.thumbY + 0.5f),
                8,
                static_cast<int>(sb.thumbH + 0.5f)));
        }

        // Restore clip so opacity/theme buttons are not clipped
        graphics.SetClip(Gdiplus::Rect(0, 0, width, height),
            Gdiplus::CombineModeReplace);
        SelectClipRgn(g_frameDC, nullptr); // also clear GDI clip on frame DC

        // Back button drawn AFTER the viewArea clip is released. It sits
        // at y=18 which is ABOVE viewArea.top=34 when inside a subfolder,
        // so drawing it inside the clip would erase the whole arrow.
        if (!Levels.empty())
        {
            const bool isDark =
                _wcsicmp(g_config.theme, L"dark") == 0;

            // Hierarchy name strip: 35px inset from left/right (clear of
            // the back chevron at x=16) and 10px from top, 20px tall.
            Gdiplus::Rect stripRect(35, 10, width - 70, 20);

            // Resolve the strip's source name from g_items via Levels.
            // While the strip is being renamed the EDIT control paints the
            // text itself, so the label underneath is skipped.
            if (!g_renameActive)
            {
                std::wstring stripName;

                // Walk Levels through g_items. Each iteration lands on
                // the folder at that level; we keep a raw pointer to the
                // final folder item so we can read its name.
                const std::vector<Item>* cur = &g_items;
                const Item* folderItem = nullptr;
                for (int idx : Levels)
                {
                    if (idx < 0 || idx >= static_cast<int>(cur->size()))
                    {
                        folderItem = nullptr;
                        break;
                    }
                    folderItem = &(*cur)[idx];
                    cur = &folderItem->children;
                }
                if (folderItem) stripName = folderItem->name;

                DrawHierarchyName(graphics, stripRect, stripName, isDark);
            }

            // Register the strip's hit area so double-clicking can start
            // rename mode. We store it in a dedicated global because the
            // back button (20×20 at 10,10) overlaps the strip's left edge
            // and must win hit-test priority — ActionAreas stores only
            // buttons/items and we want the back button to always be
            // clickable regardless of what else sits at (10,10).
            g_hierarchyStripArea = CreateArea(
                L"hierarchy_strip",
                L"hierarchy_strip",
                35,
                10,
                width - 70,
                20);

            // Rename-rejected feedback: fill a rounded rect (radius 6)
            // behind the back chevron, expanded 4px beyond its 20×20 hit
            // area → (6,6)-(34,34). Drawn before DrawBackBottom so the
            // chevron stays on top.
            if (g_backFlashVisible)
            {
                const Gdiplus::RectF flashRect(6.0f, 6.0f, 28.0f, 28.0f);
                const Gdiplus::REAL diameter = 12.0f; // 2 × radius

                Gdiplus::GraphicsPath flashPath;
                flashPath.AddArc(flashRect.X, flashRect.Y,
                    diameter, diameter, 180.0f, 90.0f);
                flashPath.AddArc(flashRect.GetRight() - diameter,
                    flashRect.Y, diameter, diameter, 270.0f, 90.0f);
                flashPath.AddArc(flashRect.GetRight() - diameter,
                    flashRect.GetBottom() - diameter,
                    diameter, diameter, 0.0f, 90.0f);
                flashPath.AddArc(flashRect.X,
                    flashRect.GetBottom() - diameter,
                    diameter, diameter, 90.0f, 90.0f);
                flashPath.CloseFigure();

                Gdiplus::SolidBrush flashBrush(
                    Gdiplus::Color(255, 252, 195, 31));
                graphics.FillPath(&flashBrush, &flashPath);
            }

            DrawBackBottom(graphics, isDark);

            // Hit region 10,10 to 30,30 — 20×20 square covering the
            // chevron and some click-safe padding around it
            ActionAreas.push_back(CreateArea(
                L"button",
                L"back",
                10,
                10,
                20,
                20));
        }

        // Light theme uses the left icon, dark theme the right one
        const bool lightTheme =
            _wcsicmp(g_config.theme, L"light") == 0;

        DrawPngResource(
            graphics,
            ASSET_OPACITY,
            lightTheme ? 0 : OPACITY_ICON_SIZE,
            0,
            OPACITY_ICON_SIZE,
            OPACITY_ICON_SIZE,
            OPACITY_ICON_MARGIN,
            height - OPACITY_ICON_MARGIN - OPACITY_ICON_SIZE,
            OPACITY_ICON_SIZE,
            OPACITY_ICON_SIZE);

        // Theme icon in the bottom-right corner, cropped like the
        // opacity icon: light theme on the left, dark theme on the right
        DrawPngResource(
            graphics,
            ASSET_THEME,
            lightTheme ? 0 : THEME_ICON_SIZE,
            0,
            THEME_ICON_SIZE,
            THEME_ICON_SIZE,
            width - THEME_ICON_MARGIN - THEME_ICON_SIZE,
            height - THEME_ICON_MARGIN - THEME_ICON_SIZE,
            THEME_ICON_SIZE,
            THEME_ICON_SIZE);

        // Register the theme icon as a clickable region
        ActionAreas.push_back(CreateArea(
            L"button",
            L"theme",
            width - THEME_ICON_MARGIN - THEME_ICON_SIZE,
            height - THEME_ICON_MARGIN - THEME_ICON_SIZE,
            THEME_ICON_SIZE,
            THEME_ICON_SIZE));

        // Trash / delete zone: 20×20, 40px from the right edge and 10px
        // from the bottom edge. The icon is drawn bottom-right aligned
        // inside this area, so it matches with or without the extra 1px
        // of the "dragging" image.
        trashArea = CreateArea(
            L"trash",
            L"trash",
            width - 40 - 20,
            height - 10 - 20,
            20,
            20);

        // While an item is being dragged, highlight the trash zone with
        // the same warning colour the back button flashes (rounded 6),
        // expanded 4px on every side.
        if (isItemDragging)
        {
            const Gdiplus::REAL diameter = 12.0f;
            const Gdiplus::RectF plate(
                static_cast<Gdiplus::REAL>(
                    trashArea.x - TRASH_PLATE_EXPAND),
                static_cast<Gdiplus::REAL>(
                    trashArea.y - TRASH_PLATE_EXPAND),
                static_cast<Gdiplus::REAL>(
                    trashArea.width + TRASH_PLATE_EXPAND * 2),
                static_cast<Gdiplus::REAL>(
                    trashArea.height + TRASH_PLATE_EXPAND * 2));

            Gdiplus::GraphicsPath platePath;
            platePath.AddArc(plate.X, plate.Y,
                diameter, diameter, 180.0f, 90.0f);
            platePath.AddArc(plate.GetRight() - diameter, plate.Y,
                diameter, diameter, 270.0f, 90.0f);
            platePath.AddArc(plate.GetRight() - diameter,
                plate.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
            platePath.AddArc(plate.X, plate.GetBottom() - diameter,
                diameter, diameter, 90.0f, 90.0f);
            platePath.CloseFigure();

            Gdiplus::SolidBrush plateBrush(
                Gdiplus::Color(255, 252, 195, 31));
            graphics.FillPath(&plateBrush, &platePath);
        }

        DrawTrash(
            graphics,
            Gdiplus::Rect(trashArea.x, trashArea.y,
                trashArea.width, trashArea.height),
            isItemDragging,
            !lightTheme);

        // Drop icon in the bottom-left corner (left=35, bottom margin=25)
        // Source row selection: opacity==1 uses row 2 (y=10), otherwise
        // row 1 for dark theme (y=5) or row 0 for light theme (y=0)
        int dropSourceY;
        if (g_config.opacity >= 1.0)
        {
            dropSourceY = 10;
        }
        else if (_wcsicmp(g_config.theme, L"dark") == 0)
        {
            dropSourceY = 5;
        }
        else
        {
            dropSourceY = 0;
        }

        DrawPngResource(
            graphics,
            ASSET_DROP,
            0,
            dropSourceY,
            10,
            5,
            35,
            height - 25 - 5,
            10,
            5);

        ActionAreas.push_back(CreateArea(
            L"button",
            L"opacity_up",
            35,
            height - 25 - 5,
            10,
            5));

        // Drop-down icon (180° rotated) in the bottom-left corner,
        // left=35, bottom margin=10. Same source-row logic as above:
        // opacity at minimum (0.1) uses row 2, otherwise row 1 (dark)
        // or row 0 (light). The drawn image is rotated 180° about the
        // center of its destination rect via a graphics transform.
        int dropDownSourceY;
        if (g_config.opacity <= 0.1)
        {
            dropDownSourceY = 10;
        }
        else if (_wcsicmp(g_config.theme, L"dark") == 0)
        {
            dropDownSourceY = 5;
        }
        else
        {
            dropDownSourceY = 0;
        }

        const int downDestX = 35;
        const int downDestY = height - 10 - 5;
        const Gdiplus::REAL downCenterX =
            static_cast<Gdiplus::REAL>(downDestX + 5);
        const Gdiplus::REAL downCenterY =
            static_cast<Gdiplus::REAL>(downDestY + 2);

        const Gdiplus::GraphicsState state = graphics.Save();
        graphics.TranslateTransform(downCenterX, downCenterY);
        graphics.ScaleTransform(-1.0f, -1.0f);
        graphics.TranslateTransform(-downCenterX, -downCenterY);

        DrawPngResource(
            graphics,
            ASSET_DROP,
            0,
            dropDownSourceY,
            10,
            5,
            downDestX,
            downDestY,
            10,
            5);

        graphics.Restore(state);

        ActionAreas.push_back(CreateArea(
            L"button",
            L"opacity_down",
            downDestX,
            downDestY,
            10,
            5));

        // Temporary test: draw the view area
        PaintViewArea(
            graphics,
            width,
            height,
            viewArea,
            Gdiplus::Color(25, 0, 0, 0));

        // Keep the previous level visible and outline the reveal rect
        // until the transition timer completes.
        DrawViewAnimationOverlay(graphics, width, height);
    }
}

// Draw the ghost cell that follows the cursor while an item is dragged.
// The cell is first drawn at full strength, then blended back against a
// snapshot of what was underneath so the ghost itself reads as 60%
// opaque without punching a translucent hole in the items below it.
void DrawDragGhost()
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    if (!GetDragGhostRect(x, y, w, h))
        return;

    if (g_dragItemIndex >= static_cast<int>(ViewItems.size()))
        return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;

    if (x1 > g_frameWidth)  x1 = g_frameWidth;
    if (y1 > g_frameHeight) y1 = g_frameHeight;
    if (x0 >= x1 || y0 >= y1)
        return;

    const int spanW = x1 - x0;
    const int spanH = y1 - y0;

    unsigned char* pixels = static_cast<unsigned char*>(g_frameBits);

    // Snapshot the background under the ghost
    std::vector<unsigned char> backdrop(
        static_cast<size_t>(spanW) * spanH * 4);

    for (int py = 0; py < spanH; ++py)
    {
        memcpy(backdrop.data() + static_cast<size_t>(py) * spanW * 4,
            pixels + (static_cast<size_t>(y0 + py) * g_frameWidth + x0) * 4,
            static_cast<size_t>(spanW) * 4);
    }

    // Draw the cell at full strength
    {
        Gdiplus::Graphics ghostGraphics(g_frameDC);
        ghostGraphics.SetCompositingMode(
            Gdiplus::CompositingModeSourceOver);

        // Hovering the trash: the ghost gets a red plate behind it
        if (g_dragOverTrash)
        {
            const Gdiplus::REAL diameter = 12.0f;
            const Gdiplus::RectF plate(
                static_cast<Gdiplus::REAL>(x),
                static_cast<Gdiplus::REAL>(y),
                static_cast<Gdiplus::REAL>(w),
                static_cast<Gdiplus::REAL>(h));

            Gdiplus::GraphicsPath platePath;
            platePath.AddArc(plate.X, plate.Y,
                diameter, diameter, 180.0f, 90.0f);
            platePath.AddArc(plate.GetRight() - diameter, plate.Y,
                diameter, diameter, 270.0f, 90.0f);
            platePath.AddArc(plate.GetRight() - diameter,
                plate.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
            platePath.AddArc(plate.X, plate.GetBottom() - diameter,
                diameter, diameter, 90.0f, 90.0f);
            platePath.CloseFigure();

            Gdiplus::SolidBrush plateBrush(
                Gdiplus::Color(255, 228, 63, 50));
            ghostGraphics.FillPath(&plateBrush, &platePath);
        }

        DrawItem(
            ghostGraphics,
            ViewItems[g_dragItemIndex],
            x,
            static_cast<float>(y),
            w,
            h,
            _wcsicmp(g_config.theme, L"dark") == 0,
            false);

        ghostGraphics.Flush();
    }

    // Blend: ghost × 60% over the snapshot × 40% (both premultiplied)
    const int keep = 255 - GHOST_ALPHA;

    for (int py = 0; py < spanH; ++py)
    {
        unsigned char* dst = pixels +
            (static_cast<size_t>(y0 + py) * g_frameWidth + x0) * 4;
        const unsigned char* bg =
            backdrop.data() + static_cast<size_t>(py) * spanW * 4;

        for (int px = 0; px < spanW; ++px)
        {
            dst[0] = static_cast<unsigned char>(
                (dst[0] * GHOST_ALPHA + bg[0] * keep) / 255);
            dst[1] = static_cast<unsigned char>(
                (dst[1] * GHOST_ALPHA + bg[1] * keep) / 255);
            dst[2] = static_cast<unsigned char>(
                (dst[2] * GHOST_ALPHA + bg[2] * keep) / 255);
            dst[3] = static_cast<unsigned char>(
                (dst[3] * GHOST_ALPHA + bg[3] * keep) / 255);

            dst += 4;
            bg += 4;
        }
    }
}

void RenderFrame(HWND hwnd)
{
    if (!hwnd)
        return;

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;

    if (width <= 0 || height <= 0)
        return;

    if (!EnsureFrameBuffer(width, height))
        return;

    // Render the destination level once, up front. Subsequent animation
    // frames reuse this snapshot and only update the reveal geometry.
    if (g_viewAnimActive &&
        (g_viewAnimNewFrame.empty() ||
         g_viewAnimNewFrameWidth != width ||
         g_viewAnimNewFrameHeight != height))
    {
        g_viewAnimActive = false;
        memset(g_frameBits, 0,
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        DrawFrameContent(width, height);
        CaptureFrameBits(g_viewAnimNewFrame,
            g_viewAnimNewFrameWidth, g_viewAnimNewFrameHeight);
        g_viewAnimActive = true;
        // Do not count the one-time snapshot rendering as visible animation
        // time; the full duration starts with the first composited frame.
        g_viewAnimStartTime = QuerySecondsNow();
    }

    memset(g_frameBits, 0,
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    DrawFrameContent(width, height);

    // Ghost cell on top of everything, blended at 60% against the pixels
    // that were underneath it.
    DrawDragGhost();

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    POINT source = {0, 0};
    POINT destination = {windowRect.left, windowRect.top};
    SIZE size = {width, height};

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha =
        static_cast<BYTE>(g_config.opacity * 255.0);
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        hwnd,
        nullptr,
        &destination,
        &size,
        g_frameDC,
        &source,
        0,
        &blend,
        ULW_ALPHA);
}

// ========================================
// Window helpers
// ========================================

constexpr int RESIZE_BORDER = 8;

void CenterWindowOnMonitor(HWND hwnd)
{
    RECT rect;
    GetWindowRect(hwnd, &rect);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;

    const RECT& work = monitorInfo.rcWork;

    SetWindowPos(
        hwnd,
        nullptr,
        work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2,
        width,
        height,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT HitTestSizingBorder(HWND hwnd, LPARAM lParam)
{
    const POINT pt = {
        static_cast<short>(LOWORD(lParam)),
        static_cast<short>(HIWORD(lParam))};

    RECT rect;
    GetWindowRect(hwnd, &rect);

    const bool left = pt.x < rect.left + RESIZE_BORDER;
    const bool right = pt.x >= rect.right - RESIZE_BORDER;
    const bool top = pt.y < rect.top + RESIZE_BORDER;
    const bool bottom = pt.y >= rect.bottom - RESIZE_BORDER;

    if (top && left)
        return HTTOPLEFT;

    if (top && right)
        return HTTOPRIGHT;

    if (bottom && left)
        return HTBOTTOMLEFT;

    if (bottom && right)
        return HTBOTTOMRIGHT;

    if (left)
        return HTLEFT;

    if (right)
        return HTRIGHT;

    if (top)
        return HTTOP;

    if (bottom)
        return HTBOTTOM;

    return HTCLIENT;
}

// ========================================
// In-place folder rename editor
// ========================================
//
// Double-clicking the hierarchy strip replaces that row with a real EDIT
// control until the rename ends. The control lives in a borderless popup
// owned by the launcher: a layered window presented through
// UpdateLayeredWindow never composites child windows, so an EDIT placed
// directly inside g_hwnd would stay invisible.
//
// The popup is WS_EX_NOACTIVATE — focus is tracked per thread, so the EDIT
// still takes the keyboard focus while the launcher stays the active window.

constexpr wchar_t RENAME_POPUP_CLASS[] = L"GarcenLauncherRenameBox";
constexpr int RENAME_STRIP_X = 35;
constexpr int RENAME_STRIP_Y = 10;
constexpr int RENAME_STRIP_HEIGHT = 20;

HWND g_renamePopup = nullptr;
HWND g_renameEdit = nullptr;
WNDPROC g_renameEditSubclass = nullptr;
HFONT g_renameFont = nullptr;
HBRUSH g_renameBrush = nullptr;
COLORREF g_renameBackColor = 0;
COLORREF g_renameTextColor = 0;

// True while the editor is being torn down, so the EDIT's own
// WM_KILLFOCUS does not start a second commit.
bool g_renameClosing = false;

// Blend a toward b by t (0..1), channel-wise.
static COLORREF BlendColor(COLORREF a, COLORREF b, float t)
{
    const auto mix = [t](int from, int to) {
        return static_cast<BYTE>(from + (to - from) * t);
    };

    return RGB(
        mix(GetRValue(a), GetRValue(b)),
        mix(GetGValue(a), GetGValue(b)),
        mix(GetBValue(a), GetBValue(b)));
}

// The folder whose name the strip shows: the node reached by walking
// Levels through g_items. Null at the root or on a stale index.
static Item* CurrentFolderItem()
{
    std::vector<Item>* owner = &g_items;
    Item* folder = nullptr;

    for (int level : Levels)
    {
        if (level < 0 || level >= static_cast<int>(owner->size()))
            return nullptr;

        folder = &(*owner)[level];
        owner = &folder->children;
    }

    return folder;
}

// Container holding CurrentFolderItem()'s node, used for the sibling scan.
static std::vector<Item>* CurrentFolderSiblings()
{
    if (Levels.empty())
        return nullptr;

    std::vector<Item>* owner = &g_items;

    for (size_t i = 0; i + 1 < Levels.size(); ++i)
    {
        const int level = Levels[i];

        if (level < 0 || level >= static_cast<int>(owner->size()))
            return nullptr;

        owner = &(*owner)[level].children;
    }

    return owner;
}

// Items of the level currently on screen (the container ViewItems mirrors).
// Defined with the drag & drop helpers further down.
static std::vector<Item>* CurrentLevelItems();

static void DestroyRenameEditor()
{
    g_renameActive = false;
    g_renameClosing = true;

    // Hand the EDIT back to its original procedure before it goes away so
    // the subclass never runs against a half-destroyed control.
    if (g_renameEdit && IsWindow(g_renameEdit) && g_renameEditSubclass)
    {
        SetWindowLongPtrW(g_renameEdit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_renameEditSubclass));
    }

    g_renameEdit = nullptr;
    g_renameEditSubclass = nullptr;

    // Destroying the popup also destroys its EDIT child.
    if (g_renamePopup)
    {
        DestroyWindow(g_renamePopup);
        g_renamePopup = nullptr;
    }

    if (g_renameFont)
    {
        DeleteObject(g_renameFont);
        g_renameFont = nullptr;
    }

    if (g_renameBrush)
    {
        DeleteObject(g_renameBrush);
        g_renameBrush = nullptr;
    }

    g_renameClosing = false;
    g_renameTarget = RenameTarget::Strip;
    g_renameItemIndex = -1;
}

static void EndFolderRename(HWND hwnd, bool apply);

// Where the editor currently belongs, in client coordinates. The strip is
// fixed; an item label follows the same grid geometry DrawItem uses
// (96x84 cells, name drawn at cellY+50).
static bool RenameEditorRect(
    int* outX,
    int* outY,
    int* outWidth,
    int* outHeight)
{
    if (!g_hwnd)
        return false;

    RECT client = {};
    GetClientRect(g_hwnd, &client);

    if (g_renameTarget == RenameTarget::Strip)
    {
        *outX = RENAME_STRIP_X;
        *outY = RENAME_STRIP_Y;
        *outWidth = client.right - RENAME_STRIP_X * 2;
        *outHeight = RENAME_STRIP_HEIGHT;
        return *outWidth >= 40;
    }

    const int index = g_renameItemIndex;

    if (index < 0 || index >= static_cast<int>(ViewItems.size()))
        return false;

    const int availableWidth = client.right - viewArea.left - viewArea.right;
    const int columns = availableWidth / ItemSize::width;

    if (columns <= 0)
        return false;

    const int col = index % columns;
    const int row = index / columns;

    *outX = viewArea.left + col * ItemSize::width + 4;
    *outY = viewArea.top + row * ItemSize::height
        - static_cast<int>(g_scrollOffsetY) + 50;
    *outWidth = ItemSize::width - 8;
    *outHeight = 20;
    return true;
}

// Keep the editor glued to its target when the launcher is resized or moved.
static void LayoutRenameEditor()
{
    if (!g_renamePopup || !g_hwnd)
        return;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    if (!RenameEditorRect(&x, &y, &width, &height))
        return;

    POINT origin = {x, y};
    ClientToScreen(g_hwnd, &origin);

    SetWindowPos(g_renamePopup, nullptr, origin.x, origin.y,
        width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_renameEdit)
        MoveWindow(g_renameEdit, 0, 0, width, height, TRUE);
}

// Single-line EDIT controls have no built-in Ctrl+A, so select everything
// explicitly. Ctrl+C / V / X / Z already work through the control.
static bool SelectAllOnCtrlA(HWND hwnd, UINT message, WPARAM wParam)
{
    if (message != WM_KEYDOWN || wParam != 'A')
        return false;

    if ((GetKeyState(VK_CONTROL) & 0x8000) == 0)
        return false;

    SendMessageW(hwnd, EM_SETSEL, 0, static_cast<LPARAM>(-1));
    return true;
}

// Enter commits, Esc discards, and focus leaving the editor (click on
// another app, tray menu, ...) commits — the same rule the old custom
// editor had when the launcher lost focus.
static LRESULT CALLBACK RenameEditProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN)
        {
            EndFolderRename(g_hwnd, true);
            return 0;
        }

        if (wParam == VK_ESCAPE)
        {
            EndFolderRename(g_hwnd, false);
            return 0;
        }

        if (SelectAllOnCtrlA(hwnd, message, wParam))
            return 0;
    }
    else if (message == WM_LBUTTONDOWN)
    {
        // The popup never activates, so make sure a click in the text
        // (re)claims the thread's focus before the EDIT positions the caret.
        SetFocus(hwnd);
    }
    else if (message == WM_KILLFOCUS && !g_renameClosing)
    {
        EndFolderRename(g_hwnd, true);

        // Switching to another application used to hide the launcher via
        // its own WM_KILLFOCUS; now the EDIT holds the focus, so mirror it.
        if (g_hwnd && IsWindowVisible(g_hwnd) &&
            GetForegroundWindow() != g_hwnd)
        {
            ShowWindow(g_hwnd, SW_HIDE);
        }

        return 0;
    }

    return CallWindowProcW(g_renameEditSubclass, hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK RenamePopupProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        // Clicking into the editor must not activate the popup: the
        // launcher stays the foreground window and keeps its opacity.
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        if (g_renameBrush)
        {
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, g_renameBrush);
        }
        return 1;

    case WM_CTLCOLOREDIT:
        {
            // Default EDIT chrome stripped: flat theme background and text.
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, g_renameBackColor);
            SetTextColor(dc, g_renameTextColor);
            return reinterpret_cast<LRESULT>(g_renameBrush);
        }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Replace the strip (or an item's label) with an EDIT control holding the
// current name, caret at the end.
static void BeginRename(HWND hwnd, RenameTarget target, int itemIndex)
{
    if (g_renameActive)
        return;

    std::wstring currentName;

    if (target == RenameTarget::Strip)
    {
        Item* folder = CurrentFolderItem();

        if (!folder)
            return;

        currentName = folder->name;
    }
    else
    {
        if (itemIndex < 0 || itemIndex >= static_cast<int>(ViewItems.size()))
            return;

        currentName = ViewItems[itemIndex].name;
    }

    const bool darkTheme = _wcsicmp(g_config.theme, L"dark") == 0;

    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen)
        ReleaseDC(nullptr, screen);

    // Match the text being replaced: the strip label is 13pt bold, item
    // labels are 10pt regular.
    const int pointSize = target == RenameTarget::Strip ? 13 : 10;
    const int weight = target == RenameTarget::Strip ? FW_BOLD : FW_NORMAL;

    g_renameFont = CreateFontW(
        -MulDiv(pointSize, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");

    // Pre-blend the faint edit tint the custom editor used (16% white on
    // dark, 16% #333 on light) because the EDIT paints a solid background.
    // Items sit on the view area, which the painter tints with 10% black
    // (PaintViewArea), so their base colour has to match that composite.
    const COLORREF tint = darkTheme
        ? RGB(255, 255, 255)
        : RGB(0x33, 0x33, 0x33);

    const COLORREF base = target == RenameTarget::Item
        ? BlendColor(g_backgroundColor, RGB(0, 0, 0), 25.0f / 255.0f)
        : g_backgroundColor;

    g_renameBackColor = BlendColor(base, tint, 0.16f);
    g_renameTextColor = darkTheme ? RGB(255, 255, 255) : RGB(0x33, 0x33, 0x33);
    g_renameBrush = CreateSolidBrush(g_renameBackColor);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RenamePopupProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32513)); // IDC_IBEAM
    wc.lpszClassName = RENAME_POPUP_CLASS;
    RegisterClassExW(&wc); // no-op once registered

    g_renamePopup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        RENAME_POPUP_CLASS,
        L"",
        WS_POPUP,
        0, 0, 0, 0,
        hwnd, // owned by the launcher, so it always stays above it
        nullptr,
        wc.hInstance,
        nullptr);

    if (!g_renamePopup)
    {
        DestroyRenameEditor();
        return;
    }

    // Match the launcher's configured opacity; LWA_ALPHA blends the EDIT
    // child along with the popup background. Never fully transparent.
    BYTE alpha = static_cast<BYTE>(g_config.opacity * 255.0);
    if (alpha == 0)
        alpha = 1;

    SetLayeredWindowAttributes(g_renamePopup, 0, alpha, LWA_ALPHA);

    g_renameEdit = CreateWindowExW(
        0,
        L"EDIT",
        currentName.c_str(),
        WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        g_renamePopup,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!g_renameEdit)
    {
        DestroyRenameEditor();
        return;
    }

    SendMessageW(g_renameEdit, WM_SETFONT,
        reinterpret_cast<WPARAM>(g_renameFont), TRUE);

    // Caret at the end, no selection — same starting point as before.
    SendMessageW(g_renameEdit, EM_SETSEL, static_cast<WPARAM>(-1),
        static_cast<LPARAM>(-1));

    g_renameEditSubclass = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_renameEdit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(RenameEditProc)));

    g_renameTarget = target;
    g_renameItemIndex = target == RenameTarget::Item ? itemIndex : -1;
    g_renameActive = true;

    LayoutRenameEditor();
    ShowWindow(g_renamePopup, SW_SHOWNOACTIVATE);

    // Focus is per thread, so this works without activating the popup.
    SetFocus(g_renameEdit);
}

// Commit (apply=true) or discard the rename and tear the editor down.
static void EndFolderRename(HWND hwnd, bool apply)
{
    if (!g_renameActive)
        return;

    if (apply && g_renameEdit)
    {
        const int length = GetWindowTextLengthW(g_renameEdit);

        std::wstring name;

        if (length > 0)
        {
            name.resize(static_cast<size_t>(length) + 1);
            GetWindowTextW(g_renameEdit, &name[0], length + 1);
            name.resize(static_cast<size_t>(length));
        }

        bool printable = false;
        for (wchar_t ch : name)
        {
            if (ch != L' ' && ch != L'\t')
            {
                printable = true;
                break;
            }
        }

        if (printable && g_renameTarget == RenameTarget::Strip)
        {
            Item* folder = CurrentFolderItem();
            std::vector<Item>* siblings = CurrentFolderSiblings();

            if (folder && siblings)
            {
                // Duplicate check: sibling folders only, case-insensitive,
                // itself excluded — the name would be ambiguous otherwise.
                bool duplicate = false;

                for (const Item& sibling : *siblings)
                {
                    if (&sibling == folder) continue;
                    if (sibling.children.empty()) continue;

                    if (_wcsicmp(sibling.name.c_str(), name.c_str()) == 0)
                    {
                        duplicate = true;
                        break;
                    }
                }

                if (duplicate)
                {
                    // Keep the old name and blink the back button twice.
                    g_backFlashVisible = true;
                    g_backFlashTogglesLeft = 3; // on → off → on → off
                    SetTimer(hwnd, BACK_FLASH_TIMER_ID, BACK_FLASH_MS,
                        nullptr);
                }
                else if (folder->name != name)
                {
                    folder->name = name;
                    UpdateList(g_items);
                }
            }
        }
        else if (printable && g_renameTarget == RenameTarget::Item)
        {
            std::vector<Item>* level = CurrentLevelItems();

            const int index = g_renameItemIndex;

            if (level && index >= 0 &&
                index < static_cast<int>(level->size()))
            {
                Item& item = (*level)[index];

                bool duplicate = false;

                // Group labels double as their identity, so two groups on
                // the same level may not share a name. Leaf labels are
                // free-form (identity is the path).
                if (!item.children.empty())
                {
                    for (int i = 0; i < static_cast<int>(level->size()); ++i)
                    {
                        if (i == index) continue;
                        if ((*level)[i].children.empty()) continue;

                        if (_wcsicmp((*level)[i].name.c_str(),
                                     name.c_str()) == 0)
                        {
                            duplicate = true;
                            break;
                        }
                    }
                }

                if (!duplicate && item.name != name)
                {
                    item.name = name;

                    // ViewItems is the painted copy of this level.
                    if (index < static_cast<int>(ViewItems.size()))
                        ViewItems[index].name = name;

                    UpdateList(g_items);
                }
            }
        }
    }

    DestroyRenameEditor();

    if (IsWindow(hwnd))
    {
        SetFocus(hwnd);
        RenderFrame(hwnd);
    }
}

// ========================================
// Item context menu
// ========================================

// Two short bilingual rows; groups get rename only.
static void ShowItemContextMenu(HWND hwnd, int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= static_cast<int>(ViewItems.size()))
        return;

    static const wchar_t* const leafLabels[] = {
        L"重命名 / Rename",
        L"位置 / Location",
    };
    static const UINT leafIds[] = {IDM_ITEM_RENAME, IDM_ITEM_LOCATION};

    static const wchar_t* const groupLabels[] = {
        L"重命名 / Rename",
    };
    static const UINT groupIds[] = {IDM_ITEM_RENAME};

    POINT cursor = {};
    GetCursorPos(&cursor);

    if (ViewItems[itemIndex].children.empty())
    {
        ShowContextMenu(hwnd, g_config, leafLabels, leafIds, 2,
            cursor.x, cursor.y);
    }
    else
    {
        ShowContextMenu(hwnd, g_config, groupLabels, groupIds, 1,
            cursor.x, cursor.y);
    }
}

// Both commands re-check the Levels stack captured when the menu opened,
// so a navigation in between cannot touch the wrong entry.
static void RenameContextItem(HWND hwnd)
{
    if (g_contextLevels != Levels)
        return;

    BeginRename(hwnd, RenameTarget::Item, g_contextItemIndex);
}

// "位置": reveal the item in Explorer, like its "Open file location".
static void RevealContextItem()
{
    if (g_contextLevels != Levels)
        return;

    const int index = g_contextItemIndex;

    if (index < 0 || index >= static_cast<int>(ViewItems.size()))
        return;

    const std::wstring& path = ViewItems[index].path;

    if (path.empty())
        return;

    // Web links have no filesystem location to reveal.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
        nullptr, SW_SHOWNORMAL);
}

// ========================================
// Toggle launcher visibility
// ========================================

void ToggleWindow()
{
    if (IsWindowVisible(g_hwnd))
    {
        // Nothing floating above the launcher should outlive it.
        HidePopupMenu();

        // Keep the name typed so far instead of dropping it silently.
        if (g_renameActive)
            EndFolderRename(g_hwnd, true);

        ShowWindow(g_hwnd, SW_HIDE);
    }
    else
    {
        // Update the layered window before showing it, so the first visible
        // frame already holds the composed content
        RenderFrame(g_hwnd);
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
}

// Surface the launcher without ever hiding it. Used when a second launch
// asks the running instance to come forward; unlike ToggleWindow this never
// closes a window that is already visible.
static void ShowWindowFront()
{
    if (!IsWindow(g_hwnd))
        return;

    HidePopupMenu();

    // Compose before showing so the first visible frame is complete.
    RenderFrame(g_hwnd);

    if (IsIconic(g_hwnd))
        ShowWindow(g_hwnd, SW_RESTORE);
    else
        ShowWindow(g_hwnd, SW_SHOW);

    SetForegroundWindow(g_hwnd);
}

// ========================================
// Temporary theme toggle for testing
// ========================================

void ToggleTheme(HWND hwnd)
{
    const bool lightTheme =
        _wcsicmp(g_config.theme, L"light") == 0;

    wcscpy_s(g_config.theme, lightTheme ? L"dark" : L"light");

    g_backgroundColor = GetThemeBackgroundColor(g_config.theme);

    UpdateTheme(g_config);

    RenderFrame(hwnd);
}

// ========================================
// Drag-and-drop helpers
// ========================================

// Case-insensitive search among the direct items of one level. Children
// are deliberately not searched: the same path may exist in other levels.
static int FindPathInLevel(
    const std::vector<Item>& items,
    const std::wstring& path)
{
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (_wcsicmp(items[i].path.c_str(), path.c_str()) == 0)
            return static_cast<int>(i);
    }

    return -1;
}

// Centre a duplicate item in the viewArea and flash its background twice.
static void RevealDuplicateItem(int index)
{
    if (index < 0 || index >= static_cast<int>(ViewItems.size()) || !g_hwnd)
        return;

    const std::vector<ActionArea> areas = GetItemsAreas(
        ViewItems, viewArea, g_frameWidth);

    if (index >= static_cast<int>(areas.size()))
        return;

    const int vaTop = viewArea.top;
    const int vaBottom = g_frameHeight - viewArea.bottom;

    int lastBottom = 0;
    for (const ActionArea& area : areas)
    {
        if (area.y + area.height > lastBottom)
            lastBottom = area.y + area.height;
    }

    int maxOffset = lastBottom - vaBottom;
    if (maxOffset < 0)
        maxOffset = 0;

    const ActionArea& target = areas[index];
    int offset = target.y - (vaBottom - vaTop - target.height) / 2;

    if (offset < 0) offset = 0;
    if (offset > maxOffset) offset = maxOffset;

    g_targetScrollOffsetY = offset;

    // Initial state is ON; three toggles produce on → off → on → off.
    g_itemFlashPath = ViewItems[index].path;
    g_itemFlashVisible = true;
    g_itemFlashTogglesLeft = 3;
    SetTimer(g_hwnd, ITEM_FLASH_TIMER_ID, ITEM_FLASH_MS, nullptr);
}

// Extract a display name (file / folder name) from a full path
static std::wstring GetNameFromPath(const std::wstring& path)
{
    std::wstring clean = path;

    // Strip trailing separators (common for folder paths)
    while (!clean.empty() &&
        (clean.back() == L'\\' || clean.back() == L'/'))
    {
        clean.pop_back();
    }

    const size_t slash = clean.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        return clean.substr(slash + 1);

    return clean;
}

// Insert one dragged path at the given index in the current level (ViewItems
// and the matching position in g_items), unless an item with the same path
// already exists. Returns true when the item was inserted.
static bool AddDroppedPath(
    const std::wstring& path,
    size_t index,
    const wchar_t* displayName = nullptr)
{
    const int existingIndex = FindPathInLevel(ViewItems, path);

    if (existingIndex >= 0)
    {
        RevealDuplicateItem(existingIndex);
        return false;
    }

    Item newItem;
    newItem.name = displayName && *displayName
        ? displayName
        : GetNameFromPath(path);
    newItem.path = path;

    // Clamp index to [0, ViewItems.size()]
    if (index > ViewItems.size())
        index = ViewItems.size();

    ViewItems.insert(
        ViewItems.begin() + static_cast<ptrdiff_t>(index), newItem);

    // Mirror the insert into g_items at the correct level.
    // Walk g_items along the Levels path to reach the same container
    // that ViewItems is viewing right now.
    auto* gLevel = &g_items;

    for (int idx : Levels)
    {
        if (idx < 0 || idx >= static_cast<int>(gLevel->size()))
            break;
        gLevel = &(*gLevel)[idx].children;
    }

    size_t gIndex = index;

    if (gIndex > gLevel->size())
        gIndex = gLevel->size();

    gLevel->insert(
        gLevel->begin() + static_cast<ptrdiff_t>(gIndex),
        std::move(newItem));

    UpdateList(g_items);
    return true;
}

// Append one dragged path to the end of a folder item's children (both
// ViewItems[folderIndex].children and the corresponding entry in g_items).
// Returns true when the item was added.
static bool AddDroppedPathToFolder(
    const std::wstring& path,
    size_t folderIndex,
    const wchar_t* displayName = nullptr)
{
    if (folderIndex >= ViewItems.size())
        return false;

    if (ViewItems[folderIndex].children.empty())
        return false;

    if (FindPathInLevel(ViewItems[folderIndex].children, path) >= 0)
        return false;

    Item newItem;
    newItem.name = displayName && *displayName
        ? displayName
        : GetNameFromPath(path);
    newItem.path = path;

    ViewItems[folderIndex].children.push_back(newItem);

    // Mirror into g_items — walk along Levels to reach the current
    // level's vector, then use folderIndex on that same container.
    auto* gLevel = &g_items;

    for (int idx : Levels)
    {
        if (idx < 0 || idx >= static_cast<int>(gLevel->size()))
            break;
        gLevel = &(*gLevel)[idx].children;
    }

    if (folderIndex < gLevel->size())
        (*gLevel)[folderIndex].children.push_back(std::move(newItem));

    UpdateList(g_items);
    return true;
}

static std::wstring NextBoxGroupName(const std::vector<Item>& level);

// Merge an externally dropped path with an existing leaf. The new group
// takes the leaf's slot and contains the leaf first, dropped item second.
static bool AddDroppedPathMerging(
    const std::wstring& path,
    size_t targetIndex,
    const wchar_t* displayName = nullptr)
{
    const int existingIndex = FindPathInLevel(ViewItems, path);

    if (existingIndex >= 0)
    {
        RevealDuplicateItem(existingIndex);
        return false;
    }

    if (targetIndex >= ViewItems.size() ||
        !ViewItems[targetIndex].children.empty())
    {
        return false;
    }

    auto* gLevel = &g_items;
    for (int idx : Levels)
    {
        if (idx < 0 || idx >= static_cast<int>(gLevel->size()))
            return false;
        gLevel = &(*gLevel)[idx].children;
    }

    if (targetIndex >= gLevel->size() ||
        !(*gLevel)[targetIndex].children.empty())
    {
        return false;
    }

    Item dropped;
    dropped.name = displayName && *displayName
        ? displayName
        : GetNameFromPath(path);
    dropped.path = path;

    Item group;
    group.name = NextBoxGroupName(*gLevel);
    group.children.push_back((*gLevel)[targetIndex]);
    group.children.push_back(std::move(dropped));

    (*gLevel)[targetIndex] = std::move(group);
    ViewItems = *gLevel;
    UpdateList(g_items);
    return true;
}

// ========================================
// Drag hover (insertion indicator)
// ========================================

// Decide where the insertion indicator belongs for the given client point.
// Called from the OLE drop target while files are still being dragged.
static void UpdateInsertionIndicator(int clientX, int clientY)
{
    g_insertionVisible = false;

    // Default destination: append after every existing item
    g_insertionIndex = static_cast<int>(ViewItems.size());
    g_insertionFolderIndex = -1;
    g_insertionMergeIndex = -1;

    if (ActionAreas.empty())
        return;

    // Items only count while the cursor is inside the clipped view area
    const int vaLeft = viewArea.left;
    const int vaTop = viewArea.top;
    const int vaRight = g_frameWidth - viewArea.right;
    const int vaBottom = g_frameHeight - viewArea.bottom;

    int hovered = -1;

    if (clientX >= vaLeft && clientX < vaRight &&
        clientY >= vaTop && clientY < vaBottom)
    {
        for (size_t i = 0; i < ActionAreas.size(); ++i)
        {
            const ActionArea& area = ActionAreas[i];

            if (clientX >= area.x && clientX < area.x + area.width &&
                clientY >= area.y && clientY < area.y + area.height)
            {
                // Item areas are inserted first and in ViewItems order,
                // so the pointer index matches ViewItems 1:1
                if (area.type == L"item")
                    hovered = static_cast<int>(i);

                break;
            }
        }
    }

    if (hovered < 0)
    {
        // Not over any item: anchor the line to the last item
        int lastItem = -1;

        for (size_t i = 0; i < ActionAreas.size(); ++i)
        {
            if (ActionAreas[i].type == L"item")
                lastItem = static_cast<int>(i);
        }

        if (lastItem < 0)
            return;

        g_insertionArea = ActionAreas[lastItem];
        g_insertionSide = InsertionSide::Right;
        g_insertionVisible = true;
        return;
    }

    const ActionArea& current = ActionAreas[hovered];

    const int relX = clientX - current.x;
    const bool targetGroup =
        static_cast<size_t>(hovered) < ViewItems.size() &&
        !ViewItems[hovered].children.empty();

    // Middle third: enter an existing group, or merge with a leaf into a
    // newly created boxN group. Left/right thirds always insert around it.
    if (relX >= current.width / 3 &&
        relX < current.width / 3 * 2)
    {
        if (targetGroup)
            g_insertionFolderIndex = hovered;
        else
            g_insertionMergeIndex = hovered;

        g_insertionVisible = false;
        return;
    }

    if (relX >= current.width / 3 * 2)
    {
        // Right third: insert after this item
        g_insertionArea = current;
        g_insertionSide = InsertionSide::Right;
        g_insertionIndex = hovered + 1;
    }
    else if (hovered > 0)
    {
        // Left third: insert before this item, i.e. after the previous one
        g_insertionArea = ActionAreas[hovered - 1];
        g_insertionSide = InsertionSide::Right;
        g_insertionIndex = hovered;
    }
    else
    {
        // No previous item: anchor the line to this item's left edge
        g_insertionArea = current;
        g_insertionSide = InsertionSide::Left;
        g_insertionIndex = 0;
    }

    g_insertionVisible = true;
}

// ========================================
// URL text drops and webpage-name prompt
// ========================================

constexpr wchar_t WEB_NAME_PROMPT_CLASS[] = L"GarcenLauncherWebNamePrompt";

struct WebNamePromptState
{
    HWND edit = nullptr;
    HWND hint = nullptr;
    HFONT font = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH editBrush = nullptr;
    COLORREF textColor = RGB(255, 255, 255);
    COLORREF editBackgroundColor = RGB(0x2e, 0x33, 0x38);
    bool lightTheme = false;
    bool accepted = false;
    std::wstring name;
};

static WebNamePromptState g_webNamePrompt;
bool g_webNamePromptActive = false;

static std::wstring TrimWebText(const std::wstring& text)
{
    size_t first = 0;
    size_t last = text.size();

    while (first < last && iswspace(text[first]))
        ++first;

    while (last > first && iswspace(text[last - 1]))
        --last;

    return text.substr(first, last - first);
}

static bool IsSupportedWebUrl(const std::wstring& url)
{
    const bool http = _wcsnicmp(url.c_str(), L"http://", 7) == 0;
    const bool https = _wcsnicmp(url.c_str(), L"https://", 8) == 0;

    if ((!http && !https) || url.size() <= (https ? 8u : 7u))
        return false;

    for (wchar_t c : url)
    {
        if (c < 0x20 || iswspace(c))
            return false;
    }

    return true;
}

static std::wstring DefaultWebName(const std::wstring& url)
{
    const size_t scheme = url.find(L"://");
    const size_t begin = scheme == std::wstring::npos ? 0 : scheme + 3;
    const size_t end = url.find_first_of(L"/?#", begin);
    std::wstring name = url.substr(begin, end - begin);

    return name.empty() ? L"Webpage" : name;
}

static bool ReadOleUnicodeText(
    IDataObject* data,
    CLIPFORMAT format,
    std::wstring& text)
{
    FORMATETC etc = {
        format,
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL};

    STGMEDIUM medium = {};

    if (FAILED(data->GetData(&etc, &medium)))
        return false;

    const wchar_t* raw = static_cast<const wchar_t*>(
        GlobalLock(medium.hGlobal));

    if (!raw)
    {
        ReleaseStgMedium(&medium);
        return false;
    }

    const size_t capacity = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
    size_t length = 0;

    while (length < capacity && raw[length] != L'\0')
        ++length;

    text.assign(raw, length);
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return !text.empty();
}

static bool TryGetDroppedUrl(IDataObject* data, std::wstring& url)
{
    if (!data)
        return false;

    const CLIPFORMAT shellUrl =
        RegisterClipboardFormatW(L"UniformResourceLocatorW");
    const CLIPFORMAT mozillaUrl =
        RegisterClipboardFormatW(L"text/x-moz-url");
    const CLIPFORMAT formats[] = {
        shellUrl,
        mozillaUrl,
        CF_UNICODETEXT};

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
    {
        std::wstring text;

        if (formats[i] == 0 ||
            !ReadOleUnicodeText(data, formats[i], text))
        {
            continue;
        }

        // Firefox supplies URL on the first line and title on the second.
        if (formats[i] == mozillaUrl)
        {
            const size_t lineEnd = text.find_first_of(L"\r\n");
            if (lineEnd != std::wstring::npos)
                text.resize(lineEnd);
        }

        text = TrimWebText(text);

        if (IsSupportedWebUrl(text))
        {
            url = text;
            return true;
        }
    }

    return false;
}

static void AcceptWebNamePrompt(HWND hwnd)
{
    const int length = GetWindowTextLengthW(g_webNamePrompt.edit);
    std::wstring name(static_cast<size_t>(length), L'\0');

    if (length > 0)
        GetWindowTextW(g_webNamePrompt.edit, &name[0], length + 1);

    name = TrimWebText(name);

    if (name.empty())
    {
        SetWindowTextW(g_webNamePrompt.hint,
            L"名称不能为空，请重新输入。\n"
            L"The name cannot be empty. Please enter a name.");
        SetFocus(g_webNamePrompt.edit);
        return;
    }

    g_webNamePrompt.name = name;
    g_webNamePrompt.accepted = true;
    DestroyWindow(hwnd);
}

static void DrawWebPromptButton(const DRAWITEMSTRUCT* draw)
{
    const int width = draw->rcItem.right - draw->rcItem.left;
    const int height = draw->rcItem.bottom - draw->rcItem.top;
    const bool isOk = draw->CtlID == 1;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;

    Gdiplus::Graphics graphics(draw->hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const Gdiplus::Color parentColor = g_webNamePrompt.lightTheme
        ? Gdiplus::Color(255, 0xff, 0xff, 0xff)
        : Gdiplus::Color(255, 0x22, 0x26, 0x2a);
    Gdiplus::SolidBrush parentBrush(parentColor);
    graphics.FillRectangle(&parentBrush,
        draw->rcItem.left, draw->rcItem.top, width, height);

    Gdiplus::Color background;
    if (isOk)
    {
        background = pressed
            ? Gdiplus::Color(255, 0x25, 0x9d, 0xd8)
            : Gdiplus::Color(255, 0x2e, 0xaf, 0xf0);
    }
    else if (g_webNamePrompt.lightTheme)
    {
        background = pressed
            ? Gdiplus::Color(255, 0xe5, 0xe5, 0xe5)
            : Gdiplus::Color(255, 0xf2, 0xf2, 0xf2);
    }
    else
    {
        background = pressed
            ? Gdiplus::Color(255, 0x27, 0x2c, 0x31)
            : Gdiplus::Color(255, 0x2e, 0x33, 0x38);
    }

    Gdiplus::RectF bounds(
        static_cast<Gdiplus::REAL>(draw->rcItem.left),
        static_cast<Gdiplus::REAL>(draw->rcItem.top),
        static_cast<Gdiplus::REAL>(width - 1),
        static_cast<Gdiplus::REAL>(height - 1));

    Gdiplus::GraphicsPath capsule;
    AddRoundedRectPath(capsule,
        bounds.X, bounds.Y, bounds.Width, bounds.Height,
        height / 2.0f);
    Gdiplus::SolidBrush backgroundBrush(background);
    graphics.FillPath(&backgroundBrush, &capsule);

    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font font(&fontFamily, 10.0f,
        Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush textBrush(isOk
        ? Gdiplus::Color(255, 0xff, 0xff, 0xff)
        : g_webNamePrompt.lightTheme
            ? Gdiplus::Color(255, 0x33, 0x33, 0x33)
            : Gdiplus::Color(255, 0xff, 0xff, 0xff));

    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    const wchar_t* text = isOk ? L"确定 / OK" : L"取消 / Cancel";
    const Gdiplus::RectF textBounds(
        static_cast<Gdiplus::REAL>(draw->rcItem.left),
        static_cast<Gdiplus::REAL>(draw->rcItem.top + 2),
        static_cast<Gdiplus::REAL>(width),
        static_cast<Gdiplus::REAL>(height));
    graphics.DrawString(text, -1, &font, textBounds, &format, &textBrush);
}

static LRESULT CALLBACK WebNamePromptProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED)
        {
            if (LOWORD(wParam) == 1)
                AcceptWebNamePrompt(hwnd);
            else if (LOWORD(wParam) == 2)
                DestroyWindow(hwnd);

            return 0;
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);

        RECT client = {};
        GetClientRect(hwnd, &client);

        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // The EDIT is an inset, borderless rectangle on top of this
        // antialiased capsule, so its native painting cannot square off
        // the visible outline.
        Gdiplus::GraphicsPath capsule;
        AddRoundedRectPath(capsule,
            20.0f, 74.0f,
            static_cast<float>(client.right - client.left - 40),
            34.0f, 17.0f);

        const COLORREF editColor = g_webNamePrompt.editBackgroundColor;
        Gdiplus::SolidBrush capsuleBrush(Gdiplus::Color(
            255,
            GetRValue(editColor),
            GetGValue(editColor),
            GetBValue(editColor)));
        graphics.FillPath(&capsuleBrush, &capsule);

        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_DRAWITEM:
        if (wParam == 1 || wParam == 2)
        {
            DrawWebPromptButton(
                reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return 0;
        }
        break;

    case WM_ERASEBKGND:
    {
        RECT client = {};
        GetClientRect(hwnd, &client);
        FillRect(
            reinterpret_cast<HDC>(wParam),
            &client,
            g_webNamePrompt.backgroundBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_webNamePrompt.textColor);
        return reinterpret_cast<LRESULT>(g_webNamePrompt.backgroundBrush);
    }

    case WM_CTLCOLOREDIT:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, g_webNamePrompt.textColor);
        SetBkColor(dc, g_webNamePrompt.editBackgroundColor);
        return reinterpret_cast<LRESULT>(g_webNamePrompt.editBrush);
    }

    case WM_CTLCOLORBTN:
        SetTextColor(
            reinterpret_cast<HDC>(wParam),
            g_webNamePrompt.textColor);
        return reinterpret_cast<LRESULT>(g_webNamePrompt.backgroundBrush);

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_webNamePrompt.font)
        {
            DeleteObject(g_webNamePrompt.font);
            g_webNamePrompt.font = nullptr;
        }
        if (g_webNamePrompt.backgroundBrush)
        {
            DeleteObject(g_webNamePrompt.backgroundBrush);
            g_webNamePrompt.backgroundBrush = nullptr;
        }
        if (g_webNamePrompt.editBrush)
        {
            DeleteObject(g_webNamePrompt.editBrush);
            g_webNamePrompt.editBrush = nullptr;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Ctrl+A in the prompt's single-line EDIT (handled by SelectAllOnCtrlA).
static WNDPROC g_webNameEditSubclass = nullptr;

static LRESULT CALLBACK WebNameEditProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (SelectAllOnCtrlA(hwnd, message, wParam))
        return 0;

    return CallWindowProcW(g_webNameEditSubclass, hwnd, message, wParam, lParam);
}

// Modal only with respect to the launcher: the main window stays visible,
// and the item is not created until this window has closed with OK.
static bool ShowWebNamePrompt(
    const std::wstring& defaultName,
    std::wstring& name)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WebNamePromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = WEB_NAME_PROMPT_CLASS;
    RegisterClassExW(&wc);

    g_webNamePrompt = {};

    const bool lightTheme = _wcsicmp(g_config.theme, L"light") == 0;
    const COLORREF background = lightTheme
        ? RGB(0xff, 0xff, 0xff)
        : RGB(0x22, 0x26, 0x2a);

    g_webNamePrompt.lightTheme = lightTheme;
    g_webNamePrompt.textColor = lightTheme
        ? RGB(0x33, 0x33, 0x33)
        : RGB(0xff, 0xff, 0xff);
    g_webNamePrompt.editBackgroundColor = lightTheme
        ? RGB(0xf2, 0xf2, 0xf2)
        : RGB(0x2e, 0x33, 0x38);
    g_webNamePrompt.backgroundBrush = CreateSolidBrush(background);
    g_webNamePrompt.editBrush = CreateSolidBrush(
        g_webNamePrompt.editBackgroundColor);
    g_webNamePrompt.font = CreateFontW(
        -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");

    constexpr int promptWidth = 420;
    constexpr int promptHeight = 190;

    RECT anchor = {0, 0, GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)};

    if (g_hwnd)
        GetWindowRect(g_hwnd, &anchor);

    const int x = anchor.left +
        (anchor.right - anchor.left - promptWidth) / 2;
    const int y = anchor.top +
        (anchor.bottom - anchor.top - promptHeight) / 2;

    g_webNamePromptActive = true;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WEB_NAME_PROMPT_CLASS,
        L"GarcenLauncher Webpage Name",
        WS_POPUP,
        x, y, promptWidth, promptHeight,
        g_hwnd,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!hwnd)
    {
        g_webNamePromptActive = false;
        DeleteObject(g_webNamePrompt.backgroundBrush);
        DeleteObject(g_webNamePrompt.editBrush);
        DeleteObject(g_webNamePrompt.font);
        g_webNamePrompt = {};
        return false;
    }

    if (g_config.radius > 0)
    {
        const int diameter = g_config.radius * 2;
        // SetWindowRgn takes ownership of the region handle.
        SetWindowRgn(hwnd, CreateRoundRectRgn(
            0, 0, promptWidth + 1, promptHeight + 1,
            diameter, diameter), TRUE);
    }

    if (g_hwnd)
        EnableWindow(g_hwnd, FALSE);

    g_webNamePrompt.hint = CreateWindowExW(
        0, L"STATIC",
        L"请输入网页名称\nEnter a name for the webpage",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 16, promptWidth - 40, 48,
        hwnd, nullptr, wc.hInstance, nullptr);

    g_webNamePrompt.edit = CreateWindowExW(
        0, L"EDIT", defaultName.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        32, 80, promptWidth - 64, 22,
        hwnd, nullptr, wc.hInstance, nullptr);

    if (g_webNamePrompt.edit)
    {
        g_webNameEditSubclass = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_webNamePrompt.edit, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(WebNameEditProc)));
    }

    HWND okButton = CreateWindowExW(
        0, L"BUTTON", L"确定 / OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        174, 132, 110, 32,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)),
        wc.hInstance, nullptr);

    HWND cancelButton = CreateWindowExW(
        0, L"BUTTON", L"取消 / Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        290, 132, 110, 32,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)),
        wc.hInstance, nullptr);

    const HWND controls[] = {
        g_webNamePrompt.hint,
        g_webNamePrompt.edit,
        okButton,
        cancelButton};

    for (HWND control : controls)
    {
        if (control)
            SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(g_webNamePrompt.font), TRUE);
    }

    if (g_webNamePrompt.edit)
        SendMessageW(g_webNamePrompt.edit, EM_LIMITTEXT, 256, 0);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    if (g_webNamePrompt.edit)
    {
        SetFocus(g_webNamePrompt.edit);
        SendMessageW(g_webNamePrompt.edit, EM_SETSEL, 0, -1);
    }

    MSG message = {};

    while (IsWindow(hwnd))
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0)
            break;

        if ((message.message == WM_KEYDOWN ||
             message.message == WM_SYSKEYDOWN) &&
            GetAncestor(message.hwnd, GA_ROOT) == hwnd)
        {
            if (message.wParam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                continue;
            }

            if (message.wParam == VK_RETURN)
            {
                if (GetFocus() == cancelButton)
                    DestroyWindow(hwnd);
                else
                    AcceptWebNamePrompt(hwnd);
                continue;
            }
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (IsWindow(hwnd))
        DestroyWindow(hwnd);

    if (g_hwnd)
    {
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
    }

    g_webNamePromptActive = false;

    if (!g_webNamePrompt.accepted)
        return false;

    name = g_webNamePrompt.name;
    return true;
}

// ========================================
// OLE drop target
// ========================================

// GUIDs declared locally so no import library (uuid) symbol is needed,
// same approach ControlConfig.cpp uses for the JSON interfaces.
static const GUID LAUNCHER_IID_UNKNOWN =
    {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static const GUID LAUNCHER_IID_DROPTARGET =
    {0x00000122, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// WM_DROPFILES only arrives after the files are released, so hover
// feedback during the drag requires an OLE drop target. Drop keeps the
// existing behaviour: append every dragged path to the list.
class LauncherDropTarget final : public IDropTarget
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** ppvObject) override
    {
        if (!ppvObject)
            return E_POINTER;

        if (IsEqualGUID(riid, LAUNCHER_IID_UNKNOWN) ||
            IsEqualGUID(riid, LAUNCHER_IID_DROPTARGET))
        {
            *ppvObject = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG count = InterlockedDecrement(&m_refCount);

        if (count == 0)
            delete this;

        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject*,
        DWORD,
        POINTL point,
        DWORD* effect) override
    {
        return DragOver(0, point, effect);
    }

    HRESULT STDMETHODCALLTYPE DragOver(
        DWORD,
        POINTL point,
        DWORD* effect) override
    {
        if (effect)
            *effect = DROPEFFECT_COPY;

        if (!g_hwnd)
            return S_OK;

        POINT client = {point.x, point.y};
        ScreenToClient(g_hwnd, &client);

        UpdateInsertionIndicator(client.x, client.y);
        RenderFrame(g_hwnd);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        const bool hadTarget =
            g_insertionVisible || g_insertionFolderIndex >= 0 ||
            g_insertionMergeIndex >= 0;

        g_insertionVisible = false;
        g_insertionFolderIndex = -1;
        g_insertionMergeIndex = -1;

        if (hadTarget && g_hwnd)
            RenderFrame(g_hwnd);

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* data,
        DWORD,
        POINTL,
        DWORD* effect) override
    {
        if (effect)
            *effect = DROPEFFECT_COPY;

        g_insertionVisible = false;

        if (data)
        {
            std::wstring url;

            if (TryGetDroppedUrl(data, url))
            {
                const bool toFolder =
                    g_insertionFolderIndex >= 0 &&
                    static_cast<size_t>(g_insertionFolderIndex) < ViewItems.size();
                const size_t folderIndex = g_insertionFolderIndex < 0
                    ? 0
                    : static_cast<size_t>(g_insertionFolderIndex);
                const bool toMerge = !toFolder &&
                    g_insertionMergeIndex >= 0 &&
                    static_cast<size_t>(g_insertionMergeIndex) < ViewItems.size() &&
                    ViewItems[g_insertionMergeIndex].children.empty();
                const size_t mergeIndex = g_insertionMergeIndex < 0
                    ? 0
                    : static_cast<size_t>(g_insertionMergeIndex);
                const size_t insertIndex = g_insertionIndex < 0
                    ? ViewItems.size()
                    : static_cast<size_t>(g_insertionIndex);

                // Freeze the destination while the modal name prompt is up.
                g_insertionFolderIndex = -1;
                g_insertionMergeIndex = -1;
                g_insertionIndex = -1;

                if (g_hwnd)
                    RenderFrame(g_hwnd);

                std::wstring webName;

                if (ShowWebNamePrompt(DefaultWebName(url), webName))
                {
                    if (toFolder)
                    {
                        AddDroppedPathToFolder(
                            url, folderIndex, webName.c_str());
                    }
                    else if (toMerge)
                    {
                        AddDroppedPathMerging(
                            url, mergeIndex, webName.c_str());
                    }
                    else
                    {
                        AddDroppedPath(url, insertIndex, webName.c_str());
                    }
                }

                if (g_hwnd)
                    RenderFrame(g_hwnd);

                return S_OK;
            }

            FORMATETC format = {
                CF_HDROP,
                nullptr,
                DVASPECT_CONTENT,
                -1,
                TYMED_HGLOBAL};

            STGMEDIUM medium = {};

            if (SUCCEEDED(data->GetData(&format, &medium)))
            {
                HDROP drop = static_cast<HDROP>(
                    GlobalLock(medium.hGlobal));

                if (drop)
                {
                    const bool toFolder =
                        g_insertionFolderIndex >= 0 &&
                        static_cast<size_t>(g_insertionFolderIndex) < ViewItems.size();
                    const bool toMerge = !toFolder &&
                        g_insertionMergeIndex >= 0 &&
                        static_cast<size_t>(g_insertionMergeIndex) < ViewItems.size() &&
                        ViewItems[g_insertionMergeIndex].children.empty();
                    const size_t mergeIndex = g_insertionMergeIndex < 0
                        ? 0
                        : static_cast<size_t>(g_insertionMergeIndex);

                    size_t insertIndex = g_insertionIndex < 0
                        ? ViewItems.size()
                        : static_cast<size_t>(g_insertionIndex);

                    const UINT count =
                        DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

                    for (UINT i = 0; i < count; ++i)
                    {
                        const UINT length =
                            DragQueryFileW(drop, i, nullptr, 0);

                        if (length == 0)
                            continue;

                        std::wstring path(
                            static_cast<size_t>(length), L'\0');

                        if (DragQueryFileW(
                            drop, i, &path[0], length + 1) != 0)
                        {
                            if (toFolder)
                            {
                                AddDroppedPathToFolder(
                                    path,
                                    static_cast<size_t>(g_insertionFolderIndex));
                            }
                            else if (toMerge)
                            {
                                // The first successful path turns the leaf
                                // into a group; remaining paths join that
                                // same group in their original drag order.
                                if (mergeIndex < ViewItems.size() &&
                                    ViewItems[mergeIndex].children.empty())
                                {
                                    AddDroppedPathMerging(path, mergeIndex);
                                }
                                else
                                {
                                    AddDroppedPathToFolder(path, mergeIndex);
                                }
                            }
                            // Keep the dragged order: each file lands
                            // right after the previous one
                            else if (AddDroppedPath(path, insertIndex))
                            {
                                ++insertIndex;
                            }
                        }
                    }

                    GlobalUnlock(medium.hGlobal);
                }

                ReleaseStgMedium(&medium);
            }
        }

        if (g_hwnd)
            RenderFrame(g_hwnd);

        g_insertionFolderIndex = -1;
        g_insertionMergeIndex = -1;
        g_insertionIndex = -1;

        return S_OK;
    }

private:
    LONG m_refCount = 1;
};

static LauncherDropTarget* g_dropTarget = nullptr;

// ========================================
// Level navigation
// ========================================

// Pop one level from the navigation stack and rebuild the visible state.
// Does not repaint. Returns false when already at the root level.
static bool NavigateUp()
{
    if (HistoryItemsAreasCollect.empty() || Levels.empty())
        return false;

    StopItemFlash();

    // The snapshot we pop is the one that captured the PREVIOUS level
    // (the one we're returning TO). Read its scroll_distance BEFORE popping.
    const int savedScroll = HistoryItemsAreasCollect.back().scroll_distance;

    HistoryItemsAreasCollect.pop_back();
    Levels.pop_back();

    // Rebuild ViewItems from the root data using the remaining path
    // through the folder hierarchy. Copy the child list into a temporary
    // first: assigning ViewItems directly from ViewItems[i].children
    // would destroy the source vector (it lives inside the very container
    // being replaced) mid-copy — a use-after-free.
    ViewItems = g_items;

    for (int levelIdx : Levels)
    {
        if (levelIdx < 0 ||
            levelIdx >= static_cast<int>(ViewItems.size()))
        {
            break;
        }

        std::vector<Item> next = ViewItems[levelIdx].children;
        ViewItems.swap(next);
    }

    // Restore viewArea.top — only the root level (Levels.empty()) uses
    // the original 10px inset
    viewArea.top = Levels.empty() ? 10 : 40;

    g_targetScrollOffsetY = savedScroll;
    g_scrollOffsetY = static_cast<float>(savedScroll);
    g_hoveredAreaIndex = -1;

    return true;
}

static void GoBackLevel(HWND hwnd)
{
    if (HistoryItemsAreasCollect.empty() || Levels.empty())
        return;

    // Collapse animation runs in reverse: the current level's viewArea
    // shrinks into the centre of the cell we originally came from. That
    // cell's areas live in the snapshot of the level we are returning to.
    const ViewAnimRect collapseFrom = MakeAnimRect(
        static_cast<float>(viewArea.left),
        static_cast<float>(viewArea.top),
        static_cast<float>(g_frameWidth - viewArea.right),
        static_cast<float>(g_frameHeight - viewArea.bottom));

    ViewAnimRect collapseTo;
    bool hasCollapseTo = false;
    {
        const HistoryItemsAreas& parent = HistoryItemsAreasCollect.back();
        const int idx = Levels.back();

        if (idx >= 0 && idx < static_cast<int>(parent.areas.size()))
        {
            const ActionArea& a = parent.areas[idx];
            const float cx = a.x + a.width * 0.5f;
            const float cy = a.y + a.height * 0.5f;

            collapseTo = MakeAnimRect(cx, cy, cx, cy);
            hasCollapseTo = true;
        }
    }

    if (!NavigateUp())
        return;

    if (hasCollapseTo)
        StartViewAnimation(hwnd, collapseFrom, collapseTo, false);

    RenderFrame(hwnd);
}

// ========================================
// Item drag & drop (inside the launcher)
// ========================================

// The g_items container holding the current level's items
static std::vector<Item>* CurrentLevelItems()
{
    std::vector<Item>* level = &g_items;

    for (int idx : Levels)
    {
        if (idx < 0 || idx >= static_cast<int>(level->size()))
            return nullptr;

        level = &(*level)[idx].children;
    }

    return level;
}

// Reset every drag flag without touching the data
static void CancelItemDrag()
{
    isItemDragging = false;
    g_dragItemIndex = -1;
    g_dragHoverFolderIndex = -1;
    g_dragInsertVisible = false;
    g_dragInsertAnchor = -1;
    g_dragPreviewIndex = -1;
    g_dragOverTrash = false;
    g_dragBackArmed = false;
}

// Carry the drag up one level: the item leaves the current level and is
// appended to the parent level, where it keeps being dragged (its slot
// shows blank) so it can be reordered.
static void DragUpOneLevel(HWND hwnd);

// Re-classify the item under the cursor while dragging
static void UpdateDragTarget(HWND hwnd, int mx, int my)
{
    g_dragHoverFolderIndex = -1;
    g_dragInsertVisible = false;
    g_dragInsertAnchor = -1;
    g_dragPreviewIndex = -1;
    g_dragOverTrash = false;

    // Over the trash zone: deleting wins over every other drop target.
    // The hit test uses the same expanded rect that is drawn.
    const int trashLeft = trashArea.x - TRASH_PLATE_EXPAND;
    const int trashTop = trashArea.y - TRASH_PLATE_EXPAND;
    const int trashRight =
        trashArea.x + trashArea.width + TRASH_PLATE_EXPAND;
    const int trashBottom =
        trashArea.y + trashArea.height + TRASH_PLATE_EXPAND;

    if (mx >= trashLeft && mx < trashRight &&
        my >= trashTop && my < trashBottom)
    {
        g_dragOverTrash = true;
        return;
    }

    // Over the back button: carry the dragged item up one level and keep
    // dragging it there.
    bool overBack = false;

    if (!Levels.empty())
    {
        for (const ActionArea& a : ActionAreas)
        {
            if (a.type != L"button" || a.key != L"back")
                continue;

            if (mx >= a.x && mx < a.x + a.width &&
                my >= a.y && my < a.y + a.height)
            {
                overBack = true;
                break;
            }
        }
    }

    if (overBack)
    {
        // One level per entry; re-armed once the cursor leaves the button
        if (g_dragBackArmed)
        {
            g_dragBackArmed = false;
            DragUpOneLevel(hwnd);
        }

        return;
    }

    g_dragBackArmed = true;

    int hovered = -1;

    for (size_t i = 0; i < ViewItems.size() &&
         i < ActionAreas.size(); ++i)
    {
        if (ActionAreas[i].type != L"item")
            break;

        const ActionArea& a = ActionAreas[i];

        if (mx >= a.x && mx < a.x + a.width &&
            my >= a.y && my < a.y + a.height)
        {
            hovered = static_cast<int>(i);
            break;
        }
    }

    // Back over its own blank slot: that's the original position, so no
    // drop indicator at all (releasing there is a no-op).
    if (hovered == g_dragItemIndex)
        return;

    if (hovered < 0)
    {
        // Empty space: move to the end. The line sits on the right edge
        // of the last item.
        if (!ViewItems.empty())
        {
            g_dragInsertVisible = true;
            g_dragInsertArea = ActionAreas[ViewItems.size() - 1];
            g_dragInsertSide = InsertionSide::Right;
            g_dragInsertAnchor = static_cast<int>(ViewItems.size()) - 1;
        }

        return;
    }

    const ActionArea& area = ActionAreas[hovered];
    const int rel = mx - area.x;

    const bool draggingGroup =
        g_dragItemIndex >= 0 &&
        g_dragItemIndex < static_cast<int>(ViewItems.size()) &&
        !ViewItems[g_dragItemIndex].children.empty();
    const bool targetGroup = !ViewItems[hovered].children.empty();

    // Any item dragged onto a group uses thirds: left/right reorder
    // around it, only the middle moves into it. A group dragged onto a
    // leaf still uses halves and can only reorder.
    const int zone = draggingGroup && !targetGroup
        ? (rel < area.width / 2 ? 0 : 2)            // halves
        : (rel < area.width / 3 ? 0
            : (rel >= area.width / 3 * 2 ? 2 : 1)); // thirds

    if (zone == 0)
    {
        // Before this item
        g_dragInsertVisible = true;

        if (hovered == 0)
        {
            g_dragInsertArea = area;
            g_dragInsertSide = InsertionSide::Left;
            g_dragInsertAnchor = hovered;
        }
        else
        {
            g_dragInsertArea = ActionAreas[hovered - 1];
            g_dragInsertSide = InsertionSide::Right;
            g_dragInsertAnchor = hovered - 1;
        }
    }
    else if (zone == 2)
    {
        // After this item
        g_dragInsertVisible = true;
        g_dragInsertArea = area;
        g_dragInsertSide = InsertionSide::Right;
        g_dragInsertAnchor = hovered;
    }
    else if (targetGroup)
    {
        // Middle third of a group target — move into the group
        g_dragHoverFolderIndex = hovered;
    }
    else
    {
        // Middle third of a leaf target — merged-group preview
        g_dragPreviewIndex = hovered;
    }
}

// First free "boxN" name in the current level
static std::wstring NextBoxGroupName(const std::vector<Item>& level)
{
    int maxNumber = 0;

    for (const Item& item : level)
    {
        if (item.children.empty())
            continue;

        if (_wcsnicmp(item.name.c_str(), L"box", 3) != 0)
            continue;

        const wchar_t* digits = item.name.c_str() + 3;

        if (*digits == L'\0')
            continue;

        bool allDigits = true;
        for (const wchar_t* p = digits; *p; ++p)
        {
            if (*p < L'0' || *p > L'9')
            {
                allDigits = false;
                break;
            }
        }

        if (!allDigits)
            continue;

        const int n = _wtoi(digits);
        if (n > maxNumber)
            maxNumber = n;
    }

    return L"box" + std::to_wstring(maxNumber + 1);
}

// Copy the (just modified) current g_items level back into ViewItems
static void SyncViewItemsFromLevel(const std::vector<Item>& level)
{
    std::vector<Item> next = level;
    ViewItems.swap(next);
}

// Carry the drag up one level: the item leaves the level being closed and
// is appended to the parent level, where the drag continues so it can be
// reordered there.
static void DragUpOneLevel(HWND hwnd)
{
    if (!isItemDragging || g_dragItemIndex < 0 || Levels.empty())
        return;

    std::vector<Item>* source = CurrentLevelItems();

    if (!source || g_dragItemIndex >= static_cast<int>(source->size()))
        return;

    Item moving = (*source)[g_dragItemIndex];
    source->erase(source->begin() + g_dragItemIndex);

    if (!NavigateUp())
        return;

    std::vector<Item>* parent = CurrentLevelItems();

    if (!parent)
        return;

    parent->push_back(std::move(moving));

    SyncViewItemsFromLevel(*parent);
    UpdateList(g_items);

    // Keep dragging, now pointing at the appended item. It renders as a
    // blank slot in the parent level until the drop resolves.
    g_dragItemIndex = static_cast<int>(parent->size()) - 1;
    g_dragStart = g_dragMouse;
    isItemDragging = true;
    g_dragHoverFolderIndex = -1;
    g_dragInsertVisible = false;
    g_dragInsertAnchor = -1;
    g_dragPreviewIndex = -1;
    g_dragOverTrash = false;

    RenderFrame(hwnd);
}

// Move an item (leaf or group) into a group: append to its children
static void MoveItemIntoGroup(int dragIdx, int groupIdx)
{
    std::vector<Item>* levelPtr = CurrentLevelItems();

    if (!levelPtr)
        return;

    std::vector<Item>& level = *levelPtr;

    if (dragIdx < 0 || dragIdx >= static_cast<int>(level.size()) ||
        groupIdx < 0 || groupIdx >= static_cast<int>(level.size()) ||
        dragIdx == groupIdx)
    {
        return;
    }

    Item moving = level[dragIdx];
    level.erase(level.begin() + dragIdx);

    // Removing an earlier element shifts the group one slot to the left
    if (dragIdx < groupIdx)
        --groupIdx;

    level[groupIdx].children.push_back(std::move(moving));

    SyncViewItemsFromLevel(level);
    UpdateList(g_items);
}

// Reorder an item (leaf or group) inside the current level
static void ReorderItem(int dragIdx, int insertIndex)
{
    std::vector<Item>* levelPtr = CurrentLevelItems();

    if (!levelPtr)
        return;

    std::vector<Item>& level = *levelPtr;

    if (dragIdx < 0 || dragIdx >= static_cast<int>(level.size()))
        return;

    Item moving = level[dragIdx];
    level.erase(level.begin() + dragIdx);

    if (insertIndex > dragIdx)
        --insertIndex;

    if (insertIndex < 0)
        insertIndex = 0;
    if (insertIndex > static_cast<int>(level.size()))
        insertIndex = static_cast<int>(level.size());

    level.insert(level.begin() + insertIndex, std::move(moving));

    SyncViewItemsFromLevel(level);
    UpdateList(g_items);
}

// Merge two leaf items into a new group: target first, dragged second
static void MergeItemsIntoGroup(int targetIdx, int dragIdx)
{
    std::vector<Item>* levelPtr = CurrentLevelItems();

    if (!levelPtr)
        return;

    std::vector<Item>& level = *levelPtr;

    if (targetIdx < 0 || targetIdx >= static_cast<int>(level.size()) ||
        dragIdx < 0 || dragIdx >= static_cast<int>(level.size()) ||
        targetIdx == dragIdx)
    {
        return;
    }

    Item first = level[targetIdx];
    Item second = level[dragIdx];

    const int lo = targetIdx < dragIdx ? targetIdx : dragIdx;
    const int hi = targetIdx < dragIdx ? dragIdx : targetIdx;

    const std::wstring groupName = NextBoxGroupName(level);

    level.erase(level.begin() + hi);
    level.erase(level.begin() + lo);

    Item group;
    group.name = groupName;
    group.path.clear();
    group.children.push_back(std::move(first));
    group.children.push_back(std::move(second));

    level.insert(level.begin() + lo, std::move(group));

    SyncViewItemsFromLevel(level);
    UpdateList(g_items);
}

// Remove one item (a group takes its children with it) from the current
// level and persist the change
static void DeleteItemAt(int idx)
{
    std::vector<Item>* levelPtr = CurrentLevelItems();

    if (!levelPtr)
        return;

    std::vector<Item>& level = *levelPtr;

    if (idx < 0 || idx >= static_cast<int>(level.size()))
        return;

    level.erase(level.begin() + idx);

    SyncViewItemsFromLevel(level);
    UpdateList(g_items);
}

// Apply the drop that ends an item drag at (mx, my)
static void FinishItemDrag(HWND hwnd, int mx, int my)
{
    UpdateDragTarget(hwnd, mx, my);

    if (g_dragOverTrash)
    {
        DeleteItemAt(g_dragItemIndex);
    }
    else if (g_dragHoverFolderIndex >= 0)
    {
        MoveItemIntoGroup(g_dragItemIndex, g_dragHoverFolderIndex);
    }
    else if (g_dragInsertVisible)
    {
        // Insertion index derived from the anchor item the line sits on
        const int insertIndex =
            g_dragInsertSide == InsertionSide::Left
                ? g_dragInsertAnchor
                : g_dragInsertAnchor + 1;

        // Dropping right before or right after itself is a no-op
        if (g_dragInsertAnchor >= 0 &&
            insertIndex != g_dragItemIndex &&
            insertIndex != g_dragItemIndex + 1)
        {
            ReorderItem(g_dragItemIndex, insertIndex);
        }
    }
    else if (g_dragPreviewIndex >= 0)
    {
        MergeItemsIntoGroup(g_dragPreviewIndex, g_dragItemIndex);
    }

    CancelItemDrag();
    g_hoveredAreaIndex = -1;

    if (GetCapture() == hwnd)
        ReleaseCapture();

    RenderFrame(hwnd);
}

// Open a leaf item / enter a group — the plain click action
static void ActivateItem(HWND hwnd, int idx)
{
    if (idx < 0 || idx >= static_cast<int>(ViewItems.size()))
        return;

    if (ViewItems[idx].children.empty())
    {
        OpenItem(ViewItems[idx].path);
        return;
    }

    StopItemFlash();

    // Centre of the clicked cell — the reveal animation starts there as a
    // 0×0 rounded rect. Item areas are registered in ViewItems order, so
    // the idx-th item area is the one that was clicked.
    ViewAnimRect revealFrom;
    bool hasRevealFrom = false;
    {
        int seen = 0;

        for (const ActionArea& a : ActionAreas)
        {
            if (a.type != L"item")
                continue;

            if (seen == idx)
            {
                const float cx = a.x + a.width * 0.5f;
                const float cy = a.y + a.height * 0.5f;

                revealFrom = MakeAnimRect(cx, cy, cx, cy);
                hasRevealFrom = true;
                break;
            }

            ++seen;
        }
    }

    // Enter the folder. Remember which child of the current level was
    // opened so the path can be reconstructed when navigating back up.
    Levels.push_back(idx);

    // Snapshot this level's item areas + scroll offset for restoration
    HistoryItemsAreas history;
    history.scroll_distance = g_targetScrollOffsetY;

    for (const ActionArea& a : ActionAreas)
    {
        if (a.type == L"item")
            history.areas.push_back(a);
    }

    HistoryItemsAreasCollect.push_back(std::move(history));

    // Strip the item areas; buttons stay
    for (size_t i = ActionAreas.size(); i > 0;)
    {
        --i;

        if (ActionAreas[i].type == L"item")
            ActionAreas.erase(ActionAreas.begin() + i);
    }

    // Descend into the folder's children. Copy first: assigning
    // ViewItems from ViewItems[idx].children would free the source
    // vector mid-copy.
    std::vector<Item> next = ViewItems[idx].children;
    ViewItems.swap(next);

    // Build the new level's item areas; inserted at the front so
    // items keep their 1:1 index mapping ahead of the button areas
    std::vector<ActionArea> itemAreas = GetItemsAreas(
        ViewItems, viewArea, g_frameWidth);

    ActionAreas.insert(
        ActionAreas.begin(),
        itemAreas.begin(),
        itemAreas.end());

    // Make room for the breadcrumb row at the top
    viewArea.top = 34;

    // Reveal the new level through a rounded rect growing from the
    // clicked cell's centre to the full viewArea in 50 ms
    if (hasRevealFrom)
    {
        const ViewAnimRect revealTo = MakeAnimRect(
            static_cast<float>(viewArea.left),
            static_cast<float>(viewArea.top),
            static_cast<float>(g_frameWidth - viewArea.right),
            static_cast<float>(g_frameHeight - viewArea.bottom));

        StartViewAnimation(hwnd, revealFrom, revealTo, true);
    }

    // The new level starts unscrolled; the old offset lives on in the
    // history snapshot
    g_targetScrollOffsetY = 0;
    g_scrollOffsetY = 0.0f;
    g_hoveredAreaIndex = -1;

    RenderFrame(hwnd);
}

// ========================================
// Window procedure
// ========================================

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_NCHITTEST:
        return HitTestSizingBorder(hwnd, lParam);

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_config.width = LOWORD(lParam);
            g_config.height = HIWORD(lParam);
            RenderFrame(hwnd);

            // Keep the in-place rename editor over the strip.
            if (g_renameActive)
                LayoutRenameEditor();
        }

        return 0;

    case WM_MOVE:
        if (g_renameActive)
            LayoutRenameEditor();
        return 0;

    case WM_EXITSIZEMOVE:
        CenterWindowOnMonitor(hwnd);
        UpdateSizeLocalize(g_config);
        return 0;

    case WM_HOTKEY:
        if (wParam == HOTKEY_ID)
        {
            ToggleWindow();
        }
        return 0;

    case WM_APP_SHOW_LAUNCHER:
        // A second copy of the exe was started; just surface this one.
        ShowWindowFront();
        return 0;

    case WM_TRAYICON:
        {
            const UINT event = LOWORD(lParam);

            // Windows XP: double-click = WM_LBUTTONDBLCLK, single click
            // = WM_LBUTTONUP (legacy). Modern trays send double-click
            // as LBUTTONDBLCLK and right-click as RBUTTONUP. We treat
            // both left varieties as "toggle window" and RBUTTONUP as
            // "show popup menu" — the standard tray convention.
            if (event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP)
            {
                ToggleWindow();
            }
            else if (event == WM_RBUTTONUP)
            {
                ShowTrayMenu(hwnd, g_config);
            }
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_TRAY_SHOW)
        {
            ToggleWindow();
            return 0;
        }
        if (LOWORD(wParam) == IDM_TRAY_EXIT)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDM_ITEM_RENAME)
        {
            RenameContextItem(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDM_ITEM_LOCATION)
        {
            RevealContextItem();
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        // Swallow Alt + F4 so the launcher cannot be closed with it
        if (wParam == VK_F4)
        {
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            // While an item is being dragged, Esc carries the item up one
            // level (same as hovering the back button) and the drag keeps
            // going, so it can be reordered in the parent list.
            if (isItemDragging && !Levels.empty())
            {
                DragUpOneLevel(hwnd);
            }
            else if (isItemDragging || g_dragItemIndex >= 0)
            {
                // Root level, or a press that never became a drag
                CancelItemDrag();
                g_hoveredAreaIndex = -1;

                if (GetCapture() == hwnd)
                    ReleaseCapture();

                RenderFrame(hwnd);
            }
            // Inside a folder level Esc acts as Back; at the root it
            // hides the launcher as before.
            else if (!Levels.empty())
            {
                GoBackLevel(hwnd);
            }
            else
            {
                ShowWindow(hwnd, SW_HIDE);
            }
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        {
            // Double-click on the hierarchy name strip swaps it for a real
            // EDIT control. The strip area is kept in a dedicated global
            // rather than ActionAreas because the back button (20×20 at
            // 10,10) must win hit-test priority over it.
            const int mx = static_cast<short>(LOWORD(lParam));
            const int my = static_cast<short>(HIWORD(lParam));

            if (!Levels.empty() &&
                mx >= g_hierarchyStripArea.x &&
                mx <  g_hierarchyStripArea.x + g_hierarchyStripArea.width &&
                my >= g_hierarchyStripArea.y &&
                my <  g_hierarchyStripArea.y + g_hierarchyStripArea.height)
            {
                BeginRename(hwnd, RenameTarget::Strip, -1);
                return 0;
            }
        }
        break;

    case WM_RBUTTONUP:
        {
            // Right-clicking the launcher commits an open rename, then
            // opens the item context menu when an item is under the cursor.
            if (g_renameActive)
                EndFolderRename(hwnd, true);

            const int mx = static_cast<short>(LOWORD(lParam));
            const int my = static_cast<short>(HIWORD(lParam));

            const ActionArea* area = HitTestArea(ActionAreas, mx, my);

            if (!area || area->type != L"item")
                return 0;

            const int index =
                static_cast<int>(area - ActionAreas.data());

            if (index < 0 || index >= static_cast<int>(ViewItems.size()))
                return 0;

            g_contextItemIndex = index;
            g_contextLevels = Levels;

            ShowItemContextMenu(hwnd, index);
        }
        return 0;

    case WM_LBUTTONDOWN:
        {
            // Any click on the launcher commits an open rename before the
            // click is handled normally. Clicks on the editor itself go to
            // the popup, never here.
            if (g_renameActive)
                EndFolderRename(hwnd, true);

            const ActionArea* area = HitTestArea(
                ActionAreas,
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam)));

            if (area && area->key == L"back")
            {
                GoBackLevel(hwnd);
            }
            else if (area && area->key == L"theme")
            {
                ToggleTheme(hwnd);
            }
            else if (area && area->key == L"opacity_up")
            {
                if (g_config.opacity < 1.0)
                {
                    g_config.opacity += 0.1;

                    if (g_config.opacity > 1.0)
                        g_config.opacity = 1.0;

                    UpdateSizeLocalize(g_config);
                    RenderFrame(hwnd);
                }
            }
            else if (area && area->key == L"opacity_down")
            {
                if (g_config.opacity > 0.1)
                {
                    g_config.opacity -= 0.1;

                    if (g_config.opacity < 0.1)
                        g_config.opacity = 0.1;

                    UpdateSizeLocalize(g_config);
                    RenderFrame(hwnd);
                }
            }

            else if (area && area->key == L"scrollbar")
            {
                // Begin thumb drag. Snap any in-flight wheel easing to the
                // current position so the thumb doesn't fight the mouse.
                g_targetScrollOffsetY =
                    static_cast<int>(g_scrollOffsetY + 0.5f);

                g_draggingScrollbar = true;
                g_dragStartMouseY = static_cast<short>(HIWORD(lParam));
                g_dragStartOffset = g_targetScrollOffsetY;
                SetCapture(hwnd); // keep receiving MOUSEMOVE outside window
            }
            else if (area && area->type == L"item")
            {
                // Items are clipped to the viewArea, so clicks outside it
                // must not trigger. The scrollbar thumb is a button and
                // wins HitTestArea priority, meaning clicks on the part of
                // an item covered by the thumb never reach this branch.
                const int mx = static_cast<short>(LOWORD(lParam));
                const int my = static_cast<short>(HIWORD(lParam));

                const int vaLeft = viewArea.left;
                const int vaTop = viewArea.top;
                const int vaRight = g_frameWidth - viewArea.right;
                const int vaBottom = g_frameHeight - viewArea.bottom;

                if (mx >= vaLeft && mx < vaRight &&
                    my >= vaTop && my < vaBottom)
                {
                    // itemAreas were inserted before any button areas and
                    // in ViewItems order, so the pointer index maps 1:1.
                    const int idx =
                        static_cast<int>(area - ActionAreas.data());

                    if (idx >= 0 && idx < static_cast<int>(ViewItems.size()))
                    {
                        // Record the press as a drag candidate. It only
                        // becomes a drag once the mouse moves past
                        // DRAG_START_THRESHOLD; otherwise WM_LBUTTONUP
                        // treats it as a plain click (open / enter group).
                        g_dragItemIndex = idx;
                        g_dragStart.x = mx;
                        g_dragStart.y = my;
                        g_dragMouse.x = mx;
                        g_dragMouse.y = my;

                        // Capture so the release outside the window still
                        // reaches us.
                        SetCapture(hwnd);
                    }
                }
            }

            return 0;
        }

    case WM_LBUTTONUP:
        {
            const int mx = static_cast<short>(LOWORD(lParam));
            const int my = static_cast<short>(HIWORD(lParam));

            if (g_draggingScrollbar)
            {
                g_draggingScrollbar = false;
                ReleaseCapture();
            }
            else if (isItemDragging)
            {
                FinishItemDrag(hwnd, mx, my);
            }
            else if (g_dragItemIndex >= 0)
            {
                // Plain click: open the item or enter the group
                const int idx = g_dragItemIndex;
                CancelItemDrag();

                if (GetCapture() == hwnd)
                    ReleaseCapture();

                ActivateItem(hwnd, idx);
            }

            return 0;
        }

    case WM_MOUSEMOVE:
        {
            POINT pt = {
                static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam))};

            // Scrollbar thumb drag: mouse dy maps to scroll offset through
            // travel/maxOffset. Applied immediately (no easing) so the
            // thumb sticks to the cursor; the frame loop presents the new
            // offset on its next tick instead of drawing per mouse event.
            if (g_draggingScrollbar)
            {
                std::vector<ActionArea> tmpAreas = GetItemsAreas(
                    ViewItems, viewArea, g_frameWidth);
                int lastBottom = 0;
                for (const auto& a : tmpAreas)
                {
                    if (a.y + a.height > lastBottom)
                        lastBottom = a.y + a.height;
                }
                const ScrollbarInfo sb = ComputeScrollbarInfo(
                    g_frameWidth, g_frameHeight, lastBottom);

                if (sb.visible && sb.travel > 0.0f)
                {
                    const int dy = pt.y - g_dragStartMouseY;
                    float newOffset = static_cast<float>(g_dragStartOffset)
                        + static_cast<float>(dy) * sb.maxOffset / sb.travel;

                    if (newOffset < 0.0f) newOffset = 0.0f;
                    if (newOffset > sb.maxOffset) newOffset = sb.maxOffset;

                    g_targetScrollOffsetY =
                        static_cast<int>(newOffset + 0.5f);
                    g_scrollOffsetY = newOffset;
                    g_scrollFrameRequested = true;
                }
                return 0;
            }

            // Item drag: promote the press to a drag once the cursor has
            // travelled far enough, then track the target every move.
            if (g_dragItemIndex >= 0)
            {
                g_dragMouse = pt;

                if (!isItemDragging)
                {
                    const int dx = pt.x - g_dragStart.x;
                    const int dy = pt.y - g_dragStart.y;

                    if (dx > DRAG_START_THRESHOLD ||
                        dx < -DRAG_START_THRESHOLD ||
                        dy > DRAG_START_THRESHOLD ||
                        dy < -DRAG_START_THRESHOLD)
                    {
                        if (g_dragItemIndex <
                            static_cast<int>(ViewItems.size()))
                        {
                            isItemDragging = true;
                            g_dragBackArmed = true;
                            g_hoveredAreaIndex = -1;
                        }
                        else
                        {
                            g_dragItemIndex = -1;
                            return 0;
                        }
                    }
                }

                if (isItemDragging)
                {
                    UpdateDragTarget(hwnd, pt.x, pt.y);
                    RenderFrame(hwnd);
                    return 0;
                }
            }

            int newHoverIndex = -1;

            // Hover only works inside the viewArea (overflow:hidden region)
            const int vaLeft = viewArea.left;
            const int vaTop = viewArea.top;
            const int vaRight = g_frameWidth - viewArea.right;
            const int vaBottom = g_frameHeight - viewArea.bottom;

            if (pt.x >= vaLeft && pt.x < vaRight &&
                pt.y >= vaTop && pt.y < vaBottom)
            {
                for (size_t i = 0; i < ActionAreas.size(); ++i)
                {
                    const ActionArea& area = ActionAreas[i];

                    if (pt.x >= area.x && pt.x < area.x + area.width &&
                        pt.y >= area.y && pt.y < area.y + area.height)
                    {
                        // Only "item" areas show the hover highlight;
                        // button areas (opacity/theme) already have their
                        // own visual affordance and shouldn't double-layer
                        if (area.type == L"item")
                        {
                            newHoverIndex = static_cast<int>(i);
                        }
                        break;
                    }
                }
            }

            if (newHoverIndex != g_hoveredAreaIndex)
            {
                g_hoveredAreaIndex = newHoverIndex;
                RenderFrame(hwnd);
            }

            // Arm TrackMouseEvent once so we receive WM_MOUSELEAVE when
            // the cursor leaves the window entirely; re-arm after each
            // WM_MOUSELEAVE fires.
            if (!g_trackMouseActive)
            {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.hwndTrack = hwnd;
                tme.dwFlags = TME_LEAVE;
                if (TrackMouseEvent(&tme))
                    g_trackMouseActive = true;
            }

            return 0;
        }

    case WM_MOUSELEAVE:
        g_trackMouseActive = false; // TrackMouseEvent auto-disarms
        if (g_hoveredAreaIndex != -1)
        {
            g_hoveredAreaIndex = -1;
            RenderFrame(hwnd);
        }
        return 0;

    case WM_MOUSEWHEEL:
        // The in-place editor is anchored to one grid cell, so the grid
        // must stay put while an item label is being renamed. (Wheel
        // messages reach the launcher even though the EDIT has focus,
        // because Windows routes the wheel to the hovered window.)
        if (g_renameActive && g_renameTarget == RenameTarget::Item)
            return 0;

        {
            // Only scroll when cursor is inside the viewArea
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);

            const int vaLeft = viewArea.left;
            const int vaTop = viewArea.top;
            const int vaRight = g_frameWidth - viewArea.right;
            const int vaBottom = g_frameHeight - viewArea.bottom;
            const bool inViewArea =
                pt.x >= vaLeft && pt.x < vaRight &&
                pt.y >= vaTop && pt.y < vaBottom;

            if (inViewArea)
            {
                // Content-up-on-wheel-down convention: wheel DOWN (negative
                // delta) → target increases → items will shift UP (a.y -= offset)
                // → items below come into view. wheel UP (positive delta)
                // → target decreases → items shift DOWN toward top.
                const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                g_targetScrollOffsetY -= wheelDelta;

                // The message loop notices the new target on its next
                // iteration and frame-paces the easing toward it.
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == BACK_FLASH_TIMER_ID)
        {
            // Two blinks = three toggles from the initial "visible" state
            g_backFlashVisible = !g_backFlashVisible;

            if (--g_backFlashTogglesLeft <= 0)
            {
                g_backFlashVisible = false;
                KillTimer(hwnd, BACK_FLASH_TIMER_ID);
            }
            RenderFrame(hwnd);
        }
        else if (wParam == ITEM_FLASH_TIMER_ID)
        {
            g_itemFlashVisible = !g_itemFlashVisible;

            if (--g_itemFlashTogglesLeft <= 0)
                StopItemFlash();

            RenderFrame(hwnd);
        }
        return 0;

    case WM_SETCURSOR:
        // Show the hand cursor while hovering a button-type area;
        // anything else falls through to the default arrow/sizing cursor
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);

            // Hand cursor only for clicks inside viewArea or buttons;
            // items outside viewArea (but still within an ActionArea hit)
            // should not show the hand cursor.
            const int vaLeft = viewArea.left;
            const int vaTop = viewArea.top;
            const int vaRight = g_frameWidth - viewArea.right;
            const int vaBottom = g_frameHeight - viewArea.bottom;
            const bool inViewArea =
                pt.x >= vaLeft && pt.x < vaRight &&
                pt.y >= vaTop && pt.y < vaBottom;

            const ActionArea* area = HitTestArea(ActionAreas, pt.x, pt.y);

            if (area && area->type == L"button")
            {
                // "Not clickable" cursor when the button is already at
                // its physical limit and cannot move further in that
                // direction (opacity_up at 1.0, opacity_down at 0.1)
                if (area->key == L"opacity_up" && g_config.opacity >= 1.0)
                {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32648)));
                    return TRUE;
                }

                if (area->key == L"opacity_down" && g_config.opacity <= 0.1)
                {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32648)));
                    return TRUE;
                }

                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                return TRUE;
            }

            if (area && area->type == L"item" && inViewArea)
            {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                return TRUE;
            }
        }
        break;

	case WM_KILLFOCUS:
		g_hoveredAreaIndex = -1;
		g_trackMouseActive = false;
		g_draggingScrollbar = false;
		CancelItemDrag();
		// The webpage-name prompt, the rename editor and the item context
		// menu are owned/transient windows that take focus while the
		// launcher stays on screen; losing focus to them must not hide it.
		if (!g_webNamePromptActive && !g_renameActive &&
			!IsContextMenuVisible())
			ShowWindow(hwnd, SW_HIDE);
		return 0;

    case WM_DROPFILES:
        {
            // Legacy drop path (used when OLE registration was unavailable)
            g_insertionVisible = false;

            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

            for (UINT i = 0; i < fileCount; ++i)
            {
                UINT length = DragQueryFileW(drop, i, nullptr, 0);

                if (length == 0)
                    continue;

                std::wstring path(static_cast<size_t>(length), L'\0');

                if (DragQueryFileW(drop, i, &path[0], length + 1) != 0)
                    AddDroppedPath(path, ViewItems.size());
            }

            DragFinish(drop);
            RenderFrame(hwnd);
            return 0;
        }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        RevokeDragDrop(hwnd);
        DestroyTrayIcon(hwnd);
        KillTimer(hwnd, ITEM_FLASH_TIMER_ID);
        DestroyRenameEditor();
        SetAnimationTimerResolution(false);
        SetAnimationThreadPriority(false);
        UnregisterHotKey(hwnd, HOTKEY_ID);
        PostQuitMessage(0);
        return 0;

	case WM_PAINT:
		{
			// The frame is presented through UpdateLayeredWindow,
			// so a normal paint would be a no-op. However when the
			// layered window was obscured by another window and then
			// revealed, the system sends WM_PAINT but does NOT restore
			// our frame buffer pixels — it just leaves those regions
			// transparent. Re-submit the full frame here to fix that.
			RenderFrame(hwnd);

			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			EndPaint(hwnd, &ps);

			return 0;
		}
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

// ========================================
// Hotkey prompt (shown when the configured hotkey is already taken)
// ========================================

struct HotkeyPromptState
{
    HWND hint = nullptr;
    UINT modifiers = 0;
    UINT key = 0;
    bool accepted = false;
};

static HotkeyPromptState g_hotkeyPrompt;

// Theme-matched surfaces for the prompt window
static HBRUSH g_hotkeyPromptBrush = nullptr;
static COLORREF g_hotkeyPromptTextColor = RGB(255, 255, 255);

// Human-readable form of a hotkey, e.g. "Alt + `"
static std::wstring HotkeyToString(UINT modifiers, UINT key)
{
    std::wstring text;

    if (modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (modifiers & MOD_ALT)     text += L"Alt + ";
    if (modifiers & MOD_SHIFT)   text += L"Shift + ";
    if (modifiers & MOD_WIN)     text += L"Win + ";

    wchar_t name[64] = {};
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);

    if (GetKeyNameTextW(static_cast<LONG>(scan) << 16, name, 64) > 0)
        text += name;
    else
        text += static_cast<wchar_t>(key);

    return text;
}

static bool IsModifierVirtualKey(UINT key)
{
    switch (key)
    {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return true;
    default:
        return false;
    }
}

static UINT CurrentModifierState()
{
    UINT modifiers = 0;

    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)    modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000)   modifiers |= MOD_SHIFT;

    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        modifiers |= MOD_WIN;

    return modifiers;
}

static LRESULT CALLBACK HotkeyPromptProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    {
        const UINT key = static_cast<UINT>(wParam);

        if (key == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }

        // Wait for the non-modifier half of the combination
        if (IsModifierVirtualKey(key))
            return 0;

        // Read the modifiers at the moment the key went down
        const UINT modifiers = CurrentModifierState();

        if (modifiers == 0)
        {
            SetWindowTextW(g_hotkeyPrompt.hint,
                L"组合键需要至少一个修饰键（Ctrl / Alt / Shift / Win）。\n"
                L"请重新按下新的快捷键，按 Esc 取消。\n\n"
                L"The shortcut needs at least one of Ctrl / Alt / Shift / Win.\n"
                L"Press a new shortcut, or Esc to cancel.");
            return 0;
        }

        g_hotkeyPrompt.modifiers = modifiers;
        g_hotkeyPrompt.key = key;
        g_hotkeyPrompt.accepted = true;

        DestroyWindow(hwnd);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        RECT client = {};
        GetClientRect(hwnd, &client);

        FillRect(
            reinterpret_cast<HDC>(wParam),
            &client,
            g_hotkeyPromptBrush ? g_hotkeyPromptBrush
                                : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

        return 1;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_hotkeyPromptTextColor);

        return reinterpret_cast<LRESULT>(
            g_hotkeyPromptBrush
                ? g_hotkeyPromptBrush
                : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // The theme brush lives exactly as long as the window does
        if (g_hotkeyPromptBrush)
        {
            DeleteObject(g_hotkeyPromptBrush);
            g_hotkeyPromptBrush = nullptr;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Show the hotkey window and wait for either a new combination or Esc.
//   occupied = true : the configured hotkey is taken by another program
//   occupied = false: the hotkey registered fine, but may be changed here
// Returns true when the user picked a new combination (written to
// modifiers/key), false when the window was dismissed with Esc.
static bool ShowHotkeyPrompt(bool occupied, UINT& modifiers, UINT& key)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HotkeyPromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = HOTKEY_PROMPT_CLASS;

    RegisterClassExW(&wc);

    g_hotkeyPrompt = {};

    // Match the launcher theme: dark background with white text, or the
    // light background with #333333 text
    const bool lightTheme = _wcsicmp(g_config.theme, L"light") == 0;

    g_hotkeyPromptTextColor = lightTheme
        ? RGB(0x33, 0x33, 0x33)
        : RGB(255, 255, 255);

    g_hotkeyPromptBrush = CreateSolidBrush(
        lightTheme ? RGB(0xff, 0xff, 0xff) : RGB(0x22, 0x26, 0x2a));

    const int promptWidth = 460;
    const int promptHeight = 210;

    const int x = (GetSystemMetrics(SM_CXSCREEN) - promptWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - promptHeight) / 2;

    // Frameless: no caption, so Esc is the way out
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        HOTKEY_PROMPT_CLASS,
        L"GarcenLauncher Hotkey",
        WS_POPUP,
        x,
        y,
        promptWidth,
        promptHeight,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!hwnd)
    {
        if (g_hotkeyPromptBrush)
        {
            DeleteObject(g_hotkeyPromptBrush);
            g_hotkeyPromptBrush = nullptr;
        }

        return false;
    }

    g_hotkeyPrompt.hint = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        16,
        16,
        promptWidth - 40,
        promptHeight - 32,
        hwnd,
        nullptr,
        wc.hInstance,
        nullptr);

    const std::wstring hotkey =
        HotkeyToString(g_config.hotkeyModifiers, g_config.hotkeyKey);

    if (occupied)
    {
        SetWindowTextW(g_hotkeyPrompt.hint,
            (L"快捷键 " + hotkey + L" 已被其他程序占用。\n"
             L"The shortcut " + hotkey + L" is already in use.\n\n"
             L"请按下新的快捷键（需包含 Ctrl / Alt / Shift / Win）。\n"
             L"Press a new shortcut (with Ctrl / Alt / Shift / Win).\n\n"
             L"按 Esc 跳过（本次不注册快捷键）。\n"
             L"Press Esc to skip.").c_str());
    }
    else
    {
        SetWindowTextW(g_hotkeyPrompt.hint,
            (L"当前快捷键\n"
             L"Current shortcut\n\n" + hotkey + L"\n\n"
             L"按下新的快捷键可更新（需包含 Ctrl / Alt / Shift / Win）。\n"
             L"Press a new shortcut to change it (with Ctrl / Alt / Shift / Win).\n\n"
             L"按 Esc 关闭。\n"
             L"Press Esc to close.").c_str());
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG message = {};

    while (IsWindow(hwnd) && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (!g_hotkeyPrompt.accepted)
        return false;

    modifiers = g_hotkeyPrompt.modifiers;
    key = g_hotkeyPrompt.key;

    return true;
}

// ========================================
// Auto start
// ========================================

// Make sure the launcher is registered to start with Windows. The entry
// is only created when missing (so removing it by hand is respected) and
// refreshed when the executable has been moved somewhere else.
static void EnsureAutoStart()
{
    wchar_t exePath[MAX_PATH] = {};

    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return;

    // Quote it so paths with spaces still resolve correctly
    const std::wstring command = L"\"" + std::wstring(exePath) + L"\"";

    HKEY key = nullptr;

    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            nullptr,
            0,
            KEY_QUERY_VALUE | KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    wchar_t current[MAX_PATH * 2] = {};
    DWORD size = sizeof(current);
    DWORD type = 0;

    const LSTATUS query = RegQueryValueExW(
        key,
        L"GarcenLauncher",
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(current),
        &size);

    const bool missing = query != ERROR_SUCCESS || type != REG_SZ;
    const bool moved = !missing &&
        wcscmp(current, command.c_str()) != 0;

    if (missing || moved)
    {
        RegSetValueExW(
            key,
            L"GarcenLauncher",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key);
}

// Hand every item path to the background icon prefetcher. Opening a group
// renders all of its children at 32px for the first time; with the sources
// already extracted, entering the group never blocks on a shell call.
static void PrefetchItemIcons(const std::vector<Item>& items)
{
    for (const Item& item : items)
    {
        if (!item.path.empty())
            PrefetchIconSource(item.path);

        if (!item.children.empty())
            PrefetchItemIcons(item.children);
    }
}

// ========================================
// Windows entry point
// ========================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    // Single instance. A second copy must not register a duplicate tray
    // icon or overwrite the config; instead it asks the running copy to
    // show itself and exits.
    g_instanceMutex = CreateMutexW(
        nullptr, FALSE, L"Local\\GarcenLauncher.SingleInstance");

    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // The running copy may still be creating its window, so look for it
        // for a short while before giving up.
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            HWND running = FindWindowW(WINDOW_CLASS, nullptr);

            if (running)
            {
                // This process was just started by the user, so it currently
                // holds the foreground right; hand that right to the running
                // instance so its SetForegroundWindow succeeds.
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(running, WM_APP_SHOW_LAUNCHER, 0, 0);
                break;
            }

            Sleep(50);
        }

        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;

        return 0;
    }

    // OLE (STA) is required by the drag-drop target registered below; it
    // also satisfies the Windows Runtime apartment used by InitConfig.
    const bool oleReady = SUCCEEDED(OleInitialize(nullptr));

    // Load config.gcson (create it with defaults when missing)
    g_config = InitConfig();
    g_backgroundColor = GetThemeBackgroundColor(g_config.theme);

    // Register the launcher to start with Windows (first run, or after
    // the executable has been moved)
    EnsureAutoStart();

    // Load list.json (create it with [] when missing)
    g_items = InitList();

    // Warm the icon cache off the UI thread
    PrefetchItemIcons(g_items);

    // Working copy of the list used by the UI layer
    ViewItems = g_items;
    // Levels stays empty initially (viewing root)

    // Initialize GDI+, used to compose the antialiased frame
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(
        &g_gdiplusToken,
        &gdiplusInput,
        nullptr);

    // Register window class
    g_appIconLarge = CreatePngResourceIcon(
        ASSET_APP_ICON, GetSystemMetrics(SM_CXICON));
    g_appIconSmall = CreatePngResourceIcon(
        ASSET_APP_ICON, GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc = {};

    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = g_appIconLarge
        ? g_appIconLarge
        : LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
    wc.hIconSm = g_appIconSmall ? g_appIconSmall : wc.hIcon;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(
            nullptr,
            L"Failed to register window class.",
            WINDOW_TITLE,
            MB_ICONERROR
        );

        return 1;
    }

    // Calculate centered position
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    const int x = (screenWidth - g_config.width) / 2;
	const int y = (screenHeight - g_config.height) / 2;

    // The frame is presented with per-pixel alpha, which requires a layered
    // window even at opacity 1. This is also what makes the rounded corners
    // antialiased instead of clipped by a hard-edged window region.
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED;

    // Create window
    g_hwnd = CreateWindowExW(
		exStyle,
		WINDOW_CLASS,
		WINDOW_TITLE,
		WS_POPUP,
		x,
		y,
		g_config.width,
		g_config.height,
		nullptr,
		nullptr,
		hInstance,
		nullptr
	);

    if (!g_hwnd)
    {
        MessageBoxW(
            nullptr,
            L"Failed to create launcher window.",
            WINDOW_TITLE,
            MB_ICONERROR
        );

        return 1;
    }

    // Accept shell drag-drop so users can drop files / folders onto the
    // window and have them registered as launcher items
    DragAcceptFiles(g_hwnd, TRUE);

    // OLE drop target — the only way to get feedback while the files are
    // still being dragged (WM_DROPFILES fires after the release)
    if (oleReady)
    {
        g_dropTarget = new LauncherDropTarget();

        if (FAILED(RegisterDragDrop(g_hwnd, g_dropTarget)))
        {
            g_dropTarget->Release();
            g_dropTarget = nullptr;
        }
    }

    // Compose the first frame before the window is ever shown
    RenderFrame(g_hwnd);

    // Register the global hotkey stored in config.gcson. When another
    // application already owns it, ask for a replacement combination.
    // Either way the prompt is shown once so the user can review and
    // change the hotkey, and every accepted change is persisted.
    for (;;)
    {
        while (!RegisterHotKey(
            g_hwnd,
            HOTKEY_ID,
            g_config.hotkeyModifiers,
            g_config.hotkeyKey))
        {
            UINT modifiers = 0;
            UINT key = 0;

            if (!ShowHotkeyPrompt(true, modifiers, key))
                goto hotkey_done; // skipped — the tray icon still works

            g_config.hotkeyModifiers = modifiers;
            g_config.hotkeyKey = key;

            UpdateSizeLocalize(g_config);
        }

        {
            UINT modifiers = 0;
            UINT key = 0;

            if (!ShowHotkeyPrompt(false, modifiers, key))
                break; // closed — keep the registered hotkey

            // Release the old combination before trying the new one
            UnregisterHotKey(g_hwnd, HOTKEY_ID);

            g_config.hotkeyModifiers = modifiers;
            g_config.hotkeyKey = key;

            UpdateSizeLocalize(g_config);
        }
    }

hotkey_done:

    // Register the system-tray icon so the app survives being hidden
    // and can be reopened / exited from the notification area.
    CreateTrayIcon(g_hwnd, hInstance);

    // Start hidden
    ShowWindow(g_hwnd, SW_HIDE);

    // Message loop with a frame clock for animations.
    //
    // Animations used to be stepped by WM_TIMER at a 16 ms interval, but the
    // timer queue is quantised to the ~15.6 ms system tick (so "16 ms" really
    // fired every ~31 ms) and WM_TIMER is the lowest-priority message. That
    // is why a scrollbar drag — driven straight from WM_MOUSEMOVE — looked
    // fluid while wheel scrolling and the level transition felt steppy.
    //
    // Instead, while anything is animating the loop drains input, submits a
    // frame, then lets DwmFlush apply vblank backpressure. If composition is
    // unavailable it sleeps until the next QPC frame deadline. When nothing
    // animates it blocks in GetMessage as before.
    MSG msg = {};
    double nextFrameTime = 0.0;

    for (;;)
    {
        if (!AnimationFramesNeeded())
        {
            SetAnimationTimerResolution(false);
            SetAnimationThreadPriority(false);

            if (GetMessageW(&msg, nullptr, 0, 0) <= 0)
                break;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }

        SetAnimationTimerResolution(true);
        SetAnimationThreadPriority(true);

        // Drain every queued message first so input stays responsive and
        // several mouse events collapse into a single presented frame.
        bool quit = false;

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                quit = true;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (quit)
            break;

        const double now = QuerySecondsNow();

        if (now < nextFrameTime)
        {
            const double remaining = nextFrameTime - now;

            if (remaining > 0.001)
                Sleep(static_cast<DWORD>(remaining * 1000.0));

            continue;
        }

        const bool viewAnimFinished = AdvanceAnimations();

        g_scrollFrameRequested = false;
        RenderFrame(g_hwnd);

        if (viewAnimFinished)
        {
            // The cached destination level was just presented as the final
            // frame; drop it now so the next frame re-renders live data.
            g_viewAnimNewFrame.clear();
            g_viewAnimNewFrameWidth = 0;
            g_viewAnimNewFrameHeight = 0;
        }

        // Present at the compositor's vblank. If DWM composition is not
        // available, retain the QPC + Sleep frame clock as a fallback.
        nextFrameTime = DwmFlush() == S_OK
            ? 0.0
            : now + DisplayFrameIntervalSeconds();
    }

    SetAnimationTimerResolution(false);
    SetAnimationThreadPriority(false);

    ReleaseFrameBuffer();

    if (g_dropTarget)
    {
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }

    if (oleReady)
        OleUninitialize();

    if (g_appIconLarge)
    {
        DestroyIcon(g_appIconLarge);
        g_appIconLarge = nullptr;
    }

    if (g_appIconSmall)
    {
        DestroyIcon(g_appIconSmall);
        g_appIconSmall = nullptr;
    }

    Gdiplus::GdiplusShutdown(g_gdiplusToken);

    if (g_instanceMutex)
    {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }

    return static_cast<int>(msg.wParam);
}
