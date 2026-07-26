#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

namespace theme {

// Cool indigo-slate base, warm amber brand accent. Status hues (toast green /
// red) are deliberately absent: they mean "outcome", not "brand".
constexpr COLORREF kBase       = RGB(0x14, 0x14, 0x1B);
constexpr COLORREF kSurface    = RGB(0x1C, 0x1C, 0x25);
constexpr COLORREF kSurfaceHi  = RGB(0x26, 0x26, 0x33);
constexpr COLORREF kLine       = RGB(0x33, 0x33, 0x42);
constexpr COLORREF kLineSoft   = RGB(0x26, 0x26, 0x32);
constexpr COLORREF kText       = RGB(0xEC, 0xEC, 0xF4);
constexpr COLORREF kTextDim    = RGB(0x9A, 0x9A, 0xAE);
constexpr COLORREF kTextFaint  = RGB(0x6A, 0x6A, 0x7C);
constexpr COLORREF kAccent     = RGB(0xE0, 0x94, 0x4A);
constexpr COLORREF kAccentDim  = RGB(0x7A, 0x52, 0x28);
constexpr COLORREF kAccentInk  = RGB(0x18, 0x11, 0x07);  // text on an amber fill
constexpr COLORREF kDanger     = RGB(0xE0, 0x5A, 0x5A);

enum class Font { Title, Body, Strong, Eyebrow, Mono, MonoSmall };

void  SetDpi(int dpi);          // rebuilds the font cache when dpi changes
int   Dpi();
int   S(int logical);           // MulDiv(logical, Dpi(), 96)
HFONT Get(Font role);
void  Shutdown();               // frees every cached font

void DarkTitleBar(HWND hwnd);   // DWMWA_USE_IMMERSIVE_DARK_MODE; no-op pre-1809

// Offscreen 32-bpp top-down BGRA surface. Owning the pixels is what makes
// antialiased rounded rectangles possible under plain GDI.
struct Canvas {
    HDC       dc        = nullptr;
    uint32_t* bits      = nullptr;   // top-down BGRA, stride == width
    int       width     = 0;
    int       height    = 0;
    HBITMAP   bitmap    = nullptr;
    HGDIOBJ   oldBitmap = nullptr;
};

bool CanvasBegin(HDC target, int width, int height, Canvas* out);
void CanvasBlit(HDC target, int x, int y, const Canvas& canvas);
void CanvasEnd(Canvas* canvas);

void Fill(Canvas& c, const RECT& r, COLORREF colour);
void FillRounded(Canvas& c, const RECT& r, int radius, COLORREF colour);
void StrokeRounded(Canvas& c, const RECT& r, int radius, int thickness, COLORREF colour);
void HLine(Canvas& c, int x0, int x1, int y, COLORREF colour);

void Text(Canvas& c, const wchar_t* s, const RECT& r, Font f, COLORREF colour, UINT dtFlags);
SIZE Measure(Canvas& c, const wchar_t* s, Font f);

COLORREF Mix(COLORREF a, COLORREF b, int t);   // t in 0..255; 0 => a, 255 => b

} // namespace theme
