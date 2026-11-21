#include "ActionArea.h"

#include <shellapi.h>

bool OpenItem(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    // Hide the launcher immediately when the item starts opening
    extern HWND g_hwnd;

    if (g_hwnd)
        ShowWindow(g_hwnd, SW_HIDE);

    // The shell may show UI for this request (e.g. the "Open with" picker
    // for a file with no association). With a null owner it would take the
    // still-active-but-now-hidden launcher as its owner, and hiding an
    // owner hides its owned windows — which made that picker flash and
    // close instantly. Hand it the desktop window as a visible owner
    // instead.
    HWND owner = GetShellWindow();

    if (!owner)
        owner = GetDesktopWindow();

    const HINSTANCE result = ShellExecuteW(
        owner,
        L"open",
        path.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);

    // ShellExecuteW returns a value greater than 32 on success.
    return reinterpret_cast<INT_PTR>(result) > 32;
}

ActionArea CreateArea(
    const wchar_t* type,
    const wchar_t* key,
    int x,
    int y,
    int width,
    int height)
{
    ActionArea area;
    area.type = type ? type : L"";
    area.key = key ? key : L"";
    area.x = x;
    area.y = y;
    area.width = width;
    area.height = height;
    return area;
}

const ActionArea* HitTestArea(
    const std::vector<ActionArea>& areas,
    int x,
    int y)
{
    const ActionArea* best = nullptr;
    int bestPriority = -1;

    for (const ActionArea& area : areas)
    {
        if (x >= area.x && x < area.x + area.width &&
            y >= area.y && y < area.y + area.height)
        {
            // Priority: button (2) > item (1) > anything else (0).
            // This ensures buttons are always hit-testable even when
            // item rects extend past the view area bottom margin.
            int priority = 0;
            if (area.type == L"button") priority = 2;
            else if (area.type == L"item") priority = 1;

            if (priority > bestPriority)
            {
                best = &area;
                bestPriority = priority;
            }
        }
    }

    return best;
}

void PaintViewArea(
    Gdiplus::Graphics& graphics,
    int width,
    int height,
    const ViewArea& viewArea,
    Gdiplus::Color color)
{
    const float x = static_cast<float>(viewArea.left);
    const float y = static_cast<float>(viewArea.top);
    const float w = static_cast<float>(
        width - viewArea.left - viewArea.right);
    const float h = static_cast<float>(
        height - viewArea.top - viewArea.bottom);
    const float r = static_cast<float>(viewArea.radius);

    Gdiplus::SolidBrush brush(color);

    if (r > 0)
    {
        const float diameter = r * 2.0f;

        Gdiplus::GraphicsPath path;

        path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(x + w - diameter, y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(x + w - diameter, y + h - diameter, diameter, diameter, 0.0f, 90.0f);
        path.AddArc(x, y + h - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();

        graphics.FillPath(&brush, &path);
    }
    else
    {
        graphics.FillRectangle(&brush, x, y, w, h);
    }
}

std::vector<ActionArea> GetItemsAreas(
    const std::vector<Item>& items,
    const ViewArea& viewArea,
    int windowWidth)
{
    std::vector<ActionArea> areas;

    const int availableWidth = windowWidth - viewArea.left - viewArea.right;
    const int itemsPerRow = availableWidth / ItemSize::width;
    const int columns = itemsPerRow > 0 ? itemsPerRow : 1;

    for (size_t i = 0; i < items.size(); ++i)
    {
        const Item& item = items[i];

        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;

        const int x = viewArea.left + col * ItemSize::width;
        const int y = viewArea.top + row * ItemSize::height;

        std::wstring key;
        if (item.children.empty())
        {
            key = item.path;
        }
        else
        {
            key = item.name;
        }

        areas.push_back(CreateArea(
            L"item",
            key.c_str(),
            x,
            y,
            ItemSize::width,
            ItemSize::height));
    }

    return areas;
}
