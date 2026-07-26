// UniPaste application icon: the mark IS the product. A lowercase Cyrillic
// "a" - U+0430, a letter that reads as a Latin "a" but is not one - drawn in
// the amber brand accent on a dark rounded tile. Nothing else: no wordmark, no
// border flourish, no second colour.
//
// The icon is rendered with plain GDI at whatever size the caller asks for, so
// the 16 px tray glyph is drawn at 16 px rather than downsampled from one
// oversized bitmap, and the build carries no resource script.

#include "appicon.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

namespace appicon {
namespace {

// ---------------------------------------------------------------------------
// Contract: every entry point below runs on the single UI thread that owns the
// message loop, so the file-static cache needs no locking.
// ---------------------------------------------------------------------------

// U+0430 CYRILLIC SMALL LETTER A, as an array so it can go straight to
// TextOutW / GetTextExtentPoint32W without an address-of dance.
constexpr wchar_t kMark[] = L"\x0430";

constexpr COLORREF kTileFill   = RGB(0x1C, 0x1C, 0x25);
constexpr COLORREF kTileStroke = RGB(0x33, 0x33, 0x42);
constexpr COLORREF kGlyphInk   = RGB(0xE0, 0x94, 0x4A);

// At or below this size a 3/16 radius plus a hairline stroke would close in on
// the letter, so the tile drops its outline and rounds on a quarter instead.
constexpr int kSmallPx    = 20;
constexpr int kInkPercent = 62;   // glyph ink height as a share of the tile side
constexpr int kSuper      = 4;    // kSuper x kSuper supersample on the corners
constexpr int kMinPx      = 8;
constexpr int kMaxPx      = 512;
constexpr int kCacheSlots = 12;

struct Entry {
    int   px;
    HICON icon;
};

Entry g_cache[kCacheSlots];
int   g_cached = 0;

// --- small helpers ---------------------------------------------------------

inline int Iround(double v) { return static_cast<int>(v < 0.0 ? v - 0.5 : v + 0.5); }

inline BYTE ByteOf(double v) {
    const int i = Iround(v);
    return static_cast<BYTE>(i < 0 ? 0 : (i > 255 ? 255 : i));
}

inline uint32_t Pack(BYTE a, BYTE r, BYTE g, BYTE b) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

struct RectD { double l, t, w, h; };

// Is (x, y) inside the rounded rect with corner radius rad?
inline bool InRound(double x, double y, const RectD& box, double rad) {
    if (x < box.l || y < box.t || x > box.l + box.w || y > box.t + box.h) return false;
    if (rad <= 0.0) return true;
    const double loX = box.l + rad, hiX = box.l + box.w - rad;
    const double loY = box.t + rad, hiY = box.t + box.h - rad;
    const double cx = x < loX ? loX : (x > hiX ? hiX : x);
    const double cy = y < loY ? loY : (y > hiY ? hiY : y);
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

// Coverage of the pixel whose top-left corner is (x, y): 0 outside, 1 inside,
// fractional on the boundary. Straight edges take the analytic box path; only
// the four corner boxes pay for the supersample.
double Coverage(int x, int y, const RectD& box, double rad) {
    if (box.w <= 0.0 || box.h <= 0.0) return 0.0;

    const double x0 = static_cast<double>(x);
    const double y0 = static_cast<double>(y);
    if (x0 + 1.0 <= box.l || y0 + 1.0 <= box.t ||
        x0 >= box.l + box.w || y0 >= box.t + box.h) {
        return 0.0;
    }

    if (rad > 0.0) {
        const bool cornerX = (x0 < box.l + rad) || (x0 + 1.0 > box.l + box.w - rad);
        const bool cornerY = (y0 < box.t + rad) || (y0 + 1.0 > box.t + box.h - rad);
        if (cornerX && cornerY) {
            int hits = 0;
            for (int sy = 0; sy < kSuper; ++sy) {
                const double sampleY = y0 + (static_cast<double>(sy) + 0.5) / kSuper;
                for (int sx = 0; sx < kSuper; ++sx) {
                    const double sampleX = x0 + (static_cast<double>(sx) + 0.5) / kSuper;
                    if (InRound(sampleX, sampleY, box, rad)) ++hits;
                }
            }
            return static_cast<double>(hits) / static_cast<double>(kSuper * kSuper);
        }
    }

    if (x0 >= box.l && x0 + 1.0 <= box.l + box.w &&
        y0 >= box.t && y0 + 1.0 <= box.t + box.h) {
        return 1.0;
    }

    const double cw = ((x0 + 1.0 < box.l + box.w) ? x0 + 1.0 : box.l + box.w) -
                      ((x0 > box.l) ? x0 : box.l);
    const double ch = ((y0 + 1.0 < box.t + box.h) ? y0 + 1.0 : box.t + box.h) -
                      ((y0 > box.t) ? y0 : box.t);
    if (cw <= 0.0 || ch <= 0.0) return 0.0;
    return cw * ch;
}

// --- glyph metrics ---------------------------------------------------------

// The glyph's ink box relative to the pen origin, which sits on the baseline:
// top is negative for the part of the letter above the baseline.
struct Ink {
    int left;
    int top;
    int width;
    int height;
};

HFONT MakeFont(int emHeight) {
    // FW_SEMIBOLD against the "Segoe UI" family resolves to Segoe UI Semibold.
    // ANTIALIASED_QUALITY rather than CLEARTYPE: subpixel fringes are coloured
    // noise once the icon is composited over an unknown background.
    return CreateFontW(-emHeight, 0, 0, 0, FW_SEMIBOLD,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// Measures the *ink* box of the mark under the font currently selected into dc.
bool MeasureInk(HDC dc, Ink* out) {
    static const MAT2 kIdentity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };

    GLYPHMETRICS gm;
    ZeroMemory(&gm, sizeof(gm));
    if (GetGlyphOutlineW(dc, static_cast<UINT>(kMark[0]), GGO_METRICS, &gm,
                         0, nullptr, &kIdentity) != GDI_ERROR &&
        gm.gmBlackBoxX > 0u && gm.gmBlackBoxY > 0u) {
        out->left   = gm.gmptGlyphOrigin.x;
        out->top    = -gm.gmptGlyphOrigin.y;
        out->width  = static_cast<int>(gm.gmBlackBoxX);
        out->height = static_cast<int>(gm.gmBlackBoxY);
        return true;
    }

    // GGO_METRICS is outline-only, so a substituted bitmap face would fail it.
    // Fall back on the text cell: "a" is an x-height letterform sitting on the
    // baseline, and Segoe UI's x-height is a shade over half of its ascent.
    SIZE cell = { 0, 0 };
    TEXTMETRICW tm;
    if (!GetTextExtentPoint32W(dc, kMark, 1, &cell) || !GetTextMetricsW(dc, &tm)) return false;

    int height = MulDiv(tm.tmAscent - tm.tmInternalLeading, 52, 100);
    if (height < 1) height = 1;
    out->left   = 0;
    out->top    = -height;
    out->width  = cell.cx > 0 ? static_cast<int>(cell.cx) : height;
    out->height = height;
    return true;
}

// Picks the em size whose ink height lands on targetInk and returns that font
// (caller owns it) plus the ink box it measured.
//
// Ink height is very nearly linear in the em size, so one proportional estimate
// from the x-height ratio plus a couple of correction passes converges. Hinting
// quantises the result, so instead of demanding an exact hit the loop keeps the
// closest candidate it has seen and stops.
HFONT FitGlyph(HDC dc, int targetInk, Ink* inkOut) {
    int height = targetInk * 2;   // Segoe UI's x-height is ~0.5 em
    if (height < 1) height = 1;

    HFONT best    = nullptr;
    Ink   bestInk = { 0, 0, 0, 0 };
    int   bestErr = INT_MAX;

    for (int pass = 0; pass < 5; ++pass) {
        HFONT font = MakeFont(height);
        if (!font) break;

        Ink ink = { 0, 0, 0, 0 };
        HGDIOBJ prevFont = SelectObject(dc, font);
        const bool measured = MeasureInk(dc, &ink);
        SelectObject(dc, prevFont);

        if (!measured || ink.height <= 0) {
            DeleteObject(font);
            break;
        }

        const int err = (ink.height > targetInk) ? ink.height - targetInk
                                                 : targetInk - ink.height;
        if (err < bestErr) {
            if (best) DeleteObject(best);
            best    = font;
            bestInk = ink;
            bestErr = err;
        } else {
            DeleteObject(font);
        }
        if (err == 0) break;

        int next = MulDiv(height, targetInk, ink.height);
        if (next == height) next = (ink.height > targetInk) ? height - 1 : height + 1;
        if (next < 1) break;
        height = next;
    }

    if (best) *inkOut = bestInk;
    return best;
}

// --- rendering -------------------------------------------------------------

// Builds the 32-bpp premultiplied BGRA colour bitmap for one icon size.
// GDI never writes the alpha byte, so the buffer is post-processed: alpha comes
// from the tile's own coverage, and the RGB is premultiplied by it.
HBITMAP RenderColour(int size) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;

    HDC dc = CreateCompatibleDC(screen);
    if (!dc) {
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = static_cast<DWORD>(sizeof(BITMAPINFOHEADER));
    bi.bmiHeader.biWidth       = size;
    bi.bmiHeader.biHeight      = -size;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* raw = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !raw) {
        if (dib) DeleteObject(dib);
        DeleteDC(dc);
        return nullptr;
    }

    uint32_t* const bits   = static_cast<uint32_t*>(raw);
    HGDIOBJ         oldBmp = SelectObject(dc, dib);

    // --- geometry ---
    // The 1/16 inset is rounded to a whole pixel so the tile's straight edges
    // stay crisp; only the corners are ever antialiased.
    int inset = (size + 8) / 16;
    if (inset < 1) inset = 1;
    int side = size - 2 * inset;
    if (side < 1) {
        inset = 0;
        side  = size;
    }

    const bool tiny = (size <= kSmallPx);

    double radius = tiny ? static_cast<double>(size) / 4.0
                         : static_cast<double>(size) * 3.0 / 16.0;
    const double halfSide = static_cast<double>(side) / 2.0;
    if (radius > halfSide) radius = halfSide;

    int stroke = 0;
    if (!tiny) {
        stroke = size / 32;
        if (stroke < 1) stroke = 1;
    }

    const RectD tile = { static_cast<double>(inset), static_cast<double>(inset),
                         static_cast<double>(side),  static_cast<double>(side) };

    const size_t pixels = static_cast<size_t>(size) * static_cast<size_t>(size);
    memset(bits, 0, pixels * sizeof(uint32_t));

    RECT full = { 0, 0, size, size };
    HBRUSH fill = CreateSolidBrush(kTileFill);
    if (fill) {
        FillRect(dc, &full, fill);
        DeleteObject(fill);
    }
    GdiFlush();

    // --- pass 1: carve the rounded tile, lay the stroke, force the alpha ---
    const double bw = static_cast<double>(stroke);
    const RectD  innerBox = { tile.l + bw, tile.t + bw,
                              tile.w - 2.0 * bw, tile.h - 2.0 * bw };
    double innerR = radius - bw;
    if (innerR < 0.0) innerR = 0.0;

    // Channel extraction by shift rather than GetRValue/GetGValue: the SDK
    // macros cast through WORD, which trips C4310 on a constant argument.
    const double strokeR = static_cast<double>((kTileStroke >> 0) & 0xFF);
    const double strokeG = static_cast<double>((kTileStroke >> 8) & 0xFF);
    const double strokeB = static_cast<double>((kTileStroke >> 16) & 0xFF);

    for (int y = 0; y < size; ++y) {
        uint32_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(size);
        for (int x = 0; x < size; ++x) {
            const double cov = Coverage(x, y, tile, radius);
            if (cov <= 0.0) {
                row[x] = 0;
                continue;
            }

            double b = static_cast<double>(row[x] & 0xFFu);
            double g = static_cast<double>((row[x] >> 8) & 0xFFu);
            double r = static_cast<double>((row[x] >> 16) & 0xFFu);

            if (stroke > 0) {
                double edge = cov - Coverage(x, y, innerBox, innerR);
                if (edge > 0.0) {
                    if (edge > 1.0) edge = 1.0;
                    const double keep = 1.0 - edge;
                    b = b * keep + strokeB * edge;
                    g = g * keep + strokeG * edge;
                    r = r * keep + strokeR * edge;
                }
            }

            row[x] = Pack(ByteOf(255.0 * cov),
                          ByteOf(r * cov), ByteOf(g * cov), ByteOf(b * cov));
        }
    }

    // --- pass 2: the mark, drawn onto the already-composited tile ---
    int targetInk = (side * kInkPercent + 50) / 100;
    if (targetInk < 1) targetInk = 1;

    Ink   ink  = { 0, 0, 0, 0 };
    HFONT font = FitGlyph(dc, targetInk, &ink);
    if (!font) {
        SelectObject(dc, oldBmp);
        DeleteDC(dc);
        DeleteObject(dib);
        return nullptr;
    }

    HGDIOBJ        oldFont  = SelectObject(dc, font);
    const int      oldBk    = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldCol   = SetTextColor(dc, kGlyphInk);
    const UINT     oldAlign = SetTextAlign(dc, TA_LEFT | TA_BASELINE | TA_NOUPDATECP);

    // Centre the measured ink box, not the text cell. The cell carries ascent,
    // descent and side bearings this letter never fills, and centring on it is
    // the classic uncentred-glyph tell.
    const double centreX = tile.l + tile.w / 2.0;
    const double centreY = tile.t + tile.h / 2.0;
    const int penX = Iround(centreX - static_cast<double>(ink.width) / 2.0 -
                            static_cast<double>(ink.left));
    const int penY = Iround(centreY - static_cast<double>(ink.height) / 2.0 -
                            static_cast<double>(ink.top));
    TextOutW(dc, penX, penY, kMark, 1);

    SetTextAlign(dc, oldAlign);
    SetTextColor(dc, oldCol);
    SetBkMode(dc, oldBk);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    GdiFlush();

    // --- pass 3: repair the alpha the text pass zeroed ---
    // GDI writes a zero alpha byte into every pixel it touches, so alpha == 0
    // is exactly the set of text-touched pixels; everything pass 1 left alone
    // keeps the value (and the premultiply) it already has. The glyph never
    // leaves the tile, so scanning the tile's bounding box is enough.
    const int end = inset + side;
    for (int y = inset; y < end; ++y) {
        uint32_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(size);
        for (int x = inset; x < end; ++x) {
            if ((row[x] >> 24) != 0u) continue;

            const double cov = Coverage(x, y, tile, radius);
            if (cov <= 0.0) {
                row[x] = 0;
                continue;
            }

            const double b = static_cast<double>(row[x] & 0xFFu);
            const double g = static_cast<double>((row[x] >> 8) & 0xFFu);
            const double r = static_cast<double>((row[x] >> 16) & 0xFFu);
            row[x] = Pack(ByteOf(255.0 * cov),
                          ByteOf(r * cov), ByteOf(g * cov), ByteOf(b * cov));
        }
    }

    SelectObject(dc, oldBmp);
    DeleteDC(dc);
    return dib;
}

// An all-zero 1-bpp mask means "take every pixel from the colour bitmap"; the
// alpha channel does the real masking. Windows still wants the mask present
// and correctly sized, or some drawing paths render garbage.
HBITMAP MakeMask(int size) {
    const size_t stride = ((static_cast<size_t>(size) + 15u) / 16u) * 2u;  // WORD-aligned rows
    std::vector<BYTE> zeroed(stride * static_cast<size_t>(size), 0);
    return CreateBitmap(size, size, 1, 1, zeroed.data());
}

HICON Create(int size) {
    HBITMAP colour = RenderColour(size);
    if (!colour) return nullptr;

    HBITMAP mask = MakeMask(size);
    if (!mask) {
        DeleteObject(colour);
        return nullptr;
    }

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = colour;

    HICON icon = CreateIconIndirect(&ii);   // copies both bitmaps
    DeleteObject(mask);
    DeleteObject(colour);
    return icon;
}

} // namespace

HICON Get(int px) {
    if (px < kMinPx) px = kMinPx;
    if (px > kMaxPx) px = kMaxPx;

    for (int i = 0; i < g_cached; ++i) {
        if (g_cache[i].px == px) return g_cache[i].icon;
    }

    HICON icon = Create(px);
    if (icon) {
        if (g_cached < kCacheSlots) {
            g_cache[g_cached].px   = px;
            g_cache[g_cached].icon = icon;
            ++g_cached;
            return icon;
        }
        // Out of slots: this module could not own the handle, and handing out
        // an unowned one would either leak it or dangle after Shutdown.
        DestroyIcon(icon);
    }

    // The tray must never end up icon-less. The stock icon is a shared system
    // resource, so it is deliberately never cached and never destroyed.
    return LoadIconW(nullptr, IDI_APPLICATION);
}

void Shutdown() {
    for (int i = 0; i < g_cached; ++i) {
        if (g_cache[i].icon) DestroyIcon(g_cache[i].icon);
        g_cache[i].icon = nullptr;
        g_cache[i].px   = 0;
    }
    g_cached = 0;   // a second Shutdown() is a no-op
}

} // namespace appicon
