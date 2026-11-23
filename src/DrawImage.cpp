#include "DrawImage.h"

#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "ActionArea.h"
#include "List.h"

// ========================================
// Draw PNG
// ========================================

// Shared blit: draw a source rect of a decoded PNG into a destination rect
static bool DrawPngImage(
    Gdiplus::Graphics& graphics,
    Gdiplus::Image& image,
    int sourceX,
    int sourceY,
    int sourceWidth,
    int sourceHeight,
    int destinationX,
    int destinationY,
    int destinationWidth,
    int destinationHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        destinationWidth <= 0 || destinationHeight <= 0)
    {
        return false;
    }

    if (image.GetLastStatus() != Gdiplus::Ok)
        return false;

    const Gdiplus::Status status = graphics.DrawImage(
        &image,
        Gdiplus::Rect(
            destinationX,
            destinationY,
            destinationWidth,
            destinationHeight),
        sourceX,
        sourceY,
        sourceWidth,
        sourceHeight,
        Gdiplus::UnitPixel);

    return status == Gdiplus::Ok;
}

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
    int destinationHeight)
{
    if (!pngPath)
        return false;

    Gdiplus::Image image(pngPath);

    return DrawPngImage(
        graphics, image,
        sourceX, sourceY, sourceWidth, sourceHeight,
        destinationX, destinationY, destinationWidth, destinationHeight);
}

// Decoded PNG resources, one image per resource id, alive for the whole
// process. Decoding a PNG from the resource table costs far more than
// blitting it, and every scrolled frame redraws these icons.
struct CachedPngResource
{
    int id;
    Gdiplus::Image* image;
};

static std::vector<CachedPngResource> g_cachedPngResources;

// Return the decoded resource, decoding and caching it on first use. The
// returned image is owned by g_cachedPngResources and stays alive until exit.
static Gdiplus::Image* GetPngResourceImage(int resourceId)
{
    for (const CachedPngResource& entry : g_cachedPngResources)
    {
        if (entry.id == resourceId)
            return entry.image;
    }

    // MAKEINTRESOURCEW(10) is RT_RCDATA. Spelling it out keeps this a wide
    // call even when a toolchain (or the IDE's parser) omits -DUNICODE, which
    // would otherwise expand RT_RCDATA to its char* form.
    const HRSRC info = FindResourceW(
        nullptr, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));

    if (!info)
        return nullptr;

    const DWORD size = SizeofResource(nullptr, info);

    if (size == 0)
        return nullptr;

    const HGLOBAL loaded = LoadResource(nullptr, info);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;

    if (!bytes)
        return nullptr;

    // GDI+ decodes from a stream, so copy the resource into an HGLOBAL
    // and wrap it (CreateStreamOnHGlobal takes ownership when fDeleteOnRelease)
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);

    if (!memory)
        return nullptr;

    void* target = GlobalLock(memory);

    if (!target)
    {
        GlobalFree(memory);
        return nullptr;
    }

    memcpy(target, bytes, size);
    GlobalUnlock(memory);

    IStream* stream = nullptr;

    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)) || !stream)
    {
        GlobalFree(memory);
        return nullptr;
    }

    Gdiplus::Image* image = new Gdiplus::Image(stream);
    stream->Release();

    if (image->GetLastStatus() != Gdiplus::Ok)
    {
        delete image;
        return nullptr;
    }

    g_cachedPngResources.push_back({resourceId, image});
    return image;
}

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
    int destinationHeight)
{
    Gdiplus::Image* image = GetPngResourceImage(resourceId);

    if (!image)
        return false;

    return DrawPngImage(
        graphics, *image,
        sourceX, sourceY, sourceWidth, sourceHeight,
        destinationX, destinationY, destinationWidth, destinationHeight);
}

HICON CreatePngResourceIcon(int resourceId, int size)
{
    Gdiplus::Image* image = GetPngResourceImage(resourceId);

    if (!image || size <= 0)
        return nullptr;

    // Render at the exact size requested by Windows (16px for the tray,
    // usually 32px for the taskbar), then let GDI+ build an alpha-aware HICON.
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppPARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    if (graphics.DrawImage(
            image,
            Gdiplus::Rect(0, 0, size, size),
            0, 0,
            static_cast<int>(image->GetWidth()),
            static_cast<int>(image->GetHeight()),
            Gdiplus::UnitPixel) != Gdiplus::Ok)
    {
        return nullptr;
    }

    HICON icon = nullptr;

    if (bitmap.GetHICON(&icon) != Gdiplus::Ok)
        return nullptr;

    return icon;
}

// ========================================
// Draw file/icon (resource-manager icon for any path, file or folder)
// ========================================

// ========================================
// Icon tile cache
// ========================================

// Extracting a shell icon costs a shell call plus two GetDIBits round trips
// and a rescale, and every scrolled frame redraws every visible icon. The
// finished tile (premultiplied BGRA, already at its final size) is kept per
// path+size, leaving only the small SrcOver composite per frame.
struct CachedIconTile
{
    std::wstring path;
    int width;
    int height;
    std::vector<unsigned char> pixels;
};

static std::vector<CachedIconTile> g_cachedIconTiles;

static const std::vector<unsigned char>* FindCachedIconTile(
    const wchar_t* path,
    int width,
    int height)
{
    for (const CachedIconTile& entry : g_cachedIconTiles)
    {
        if (entry.width == width && entry.height == height &&
            _wcsicmp(entry.path.c_str(), path) == 0)
        {
            return &entry.pixels;
        }
    }

    return nullptr;
}

static void CacheIconTile(
    const wchar_t* path,
    int width,
    int height,
    const unsigned char* pixels)
{
    constexpr size_t MAX_CACHED_ICON_TILES = 512;

    if (g_cachedIconTiles.size() >= MAX_CACHED_ICON_TILES)
        g_cachedIconTiles.clear();

    CachedIconTile entry;
    entry.path = path;
    entry.width = width;
    entry.height = height;
    entry.pixels.assign(pixels,
        pixels + static_cast<size_t>(width) * height * 4);
    g_cachedIconTiles.push_back(std::move(entry));
}

// SrcOver one already-clipped destination span into the frame buffer.
static void CompositeIconSpan(
    const unsigned char* tile,
    int tileWidth,
    int destinationX,
    int destinationY,
    int left,
    int top,
    int right,
    int bottom)
{
    extern void* g_frameBits;
    extern int g_frameWidth;

    unsigned char* frame = static_cast<unsigned char*>(g_frameBits);

    for (int y = top; y < bottom; ++y)
    {
        const unsigned char* s = tile +
            (static_cast<size_t>(y - destinationY) * tileWidth +
             (left - destinationX)) * 4;
        unsigned char* d = frame +
            (static_cast<size_t>(y) * g_frameWidth + left) * 4;

        for (int x = left; x < right; ++x)
        {
            const unsigned char sa = s[3];

            if (sa == 255)
            {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            }
            else if (sa != 0)
            {
                const int invSa = 255 - sa;
                d[0] = static_cast<unsigned char>(
                    s[0] + static_cast<int>(d[0]) * invSa / 255);
                d[1] = static_cast<unsigned char>(
                    s[1] + static_cast<int>(d[1]) * invSa / 255);
                d[2] = static_cast<unsigned char>(
                    s[2] + static_cast<int>(d[2]) * invSa / 255);
                d[3] = static_cast<unsigned char>(
                    sa + static_cast<int>(d[3]) * invSa / 255);
            }

            s += 4;
            d += 4;
        }
    }
}

// SrcOver one premultiplied icon tile directly onto the frame buffer. The
// DC clip region is still honoured, including the rounded viewArea region.
static bool CompositeIconTile(
    const unsigned char* tile,
    int width,
    int height,
    int destinationX,
    int destinationY)
{
    extern HDC g_frameDC;
    extern void* g_frameBits;
    extern int g_frameWidth;
    extern int g_frameHeight;

    if (!tile || width <= 0 || height <= 0 ||
        !g_frameDC || !g_frameBits ||
        g_frameWidth <= 0 || g_frameHeight <= 0)
    {
        return false;
    }

    const int boundsLeft = destinationX > 0 ? destinationX : 0;
    const int boundsTop = destinationY > 0 ? destinationY : 0;
    const int boundsRight = destinationX + width < g_frameWidth
        ? destinationX + width : g_frameWidth;
    const int boundsBottom = destinationY + height < g_frameHeight
        ? destinationY + height : g_frameHeight;

    if (boundsLeft >= boundsRight || boundsTop >= boundsBottom)
        return true;

    HRGN clipRegion = CreateRectRgn(0, 0, 0, 0);

    if (!clipRegion)
        return false;

    const int clipResult = GetClipRgn(g_frameDC, clipRegion);

    if (clipResult < 0)
    {
        DeleteObject(clipRegion);
        return false;
    }

    if (clipResult == 0)
    {
        CompositeIconSpan(tile, width, destinationX, destinationY,
            boundsLeft, boundsTop, boundsRight, boundsBottom);
        DeleteObject(clipRegion);
        return true;
    }

    const DWORD dataSize = GetRegionData(clipRegion, 0, nullptr);
    bool composited = true;

    if (dataSize > 0)
    {
        std::vector<unsigned char> bytes(dataSize);
        RGNDATA* regionData = reinterpret_cast<RGNDATA*>(bytes.data());

        if (GetRegionData(clipRegion, dataSize, regionData) == dataSize)
        {
            const RECT* rects =
                reinterpret_cast<const RECT*>(regionData->Buffer);

            for (DWORD i = 0; i < regionData->rdh.nCount; ++i)
            {
                const int left = rects[i].left > boundsLeft
                    ? rects[i].left : boundsLeft;
                const int top = rects[i].top > boundsTop
                    ? rects[i].top : boundsTop;
                const int right = rects[i].right < boundsRight
                    ? rects[i].right : boundsRight;
                const int bottom = rects[i].bottom < boundsBottom
                    ? rects[i].bottom : boundsBottom;

                if (left < right && top < bottom)
                {
                    CompositeIconSpan(tile, width,
                        destinationX, destinationY,
                        left, top, right, bottom);
                }
            }
        }
        else
        {
            composited = false;
        }
    }

    DeleteObject(clipRegion);
    return composited;
}

// Extracted icon pixels do not depend on the size the icon is drawn at.
// A group card paints its children at 13px, so without a size-independent
// cache, opening that group would have to query the shell again for every
// child at 32px — a visible stall right as the transition starts. Sharing
// the extracted sources lets every size reuse one shell call.
struct IconSource
{
    std::wstring path;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels; // premultiplied BGRA, top-down
};

// Sources are shared between the UI thread and the prefetch thread, so they
// are immutable once inserted and handed out as shared_ptr.
struct IconCacheState
{
    CRITICAL_SECTION lock;
    std::vector<std::shared_ptr<const IconSource>> sources;
    std::vector<std::wstring> attempted;
    std::deque<std::wstring> queue;
    HANDLE wake = nullptr;
    bool workerStarted = false;
};

static IconCacheState& IconCache()
{
    // Intentionally leaked: the prefetch thread may still be running while
    // the process tears down its other static objects.
    static IconCacheState* state = [] {
        IconCacheState* created = new IconCacheState();
        InitializeCriticalSection(&created->lock);
        created->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return created;
    }();

    return *state;
}

// -------- Step 1: Get Shell HICON --------
static bool ExtractIconSource(const wchar_t* exePath, IconSource& out)
{
    out.width = 0;
    out.height = 0;
    out.pixels.clear();

    if (!exePath)
        return false;

    out.path = exePath;

    HICON iconHandle = nullptr;
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(
            exePath, 0, &sfi, sizeof(sfi),
            SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon)
    {
        iconHandle = sfi.hIcon;
    }
    else
    {
        if (SHGetFileInfoW(
                exePath, 0, &sfi, sizeof(sfi),
                SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)
            && sfi.hIcon)
        {
            iconHandle = sfi.hIcon;
        }
    }

    if (!iconHandle)
        return false;

    // -------- Step 2: Extract hbmColor + GetDIBits BEFORE touching DDB DCs --------
    // This is the critical fix for black-edge artifacts. If we SelectObject
    // the 32bpp DIBSection (hbmColor) into a CreateCompatibleDC (DDB), GDI
    // may convert the alpha channel into a 1bpp mask interpretation during
    // the DIB→DDB transition, zeroing RGB on semi-transparent edge pixels.
    // GetDIBits reads the raw pixel buffer directly with zero conversion.
    ICONINFO iinfo = {};
    if (!GetIconInfo(iconHandle, &iinfo))
    {
        DestroyIcon(iconHandle);
        return false;
    }
    DestroyIcon(iconHandle);

    if (!iinfo.hbmColor)
    {
        if (iinfo.hbmMask) DeleteObject(iinfo.hbmMask);
        return false;
    }

    const HDC screen = GetDC(nullptr);

    // GetDIBits — read hbmColor's raw pixels directly.
    BITMAPINFOHEADER srcBIH = {};
    srcBIH.biSize = sizeof(BITMAPINFOHEADER);
    // First call to GetDIBits returns the bitmap's dimensions
    if (!GetDIBits(screen, iinfo.hbmColor, 0, 0, nullptr,
                   reinterpret_cast<BITMAPINFO*>(&srcBIH), DIB_RGB_COLORS))
    {
        DeleteObject(iinfo.hbmColor);
        if (iinfo.hbmMask) DeleteObject(iinfo.hbmMask);
        ReleaseDC(nullptr, screen);
        return false;
    }

    const int srcW = srcBIH.biWidth;
    const int srcH = srcBIH.biHeight < 0 ? -srcBIH.biHeight : srcBIH.biHeight;
    // Normalize to top-down
    srcBIH.biHeight = -srcH;

    const size_t srcPixelBytes = static_cast<size_t>(srcW) * srcH * 4;
    std::vector<unsigned char> srcPixels(srcPixelBytes);

    if (!GetDIBits(screen, iinfo.hbmColor, 0, static_cast<UINT>(srcH),
                   srcPixels.data(),
                   reinterpret_cast<BITMAPINFO*>(&srcBIH), DIB_RGB_COLORS))
    {
        DeleteObject(iinfo.hbmColor);
        if (iinfo.hbmMask) DeleteObject(iinfo.hbmMask);
        ReleaseDC(nullptr, screen);
        return false;
    }

    DeleteObject(iinfo.hbmColor);
    if (iinfo.hbmMask) DeleteObject(iinfo.hbmMask);

    // -------- Step 3: Normalize srcPixels to premultiplied BGRA --------
    // GetDIBits with DIB_RGB_COLORS returns BGRA (32bpp). But we don't
    // know whether the source icon was premultiplied or straight alpha.
    // Normalize: if straight alpha, premultiply now.
    // Premultiplied invariant: for every pixel, max(R,G,B) <= A.
    bool isPremult = true;
    for (size_t i = 0; i < srcPixelBytes; i += 4)
    {
        unsigned char a = srcPixels[i + 3];
        if (a == 0) continue;
        if (srcPixels[i] > a || srcPixels[i + 1] > a || srcPixels[i + 2] > a)
        {
            isPremult = false;
            break;
        }
    }

    if (!isPremult)
    {
        for (size_t i = 0; i < srcPixelBytes; i += 4)
        {
            unsigned char a = srcPixels[i + 3];
            if (a == 0)
            {
                srcPixels[i] = srcPixels[i + 1] = srcPixels[i + 2] = 0;
            }
            else if (a < 255)
            {
                srcPixels[i]     = static_cast<unsigned char>(
                    static_cast<unsigned>(srcPixels[i])     * a / 255);
                srcPixels[i + 1] = static_cast<unsigned char>(
                    static_cast<unsigned>(srcPixels[i + 1]) * a / 255);
                srcPixels[i + 2] = static_cast<unsigned char>(
                    static_cast<unsigned>(srcPixels[i + 2]) * a / 255);
            }
        }
    }

    out.width = srcW;
    out.height = srcH;
    out.pixels = std::move(srcPixels);

    ReleaseDC(nullptr, screen);
    return true;
}

// -------- Step 4: Scale the cached source to the drawn size --------
// Pure memory ops in premultiplied BGRA space — zero GDI/DIB/DDB
// conversion risk, always works.
static bool BuildIconTile(
    const IconSource& source,
    int width,
    int height,
    std::vector<unsigned char>& out)
{
    const int srcW = source.width;
    const int srcH = source.height;

    if (srcW <= 0 || srcH <= 0 || width <= 0 || height <= 0)
        return false;

    if (source.pixels.size() !=
        static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4)
    {
        return false;
    }

    out.assign(static_cast<size_t>(width) * height * 4, 0);

    if (srcW == width && srcH == height)
    {
        // Exact match — direct memcpy
        memcpy(out.data(), source.pixels.data(), out.size());
        return true;
    }

    unsigned char* dst = out.data();
    const int srcStride = srcW * 4;

    for (int y = 0; y < height; ++y)
    {
        // Map dst y to src range [sy0, sy1)
        const int sy0 = y * srcH / height;
        int sy1 = (y + 1) * srcH / height;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > srcH) sy1 = srcH;
        const int syCount = sy1 - sy0;

        for (int x = 0; x < width; ++x)
        {
            const int sx0 = x * srcW / width;
            int sx1 = (x + 1) * srcW / width;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > srcW) sx1 = srcW;

            unsigned int sumB = 0, sumG = 0, sumR = 0, sumA = 0;
            const int total = syCount * (sx1 - sx0);

            for (int sy = sy0; sy < sy1; ++sy)
            {
                const unsigned char* srcRow =
                    source.pixels.data() + sy * srcStride;
                for (int sx = sx0; sx < sx1; ++sx)
                {
                    const unsigned char* p = srcRow + sx * 4;
                    sumB += p[0]; sumG += p[1];
                    sumR += p[2]; sumA += p[3];
                }
            }

            if (total > 0)
            {
                dst[0] = static_cast<unsigned char>(sumB / total);
                dst[1] = static_cast<unsigned char>(sumG / total);
                dst[2] = static_cast<unsigned char>(sumR / total);
                dst[3] = static_cast<unsigned char>(sumA / total);
            }
            dst += 4;
        }
    }

    return true;
}

static bool IsAttemptedPath(
    const IconCacheState& state,
    const std::wstring& path)
{
    for (const std::wstring& tried : state.attempted)
    {
        if (_wcsicmp(tried.c_str(), path.c_str()) == 0)
            return true;
    }

    return false;
}

static std::shared_ptr<const IconSource> FindIconSource(const wchar_t* path)
{
    IconCacheState& state = IconCache();
    std::shared_ptr<const IconSource> found;

    EnterCriticalSection(&state.lock);

    for (const auto& source : state.sources)
    {
        if (_wcsicmp(source->path.c_str(), path) == 0)
        {
            found = source;
            break;
        }
    }

    LeaveCriticalSection(&state.lock);
    return found;
}

static void StoreIconSource(std::shared_ptr<const IconSource> source)
{
    IconCacheState& state = IconCache();

    EnterCriticalSection(&state.lock);

    if (!IsAttemptedPath(state, source->path))
        state.attempted.push_back(source->path);

    state.sources.push_back(std::move(source));
    LeaveCriticalSection(&state.lock);
}

// Prefetch worker: extracts icons below normal priority so warming the cache
// never competes with the UI thread or the app the launcher was opened on.
static DWORD WINAPI IconPrefetchThread(LPVOID)
{
    // SHGetFileInfoW can reach the shell namespace, so give the worker its
    // own apartment. The thread lives until the process exits, which is why
    // CoUninitialize is never paired with this call.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IconCacheState& state = IconCache();

    for (;;)
    {
        std::wstring path;

        EnterCriticalSection(&state.lock);

        while (!state.queue.empty())
        {
            std::wstring candidate = std::move(state.queue.front());
            state.queue.pop_front();

            if (IsAttemptedPath(state, candidate))
                continue;

            state.attempted.push_back(candidate);
            path = std::move(candidate);
            break;
        }

        LeaveCriticalSection(&state.lock);

        if (path.empty())
        {
            // No periodic wake-ups: every enqueue signals this event.
            WaitForSingleObject(state.wake, INFINITE);
            continue;
        }

        IconSource extracted;

        if (ExtractIconSource(path.c_str(), extracted))
        {
            auto source = std::make_shared<const IconSource>(
                std::move(extracted));

            EnterCriticalSection(&state.lock);
            state.sources.push_back(source);
            LeaveCriticalSection(&state.lock);
        }
    }

    return 0;
}

void PrefetchIconSource(const std::wstring& path)
{
    if (path.empty() ||
        _wcsnicmp(path.c_str(), L"http://", 7) == 0 ||
        _wcsnicmp(path.c_str(), L"https://", 8) == 0)
    {
        return;
    }

    IconCacheState& state = IconCache();

    EnterCriticalSection(&state.lock);

    if (!state.workerStarted)
    {
        HANDLE worker = CreateThread(
            nullptr, 0, IconPrefetchThread, nullptr, 0, nullptr);

        if (worker)
        {
            SetThreadPriority(worker, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(worker);
            state.workerStarted = true;
        }
    }

    state.queue.push_back(path);
    LeaveCriticalSection(&state.lock);

    SetEvent(state.wake);
}

bool DrawExeIcon(
    Gdiplus::Graphics& graphics,
    const wchar_t* exePath,
    int destinationX,
    int destinationY,
    int destinationWidth,
    int destinationHeight)
{
    (void)graphics; // reserved for future GDI+-only pipeline

    if (!exePath || destinationWidth <= 0 || destinationHeight <= 0)
        return false;

    // Cached tile: skip the shell call and pixel extraction entirely.
    if (const std::vector<unsigned char>* cachedIcon =
            FindCachedIconTile(exePath, destinationWidth, destinationHeight))
    {
        return CompositeIconTile(
            cachedIcon->data(),
            destinationWidth, destinationHeight,
            destinationX, destinationY);
    }

    // The shell extraction is the expensive part and is size-independent,
    // so a tile for any size can be built from an already extracted source.
    std::shared_ptr<const IconSource> source = FindIconSource(exePath);

    if (!source)
    {
        IconSource extracted;

        if (!ExtractIconSource(exePath, extracted))
            return false;

        source = std::make_shared<const IconSource>(std::move(extracted));
        StoreIconSource(source);
    }

    std::vector<unsigned char> tile;

    if (!BuildIconTile(*source, destinationWidth, destinationHeight, tile))
        return false;

    CacheIconTile(exePath, destinationWidth, destinationHeight, tile.data());

    return CompositeIconTile(
        tile.data(), destinationWidth, destinationHeight,
        destinationX, destinationY);
}

// ========================================
// Scrollbar
// ========================================

void DrawScrollbar(
    Gdiplus::Graphics& graphics,
    float x,
    float y,
    float width,
    float height)
{
    if (width <= 0.0f || height <= 0.0f)
        return;

    // Capsule: corner radius = half the thumb width (clamped so the
    // arcs never overlap when the thumb is very short).
    Gdiplus::REAL radius = width * 0.5f;
    if (radius * 2.0f > height)
        radius = height * 0.5f;
    const Gdiplus::REAL diameter = radius * 2.0f;

    const Gdiplus::RectF rect(x, y, width, height);

    Gdiplus::GraphicsPath path;
    path.AddArc(rect.X, rect.Y,
        diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y,
        diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
        diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter,
        diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();

    // argb(0.2, 0, 0, 0) ≈ alpha 51
    Gdiplus::SolidBrush brush(Gdiplus::Color(51, 0, 0, 0));
    graphics.FillPath(&brush, &path);
}

// ========================================
// Insertion line
// ========================================

void DrawInsertionLine(
    Gdiplus::Graphics& graphics,
    const ActionArea& area,
    InsertionSide side)
{
    constexpr Gdiplus::REAL BAR_WIDTH = 16.0f;  // full bar width
    constexpr Gdiplus::REAL CUT_WIDTH = 6.0f;   // side strip width
    constexpr Gdiplus::REAL CUT_RADIUS = 6.0f;  // inner corner radius

    if (area.height <= 0 || area.width < 0)
        return;

    // The chosen edge of the area lands exactly in the middle of the bar
    const Gdiplus::REAL edge = side == InsertionSide::Left
        ? static_cast<Gdiplus::REAL>(area.x)
        : static_cast<Gdiplus::REAL>(area.x + area.width);
    const Gdiplus::REAL left = edge - BAR_WIDTH * 0.5f;
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(area.y);
    const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(area.height);

    const Gdiplus::REAL d = CUT_RADIUS * 2.0f;

    // One path holding three figures: the full 16px bar plus the two
    // side strips that must be cut out. FillModeAlternate makes the two
    // strips (fully inside the bar) cancel out, i.e. they are clipped.
    Gdiplus::GraphicsPath path;
    path.SetFillMode(Gdiplus::FillModeAlternate);

    // Outer bar: [left, left+16] x [top, top+height]
    path.AddRectangle(Gdiplus::RectF(left, top, BAR_WIDTH, height));

    // Left strip [left, left+6]: square left edge, only the top-right
    // and bottom-right corners rounded (radius 6)
    path.StartFigure();
    path.AddArc(Gdiplus::RectF(left - CUT_RADIUS, top, d, d), 270.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(left - CUT_RADIUS,
            top + height - d, d, d), 0.0f, 90.0f);
    path.CloseFigure();

    // Right strip [left+10, left+16]: square right edge, only the
    // top-left and bottom-left corners rounded (radius 6)
    path.StartFigure();
    path.AddArc(Gdiplus::RectF(left + BAR_WIDTH - CUT_WIDTH, top, d, d),
        180.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(left + BAR_WIDTH - CUT_WIDTH,
            top + height - d, d, d), 90.0f, 90.0f);
    path.CloseFigure();

    // argb(0.2, 46, 175, 240)
    Gdiplus::SolidBrush brush(Gdiplus::Color(51, 46, 175, 240));
    graphics.FillPath(&brush, &path);
}

// ========================================
// Hierarchy name strip
// ========================================

void DrawHierarchyName(
    Gdiplus::Graphics& graphics,
    const Gdiplus::Rect& stripRect,
    const std::wstring& displayName,
    bool isDarkTheme)
{
    const Gdiplus::Color textColor = isDarkTheme
        ? Gdiplus::Color(255, 255, 255, 255)
        : Gdiplus::Color(255, 0x33, 0x33, 0x33);

    Gdiplus::Font font(L"Microsoft YaHei", 13.0f, Gdiplus::FontStyleBold,
        Gdiplus::UnitPoint);
    Gdiplus::SolidBrush brush(textColor);
    Gdiplus::StringFormat sfmt;
    sfmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    // Draw the text centred horizontally and vertically in the strip.
    const Gdiplus::RectF textRect(
        static_cast<Gdiplus::REAL>(stripRect.X),
        static_cast<Gdiplus::REAL>(stripRect.Y),
        static_cast<Gdiplus::REAL>(stripRect.Width),
        static_cast<Gdiplus::REAL>(stripRect.Height));
    graphics.DrawString(displayName.c_str(), -1, &font, textRect, &sfmt,
        &brush);
}

// ========================================
// Back button
// ========================================

void DrawBackBottom(
    Gdiplus::Graphics& graphics,
    bool isDarkTheme)
{
    const Gdiplus::Color color = isDarkTheme
        ? Gdiplus::Color(255, 255, 255, 255)
        : Gdiplus::Color(255, 0x33, 0x33, 0x33);

    // Two rotated rounded rectangles forming a left-pointing chevron.
    // The two arms have independent y values so the chevron angle can
    // be fine-tuned. Geometry settled from live JSON tuning.
    constexpr float RECT_X = 10.3f;
    constexpr float RECT_Y_TOP = 18.5f;
    constexpr float RECT_Y_BOTTOM = 17.5f;
    constexpr float RECT_W = 13.5f;
    constexpr float RECT_H = 3.0f;
    constexpr float RECT_RADIUS = 2.0f;

    Gdiplus::SolidBrush brush(color);
    Gdiplus::Matrix saveMatrix;
    graphics.GetTransform(&saveMatrix);

    const float diameter = RECT_RADIUS * 2.0f;

    auto drawArm = [&](float rectY, float rotation) {
        Gdiplus::GraphicsPath path;
        const Gdiplus::RectF r(RECT_X, rectY, RECT_W, RECT_H);

        path.AddArc(r.X, r.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(r.GetRight() - diameter, r.Y,
            diameter, diameter, 270.0f, 90.0f);
        path.AddArc(r.GetRight() - diameter, r.GetBottom() - diameter,
            diameter, diameter, 0.0f, 90.0f);
        path.AddArc(r.X, r.GetBottom() - diameter,
            diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();

        Gdiplus::PointF pivot(RECT_X, rectY + RECT_H * 0.5f);

        graphics.SetTransform(&saveMatrix);
        graphics.TranslateTransform(pivot.X, pivot.Y);
        graphics.RotateTransform(rotation);
        graphics.TranslateTransform(-pivot.X, -pivot.Y);
        graphics.FillPath(&brush, &path);
    };

    // Top arm rotates -45° (clockwise), bottom arm rotates +45°
    // (counter-clockwise) so they converge into a left-pointing chevron
    drawArm(RECT_Y_TOP,    -45.0f);
    drawArm(RECT_Y_BOTTOM, +45.0f);

    graphics.SetTransform(&saveMatrix);
}

// ========================================
// Trash
// ========================================

void DrawTrash(
    Gdiplus::Graphics& graphics,
    const Gdiplus::Rect& area,
    bool isItemDragging,
    bool isDarkTheme)
{
    // Both sheets ship inside the executable as RCDATA resources.
    const int resourceId = isItemDragging
        ? ASSET_TRASH_ON
        : ASSET_TRASH;

    // Each sheet holds two theme halves side by side: the light variant
    // on the left, the dark one on the right.
    const int srcW = 20;
    const int srcH = isItemDragging ? 21 : 20;
    const int srcX = isDarkTheme ? srcW : 0;

    // Anchor to the area's bottom-right corner. The "dragging" image is
    // 1px taller, so it grows upward from the same bottom edge.
    const int dstX = area.X + area.Width - srcW;
    const int dstY = area.Y + area.Height - srcH;

    DrawPngResource(graphics, resourceId,
        srcX, 0, srcW, srcH,
        dstX, dstY, srcW, srcH);
}

// ========================================
// Launcher item
// ========================================

static bool IsWebIconPath(const std::wstring& path)
{
    return _wcsnicmp(path.c_str(), L"http://", 7) == 0 ||
        _wcsnicmp(path.c_str(), L"https://", 8) == 0;
}

void DrawItem(
    Gdiplus::Graphics& graphics,
    const Item& item,
    int areaX,
    float areaY,
    int areaWidth,
    int areaHeight,
    bool isDarkTheme,
    bool isDragTarget,
    bool hideName)
{
    (void)areaHeight; // reserved for future layout adjustments
    const Gdiplus::Color textColor = isDarkTheme
        ? Gdiplus::Color(255, 255, 255, 255)
        : Gdiplus::Color(255, 0x33, 0x33, 0x33);

    // One font for every item; constructing a GDI+ Font per item per frame
    // costs more than it looks. Intentionally leaked so its destructor never
    // runs after GdiplusShutdown.
    static Gdiplus::Font* const itemFont = new Gdiplus::Font(
        L"Microsoft YaHei", 10.0f, Gdiplus::FontStyleRegular,
        Gdiplus::UnitPoint);

    const Gdiplus::REAL lineHeight = itemFont->GetHeight(&graphics);
    const Gdiplus::REAL twoLineHeight = lineHeight * 2;

    Gdiplus::SolidBrush brush(textColor);

    Gdiplus::StringFormat sfmt;
    sfmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    // Shared text drawer — same formatting for leaf (icon label) and
    // folder (just text, no icon). areaY+54 sits flush below the 32px
    // icon that starts at areaY+20 in the leaf branch.
    auto drawName = [&](const std::wstring& name) {
        if (hideName) return;
        if (name.empty()) return;
        const Gdiplus::REAL top = areaY + 50.0f;
        const Gdiplus::RectF textRect(
            static_cast<Gdiplus::REAL>(areaX),
            top,
            static_cast<Gdiplus::REAL>(areaWidth),
            twoLineHeight);
        graphics.DrawString(name.c_str(), -1,
            itemFont, textRect, &sfmt, &brush);
    };

    if (item.children.empty())
    {
        // Leaf: web links use the theme half of web.png; other paths
        // keep their shell icon.
        if (!item.path.empty())
        {
            const int iconX = areaX + 32;
            const int iconY = static_cast<int>(areaY + 16.0f + 0.5f);

            if (IsWebIconPath(item.path))
            {
                DrawPngResource(graphics, ASSET_WEB,
                    isDarkTheme ? 32 : 0, 0, 32, 32,
                    iconX, iconY, 32, 32);
            }
            else
            {
                DrawExeIcon(graphics, item.path.c_str(),
                    iconX, iconY, 32, 32);
            }
        }

        drawName(item.name);
    }
    else
    {
        // Category card: rounded-rect container (same position/size as
        // the leaf branch icon — areaX+32, areaY+16, 32×32) with up
        // to 4 child icons in a 2×2 grid, then category name below.
        constexpr int CHILD_GAP = 2;     // edge padding + icon spacing
        constexpr int CARD_W = 32;       // same as leaf icon width
        constexpr int CARD_H = 32;       // same as leaf icon height
        constexpr int CARD_RADIUS = 6;
        constexpr int CHILD_GRID_COLS = 2;
        constexpr int CHILD_GRID_ROWS = 2;
        // iconSize derived from CARD_W/H: 2*iconSize + 1*gap + 2*gap = 32
        constexpr int CHILD_ICON_SIZE =
            (CARD_W - (CHILD_GRID_COLS - 1) * CHILD_GAP - 2 * CHILD_GAP)
            / CHILD_GRID_COLS;

        const float cardX = static_cast<float>(areaX + 32);
        const float cardY = areaY + 16.0f;

        // Rounded-rect background (radius=CARD_RADIUS, argb=0.1,0,0,0)
        {
            const Gdiplus::REAL diameter =
                static_cast<Gdiplus::REAL>(CARD_RADIUS * 2);
            const Gdiplus::RectF cardRect(
                static_cast<Gdiplus::REAL>(cardX),
                static_cast<Gdiplus::REAL>(cardY),
                static_cast<Gdiplus::REAL>(CARD_W),
                static_cast<Gdiplus::REAL>(CARD_H));

            Gdiplus::GraphicsPath cardPath;
            cardPath.AddArc(cardRect.X, cardRect.Y,
                diameter, diameter, 180.0f, 90.0f);
            cardPath.AddArc(cardRect.GetRight() - diameter, cardRect.Y,
                diameter, diameter, 270.0f, 90.0f);
            cardPath.AddArc(cardRect.GetRight() - diameter,
                cardRect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
            cardPath.AddArc(cardRect.X, cardRect.GetBottom() - diameter,
                diameter, diameter, 90.0f, 90.0f);
            cardPath.CloseFigure();

            Gdiplus::SolidBrush cardBrush(
                Gdiplus::Color(25, 0, 0, 0)); // ~10% alpha
            graphics.FillPath(&cardBrush, &cardPath);

            // Drag target: argb(1, 46, 175, 240) outline, 2px wide
            if (isDragTarget)
            {
                Gdiplus::Pen dragPen(
                    Gdiplus::Color(255, 46, 175, 240), 2.0f);
                graphics.DrawPath(&dragPen, &cardPath);
            }
        }

        // Draw up to 4 leaf children in the grid
        int childIdx = 0;
        for (int row = 0; row < CHILD_GRID_ROWS &&
             childIdx < static_cast<int>(item.children.size()); ++row)
        {
            for (int col = 0; col < CHILD_GRID_COLS &&
                 childIdx < static_cast<int>(item.children.size()); ++col)
            {
                const Item& child = item.children[childIdx];
                if (child.path.empty())
                {
                    ++childIdx;
                    continue;
                }

                const int ix = areaX + 32 + CHILD_GAP
                    + col * (CHILD_ICON_SIZE + CHILD_GAP);
                const int iy = static_cast<int>(cardY + CHILD_GAP
                    + row * (CHILD_ICON_SIZE + CHILD_GAP) + 0.5f);

                if (IsWebIconPath(child.path))
                {
                    DrawPngResource(graphics, ASSET_WEB,
                        isDarkTheme ? 32 : 0, 0, 32, 32,
                        ix, iy, CHILD_ICON_SIZE, CHILD_ICON_SIZE);
                }
                else
                {
                    DrawExeIcon(graphics, child.path.c_str(),
                        ix, iy, CHILD_ICON_SIZE, CHILD_ICON_SIZE);
                }

                ++childIdx;
            }
        }

        drawName(item.name);
    }
}
