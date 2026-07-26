// UniPaste transient toast: a borderless, click-through, never-focus-stealing
// layered window anchored to the top-right work area of the monitor holding the
// foreground window. Pure Win32 + GDI, single UpdateLayeredWindow surface.

#include "overlay.h"

#include <cstddef>
#include <cstring>

namespace overlay {
namespace {

// ---------------------------------------------------------------------------
// Contract: every entry point below is called from the single UI thread that
// owns the message loop, so the file-static state needs no locking.
// ---------------------------------------------------------------------------

constexpr wchar_t kClassName[] = L"UniPasteToast";

constexpr UINT_PTR kTimerId = 1;
constexpr UINT     kTimerMs = 16;   // ~60 Hz, the only animation driver.

constexpr ULONGLONG kFadeInMs  = 140;
constexpr ULONGLONG kHoldMs    = 1400;
constexpr ULONGLONG kFadeOutMs = 260;

// Logical (96-dpi) metrics; every one of these is scaled by the target DPI.
constexpr int kCardH    = 44;
constexpr int kRadius   = 10;
constexpr int kAccentW  = 4;
constexpr int kPadLeft  = 16;   // gap between the accent bar and the text
constexpr int kPadRight = 18;
constexpr int kMargin   = 18;   // distance from the work-area top-right corner
constexpr int kSlidePx  = 24;   // horizontal slide-in distance
constexpr int kFontPx   = 15;
constexpr int kMinW     = 180;
constexpr int kMaxW     = 520;

// 0xAARRGGBB literals (the alpha byte is documentation only; the card is opaque
// and the fade is carried entirely by BLENDFUNCTION::SourceConstantAlpha).
constexpr unsigned kColCardBg   = 0xFF1F1F22u;
constexpr unsigned kColBorder   = 0xFF3A3A40u;
constexpr unsigned kColAccentOk = 0xFF22C55Eu;
constexpr unsigned kColAccentNo = 0xFFEF4444u;
constexpr unsigned kColTextOk   = 0xFFF2F2F5u;
constexpr unsigned kColTextNo   = 0xFFFFD7D7u;

constexpr int kMaxText = 192;

enum class Phase { Idle, In, Hold, Out };

// Resolved at runtime so this TU never depends on the project's WINVER level.
using GetDpiForWindowFn  = UINT(WINAPI*)(HWND);
using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

struct Toast {
    HINSTANCE hInst;
    HWND      hwnd;
    bool      classRegistered;

    HDC     memDC;
    HBITMAP dib;
    HBITMAP oldBmp;     // the 1x1 bitmap the memory DC was born with
    BYTE*   bits;
    int     dibW;
    int     dibH;

    HFONT font;
    int   fontDpi;

    int cardW;
    int cardH;
    int targetX;        // fully-slid-in position
    int targetY;
    int slide;          // device-px slide distance for this toast

    Phase     phase;
    ULONGLONG phaseStart;
    int       alpha;    // last SourceConstantAlpha pushed to the window
    bool      timerOn;

    Kind    kind;
    wchar_t text[kMaxText];

    GetDpiForWindowFn  dpiFn;
    GetDpiForMonitorFn dpiMonFn;
    bool               dpiFnResolved;
};

Toast g;

// --- small helpers ---------------------------------------------------------

inline BYTE ChR(unsigned c) { return static_cast<BYTE>((c >> 16) & 0xFFu); }
inline BYTE ChG(unsigned c) { return static_cast<BYTE>((c >> 8) & 0xFFu); }
inline BYTE ChB(unsigned c) { return static_cast<BYTE>(c & 0xFFu); }
inline COLORREF Cref(unsigned c) { return RGB(ChR(c), ChG(c), ChB(c)); }

inline int Iround(double v) { return static_cast<int>(v < 0.0 ? v - 0.5 : v + 0.5); }
inline BYTE ByteOf(double v) {
    const int i = Iround(v);
    return static_cast<BYTE>(i < 0 ? 0 : (i > 255 ? 255 : i));
}
inline int Scale(int logical, int dpi) { return MulDiv(logical, dpi, 96); }

// Is (x, y) inside the rounded rect [l, l+w) x [t, t+h) with corner radius r?
inline bool InRound(double x, double y, double l, double t, double w, double h, double r) {
    if (x < l || y < t || x > l + w || y > t + h) return false;
    if (r <= 0.0) return true;
    const double lo_x = l + r, hi_x = l + w - r;
    const double lo_y = t + r, hi_y = t + h - r;
    const double cx = x < lo_x ? lo_x : (x > hi_x ? hi_x : x);
    const double cy = y < lo_y ? lo_y : (y > hi_y ? hi_y : y);
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

// Coverage of the pixel whose top-left corner is (px, py). Straight edges take
// the analytic fast path; only the corner boxes pay for the 3x3 supersample.
double Coverage(int px, int py, double l, double t, double w, double h, double r) {
    if (w <= 0.0 || h <= 0.0) return 0.0;
    const double x0 = static_cast<double>(px);
    const double y0 = static_cast<double>(py);
    if (x0 + 1.0 <= l || y0 + 1.0 <= t || x0 >= l + w || y0 >= t + h) return 0.0;

    if (r > 0.0) {
        const bool cornerX = (x0 < l + r) || (x0 + 1.0 > l + w - r);
        const bool cornerY = (y0 < t + r) || (y0 + 1.0 > t + h - r);
        if (cornerX && cornerY) {
            int hits = 0;
            for (int sy = 0; sy < 3; ++sy) {
                const double y = y0 + (static_cast<double>(sy) + 0.5) / 3.0;
                for (int sx = 0; sx < 3; ++sx) {
                    const double x = x0 + (static_cast<double>(sx) + 0.5) / 3.0;
                    if (InRound(x, y, l, t, w, h, r)) ++hits;
                }
            }
            return static_cast<double>(hits) / 9.0;
        }
    }

    // Fully inside one of the two straight bands (or inside a square rect).
    if (x0 >= l && x0 + 1.0 <= l + w && y0 >= t && y0 + 1.0 <= t + h) return 1.0;

    // Straddles the outer edge of a square-cornered rect: box coverage.
    const double cw = (x0 + 1.0 < l + w ? x0 + 1.0 : l + w) - (x0 > l ? x0 : l);
    const double ch = (y0 + 1.0 < t + h ? y0 + 1.0 : t + h) - (y0 > t ? y0 : t);
    if (cw <= 0.0 || ch <= 0.0) return 0.0;
    return cw * ch;
}

// GetDpiForWindow reports the DPI *context* of the window it is given, which is
// a flat 96 for a DPI-unaware process. The foreground window belongs to someone
// else, so asking it would size our toast according to a foreign app's manifest
// - Steam would give a 96-dpi card on a 144-dpi screen. The toast is our own
// per-monitor-aware window, so the monitor's effective DPI is the right answer;
// the foreground window only selects *which* monitor.
HMONITOR MonitorFor(HWND fg) {
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!mon) {
        const POINT origin = { 0, 0 };
        mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    return mon;
}

int DpiOfMonitor(HMONITOR mon) {
    if (!g.dpiFnResolved) {
        g.dpiFnResolved = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            g.dpiMonFn = reinterpret_cast<GetDpiForMonitorFn>(
                GetProcAddress(shcore, "GetDpiForMonitor"));
        }
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            g.dpiFn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(user32, "GetDpiForWindow"));
        }
    }

    UINT dpi = 0;
    if (mon && g.dpiMonFn) {
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (SUCCEEDED(g.dpiMonFn(mon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)))
            dpi = dpiX;
    }
    if (dpi == 0 && g.hwnd && g.dpiFn) dpi = g.dpiFn(g.hwnd);  // our own window
    if (dpi == 0) {
        HDC screen = GetDC(nullptr);
        if (screen) {
            dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
            ReleaseDC(nullptr, screen);
        }
    }
    if (dpi < 72) dpi = 96;
    return static_cast<int>(dpi);
}

void WorkAreaOf(HMONITOR mon, RECT* out) {
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        *out = mi.rcWork;
        return;
    }
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, out, 0)) {
        out->left = 0;
        out->top = 0;
        out->right = GetSystemMetrics(SM_CXSCREEN);
        out->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

void CopyText(const wchar_t* src) {
    int i = 0;
    if (src) {
        for (; i < kMaxText - 1 && src[i]; ++i) g.text[i] = src[i];
    }
    g.text[i] = L'\0';
}

// --- GDI resources ---------------------------------------------------------

bool EnsureFont(int dpi) {
    if (g.font && g.fontDpi == dpi) return true;
    HFONT f = CreateFontW(-MulDiv(kFontPx, dpi, 96), 0, 0, 0, FW_SEMIBOLD,
                          FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!f) return g.font != nullptr;
    if (g.font) DeleteObject(g.font);
    g.font = f;
    g.fontDpi = dpi;
    return true;
}

// Cached surface: recreated only when the required size actually changes.
bool EnsureSurface(int w, int h) {
    if (g.dib && g.dibW == w && g.dibH == h) return true;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    if (!g.memDC) g.memDC = CreateCompatibleDC(screen);
    if (!g.memDC) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return false;
    }

    HGDIOBJ prev = SelectObject(g.memDC, dib);
    if (g.dib) DeleteObject(g.dib);          // prev is the outgoing DIB
    else       g.oldBmp = static_cast<HBITMAP>(prev);

    g.dib  = dib;
    g.bits = static_cast<BYTE*>(bits);
    g.dibW = w;
    g.dibH = h;
    return true;
}

// --- painting --------------------------------------------------------------

// Draws the card once per Show. GDI never writes the alpha byte, so the whole
// buffer is post-processed afterwards: alpha is forced to 255 inside the
// rounded card (colors are opaque, so premultiplied == straight) and the pixel
// is zeroed outside it. Corner pixels get coverage-scaled alpha + premultiply.
void RenderCard(int w, int h, int dpi) {
    if (!g.bits || !g.memDC) return;

    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    memset(g.bits, 0, pixels * 4u);

    const int accentW = Scale(kAccentW, dpi);
    const int borderW = Scale(1, dpi) < 1 ? 1 : Scale(1, dpi);
    double radius = static_cast<double>(Scale(kRadius, dpi));
    const double halfMin = static_cast<double>(w < h ? w : h) * 0.5;
    if (radius > halfMin) radius = halfMin;

    const unsigned accentCol = (g.kind == Kind::Error) ? kColAccentNo : kColAccentOk;
    const unsigned textCol   = (g.kind == Kind::Error) ? kColTextNo   : kColTextOk;

    RECT full = { 0, 0, w, h };
    HBRUSH bgBrush = CreateSolidBrush(Cref(kColCardBg));
    if (bgBrush) {
        FillRect(g.memDC, &full, bgBrush);
        DeleteObject(bgBrush);
    }

    RECT accent = { 0, 0, accentW, h };
    HBRUSH accentBrush = CreateSolidBrush(Cref(accentCol));
    if (accentBrush) {
        FillRect(g.memDC, &accent, accentBrush);
        DeleteObject(accentBrush);
    }

    if (g.font) {
        HGDIOBJ oldFont = SelectObject(g.memDC, g.font);
        const int oldMode = SetBkMode(g.memDC, TRANSPARENT);
        const COLORREF oldCol = SetTextColor(g.memDC, Cref(textCol));
        RECT tr = { accentW + Scale(kPadLeft, dpi), 0, w - Scale(kPadRight, dpi), h };
        DrawTextW(g.memDC, g.text, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        SetTextColor(g.memDC, oldCol);
        SetBkMode(g.memDC, oldMode);
        SelectObject(g.memDC, oldFont);
    }
    GdiFlush();

    // Alpha + rounded-corner + rounded-border pass.
    const double bw = static_cast<double>(borderW);
    double innerR = radius - bw;
    if (innerR < 0.0) innerR = 0.0;
    const double innerW = static_cast<double>(w) - 2.0 * bw;
    const double innerH = static_cast<double>(h) - 2.0 * bw;
    const double brR = static_cast<double>(ChR(kColBorder));
    const double brG = static_cast<double>(ChG(kColBorder));
    const double brB = static_cast<double>(ChB(kColBorder));

    for (int y = 0; y < h; ++y) {
        BYTE* row = g.bits + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
        for (int x = 0; x < w; ++x) {
            BYTE* p = row + static_cast<size_t>(x) * 4u;   // B, G, R, A

            const double cov = Coverage(x, y, 0.0, 0.0,
                                        static_cast<double>(w), static_cast<double>(h), radius);
            if (cov <= 0.0) {
                p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;
                continue;
            }

            // 1px inner border, skipped over the accent bar so it stays pure.
            if (x >= accentW) {
                const double inner = Coverage(x, y, bw, bw, innerW, innerH, innerR);
                double edge = cov - inner;
                if (edge > 0.0) {
                    if (edge > 1.0) edge = 1.0;
                    const double keep = 1.0 - edge;
                    p[0] = ByteOf(static_cast<double>(p[0]) * keep + brB * edge);
                    p[1] = ByteOf(static_cast<double>(p[1]) * keep + brG * edge);
                    p[2] = ByteOf(static_cast<double>(p[2]) * keep + brR * edge);
                }
            }

            if (cov >= 1.0) {
                p[3] = static_cast<BYTE>(255);   // opaque card interior
            } else {
                p[0] = ByteOf(static_cast<double>(p[0]) * cov);   // premultiply
                p[1] = ByteOf(static_cast<double>(p[1]) * cov);
                p[2] = ByteOf(static_cast<double>(p[2]) * cov);
                p[3] = ByteOf(255.0 * cov);
            }
        }
    }
}

void Push(int alpha, double slideOffset) {
    if (!g.hwnd || !g.memDC || !g.dib) return;
    g.alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);

    POINT src = { 0, 0 };
    POINT dst = { g.targetX + Iround(slideOffset), g.targetY };
    SIZE  sz  = { g.cardW, g.cardH };

    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = static_cast<BYTE>(g.alpha);
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(g.hwnd, nullptr, &dst, &sz, g.memDC, &src, 0, &bf, ULW_ALPHA);
}

void StopTimer() {
    if (g.timerOn && g.hwnd) KillTimer(g.hwnd, kTimerId);
    g.timerOn = false;
}

void StartTimer() {
    if (!g.hwnd) return;
    SetTimer(g.hwnd, kTimerId, kTimerMs, nullptr);
    g.timerOn = true;
}

void HideNow() {
    g.phase = Phase::Idle;
    g.alpha = 0;
    StopTimer();
    if (g.hwnd) ShowWindow(g.hwnd, SW_HIDE);
}

void Tick() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG el  = (now >= g.phaseStart) ? (now - g.phaseStart) : 0ull;

    int    alpha = 255;
    double slide = 0.0;

    switch (g.phase) {
    case Phase::In:
        if (el >= kFadeInMs) {
            g.phase = Phase::Hold;
            g.phaseStart = now;
        } else {
            const double t = static_cast<double>(el) / static_cast<double>(kFadeInMs);
            const double u = 1.0 - t;
            const double ease = 1.0 - u * u * u;          // cubic ease-out
            alpha = Iround(255.0 * t);
            slide = static_cast<double>(g.slide) * (1.0 - ease);
        }
        break;

    case Phase::Hold:
        if (el >= kHoldMs) {
            g.phase = Phase::Out;
            g.phaseStart = now;
        }
        break;

    case Phase::Out:
        if (el >= kFadeOutMs) {
            HideNow();
            return;
        }
        alpha = Iround(255.0 * (1.0 - static_cast<double>(el) / static_cast<double>(kFadeOutMs)));
        break;

    case Phase::Idle:
    default:
        StopTimer();
        return;
    }

    Push(alpha, slide);
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kTimerId) {
            Tick();
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HTTRANSPARENT;   // belt-and-braces alongside WS_EX_TRANSPARENT
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ---------------------------------------------------------------------------

bool Init(HINSTANCE hInst) {
    if (g.hwnd) return true;

    g.hInst = hInst;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = ToastProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;

    if (RegisterClassExW(&wc) == 0) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    } else {
        g.classRegistered = true;
    }

    g.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                 WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                             kClassName, L"UniPaste", WS_POPUP,
                             0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
    if (!g.hwnd) {
        if (g.classRegistered) {
            UnregisterClassW(kClassName, hInst);
            g.classRegistered = false;
        }
        return false;
    }

    g.phase = Phase::Idle;
    g.alpha = 0;
    return true;
}

void Show(const wchar_t* text, Kind kind) {
    if (!g.hwnd) return;

    CopyText(text);
    g.kind = kind;

    HWND fg = GetForegroundWindow();
    if (fg == g.hwnd) fg = nullptr;

    HMONITOR mon = MonitorFor(fg);
    RECT work;
    WorkAreaOf(mon, &work);
    const int dpi = DpiOfMonitor(mon);
    if (!EnsureFont(dpi)) return;

    // Measure the text with the real font on a scratch DC.
    int textW = 0;
    HDC measure = CreateCompatibleDC(nullptr);
    if (measure) {
        HGDIOBJ old = SelectObject(measure, g.font);
        RECT calc = { 0, 0, 0, 0 };
        DrawTextW(measure, g.text, -1, &calc,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        textW = calc.right - calc.left;
        SelectObject(measure, old);
        DeleteDC(measure);
    }

    const int accentW = Scale(kAccentW, dpi);
    int w = accentW + Scale(kPadLeft, dpi) + textW + Scale(kPadRight, dpi);
    const int minW = Scale(kMinW, dpi);
    const int maxW = Scale(kMaxW, dpi);
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    const int h = Scale(kCardH, dpi);

    if (!EnsureSurface(w, h)) return;
    g.cardW = w;
    g.cardH = h;
    RenderCard(w, h, dpi);

    const int margin = Scale(kMargin, dpi);
    g.targetX = work.right - margin - w;
    g.targetY = work.top + margin;
    if (g.targetX < work.left) g.targetX = work.left;
    g.slide = Scale(kSlidePx, dpi);

    const ULONGLONG now = GetTickCount64();
    if (g.phase == Phase::Hold) {
        // Already fully faded in: restart the hold so a repeated hotkey does
        // not re-play the slide as a stutter.
        g.phaseStart = now;
        Push(255, 0.0);
    } else {
        // Resume the fade-in from wherever the current alpha sits.
        double t0 = static_cast<double>(g.alpha) / 255.0;
        if (t0 < 0.0) t0 = 0.0;
        if (t0 > 1.0) t0 = 1.0;
        g.phase = Phase::In;
        g.phaseStart = now - static_cast<ULONGLONG>(t0 * static_cast<double>(kFadeInMs));
        Tick();
    }

    SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    StartTimer();
}

void Shutdown() {
    StopTimer();

    if (g.memDC) {
        if (g.oldBmp) SelectObject(g.memDC, g.oldBmp);
        DeleteDC(g.memDC);
    }
    if (g.dib)  DeleteObject(g.dib);
    if (g.font) DeleteObject(g.font);
    if (g.hwnd) DestroyWindow(g.hwnd);

    const HINSTANCE inst = g.hInst;
    const bool registered = g.classRegistered;

    g = Toast();   // full reset: a second Shutdown() is a no-op

    if (registered) UnregisterClassW(kClassName, inst);
}

} // namespace overlay
