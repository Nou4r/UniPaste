// UniPaste settings theme: the font cache, an offscreen 32-bpp canvas and the
// antialiased primitives the redesigned settings window is painted with. Pure
// Win32 + GDI - no GDI+, no Direct2D. Owning the pixels is the whole point:
// GDI cannot antialias a rounded rectangle, so we rasterise coverage ourselves
// and blend into the buffer.

#include "theme.h"

#include <dwmapi.h>

#include <cstddef>

namespace theme {
namespace {

// ---------------------------------------------------------------------------
// Contract: every entry point below runs on the single UI thread that owns the
// settings window, so the file-static caches need no locking.
// ---------------------------------------------------------------------------

constexpr int kSamples     = 4;         // 4x4 supersample of the corner test
constexpr int kMaxCanvasPx = 1 << 14;   // keeps `y * width` well inside an int

constexpr wchar_t kSans[] = L"Segoe UI";

// Probed in order; the first family GDI actually has wins. Naming a missing
// face is not an error - the mapper silently substitutes whatever it likes,
// and a proportional stand-in would wreck the code-point column.
const wchar_t* const kSemiChain[] = { L"Segoe UI Semibold", L"Segoe UI" };
const wchar_t* const kMonoChain[] = { L"Cascadia Mono", L"Consolas", L"Courier New" };

// Logical (96-dpi) pixel sizes; lfHeight is -MulDiv(px, dpi, 96) everywhere.
struct Role {
    int  px;
    bool semibold;
    bool mono;
};

constexpr Role kRoles[] = {
    { 19, true,  false },   // Title
    { 14, false, false },   // Body
    { 14, true,  false },   // Strong
    { 11, true,  false },   // Eyebrow   - callers draw this uppercase
    { 13, false, true  },   // Mono      - code points and glyph pairs
    { 11, false, true  },   // MonoSmall
};

constexpr int kRoleCount = static_cast<int>(sizeof(kRoles) / sizeof(kRoles[0]));
static_assert(kRoleCount == static_cast<int>(Font::MonoSmall) + 1,
              "kRoles is out of sync with enum class Font");

struct State {
    HFONT          fonts[kRoleCount];
    int            dpi;
    bool           built;
    const wchar_t* semiFace;
    const wchar_t* monoFace;
};

State g;

// --- font family probing ---------------------------------------------------

int CALLBACK ProbeFamilyProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM data) {
    *reinterpret_cast<bool*>(data) = true;
    return 0;   // one hit settles it; stop enumerating charsets
}

bool FamilyExists(HDC dc, const wchar_t* face) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet        = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = 0;

    int i = 0;
    for (; i < LF_FACESIZE - 1 && face[i]; ++i) lf.lfFaceName[i] = face[i];
    lf.lfFaceName[i] = L'\0';

    bool found = false;
    EnumFontFamiliesExW(dc, &lf, ProbeFamilyProc, reinterpret_cast<LPARAM>(&found), 0);
    return found;
}

// Picks the first installed family of a chain; the last entry is the "it does
// not matter any more, GDI will map something" backstop and is never probed.
const wchar_t* PickFamily(HDC dc, const wchar_t* const* chain, int count) {
    for (int i = 0; i < count - 1; ++i) {
        if (FamilyExists(dc, chain[i])) return chain[i];
    }
    return chain[count - 1];
}

void ResolveFamilies() {
    if (g.semiFace && g.monoFace) return;   // families do not change at runtime

    HDC screen = GetDC(nullptr);
    if (!screen) {
        g.semiFace = kSemiChain[0];
        g.monoFace = kMonoChain[1];         // Consolas ships with every Windows
        return;
    }
    g.semiFace = PickFamily(screen, kSemiChain,
                            static_cast<int>(sizeof(kSemiChain) / sizeof(kSemiChain[0])));
    g.monoFace = PickFamily(screen, kMonoChain,
                            static_cast<int>(sizeof(kMonoChain) / sizeof(kMonoChain[0])));
    ReleaseDC(nullptr, screen);
}

// --- font cache ------------------------------------------------------------

HFONT MakeFont(const Role& role, int dpi) {
    const wchar_t* face = role.mono ? g.monoFace
                                    : (role.semibold ? g.semiFace : kSans);
    // FIXED_PITCH nudges the mapper towards another monospace face if the
    // probed family disappears between the probe and the CreateFont call.
    const DWORD pitch = static_cast<DWORD>(role.mono ? (FIXED_PITCH | FF_MODERN)
                                                     : (DEFAULT_PITCH | FF_DONTCARE));
    return CreateFontW(-MulDiv(role.px, dpi, 96), 0, 0, 0,
                       role.semibold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       // Grayscale AA, not ClearType: on these dark surfaces
                       // subpixel fringes read as blue/orange colour noise
                       // around every glyph (measured #14254E and #6A503E
                       // fringes on the footer text before this change).
                       ANTIALIASED_QUALITY, pitch, face);
}

void DestroyFonts() {
    for (int i = 0; i < kRoleCount; ++i) {
        if (g.fonts[i]) DeleteObject(g.fonts[i]);
        g.fonts[i] = nullptr;
    }
}

// --- pixel helpers ---------------------------------------------------------

// Little-endian BGRA: the uint32_t reads as 0xAARRGGBB. Alpha is don't-care on
// this opaque surface, but every primitive stamps 0xFF so a memory window over
// the buffer stays readable.
inline uint32_t Pack(COLORREF c) {
    return 0xFF000000u
         | (static_cast<uint32_t>(GetRValue(c)) << 16)
         | (static_cast<uint32_t>(GetGValue(c)) << 8)
         |  static_cast<uint32_t>(GetBValue(c));
}

inline int Alpha255(double coverage) {
    const int a = static_cast<int>(coverage * 255.0 + 0.5);
    return a < 0 ? 0 : (a > 255 ? 255 : a);
}

// src over dst with `a` as the source alpha; dst is whatever the buffer already
// holds, which is what makes corners composite against their real backdrop.
inline uint32_t BlendOver(uint32_t dst, int sr, int sg, int sb, int a) {
    const int inv = 255 - a;
    const int dr = static_cast<int>((dst >> 16) & 0xFFu);
    const int dg = static_cast<int>((dst >> 8) & 0xFFu);
    const int db = static_cast<int>(dst & 0xFFu);
    const uint32_t nr = static_cast<uint32_t>((sr * a + dr * inv + 127) / 255);
    const uint32_t ng = static_cast<uint32_t>((sg * a + dg * inv + 127) / 255);
    const uint32_t nb = static_cast<uint32_t>((sb * a + db * inv + 127) / 255);
    return 0xFF000000u | (nr << 16) | (ng << 8) | nb;
}

inline uint32_t* RowAt(const Canvas& c, int y) {
    return c.bits + static_cast<size_t>(y) * static_cast<size_t>(c.width);
}

// Solid horizontal run, clipped to the canvas. Every fill in this file funnels
// through here, so there is exactly one place that can write out of bounds.
void WriteSpan(const Canvas& c, int x0, int x1, int y, uint32_t px) {
    if (!c.bits || y < 0 || y >= c.height) return;
    if (x0 < 0) x0 = 0;
    if (x1 > c.width) x1 = c.width;
    uint32_t* row = RowAt(c, y);
    for (int x = x0; x < x1; ++x) row[x] = px;
}

// --- rounded-rect coverage -------------------------------------------------

// Is the sample point (x, y) inside the rounded rect [l, l+w) x [t, t+h)?
// The corner test is the classic clamp-to-the-inner-box distance check.
inline bool InRound(double x, double y,
                    double l, double t, double w, double h, double r) {
    if (x < l || y < t || x >= l + w || y >= t + h) return false;
    if (r <= 0.0) return true;
    const double loX = l + r, hiX = l + w - r;
    const double loY = t + r, hiY = t + h - r;
    const double cx = x < loX ? loX : (x > hiX ? hiX : x);
    const double cy = y < loY ? loY : (y > hiY ? hiY : y);
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

// Coverage of the pixel whose top-left corner is (px, py) by the rounded rect
// [l, l+w) x [t, t+h) with corner radius r.
//
// Every argument is an integer, so the straight edges land exactly on pixel
// boundaries: a pixel is either wholly in or wholly out unless it sits in one
// of the four corner boxes. Only those pay for the 4x4 (16 sample) distance
// test - the toast proved 3x3 is enough at 44 px, but these cards are far
// bigger and the corner arcs are correspondingly longer, so 4x4 is the right
// cost/quality point.
double Coverage(int px, int py, int l, int t, int w, int h, int r) {
    if (w <= 0 || h <= 0) return 0.0;
    if (px < l || py < t || px >= l + w || py >= t + h) return 0.0;
    if (r <= 0) return 1.0;

    const bool cornerX = (px < l + r) || (px >= l + w - r);
    const bool cornerY = (py < t + r) || (py >= t + h - r);
    if (!cornerX || !cornerY) return 1.0;

    const double dl = static_cast<double>(l);
    const double dt = static_cast<double>(t);
    const double dw = static_cast<double>(w);
    const double dh = static_cast<double>(h);
    const double dr = static_cast<double>(r);

    int hits = 0;
    for (int sy = 0; sy < kSamples; ++sy) {
        const double y = static_cast<double>(py)
                       + (static_cast<double>(sy) + 0.5) / static_cast<double>(kSamples);
        for (int sx = 0; sx < kSamples; ++sx) {
            const double x = static_cast<double>(px)
                           + (static_cast<double>(sx) + 0.5) / static_cast<double>(kSamples);
            if (InRound(x, y, dl, dt, dw, dh, dr)) ++hits;
        }
    }
    return static_cast<double>(hits) / static_cast<double>(kSamples * kSamples);
}

inline int ClampRadius(int radius, int w, int h) {
    if (radius < 0) return 0;
    const int half = (w < h ? w : h) / 2;
    return radius > half ? half : radius;
}

// SetTextCharacterExtra reports failure as 0x80000000; never feed that back in.
inline int SanitizeExtra(int prev) {
    return static_cast<unsigned>(prev) == 0x80000000u ? 0 : prev;
}

// Eyebrow labels are the only tracked role: small uppercase text needs the air.
inline bool Tracked(Font f) { return f == Font::Eyebrow; }

}  // namespace

// --- fonts -----------------------------------------------------------------

void SetDpi(int dpi) {
    if (dpi < 72) dpi = 96;                     // a bogus dpi must not size fonts to nothing
    if (g.built && g.dpi == dpi) return;

    ResolveFamilies();
    DestroyFonts();
    g.dpi = dpi;
    for (int i = 0; i < kRoleCount; ++i) g.fonts[i] = MakeFont(kRoles[i], dpi);
    g.built = true;
}

int Dpi() {
    return g.dpi > 0 ? g.dpi : 96;
}

int S(int logical) {
    return MulDiv(logical, Dpi(), 96);
}

HFONT Get(Font role) {
    const int i = static_cast<int>(role);
    if (i >= 0 && i < kRoleCount && g.fonts[i]) return g.fonts[i];
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));   // never null
}

void Shutdown() {
    DestroyFonts();     // already null after the first call; DeleteObject is skipped
    g.dpi   = 0;
    g.built = false;
}

// --- title bar -------------------------------------------------------------

void DarkTitleBar(HWND hwnd) {
    if (!hwnd) return;

    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE from 20H1 onwards; 19 was the
    // undocumented value in 1809..1903. Both fail harmlessly before that, and
    // the window simply keeps a light title bar.
    constexpr DWORD kDarkAttr    = 20;
    constexpr DWORD kDarkAttrOld = 19;

    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDarkAttr, &on, static_cast<DWORD>(sizeof(on))))) {
        static_cast<void>(
            DwmSetWindowAttribute(hwnd, kDarkAttrOld, &on, static_cast<DWORD>(sizeof(on))));
    }
}

// --- canvas ----------------------------------------------------------------

bool CanvasBegin(HDC target, int width, int height, Canvas* out) {
    if (!out) return false;
    *out = Canvas();
    if (width <= 0 || height <= 0) return false;
    if (width > kMaxCanvasPx || height > kMaxCanvasPx) return false;

    HDC dc = CreateCompatibleDC(target);
    if (!dc) return false;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;   // top-down: row 0 is the top row
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;        // stride == width * 4, no padding
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ old = SelectObject(dc, bmp);
    if (!old) {
        DeleteObject(bmp);
        DeleteDC(dc);
        return false;
    }

    out->dc        = dc;
    out->bits      = static_cast<uint32_t*>(bits);
    out->width     = width;
    out->height    = height;
    out->bitmap    = bmp;
    out->oldBitmap = old;
    return true;
}

void CanvasBlit(HDC target, int x, int y, const Canvas& canvas) {
    if (!target || !canvas.dc || canvas.width <= 0 || canvas.height <= 0) return;
    BitBlt(target, x, y, canvas.width, canvas.height, canvas.dc, 0, 0, SRCCOPY);
}

void CanvasEnd(Canvas* canvas) {
    if (!canvas) return;
    if (canvas->dc && canvas->oldBitmap) SelectObject(canvas->dc, canvas->oldBitmap);
    if (canvas->bitmap) DeleteObject(canvas->bitmap);
    if (canvas->dc) DeleteDC(canvas->dc);
    *canvas = Canvas();
}

// --- primitives ------------------------------------------------------------

void Fill(Canvas& c, const RECT& r, COLORREF colour) {
    if (!c.bits) return;
    const int y0 = r.top    < 0 ? 0 : r.top;
    const int y1 = r.bottom > c.height ? c.height : r.bottom;
    const uint32_t px = Pack(colour);
    for (int y = y0; y < y1; ++y) WriteSpan(c, r.left, r.right, y, px);
}

void FillRounded(Canvas& c, const RECT& rc, int radius, COLORREF colour) {
    if (!c.bits) return;
    const int l = rc.left;
    const int t = rc.top;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const int r = ClampRadius(radius, w, h);
    if (r == 0) {
        Fill(c, rc, colour);
        return;
    }

    const int x0 = l < 0 ? 0 : l;
    const int x1 = (l + w) > c.width ? c.width : (l + w);
    const int y0 = t < 0 ? 0 : t;
    const int y1 = (t + h) > c.height ? c.height : (t + h);

    const uint32_t px = Pack(colour);
    const int cr = static_cast<int>(GetRValue(colour));
    const int cg = static_cast<int>(GetGValue(colour));
    const int cb = static_cast<int>(GetBValue(colour));

    for (int y = y0; y < y1; ++y) {
        // Rows between the corner bands are a straight run: no sampling needed.
        if (y >= t + r && y < t + h - r) {
            WriteSpan(c, x0, x1, y, px);
            continue;
        }
        uint32_t* row = RowAt(c, y);
        for (int x = x0; x < x1; ++x) {
            if (x >= l + r && x < l + w - r) {
                row[x] = px;
                continue;
            }
            const int a = Alpha255(Coverage(x, y, l, t, w, h, r));
            if (a <= 0) continue;
            row[x] = (a >= 255) ? px : BlendOver(row[x], cr, cg, cb, a);
        }
    }
}

void StrokeRounded(Canvas& c, const RECT& rc, int radius, int thickness, COLORREF colour) {
    if (!c.bits) return;
    const int l = rc.left;
    const int t = rc.top;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const int th = thickness < 1 ? 1 : thickness;
    const int r  = ClampRadius(radius, w, h);

    // The hole: the same rect inset by the thickness, radius reduced to match.
    // A thickness past the half-size collapses it, which correctly degenerates
    // the stroke into a solid rounded fill.
    const int il = l + th;
    const int it = t + th;
    const int iw = w - 2 * th;
    const int ih = h - 2 * th;
    int ir = r - th;
    if (ir < 0) ir = 0;
    if (iw > 0 && ih > 0) ir = ClampRadius(ir, iw, ih);

    const int x0 = l < 0 ? 0 : l;
    const int x1 = (l + w) > c.width ? c.width : (l + w);
    const int y0 = t < 0 ? 0 : t;
    const int y1 = (t + h) > c.height ? c.height : (t + h);

    const uint32_t px = Pack(colour);
    const int cr = static_cast<int>(GetRValue(colour));
    const int cg = static_cast<int>(GetGValue(colour));
    const int cb = static_cast<int>(GetBValue(colour));

    const bool hasHole = (iw > 0 && ih > 0);

    for (int y = y0; y < y1; ++y) {
        const bool outerStraight = (y >= t + r) && (y < t + h - r);
        const bool innerStraight = hasHole && (y >= it + ir) && (y < it + ih - ir);
        if (outerStraight && innerStraight) {
            // Both edges are straight on this row: two solid side bands, and a
            // middle the hole covers exactly. No sampling, no blending.
            WriteSpan(c, x0, il < x1 ? il : x1, y, px);
            WriteSpan(c, (il + iw) > x0 ? (il + iw) : x0, x1, y, px);
            continue;
        }
        uint32_t* row = RowAt(c, y);
        for (int x = x0; x < x1; ++x) {
            const double outer = Coverage(x, y, l, t, w, h, r);
            if (outer <= 0.0) continue;
            double edge = outer - Coverage(x, y, il, it, iw, ih, ir);
            if (edge <= 0.0) continue;
            if (edge > 1.0) edge = 1.0;
            const int a = Alpha255(edge);
            if (a <= 0) continue;
            row[x] = (a >= 255) ? px : BlendOver(row[x], cr, cg, cb, a);
        }
    }
}

void HLine(Canvas& c, int x0, int x1, int y, COLORREF colour) {
    if (!c.bits) return;
    if (x0 > x1) { const int swap = x0; x0 = x1; x1 = swap; }
    WriteSpan(c, x0, x1, y, Pack(colour));
}

COLORREF Mix(COLORREF a, COLORREF b, int t) {
    if (t < 0) t = 0;
    else if (t > 255) t = 255;
    const int inv = 255 - t;
    const int mr = (static_cast<int>(GetRValue(a)) * inv + static_cast<int>(GetRValue(b)) * t + 127) / 255;
    const int mg = (static_cast<int>(GetGValue(a)) * inv + static_cast<int>(GetGValue(b)) * t + 127) / 255;
    const int mb = (static_cast<int>(GetBValue(a)) * inv + static_cast<int>(GetBValue(b)) * t + 127) / 255;
    return RGB(mr, mg, mb);
}

// --- text ------------------------------------------------------------------

// GDI writes RGB into a 32-bpp DIB and leaves the alpha byte at whatever it
// was - usually 0. That is harmless here: this surface is opaque and reaches
// the screen through BitBlt(SRCCOPY), which ignores alpha entirely. Do NOT
// "fix" it by forcing alpha to 0xFF after drawing text; the corner blends read
// the RGB channels only, and a post-pass over the whole buffer would cost a
// full-surface write per paint for no visible gain.
void Text(Canvas& c, const wchar_t* s, const RECT& r, Font f, COLORREF colour, UINT dtFlags) {
    if (!c.dc || !s) return;

    HGDIOBJ        oldFont  = SelectObject(c.dc, Get(f));
    const int      oldBk    = SetBkMode(c.dc, TRANSPARENT);
    const COLORREF oldCol   = SetTextColor(c.dc, colour);
    const int      oldExtra = Tracked(f) ? SetTextCharacterExtra(c.dc, S(1)) : 0;

    RECT box = r;
    DrawTextW(c.dc, s, -1, &box, dtFlags);

    if (Tracked(f)) SetTextCharacterExtra(c.dc, SanitizeExtra(oldExtra));
    SetTextColor(c.dc, oldCol);
    SetBkMode(c.dc, oldBk);
    SelectObject(c.dc, oldFont);

    // GDI batches drawing calls; the direct pixel writes above and below do
    // not. Flush so the glyphs are in the DIB before anything reads it back.
    GdiFlush();
}

SIZE Measure(Canvas& c, const wchar_t* s, Font f) {
    SIZE out = { 0, 0 };
    if (!c.dc || !s) return out;

    HGDIOBJ  oldFont  = SelectObject(c.dc, Get(f));
    // Track while measuring too, or an eyebrow measures narrower than it draws
    // and the caller clips it.
    const int oldExtra = Tracked(f) ? SetTextCharacterExtra(c.dc, S(1)) : 0;

    RECT box = { 0, 0, kMaxCanvasPx, kMaxCanvasPx };
    DrawTextW(c.dc, s, -1, &box, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    out.cx = box.right - box.left;
    out.cy = box.bottom - box.top;

    if (Tracked(f)) SetTextCharacterExtra(c.dc, SanitizeExtra(oldExtra));
    SelectObject(c.dc, oldFont);
    return out;
}

}  // namespace theme
