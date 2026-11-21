#include "Menu.h"

#include <cstring>

#include <windowsx.h>
#include <gdiplus.h>

#include "DrawImage.h"

// ========================================
// Tray icon state
// ========================================

static HWND s_trayHwnd = nullptr; // window that owns the tray icon
static UINT_PTR s_trayId = 1;     // unique ID inside the notification area
static bool s_trayCreated = false;
static HICON s_trayIcon = nullptr; // owned PNG-derived icon, if available

// ========================================
// Popup menu layout
// ========================================

constexpr wchar_t MENU_WINDOW_CLASS[] = L"GarcenLauncherTrayMenu";

constexpr int MENU_ROW_HEIGHT = 28;        // height of a single row
constexpr int MENU_MIN_WIDTH = 110;        // narrowest popup (tray menu)
constexpr int MENU_MAX_ITEMS = 8;          // rows a popup may hold

constexpr float MENU_RADIUS = 6.0f;        // corner radius (popup + hover)
constexpr float MENU_FONT_SIZE = 14.0f;    // pixels
constexpr float MENU_TEXT_PADDING = 12.0f; // label left inset

// Hand cursor, same one the launcher uses for its buttons (IDC_HAND)
constexpr WORD MENU_HAND_CURSOR_ID = 32649;

// Force the hand cursor on the menu. Safe to call at any time — the
// system cursor may have been left as busy / sizing / arrow by whatever
// window was under the pointer before the popup appeared.
static void ApplyMenuCursor()
{
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(MENU_HAND_CURSOR_ID)));
}

// Small gap so the cursor does not sit on the first/last row right away
constexpr int MENU_GAP_ABOVE_CURSOR = 4;

// ========================================
// Menu state
// ========================================

struct MenuTheme
{
    Gdiplus::Color background;
    Gdiplus::Color foreground;
    Gdiplus::Color hoverBackground;
    Gdiplus::Color hoverForeground;
};

static HWND s_menuHwnd = nullptr;   // popup window (created once, reused)
static HWND s_menuOwner = nullptr;  // window that receives WM_COMMAND
static MenuTheme s_menuTheme = {};
static int s_menuHover = -1;        // hovered row index, or -1

// Rows currently shown. Labels are static literals owned by the caller,
// so no storage is allocated here.
static const wchar_t* s_menuLabels[MENU_MAX_ITEMS] = {};
static UINT s_menuIds[MENU_MAX_ITEMS] = {};
static int s_menuCount = 0;
static int s_menuWidth = MENU_MIN_WIDTH;
static int s_menuHeight = 0;

// True while a context menu (not the tray menu) is on screen: the
// launcher must not hide itself when focus moves to it.
static bool s_menuIsContext = false;

static void SetMenuItems(
    const wchar_t* const* labels,
    const UINT* ids,
    int count)
{
    if (count > MENU_MAX_ITEMS)
        count = MENU_MAX_ITEMS;

    if (count < 1)
        count = 1;

    s_menuCount = count;

    for (int i = 0; i < count; ++i)
    {
        s_menuLabels[i] = labels[i];
        s_menuIds[i] = ids[i];
    }
}

// Width that fits the longest label, snapped to MENU_MIN_WIDTH.
static int MeasureMenuWidth()
{
    int textWidth = 0;

    HDC screen = GetDC(nullptr);
    HDC memDC = screen ? CreateCompatibleDC(screen) : nullptr;

    if (screen)
        ReleaseDC(nullptr, screen);

    if (memDC)
    {
        {
            Gdiplus::Graphics graphics(memDC);

            Gdiplus::FontFamily family(L"Microsoft YaHei");
            Gdiplus::Font font(
                &family,
                MENU_FONT_SIZE,
                Gdiplus::FontStyleBold,
                Gdiplus::UnitPixel);

            for (int i = 0; i < s_menuCount; ++i)
            {
                Gdiplus::RectF bound;

                graphics.MeasureString(
                    s_menuLabels[i],
                    -1,
                    &font,
                    Gdiplus::RectF(0.0f, 0.0f, 1000.0f, 100.0f),
                    &bound);

                const int width = static_cast<int>(bound.Width + 0.5f);

                if (width > textWidth)
                    textWidth = width;
            }
        }

        DeleteDC(memDC);
    }

    const int width = textWidth +
        static_cast<int>(MENU_TEXT_PADDING * 2.0f) + 8;

    return width > MENU_MIN_WIDTH ? width : MENU_MIN_WIDTH;
}

// Resolve menu colours from the launcher theme. Kept in a plain struct so
// the paint path needs no allocation and no locking.
static MenuTheme ResolveTheme(const LauncherConfig& config)
{
    MenuTheme theme = {};

    if (_wcsicmp(config.theme, L"dark") == 0)
    {
        // dark theme: dark surface, white text, no border
        theme.background = Gdiplus::Color(255, 0x22, 0x26, 0x2a);
        theme.foreground = Gdiplus::Color(255, 0xff, 0xff, 0xff);
        theme.hoverBackground = Gdiplus::Color(255, 0x3a, 0x40, 0x48);
        theme.hoverForeground = Gdiplus::Color(255, 0xff, 0xff, 0xff);
    }
    else
    {
        // light theme: white surface, #333333 text
        theme.background = Gdiplus::Color(255, 0xff, 0xff, 0xff);
        theme.foreground = Gdiplus::Color(255, 0x33, 0x33, 0x33);
        theme.hoverBackground = Gdiplus::Color(255, 0xee, 0xee, 0xee);
        theme.hoverForeground = Gdiplus::Color(255, 0x33, 0x33, 0x33);
    }

    return theme;
}

// Hit test a client-space point against the menu rows
static int HitTestMenuItem(int x, int y)
{
    if (x < 0 || x >= s_menuWidth || y < 0 || y >= s_menuHeight)
        return -1;

    const int index = y / MENU_ROW_HEIGHT;

    return index < s_menuCount ? index : -1;
}

static void HideMenu()
{
    if (!s_menuHwnd)
        return;

    const bool wasVisible = IsWindowVisible(s_menuHwnd) == TRUE;

    if (GetCapture() == s_menuHwnd)
        ReleaseCapture();

    if (wasVisible)
        ShowWindow(s_menuHwnd, SW_HIDE);

    s_menuHover = -1;

    // A context menu stole focus from the launcher; hand it back so the
    // launcher keeps receiving keys after the menu closes.
    if (wasVisible && s_menuIsContext && s_menuOwner &&
        IsWindow(s_menuOwner) && IsWindowVisible(s_menuOwner))
    {
        SetForegroundWindow(s_menuOwner);
        SetFocus(s_menuOwner);
    }
}

// Append a rounded-rectangle figure (four 90° arcs) to the path
static void AddRoundedRect(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rect,
    Gdiplus::REAL radius)
{
    const Gdiplus::REAL d = radius * 2.0f;

    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// Compose the popup into a premultiplied-alpha DIB and push it to the
// layered window, exactly like the launcher frame is presented.
static void RenderMenu(HWND hwnd)
{
    HDC screen = GetDC(nullptr);

    if (!screen)
        return;

    HDC memDC = CreateCompatibleDC(screen);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = s_menuWidth;
    info.bmiHeader.biHeight = -s_menuHeight; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;

    HBITMAP bitmap = CreateDIBSection(
        screen,
        &info,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0);

    ReleaseDC(nullptr, screen);

    if (!memDC || !bitmap || !bits)
    {
        if (bitmap)
            DeleteObject(bitmap);

        if (memDC)
            DeleteDC(memDC);

        return;
    }

    HGDIOBJ previousBitmap = SelectObject(memDC, bitmap);

    memset(bits, 0,
        static_cast<size_t>(s_menuWidth) * static_cast<size_t>(s_menuHeight) * 4);

    {
        Gdiplus::Graphics graphics(memDC);

        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

        const Gdiplus::RectF bounds(
            0.0f,
            0.0f,
            static_cast<Gdiplus::REAL>(s_menuWidth),
            static_cast<Gdiplus::REAL>(s_menuHeight));

        // Rounded menu surface (radius 6), no border of any kind
        Gdiplus::GraphicsPath backgroundPath;
        AddRoundedRect(backgroundPath, bounds, MENU_RADIUS);

        Gdiplus::SolidBrush backgroundBrush(s_menuTheme.background);
        graphics.FillPath(&backgroundBrush, &backgroundPath);

        // Rounded hover highlight (radius 6) for the hovered row
        if (s_menuHover >= 0 && s_menuHover < s_menuCount)
        {
            const Gdiplus::RectF hoverRect(
                0.0f,
                static_cast<Gdiplus::REAL>(s_menuHover * MENU_ROW_HEIGHT),
                static_cast<Gdiplus::REAL>(s_menuWidth),
                static_cast<Gdiplus::REAL>(MENU_ROW_HEIGHT));

            Gdiplus::GraphicsPath hoverPath;
            AddRoundedRect(hoverPath, hoverRect, MENU_RADIUS);

            Gdiplus::SolidBrush hoverBrush(s_menuTheme.hoverBackground);
            graphics.FillPath(&hoverBrush, &hoverPath);
        }

        // Item labels
        Gdiplus::FontFamily family(L"Microsoft YaHei");
        Gdiplus::Font font(
            &family,
            MENU_FONT_SIZE,
            Gdiplus::FontStyleBold,
            Gdiplus::UnitPixel);

        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        for (int i = 0; i < s_menuCount; ++i)
        {
            Gdiplus::SolidBrush textBrush(
                i == s_menuHover
                    ? s_menuTheme.hoverForeground
                    : s_menuTheme.foreground);

            const Gdiplus::RectF textRect(
                MENU_TEXT_PADDING,
                static_cast<Gdiplus::REAL>(i * MENU_ROW_HEIGHT),
                static_cast<Gdiplus::REAL>(s_menuWidth) - MENU_TEXT_PADDING * 2.0f,
                static_cast<Gdiplus::REAL>(MENU_ROW_HEIGHT));

            graphics.DrawString(
                s_menuLabels[i],
                -1,
                &font,
                textRect,
                &format,
                &textBrush);
        }
    }

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    POINT source = {0, 0};
    POINT destination = {windowRect.left, windowRect.top};
    SIZE size = {s_menuWidth, s_menuHeight};

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        hwnd,
        nullptr,
        &destination,
        &size,
        memDC,
        &source,
        0,
        &blend,
        ULW_ALPHA);

    SelectObject(memDC, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDC);
}

// ========================================
// Popup window
// ========================================

static LRESULT CALLBACK MenuWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEMOVE:
        {
            // The menu holds the mouse capture, so the system stops updating
            // the cursor on its own — enforce the hand cursor on every move,
            // otherwise whatever cursor the previous window left behind
            // (busy, sizing, arrow) stays visible.
            ApplyMenuCursor();

            const int index = HitTestMenuItem(
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam));

            if (index != s_menuHover)
            {
                s_menuHover = index;
                RenderMenu(hwnd);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        // Capture is held while the menu is open, so clicks outside the
        // popup arrive here too — that is how outside-clicks dismiss it.
        // Clicking inside is handled on button-up.
        if (HitTestMenuItem(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) < 0)
        {
            HideMenu();
        }
        return 0;

    case WM_LBUTTONUP:
        {
            const int index = HitTestMenuItem(
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam));

            if (index < 0)
            {
                HideMenu();
                return 0;
            }

            const HWND owner = s_menuOwner;

            HideMenu();

            if (owner)
            {
                PostMessageW(
                    owner,
                    WM_COMMAND,
                    static_cast<WPARAM>(s_menuIds[index]),
                    0);
            }
        }
        return 0;

    case WM_RBUTTONDOWN:
        // Right-click anywhere dismisses the popup
        HideMenu();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            HideMenu();
            return 0;
        }
        break;

    case WM_KILLFOCUS:
        // Requirement: the menu must disappear as soon as it loses focus
        HideMenu();
        return 0;

    case WM_CAPTURECHANGED:
        // Some other window grabbed the mouse — treat it like focus loss.
        // Guarded by the visibility check so our own ReleaseCapture in
        // HideMenu does not recurse.
        if (IsWindowVisible(hwnd))
            HideMenu();
        return 0;

    case WM_SETCURSOR:
        // The popup is entirely clickable, so the hand cursor is enforced
        // here and wins over any default cursor handling (highest priority:
        // returning TRUE stops DefWindowProc from resetting it).
        ApplyMenuCursor();
        return TRUE;

    case WM_DESTROY:
        if (hwnd == s_menuHwnd)
            s_menuHwnd = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void RegisterMenuClass()
{
    static bool registered = false;

    if (registered)
        return;

    WNDCLASSEXW wc = {};

    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MenuWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = MENU_WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(MENU_HAND_CURSOR_ID));

    RegisterClassExW(&wc);

    registered = true;
}

// Common popup: everything is positioned/painted here, both for the tray
// menu (anchored above the cursor) and for context menus (at the cursor).
static void ShowMenuAt(
    HWND owner,
    const LauncherConfig& config,
    const wchar_t* const* labels,
    const UINT* ids,
    int count,
    int anchorX,
    int anchorY,
    bool context)
{
    if (!labels || !ids || count < 1)
        return;

    RegisterMenuClass();

    SetMenuItems(labels, ids, count);

    s_menuWidth = MeasureMenuWidth();
    s_menuHeight = MENU_ROW_HEIGHT * s_menuCount;

    if (!s_menuHwnd)
    {
        s_menuHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
            MENU_WINDOW_CLASS,
            L"",
            WS_POPUP,
            0,
            0,
            s_menuWidth,
            s_menuHeight,
            nullptr, // unowned: it must be showable while the owner is hidden
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        if (!s_menuHwnd)
            return;
    }

    s_menuOwner = owner;
    s_menuTheme = ResolveTheme(config);
    s_menuHover = -1;
    s_menuIsContext = context;

    int x = anchorX;
    int y = anchorY;

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);

    if (GetMonitorInfoW(
        MonitorFromPoint(POINT{anchorX, anchorY}, MONITOR_DEFAULTTONEAREST),
        &monitorInfo))
    {
        const RECT& work = monitorInfo.rcWork;

        if (x + s_menuWidth > work.right) x = work.right - s_menuWidth;
        if (x < work.left)               x = work.left;
        if (y + s_menuHeight > work.bottom) y = work.bottom - s_menuHeight;
        if (y < work.top)                y = work.top;
    }

    SetWindowPos(
        s_menuHwnd,
        HWND_TOPMOST,
        x,
        y,
        s_menuWidth,
        s_menuHeight,
        SWP_NOACTIVATE);

    RenderMenu(s_menuHwnd);

    ShowWindow(s_menuHwnd, SW_SHOW);

    // Mouse capture guarantees we hear the next click even if activation
    // is refused; taking focus additionally gives us Escape handling and
    // WM_KILLFOCUS for ALT+TAB-style dismissal.
    SetCapture(s_menuHwnd);
    SetForegroundWindow(s_menuHwnd);
    SetFocus(s_menuHwnd);

    // The popup may appear under a stationary pointer, so no WM_SETCURSOR
    // or WM_MOUSEMOVE will arrive until the user moves — stamp the hand
    // cursor right away instead of inheriting the previous window's cursor.
    ApplyMenuCursor();
}

void ShowTrayMenu(HWND owner, const LauncherConfig& config)
{
    static const wchar_t* const labels[] = {L"显示 / Show", L"退出 / Exit"};
    static const UINT ids[] = {IDM_TRAY_SHOW, IDM_TRAY_EXIT};

    // Anchor just above the cursor, like a notification-area menu.
    POINT cursor = {};
    GetCursorPos(&cursor);

    s_menuWidth = MENU_MIN_WIDTH;
    s_menuHeight = MENU_ROW_HEIGHT * 2;

    ShowMenuAt(owner, config, labels, ids, 2,
        cursor.x, cursor.y - s_menuHeight - MENU_GAP_ABOVE_CURSOR, false);
}

void ShowContextMenu(
    HWND owner,
    const LauncherConfig& config,
    const wchar_t* const* labels,
    const UINT* ids,
    int count,
    int screenX,
    int screenY)
{
    ShowMenuAt(owner, config, labels, ids, count,
        screenX, screenY, true);
}

bool IsContextMenuVisible()
{
    return s_menuHwnd && s_menuIsContext && IsWindowVisible(s_menuHwnd);
}

void HidePopupMenu()
{
    HideMenu();
}

// ========================================
// Tray icon lifecycle
// ========================================

bool CreateTrayIcon(HWND hwnd, HINSTANCE hInstance)
{
    (void)hInstance; // the PNG is loaded from this module's resources

    if (s_trayCreated)
        return true;

    s_trayHwnd = hwnd;

    // Use the embedded PNG at the notification area's native size. Fall
    // back to the stock application icon if decoding ever fails.
    HICON hIcon = CreatePngResourceIcon(
        ASSET_APP_ICON, GetSystemMetrics(SM_CXSMICON));
    const bool ownsIcon = hIcon != nullptr;

    if (!hIcon)
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(NOTIFYICONDATAW);
    nid.hWnd             = hwnd;
    nid.uID              = static_cast<UINT>(s_trayId);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = hIcon;

    constexpr wchar_t tooltip[] = L"GarcenLauncher";
    memcpy(nid.szTip, tooltip, sizeof(tooltip));

    const BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);

    if (ok)
    {
        s_trayCreated = true;
        s_trayIcon = ownsIcon ? hIcon : nullptr;
    }
    else if (ownsIcon)
    {
        DestroyIcon(hIcon);
    }

    return ok == TRUE;
}

void DestroyTrayIcon(HWND hwnd)
{
    if (s_menuHwnd)
    {
        HideMenu();
        DestroyWindow(s_menuHwnd);
        s_menuHwnd = nullptr;
    }

    if (!s_trayCreated)
        return;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd   = hwnd;
    nid.uID    = static_cast<UINT>(s_trayId);

    Shell_NotifyIconW(NIM_DELETE, &nid);

    s_trayCreated = false;
    s_trayHwnd = nullptr;

    if (s_trayIcon)
    {
        DestroyIcon(s_trayIcon);
        s_trayIcon = nullptr;
    }
}
