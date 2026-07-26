// UniPaste settings window.
//
// The product turns Latin letters into look-alike Cyrillic/Greek glyphs, so the
// window is built around that: letterforms are the subject matter. Cool
// indigo-slate chrome, one warm amber accent, Segoe UI for prose and a
// monospace face wherever the pixels *are* the data (code points, the list file
// path, the key caps).
//
// Everything is drawn by hand through theme::Canvas. Real USER32 controls
// survive only where they buy behaviour that is expensive to reimplement:
// a LISTBOX (selection, keyboard, scrolling), an EDIT (caret, IME, clipboard)
// and BUTTONs (focus, keyboard activation, BN_CLICKED). All of them are
// owner-drawn; none of them shows a single system-themed pixel.
//
// Three eyebrow-led sections, top to bottom:
//   MODE        segmented bar, one quiet description line, live specimen card.
//   WORD LISTS  a two-cell segmented bar that picks the visible list *and* the
//               conversion policy at once - "Never convert" runs the normal
//               policy, "Only convert" runs blacklist mode - over the list
//               card, the policy sentence, the add row and the two switches.
//   KEYBINDS    a card of drawn key caps, with a live on/off chip for the
//               convert-on-copy chord.
//
// The window is created lazily on the first Show() and then reused forever;
// closing it only hides it, so the tray application keeps running.

#include "settings.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "appicon.h"
#include "theme.h"
#include "wordlist.h"

// Present in the Windows 8.1 / 10 SDK headers only; the message is consumed
// through a plain switch case, so an older *runtime* simply never sends it.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace settings {
namespace {

// ---------------------------------------------------------------------------
// Contract: every entry point below runs on the single UI thread that owns the
// application message loop, so the file-static state needs no locking.
// ---------------------------------------------------------------------------

using theme::S;
using uni::wordlist::Kind;

constexpr wchar_t kClassName[] = L"UniPasteSettings";
constexpr wchar_t kTitle[]     = L"UniPaste";
constexpr wchar_t kAppName[]   = L"UniPaste";

// Resizable (WS_THICKFRAME) but never maximised: the layout has exactly one
// elastic row and a maximised window would just stretch it absurdly.
// WS_CLIPCHILDREN keeps the full-client backbuffer blit out of the child
// rectangles, which is what makes the redraw flicker-free.
constexpr DWORD kStyle   = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
constexpr DWORD kExStyle = WS_EX_APPWINDOW;

constexpr int kModeCount = 4;    // uni::Mode 0..3
constexpr int kKindCount = 2;    // uni::wordlist::Kind Never / Only
constexpr int kBindCount = 3;    // rows in the keybinds card
constexpr int kAutoBind  = 2;    // the row that carries the live on/off chip
constexpr int kMaxEntry  = 512;  // characters accepted from the edit control

// Control identifiers. Add is IDOK on purpose: IsDialogMessageW turns an
// unhandled Enter into WM_COMMAND(IDOK), which is exactly the Enter-to-add
// behaviour we want without subclassing the edit control for it.
constexpr int kIdAdd         = IDOK;
constexpr int kIdClose       = IDCANCEL;
constexpr int kIdList        = 1104;
constexpr int kIdEdit        = 1105;
constexpr int kIdRemove      = 1106;
constexpr int kIdSegBase     = 1110;  // 1110..1113, one per uni::Mode
constexpr int kIdKindBase    = 1120;  // 1120..1121, one per wordlist::Kind
constexpr int kIdToggleLinks = 1130;
constexpr int kIdToggleAuto  = 1131;

// --- logical (96-dpi) metrics; every one of these goes through S() ---------
//
// Vertical chain, top down:  20 pad
//                            12 eyebrow | 8 | 32 mode bar | 8 | 18 description
//                            12
//                            74 specimen card
//                            20 section
//                            12 eyebrow | 8 | 28 list bar | 8
//                            ** list card: the only elastic row **
//                             8 | 17 policy line
//                            10 | 32 add row
//                            12 | 26 switch | 4 | 42 switch + sub-line
//                            20 section
//                            12 eyebrow | 8 | 88 keybinds card
//                            12 | 32 footer | 20 pad
//
// Fixed cost = 603, so the 680 default client leaves 77 for the list (two full
// 30px rows plus a hint of the third) and the 628 minimum still leaves the 24
// the empty state needs. 628 rather than the round 600: the sections above
// simply do not compress below 603 without shaving the 20/8 rhythm everywhere.
constexpr int kClientW    = 560;
constexpr int kClientH    = 680;
constexpr int kMinClientW = 520;
constexpr int kMinClientH = 628;

constexpr int kPad        = 20;
constexpr int kSectionGap = 20;
constexpr int kLabelGap   = 8;

constexpr int kEyebrowH   = 12;
constexpr int kSegH       = 32;
constexpr int kKindSegH   = 28;  // the list bar is a sub-selector, so it is shorter
constexpr int kSegDescGap = 8;
constexpr int kModeDescH  = 18;
constexpr int kSpecGap    = 12;  // description -> specimen card

constexpr int kSpecPadX    = 14;
constexpr int kSpecPadY    = 8;
constexpr int kSpecLineH   = 18;
constexpr int kSpecLineGap = 2;  // source line -> converted line, kept tight
constexpr int kSpecPairGap = 6;  // converted line -> code-point pairs
constexpr int kSpecPairH   = 14;
constexpr int kSpecimenH   = kSpecPadY * 2 + kSpecLineH * 2 + kSpecLineGap +
                             kSpecPairGap + kSpecPairH;  // 74

constexpr int kPolicyGap     = 8;   // list card -> policy sentence
constexpr int kPolicyH       = 17;
constexpr int kListGap       = 10;  // policy sentence -> add row
constexpr int kListMinH      = 24;
constexpr int kListItemH     = 30;
constexpr int kListTextInset = 14;
constexpr int kSelBarW       = 3;

constexpr int kRowH   = 32;  // edit field and every push button
constexpr int kEditH  = 20;  // the bare EDIT inside that field
constexpr int kRowGap = 10;  // between edit / Add / Remove

// Switch rows. The BUTTON is grown kTogglePadX past the content margin on both
// sides so the track lands exactly on the margin and the focus ring still has
// room to clear it; the ring bleeds into the window padding, which is empty.
constexpr int kToggleGap    = 12;  // add row -> first switch
constexpr int kToggleH      = 26;
constexpr int kToggleRowGap = 4;
constexpr int kToggleSubH   = 16;  // the faint line under "Convert on copy"
constexpr int kTogglePadX   = 8;
constexpr int kSwitchW      = 34;
constexpr int kSwitchH      = 18;
constexpr int kSwitchInset  = 3;   // track edge -> knob
constexpr int kSwitchGap    = 12;  // track -> label

// Keybinds card. Cap width is measured from the cap's own text, so "Numpad 9"
// and "Shift" size themselves; only the height is fixed.
constexpr int kKeyPadX     = 14;
constexpr int kKeyPadY     = 8;
constexpr int kKeyRowH     = 24;
constexpr int kKeyDescGap  = 12;  // cap group -> description, and chip -> text
constexpr int kCapH        = 22;
constexpr int kCapPadX     = 8;
constexpr int kCapRadius   = 4;
constexpr int kCapGap      = 6;   // either side of the "+"
constexpr int kChipH       = 16;
constexpr int kChipPadX    = 7;
// Card height is derived from the *scaled* padding and row height in Layout(),
// never from a scaled logical total: at 150% the two round differently and the
// last row would sit a pixel outside its own card. Logically it is 88.

constexpr int kFooterGap = 12;
constexpr int kFooterH   = 16;  // one mono line: the visible list's file path

constexpr int kBtnPadX    = 18;
constexpr int kBtnMinW    = 76;
constexpr int kSegMinW    = 78;
constexpr int kFieldMinW  = 120;
constexpr int kCardRadius = 8;
constexpr int kCtlRadius  = 6;
constexpr int kRowRadius  = 4;
constexpr int kEditInsetX = 10;
constexpr int kFocusInset = 3;
constexpr int kSepInsetY  = 7;  // segment hairline separators are inset

// --- copy ------------------------------------------------------------------

constexpr wchar_t kModeEyebrow[] = L"MODE";
constexpr wchar_t kListEyebrow[] = L"WORD LISTS";
constexpr wchar_t kKeysEyebrow[] = L"KEYBINDS";
constexpr wchar_t kAddText[]     = L"Add";
constexpr wchar_t kRemoveText[]  = L"Remove";
constexpr wchar_t kCloseText[]   = L"Close";
constexpr wchar_t kPlaceholder[] = L"Add a word or phrase";
constexpr wchar_t kPlus[]        = L"+";
constexpr wchar_t kChipOn[]      = L"on";
constexpr wchar_t kChipOff[]     = L"off";

// The segmented bar over the list card names the policy, not the file: the
// visible tab *is* the active policy.
const wchar_t* KindLabel(int index)
{
    return (index == static_cast<int>(Kind::Only)) ? L"Only convert" : L"Never convert";
}

// One quiet sentence under the card, in the same voice as the mode
// descriptions: what happens to everything that is *not* on the list.
const wchar_t* PolicyText(Kind kind)
{
    // "you copy" would be wrong: conversion normally happens on the paste
    // hotkey. Copying only converts when the separate toggle below is on.
    return (kind == Kind::Only)
               ? L"Nothing is converted except the words on this list."
               : L"Everything is converted except the words on this list.";
}

// An empty Only list converts nothing at all, which looks exactly like a
// broken application; that is the one body line allowed to be amber.
const wchar_t* EmptyText(Kind kind)
{
    return (kind == Kind::Only)
               ? L"Nothing will be converted until you add a word to this list."
               : L"No words yet. Anything you add here is pasted unchanged.";
}

const wchar_t* KindNoun(Kind kind)
{
    return (kind == Kind::Only) ? L"only-convert list" : L"never-convert list";
}

constexpr wchar_t kToggleLinksText[] = L"Never convert links, paths and e-mails";
constexpr wchar_t kToggleAutoText[]  = L"Convert on copy (Ctrl+C)";
constexpr wchar_t kToggleAutoSub[]   =
    L"Rewrites the clipboard the moment anything is copied.";

// The specimen sample. Fixed on purpose: upper case, lower case, a space and a
// bare domain, so every mode has something to change, something to leave alone,
// and link protection has something to demonstrate.
constexpr wchar_t kSample[] = L"Copy this from example.com now";

constexpr wchar_t kNoSubstitutions[] = L"No substitutions in this mode.";

struct KeyBind {
    const wchar_t* caps[2];
    const wchar_t* text;
};

// The chords the low-level keyboard hook owns, in the order they were added.
constexpr KeyBind kBinds[kBindCount] = {
    { { L"Shift", L"Numpad 9" }, L"Convert the clipboard and paste it" },
    { { L"Shift", L"Numpad 8" }, L"Cycle the conversion mode" },
    { { L"Shift", L"Numpad 7" }, L"Toggle convert on copy" },
};

// One quiet sentence per mode, written from the tables in spoof.cpp rather
// than from marketing.
const wchar_t* ModeDescription(int mode)
{
    switch (mode) {
    case 1:
        return L"Swaps twenty-five letters; a heavier disguise, and a few glyphs look off.";
    case 2:
        return L"Advanced glyph for the first character only, then basic for the rest.";
    case 3:
        return L"Advanced for the first two characters basic cannot change; basic elsewhere.";
    case 0:
    default:
        return L"Swaps ten letters (a c e i j o p s x y) for Cyrillic look-alikes.";
    }
}

// --- resolved at runtime so this TU never depends on the project's WINVER ---

using GetDpiForWindowFn          = UINT(WINAPI*)(HWND);
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

// Every rectangle the window draws or positions, in client coordinates.
// Computed once per size change so WM_PAINT and SetWindowPos can never
// disagree about where anything is.
struct Rects {
    RECT modeEyebrow{};
    RECT segCell[kModeCount]{};
    RECT modeDesc{};
    RECT specimen{};
    RECT listEyebrow{};
    RECT kindCell[kKindCount]{};
    RECT listCard{};
    RECT listBox{};
    RECT policyLine{};
    RECT field{};
    RECT editBox{};
    RECT addBtn{};
    RECT removeBtn{};
    RECT toggleLinks{};
    RECT toggleAuto{};
    RECT keysEyebrow{};
    RECT keysCard{};
    RECT pathLine{};
    RECT closeBtn{};
};

// The live preview, recomputed whenever the mode, the options or either list
// changes.
struct Specimen {
    std::wstring                             converted;
    std::vector<bool>                        changed;  // per source index
    std::vector<std::pair<wchar_t, wchar_t>> pairs;    // deduped, first-seen order
};

struct State {
    HINSTANCE hInst           = nullptr;
    Callbacks cb{};
    bool      initialised     = false;
    bool      classRegistered = false;
    bool      saveWarned      = false;  // one modal per failure streak
    bool      applying        = false;  // re-entrancy guard around every setter
    bool      synced          = false;  // first SyncState() must refresh the list

    HWND hwnd                = nullptr;
    HWND segment[kModeCount] = { nullptr, nullptr, nullptr, nullptr };
    HWND kind[kKindCount]    = { nullptr, nullptr };
    HWND list                = nullptr;
    HWND edit                = nullptr;
    HWND add                 = nullptr;
    HWND remove              = nullptr;
    HWND toggleLinks         = nullptr;
    HWND toggleAuto          = nullptr;
    HWND close               = nullptr;
    HWND hover               = nullptr;  // at most one control is hovered

    HBRUSH surfaceBrush = nullptr;
    HBRUSH baseBrush    = nullptr;

    int                  dpi         = 96;
    int                  mode        = 0;
    uni::policy::Options options{};
    bool                 autoConvert = false;
    Kind                 listKind    = Kind::Never;
    bool                 loaded[kKindCount] = { false, false };
    bool                 editFocused = false;
    bool                 editEmpty   = true;

    Rects        r;
    Specimen     spec;
    std::wstring pathText;
    std::wstring countText;

    bool                       apisResolved = false;
    GetDpiForWindowFn          dpiForWindow = nullptr;
    AdjustWindowRectExForDpiFn adjustForDpi = nullptr;
};

State g;

// --- small helpers ---------------------------------------------------------

void ResolveApis()
{
    if (g.apisResolved)
        return;
    g.apisResolved = true;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;

    g.dpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(user32, "GetDpiForWindow"));
    g.adjustForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
        GetProcAddress(user32, "AdjustWindowRectExForDpi"));
}

int SystemDpi()
{
    int dpi = 96;
    HDC screen = GetDC(nullptr);
    if (screen) {
        const int x = GetDeviceCaps(screen, LOGPIXELSX);
        if (x >= 72)
            dpi = x;
        ReleaseDC(nullptr, screen);
    }
    return dpi;
}

int QueryDpi(HWND hwnd)
{
    ResolveApis();
    if (hwnd && g.dpiForWindow) {
        const UINT dpi = g.dpiForWindow(hwnd);
        if (dpi >= 72)
            return static_cast<int>(dpi);
    }
    return SystemDpi();
}

// theme::S() reads theme::Dpi(), so the theme module is the single source of
// truth for scaling; g.dpi only mirrors it for AdjustWindowRectExForDpi.
void AdoptDpi(int dpi)
{
    g.dpi = (dpi >= 72) ? dpi : SystemDpi();
    theme::SetDpi(g.dpi);
}

std::wstring Trim(const std::wstring& text)
{
    size_t first = 0;
    size_t last  = text.size();
    while (first < last && iswspace(text[first]))
        ++first;
    while (last > first && iswspace(text[last - 1]))
        --last;
    return text.substr(first, last - first);
}

inline RECT MakeRect(int x, int y, int w, int h)
{
    RECT r;
    r.left   = x;
    r.top    = y;
    r.right  = x + w;
    r.bottom = y + h;
    return r;
}

inline RECT Deflate(const RECT& r, int dx, int dy)
{
    RECT out = r;
    out.left += dx;
    out.right -= dx;
    out.top += dy;
    out.bottom -= dy;
    return out;
}

inline int Width(const RECT& r) { return static_cast<int>(r.right - r.left); }
inline int Height(const RECT& r) { return static_cast<int>(r.bottom - r.top); }

void Repaint(const RECT& r)
{
    if (g.hwnd)
        InvalidateRect(g.hwnd, &r, FALSE);
}

// A hovered/unhovered owner-drawn control repaints itself, not the window:
// BS_OWNERDRAW buttons are children, so invalidating just the button keeps the
// hover cost to one WM_DRAWITEM for one cell.
void RepaintControl(HWND control)
{
    if (control)
        InvalidateRect(control, nullptr, FALSE);
}

HBRUSH EnsureBrush(HBRUSH& slot, COLORREF colour)
{
    if (!slot)
        slot = CreateSolidBrush(colour);
    return slot;
}

void FreeBrushes()
{
    if (g.surfaceBrush) {
        DeleteObject(g.surfaceBrush);
        g.surfaceBrush = nullptr;
    }
    if (g.baseBrush) {
        DeleteObject(g.baseBrush);
        g.baseBrush = nullptr;
    }
}

inline int KindIndex(Kind kind) { return static_cast<int>(kind); }

inline Kind KindAt(int index)
{
    return (index == static_cast<int>(Kind::Only)) ? Kind::Only : Kind::Never;
}

const std::vector<std::wstring>& VisibleEntries()
{
    return uni::wordlist::Entries(g.listKind);
}

// A list is read from disk at most once per session; every mutation writes it
// straight back, so the in-memory vector stays authoritative afterwards.
void EnsureLoaded(Kind kind)
{
    const int index = KindIndex(kind);
    if (index < 0 || index >= kKindCount || g.loaded[index])
        return;
    g.loaded[index] = true;
    uni::wordlist::Load(kind);
}

// --- specimen --------------------------------------------------------------

// Runs the real pipeline - uni::policy::Apply, mask and all - so the preview
// cannot drift from what a paste would actually produce: blacklist mode and
// link protection are both visible in it.
void RebuildSpecimen()
{
    const std::wstring_view sample(kSample);

    g.spec.converted =
        uni::policy::Apply(sample, static_cast<uni::Mode>(g.mode), g.options);
    g.spec.changed.assign(sample.size(), false);
    g.spec.pairs.clear();

    // Convert() is strictly 1:1 on code units, but never assume that of code
    // that lives in another translation unit: walk the shorter of the two.
    const size_t count = g.spec.converted.size() < sample.size()
                             ? g.spec.converted.size()
                             : sample.size();
    for (size_t i = 0; i < count; ++i) {
        const wchar_t from = sample[i];
        const wchar_t to   = g.spec.converted[i];
        if (from == to)
            continue;

        g.spec.changed[i] = true;

        bool seen = false;
        for (const std::pair<wchar_t, wchar_t>& pair : g.spec.pairs) {
            if (pair.first == from && pair.second == to) {
                seen = true;
                break;
            }
        }
        if (!seen)
            g.spec.pairs.emplace_back(from, to);
    }
}

// "x -> U+0445". Split from the rendering so the U+XXXX token can be measured
// and coloured separately.
std::wstring CodePointToken(wchar_t c)
{
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring         out    = L"U+";
    const unsigned       value  = static_cast<unsigned>(c);
    for (int shift = 12; shift >= 0; shift -= 4)
        out.push_back(kHex[(value >> shift) & 0xFu]);
    return out;
}

void RefreshCountText()
{
    const size_t count = VisibleEntries().size();
    g.countText        = std::to_wstring(count);
    g.countText += (count == 1) ? L" word" : L" words";
}

// --- measurement -----------------------------------------------------------

// Layout runs outside WM_PAINT, so it measures against a scratch screen DC
// rather than a theme::Canvas.
SIZE TextSize(const wchar_t* text, theme::Font role)
{
    SIZE size = { 0, 0 };
    if (!text || !*text)
        return size;

    HDC dc = GetDC(g.hwnd);
    if (!dc)
        return size;

    HGDIOBJ old = SelectObject(dc, theme::Get(role));
    GetTextExtentPoint32W(dc, text, static_cast<int>(std::wcslen(text)), &size);
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    return size;
}

int ButtonWidth(const wchar_t* text, theme::Font role)
{
    const int w = static_cast<int>(TextSize(text, role).cx) + 2 * S(kBtnPadX);
    const int floorW = S(kBtnMinW);
    return w > floorW ? w : floorW;
}

// --- geometry --------------------------------------------------------------

void WindowSizeFor(int dpi, int logicalW, int logicalH, int& outW, int& outH)
{
    RECT r = { 0, 0, MulDiv(logicalW, dpi, 96), MulDiv(logicalH, dpi, 96) };
    ResolveApis();

    BOOL ok = FALSE;
    if (g.adjustForDpi)
        ok = g.adjustForDpi(&r, kStyle, FALSE, kExStyle, static_cast<UINT>(dpi));
    if (!ok) {
        r.left   = 0;
        r.top    = 0;
        r.right  = MulDiv(logicalW, dpi, 96);
        r.bottom = MulDiv(logicalH, dpi, 96);
        AdjustWindowRectEx(&r, kStyle, FALSE, kExStyle);
    }

    outW = Width(r);
    outH = Height(r);
}

void WorkAreaOf(HMONITOR mon, RECT* out)
{
    MONITORINFO mi;
    mi.cbSize = static_cast<DWORD>(sizeof(mi));
    if (mon && GetMonitorInfoW(mon, &mi)) {
        *out = mi.rcWork;
        return;
    }
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, out, 0)) {
        out->left   = 0;
        out->top    = 0;
        out->right  = GetSystemMetrics(SM_CXSCREEN);
        out->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

void CentreOn(HMONITOR mon)
{
    if (!g.hwnd)
        return;

    int w = 0;
    int h = 0;
    WindowSizeFor(g.dpi, kClientW, kClientH, w, h);

    RECT work;
    WorkAreaOf(mon, &work);

    // A short work area (a laptop panel, a tall taskbar) shrinks the window
    // rather than pushing its footer off the bottom edge.
    if (h > Height(work)) {
        h = Height(work);
        int minW = 0;
        int minH = 0;
        WindowSizeFor(g.dpi, kMinClientW, kMinClientH, minW, minH);
        if (h < minH)
            h = minH;
    }

    int x = work.left + (Width(work) - w) / 2;
    int y = work.top + (Height(work) - h) / 2;
    if (x < work.left)
        x = work.left;
    if (y < work.top)
        y = work.top;

    SetWindowPos(g.hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Lays one connected bar of `count` equal cells into `cells`. Boundaries are
// interpolated so rounding never leaves a one-pixel crack between neighbours.
void LayoutBar(RECT* cells, int count, const wchar_t* const* labels, int left, int top,
               int contentW, int cellH)
{
    int cellW = S(kSegMinW);
    for (int i = 0; i < count; ++i) {
        const int need =
            static_cast<int>(TextSize(labels[i], theme::Font::Strong).cx) + 2 * S(kBtnPadX);
        if (need > cellW)
            cellW = need;
    }

    int barW = cellW * count;
    if (barW > contentW)
        barW = contentW;
    for (int i = 0; i < count; ++i) {
        const int x0 = left + MulDiv(i, barW, count);
        const int x1 = left + MulDiv(i + 1, barW, count);
        cells[i]     = MakeRect(x0, top, x1 - x0, cellH);
    }
}

// Single source of truth for every rectangle: WM_PAINT reads g.r, and the real
// controls are positioned from the same struct, so a drawn frame and the child
// it wraps can never disagree. Called from WM_SIZE, WM_DPICHANGED and once
// after creation. Idempotent.
void Layout(int width, int height)
{
    if (!g.hwnd || width <= 0 || height <= 0)
        return;

    Rects&    r        = g.r;
    const int pad      = S(kPad);
    const int left     = pad;
    const int contentW = (width - 2 * pad > 0) ? width - 2 * pad : 1;

    // --- top block, laid out downwards -------------------------------------
    int y = pad;

    r.modeEyebrow = MakeRect(left, y, contentW, S(kEyebrowH));
    y = r.modeEyebrow.bottom + S(kLabelGap);

    const wchar_t* modeLabels[kModeCount];
    for (int i = 0; i < kModeCount; ++i)
        modeLabels[i] = uni::ModeName(static_cast<uni::Mode>(i));
    LayoutBar(r.segCell, kModeCount, modeLabels, left, y, contentW, S(kSegH));
    y = r.segCell[0].bottom + S(kSegDescGap);

    r.modeDesc = MakeRect(left, y, contentW, S(kModeDescH));
    y = r.modeDesc.bottom + S(kSpecGap);

    r.specimen = MakeRect(left, y, contentW, S(kSpecimenH));
    y = r.specimen.bottom + S(kSectionGap);

    r.listEyebrow = MakeRect(left, y, contentW, S(kEyebrowH));
    y = r.listEyebrow.bottom + S(kLabelGap);

    const wchar_t* kindLabels[kKindCount];
    for (int i = 0; i < kKindCount; ++i)
        kindLabels[i] = KindLabel(i);
    LayoutBar(r.kindCell, kKindCount, kindLabels, left, y, contentW, S(kKindSegH));
    const int listTop = r.kindCell[0].bottom + S(kLabelGap);

    // --- bottom block, laid out upwards ------------------------------------
    const int rowH       = S(kRowH);
    const int footerH    = S(kFooterH);
    const int footerBand = footerH > rowH ? footerH : rowH;
    const int footerTop  = height - pad - footerBand;

    const int closeW = ButtonWidth(kCloseText, theme::Font::Body);
    r.closeBtn = MakeRect(width - pad - closeW, footerTop + (footerBand - rowH) / 2, closeW,
                          rowH);

    int pathW = r.closeBtn.left - S(kRowGap) - left;
    if (pathW < 1)
        pathW = 1;
    r.pathLine = MakeRect(left, footerTop + (footerBand - footerH) / 2, pathW, footerH);

    const int keysH   = 2 * S(kKeyPadY) + kBindCount * S(kKeyRowH);
    const int keysTop = footerTop - S(kFooterGap) - keysH;
    r.keysCard    = MakeRect(left, keysTop, contentW, keysH);
    r.keysEyebrow = MakeRect(left, keysTop - S(kLabelGap) - S(kEyebrowH), contentW,
                             S(kEyebrowH));

    // The switch rows overhang the content margin so the track sits on it.
    const int togPad  = S(kTogglePadX);
    const int togLeft = left - togPad;
    const int togW    = contentW + 2 * togPad;
    const int autoH   = S(kToggleH) + S(kToggleSubH);
    const int autoTop = r.keysEyebrow.top - S(kSectionGap) - autoH;
    r.toggleAuto      = MakeRect(togLeft, autoTop, togW, autoH);

    const int linksTop = autoTop - S(kToggleRowGap) - S(kToggleH);
    r.toggleLinks      = MakeRect(togLeft, linksTop, togW, S(kToggleH));

    const int rowTop  = linksTop - S(kToggleGap) - rowH;
    const int gapRow  = S(kRowGap);
    const int removeW = ButtonWidth(kRemoveText, theme::Font::Body);
    const int addW    = ButtonWidth(kAddText, theme::Font::Strong);

    r.removeBtn = MakeRect(width - pad - removeW, rowTop, removeW, rowH);
    r.addBtn    = MakeRect(r.removeBtn.left - gapRow - addW, rowTop, addW, rowH);

    int fieldW = r.addBtn.left - gapRow - left;
    if (fieldW < S(kFieldMinW))
        fieldW = S(kFieldMinW);
    r.field = MakeRect(left, rowTop, fieldW, rowH);

    // The EDIT is a bare child sitting inside the field we paint; give it the
    // text height only, vertically centred, so the caret never touches the
    // rounded stroke.
    const int editH = S(kEditH);
    r.editBox = MakeRect(r.field.left + S(kEditInsetX), rowTop + (rowH - editH) / 2,
                         Width(r.field) - 2 * S(kEditInsetX), editH);

    const int policyTop = rowTop - S(kListGap) - S(kPolicyH);
    r.policyLine = MakeRect(left, policyTop, contentW, S(kPolicyH));

    // --- the list card absorbs everything that is left ---------------------
    int cardH = policyTop - S(kPolicyGap) - listTop;
    if (cardH < S(kListMinH))
        cardH = S(kListMinH);
    r.listCard = MakeRect(left, listTop, contentW, cardH);
    r.listBox  = Deflate(r.listCard, 1, 1);

    // --- push the real controls onto the computed rectangles ---------------
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    for (int i = 0; i < kModeCount; ++i) {
        if (g.segment[i]) {
            SetWindowPos(g.segment[i], nullptr, r.segCell[i].left, r.segCell[i].top,
                         Width(r.segCell[i]), Height(r.segCell[i]), flags);
        }
    }
    for (int i = 0; i < kKindCount; ++i) {
        if (g.kind[i]) {
            SetWindowPos(g.kind[i], nullptr, r.kindCell[i].left, r.kindCell[i].top,
                         Width(r.kindCell[i]), Height(r.kindCell[i]), flags);
        }
    }
    if (g.list) {
        SetWindowPos(g.list, nullptr, r.listBox.left, r.listBox.top, Width(r.listBox),
                     Height(r.listBox), flags);
        // Clip the listbox to the card's rounded shape. Without this its opaque
        // kSurface fill would square off the four corners the card just drew,
        // and WS_CLIPCHILDREN would stop the parent from repairing them.
        const int inner = S(kCardRadius) - 1;
        SetWindowRgn(g.list,
                     CreateRoundRectRgn(0, 0, Width(r.listBox), Height(r.listBox),
                                        inner * 2, inner * 2),
                     TRUE);
    }
    if (g.edit) {
        SetWindowPos(g.edit, nullptr, r.editBox.left, r.editBox.top, Width(r.editBox),
                     Height(r.editBox), flags);
    }
    if (g.add) {
        SetWindowPos(g.add, nullptr, r.addBtn.left, r.addBtn.top, Width(r.addBtn),
                     Height(r.addBtn), flags);
    }
    if (g.remove) {
        SetWindowPos(g.remove, nullptr, r.removeBtn.left, r.removeBtn.top,
                     Width(r.removeBtn), Height(r.removeBtn), flags);
    }
    if (g.toggleLinks) {
        SetWindowPos(g.toggleLinks, nullptr, r.toggleLinks.left, r.toggleLinks.top,
                     Width(r.toggleLinks), Height(r.toggleLinks), flags);
    }
    if (g.toggleAuto) {
        SetWindowPos(g.toggleAuto, nullptr, r.toggleAuto.left, r.toggleAuto.top,
                     Width(r.toggleAuto), Height(r.toggleAuto), flags);
    }
    if (g.close) {
        SetWindowPos(g.close, nullptr, r.closeBtn.left, r.closeBtn.top, Width(r.closeBtn),
                     Height(r.closeBtn), flags);
    }

    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void LayoutFromClient()
{
    if (!g.hwnd)
        return;
    RECT rc;
    if (GetClientRect(g.hwnd, &rc))
        Layout(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
}

// --- painting --------------------------------------------------------------

constexpr UINT kTextFlags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;

// Segoe UI at 14px has a 19px line box (15 ascent + 4 descent) and the eyebrow
// face a 13px one, so every slot shorter than that - the 12px eyebrows, the
// 18px mode description, the 17px policy line, the 16px sub-line under the
// convert-on-copy switch and the 18px specimen rows - would clip a descender.
// Each of them is surrounded by background, so let the glyphs bleed instead.
// This matters more than it used to: the specimen sample now contains p and y.
constexpr UINT kBleedFlags = kTextFlags | DT_NOCLIP;

// theme::Measure() routes through DrawText(DT_CALCRECT), which silently drops
// trailing spaces from the width. That is fatal here twice over: a lone space
// must still own its column in the glyph-aligned specimen, and the "x -> "
// lead of a substitution pair ends in one. GetTextExtentPoint32W counts every
// advance. Only the Eyebrow role carries letter tracking and neither the
// specimen nor the key caps use it, so the two measurements agree elsewhere.
int SpanWidth(theme::Canvas& c, const wchar_t* s, theme::Font f)
{
    if (!c.dc || !s || !*s)
        return 0;

    SIZE    size = { 0, 0 };
    HGDIOBJ old  = SelectObject(c.dc, theme::Get(f));
    GetTextExtentPoint32W(c.dc, s, static_cast<int>(std::wcslen(s)), &size);
    SelectObject(c.dc, old);
    return static_cast<int>(size.cx);
}

void PaintSpecimen(theme::Canvas& c)
{
    const RECT& card = g.r.specimen;
    theme::FillRounded(c, card, S(kCardRadius), theme::kSurface);
    theme::StrokeRounded(c, card, S(kCardRadius), 1, theme::kLine);

    const int innerLeft  = card.left + S(kSpecPadX);
    const int innerRight = card.right - S(kSpecPadX);
    const int lineH      = S(kSpecLineH);
    const int srcY       = card.top + S(kSpecPadY);
    const int dstY       = srcY + lineH + S(kSpecLineGap);

    // Two lines, one loop: each character gets a cell as wide as the wider of
    // the two glyphs and both are centred in it, so the converted line stays
    // column-for-column under its source however far the substituted glyph's
    // advance width drifts from the Latin original.
    const std::wstring_view source(kSample);
    int                     pen = innerLeft;
    for (size_t i = 0; i < source.size() && pen < innerRight; ++i) {
        const wchar_t srcCh[2] = { source[i], L'\0' };
        const wchar_t dstCh[2] = {
            i < g.spec.converted.size() ? g.spec.converted[i] : source[i], L'\0'
        };

        const int srcW = SpanWidth(c, srcCh, theme::Font::Body);
        const int dstW = SpanWidth(c, dstCh, theme::Font::Body);
        const int cell = (srcW > dstW ? srcW : dstW);

        const bool changed = i < g.spec.changed.size() && g.spec.changed[i];
        RECT       box     = MakeRect(pen, srcY, cell, lineH);
        theme::Text(c, srcCh, box, theme::Font::Body, theme::kTextFaint,
                    DT_CENTER | kBleedFlags);
        box = MakeRect(pen, dstY, cell, lineH);
        theme::Text(c, dstCh, box, theme::Font::Body,
                    changed ? theme::kAccent : theme::kTextDim, DT_CENTER | kBleedFlags);

        pen += cell;
    }

    // Third line: the substitutions themselves, in the mono face.
    const int pairY = dstY + lineH + S(kSpecPairGap);
    const int pairH = S(kSpecPairH);

    if (g.spec.pairs.empty()) {
        RECT box = MakeRect(innerLeft, pairY, innerRight - innerLeft, pairH);
        theme::Text(c, kNoSubstitutions, box, theme::Font::MonoSmall, theme::kTextFaint,
                    kTextFlags);
        return;
    }

    const int     ellipsisW    = SpanWidth(c, L"...", theme::Font::MonoSmall);
    const wchar_t kSeparator[] = L"   ";
    const int     separatorW   = SpanWidth(c, kSeparator, theme::Font::MonoSmall);

    pen = innerLeft;
    for (size_t i = 0; i < g.spec.pairs.size(); ++i) {
        const wchar_t lead[6] = { g.spec.pairs[i].first, L' ', L'-', L'>', L' ', L'\0' };
        const std::wstring token = CodePointToken(g.spec.pairs[i].second);

        const int  leadW  = SpanWidth(c, lead, theme::Font::MonoSmall);
        const int  tokenW = SpanWidth(c, token.c_str(), theme::Font::MonoSmall);
        const bool last   = (i + 1 == g.spec.pairs.size());

        // Reserve room for the ellipsis unless this pair is the last one: a
        // truncated list must always say that it was truncated.
        const int need = leadW + tokenW + (last ? 0 : ellipsisW);
        if (pen + need > innerRight) {
            RECT box = MakeRect(pen, pairY, innerRight - pen, pairH);
            theme::Text(c, L"...", box, theme::Font::MonoSmall, theme::kTextFaint,
                        kTextFlags);
            break;
        }

        RECT box = MakeRect(pen, pairY, leadW, pairH);
        theme::Text(c, lead, box, theme::Font::MonoSmall, theme::kTextFaint, kTextFlags);
        pen += leadW;

        box = MakeRect(pen, pairY, tokenW, pairH);
        theme::Text(c, token.c_str(), box, theme::Font::MonoSmall, theme::kAccent,
                    kTextFlags);
        pen += tokenW + separatorW;
    }
}

// One key cap: a rounded plate with a darker line along the bottom inside edge,
// which is the whole trick that makes it read as a physical key rather than a
// tag. Everything about it is measured from its own text.
void DrawCap(theme::Canvas& c, const RECT& box, const wchar_t* label)
{
    const int radius = S(kCapRadius);
    theme::FillRounded(c, box, radius, theme::kSurfaceHi);
    theme::StrokeRounded(c, box, radius, 1, theme::kLine);
    theme::HLine(c, box.left + radius, box.right - radius, box.bottom - 2, theme::kBase);
    theme::Text(c, label, box, theme::Font::MonoSmall, theme::kText,
                DT_CENTER | kTextFlags);
}

// The keybinds card. Cap widths come from the caps' own text, the descriptions
// share one column derived from the widest cap group, and the third row carries
// a live chip so the panel is a status display rather than a static legend.
void PaintKeybinds(theme::Canvas& c)
{
    const RECT& card = g.r.keysCard;
    theme::FillRounded(c, card, S(kCardRadius), theme::kSurface);
    theme::StrokeRounded(c, card, S(kCardRadius), 1, theme::kLine);

    const int innerLeft  = card.left + S(kKeyPadX);
    const int innerRight = card.right - S(kKeyPadX);
    const int capPad     = S(kCapPadX);
    const int capGap     = S(kCapGap);
    const int capH       = S(kCapH);
    const int rowH       = S(kKeyRowH);
    const int plusW      = SpanWidth(c, kPlus, theme::Font::Body);

    int capW[kBindCount][2];
    int groupW = 0;
    for (int i = 0; i < kBindCount; ++i) {
        for (int k = 0; k < 2; ++k) {
            capW[i][k] =
                SpanWidth(c, kBinds[i].caps[k], theme::Font::MonoSmall) + 2 * capPad;
        }
        const int w = capW[i][0] + capGap + plusW + capGap + capW[i][1];
        if (w > groupW)
            groupW = w;
    }

    const int descLeft = innerLeft + groupW + S(kKeyDescGap);

    for (int i = 0; i < kBindCount; ++i) {
        const int rowTop = card.top + S(kKeyPadY) + i * rowH;
        const int capY   = rowTop + (rowH - capH) / 2;

        int  pen = innerLeft;
        RECT box = MakeRect(pen, capY, capW[i][0], capH);
        DrawCap(c, box, kBinds[i].caps[0]);
        pen += capW[i][0] + capGap;

        box = MakeRect(pen, rowTop, plusW, rowH);
        theme::Text(c, kPlus, box, theme::Font::Body, theme::kTextFaint,
                    DT_CENTER | kTextFlags);
        pen += plusW + capGap;

        box = MakeRect(pen, capY, capW[i][1], capH);
        DrawCap(c, box, kBinds[i].caps[1]);

        int descRight = innerRight;
        if (i == kAutoBind) {
            const wchar_t* chip  = g.autoConvert ? kChipOn : kChipOff;
            const int      chipH = S(kChipH);
            const int      chipW =
                SpanWidth(c, chip, theme::Font::MonoSmall) + 2 * S(kChipPadX);

            box = MakeRect(innerRight - chipW, rowTop + (rowH - chipH) / 2, chipW, chipH);
            theme::FillRounded(c, box, chipH / 2, theme::kSurfaceHi);
            theme::StrokeRounded(c, box, chipH / 2, 1,
                                 g.autoConvert ? theme::kAccentDim : theme::kLine);
            theme::Text(c, chip, box, theme::Font::MonoSmall,
                        g.autoConvert ? theme::kAccent : theme::kTextFaint,
                        DT_CENTER | kTextFlags);
            descRight = box.left - S(kKeyDescGap);
        }

        if (descRight > descLeft) {
            box = MakeRect(descLeft, rowTop, descRight - descLeft, rowH);
            theme::Text(c, kBinds[i].text, box, theme::Font::Body, theme::kTextDim,
                        kTextFlags | DT_END_ELLIPSIS);
        }
    }
}

void PaintClient(theme::Canvas& c, int width, int height)
{
    const Rects& r = g.r;

    const RECT all = MakeRect(0, 0, width, height);
    theme::Fill(c, all, theme::kBase);

    theme::Text(c, kModeEyebrow, r.modeEyebrow, theme::Font::Eyebrow, theme::kTextDim,
                kBleedFlags);
    theme::Text(c, ModeDescription(g.mode), r.modeDesc, theme::Font::Body,
                theme::kTextDim, kBleedFlags);

    PaintSpecimen(c);

    theme::Text(c, kListEyebrow, r.listEyebrow, theme::Font::Eyebrow, theme::kTextDim,
                kBleedFlags);
    theme::Text(c, g.countText.c_str(), r.listEyebrow, theme::Font::Eyebrow,
                theme::kTextFaint, kBleedFlags | DT_RIGHT);

    theme::FillRounded(c, r.listCard, S(kCardRadius), theme::kSurface);
    theme::StrokeRounded(c, r.listCard, S(kCardRadius), 1, theme::kLine);
    if (VisibleEntries().empty()) {
        // The listbox is hidden while it is empty (see SyncListVisibility), so
        // this is the only thing inside the card and it is not painted over.
        // An empty Only list means nothing is converted at all, so that one
        // sentence earns the accent colour; every other body line stays dim.
        const bool warn = (g.listKind == Kind::Only);
        RECT       box  = Deflate(r.listCard, S(kSpecPadX), 0);
        theme::Text(c, EmptyText(g.listKind), box, theme::Font::Body,
                    warn ? theme::kAccent : theme::kTextFaint,
                    DT_CENTER | DT_END_ELLIPSIS | kTextFlags);
    }

    theme::Text(c, PolicyText(g.listKind), r.policyLine, theme::Font::Body,
                theme::kTextDim, kBleedFlags | DT_END_ELLIPSIS);

    // The edit's own background is the same kSurface, so the field reads as one
    // shape; only the stroke tells you where the focus is.
    theme::FillRounded(c, r.field, S(kCtlRadius), theme::kSurface);
    theme::StrokeRounded(c, r.field, S(kCtlRadius), 1,
                         g.editFocused ? theme::kAccent : theme::kLine);

    theme::Text(c, kKeysEyebrow, r.keysEyebrow, theme::Font::Eyebrow, theme::kTextDim,
                kBleedFlags);
    PaintKeybinds(c);

    theme::Text(c, g.pathText.c_str(), r.pathLine, theme::Font::MonoSmall,
                theme::kTextFaint, kTextFlags | DT_PATH_ELLIPSIS);
}

void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    ZeroMemory(&ps, sizeof(ps));
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) {
        EndPaint(hwnd, &ps);
        return;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int width  = Width(rc);
    const int height = Height(rc);

    theme::Canvas canvas;
    if (width > 0 && height > 0 && theme::CanvasBegin(dc, width, height, &canvas)) {
        // One backbuffer for the whole client area: every pixel below the
        // children is composed off-screen and blitted once.
        PaintClient(canvas, width, height);
        theme::CanvasBlit(dc, 0, 0, canvas);
        theme::CanvasEnd(&canvas);
    } else if (HBRUSH brush = EnsureBrush(g.baseBrush, theme::kBase)) {
        FillRect(dc, &ps.rcPaint, brush);
    }

    EndPaint(hwnd, &ps);
}

// --- owner draw ------------------------------------------------------------

// Every owner-drawn cell composes into its own small canvas and blits once,
// which is what keeps hover repaints from flashing.
bool CellCanvas(const DRAWITEMSTRUCT* dis, theme::Canvas* canvas, RECT* local)
{
    const int w = Width(dis->rcItem);
    const int h = Height(dis->rcItem);
    if (w <= 0 || h <= 0)
        return false;
    if (!theme::CanvasBegin(dis->hDC, w, h, canvas))
        return false;
    *local = MakeRect(0, 0, w, h);
    return true;
}

// Shared by both segmented bars: the mode bar picks a uni::Mode, the word-list
// bar picks a uni::wordlist::Kind *and* the conversion policy with it.
void DrawSegment(const DRAWITEMSTRUCT* dis, int index, int count, int active,
                 const wchar_t* text)
{
    theme::Canvas c;
    RECT          box;
    if (!CellCanvas(dis, &c, &box))
        return;

    const bool selected = (index == active);
    const bool hovered  = (g.hover == dis->hwndItem);
    const int  radius   = S(kCtlRadius);

    COLORREF fill = theme::kSurface;
    if (selected)
        fill = theme::kAccent;
    else if (hovered)
        fill = theme::kSurfaceHi;

    // Only the outer corners of the bar are rounded: the first cell rounds
    // left, the last rounds right, anything in between stays square. Squaring
    // is done by refilling a radius-wide strip on the joined edge.
    theme::Fill(c, box, theme::kBase);
    if (index == 0 || index == count - 1) {
        theme::FillRounded(c, box, radius, fill);
        const RECT strip = (index == 0)
                               ? MakeRect(box.right - radius, box.top, radius, Height(box))
                               : MakeRect(box.left, box.top, radius, Height(box));
        theme::Fill(c, strip, fill);
    } else {
        theme::Fill(c, box, fill);
    }

    // Hairline between neighbours, drawn by the right-hand cell and suppressed
    // wherever it would land against the amber fill.
    if (index > 0 && !selected && index - 1 != active) {
        const RECT sep = MakeRect(box.left, box.top + S(kSepInsetY), 1,
                                  Height(box) - 2 * S(kSepInsetY));
        theme::Fill(c, sep, theme::kLine);
    }

    COLORREF ink = theme::kTextDim;
    if (selected)
        ink = theme::kAccentInk;
    else if (hovered)
        ink = theme::kText;
    theme::Text(c, text, box, selected ? theme::Font::Strong : theme::Font::Body, ink,
                DT_CENTER | kTextFlags);

    if (dis->itemState & ODS_FOCUS) {
        const RECT ring = Deflate(box, S(kFocusInset), S(kFocusInset));
        theme::StrokeRounded(c, ring, S(kRowRadius), 1,
                             selected ? theme::kAccentInk : theme::kAccent);
    }

    theme::CanvasBlit(dis->hDC, dis->rcItem.left, dis->rcItem.top, c);
    theme::CanvasEnd(&c);
}

void DrawButton(const DRAWITEMSTRUCT* dis, bool primary, const wchar_t* text)
{
    theme::Canvas c;
    RECT          box;
    if (!CellCanvas(dis, &c, &box))
        return;

    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool hovered  = (g.hover == dis->hwndItem) && !disabled;
    const int  radius   = S(kCtlRadius);

    COLORREF fill   = theme::kSurface;
    COLORREF ink    = theme::kText;
    bool     stroke = false;

    if (disabled) {
        ink = theme::kTextFaint;
    } else if (primary) {
        fill = pressed ? theme::kAccentDim
                       : (hovered ? theme::Mix(theme::kAccent, RGB(255, 255, 255), 28)
                                  : theme::kAccent);
        ink  = theme::kAccentInk;
    } else {
        fill   = pressed ? theme::kBase : (hovered ? theme::kSurfaceHi : theme::kSurface);
        stroke = true;
    }

    theme::Fill(c, box, theme::kBase);
    theme::FillRounded(c, box, radius, fill);
    if (stroke)
        theme::StrokeRounded(c, box, radius, 1, theme::kLine);

    theme::Text(c, text, box, primary ? theme::Font::Strong : theme::Font::Body, ink,
                DT_CENTER | kTextFlags);

    // A real focus ring: the system's dotted rectangle is invisible on a dark
    // surface and reads as dirt where it is visible.
    if ((dis->itemState & ODS_FOCUS) && !disabled) {
        const RECT ring = Deflate(box, S(kFocusInset), S(kFocusInset));
        theme::StrokeRounded(c, ring, S(kRowRadius), 1,
                             primary ? theme::kAccentInk : theme::kAccent);
    }

    theme::CanvasBlit(dis->hDC, dis->rcItem.left, dis->rcItem.top, c);
    theme::CanvasEnd(&c);
}

// A drawn pill switch on a real BUTTON: Tab reaches it, Space and Enter flip
// it, and the amber ring is the same one every other control uses.
void DrawToggle(const DRAWITEMSTRUCT* dis, bool on, const wchar_t* label,
                const wchar_t* sub)
{
    theme::Canvas c;
    RECT          box;
    if (!CellCanvas(dis, &c, &box))
        return;

    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool hovered = (g.hover == dis->hwndItem);

    theme::Fill(c, box, theme::kBase);
    if (pressed || hovered) {
        theme::FillRounded(c, box, S(kCtlRadius), pressed ? theme::kSurfaceHi
                                                          : theme::kSurface);
    }

    const int padX   = S(kTogglePadX);
    const int trackH = S(kSwitchH);
    const int rowH   = S(kToggleH);
    const RECT track = MakeRect(box.left + padX, box.top + (rowH - trackH) / 2,
                                S(kSwitchW), trackH);

    theme::FillRounded(c, track, trackH / 2, on ? theme::kAccent : theme::kSurfaceHi);
    if (!on)
        theme::StrokeRounded(c, track, trackH / 2, 1, theme::kLine);

    const int inset = S(kSwitchInset);
    const int knob  = trackH - 2 * inset;
    const int knobX = on ? (track.right - inset - knob) : (track.left + inset);
    const RECT knobBox = MakeRect(knobX, track.top + inset, knob, knob);
    theme::FillRounded(c, knobBox, knob / 2, on ? theme::kAccentInk : theme::kTextDim);

    const int textLeft  = track.right + S(kSwitchGap);
    const int textWidth = box.right - padX - textLeft;
    if (textWidth > 0) {
        RECT labelBox = MakeRect(textLeft, box.top, textWidth, rowH);
        theme::Text(c, label, labelBox, theme::Font::Body, theme::kText,
                    kTextFlags | DT_END_ELLIPSIS);
        if (sub) {
            RECT subBox = MakeRect(textLeft, box.top + rowH, textWidth, S(kToggleSubH));
            theme::Text(c, sub, subBox, theme::Font::Body, theme::kTextFaint,
                        kBleedFlags | DT_END_ELLIPSIS);
        }
    }

    if (dis->itemState & ODS_FOCUS) {
        const RECT ring = Deflate(box, S(kFocusInset), S(kFocusInset));
        theme::StrokeRounded(c, ring, S(kCtlRadius), 1, theme::kAccent);
    }

    theme::CanvasBlit(dis->hDC, dis->rcItem.left, dis->rcItem.top, c);
    theme::CanvasEnd(&c);
}

void DrawListItem(const DRAWITEMSTRUCT* dis)
{
    theme::Canvas c;
    RECT          box;
    if (!CellCanvas(dis, &c, &box))
        return;

    theme::Fill(c, box, theme::kSurface);

    if (dis->itemID != static_cast<UINT>(-1)) {
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        if (selected) {
            theme::FillRounded(c, box, S(kRowRadius), theme::kSurfaceHi);
            const int  inset = S(kRowRadius);
            const RECT bar   = MakeRect(box.left, box.top + inset, S(kSelBarW),
                                        Height(box) - 2 * inset);
            theme::Fill(c, bar, theme::kAccent);
        }

        std::wstring  text;
        const LRESULT length = SendMessageW(dis->hwndItem, LB_GETTEXTLEN,
                                            static_cast<WPARAM>(dis->itemID), 0);
        if (length > 0) {
            text.resize(static_cast<size_t>(length) + 1, L'\0');
            const LRESULT copied = SendMessageW(dis->hwndItem, LB_GETTEXT,
                                                static_cast<WPARAM>(dis->itemID),
                                                reinterpret_cast<LPARAM>(&text[0]));
            text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        }

        RECT label = box;
        label.left += S(kListTextInset);
        label.right -= S(kLabelGap);
        theme::Text(c, text.c_str(), label,
                    selected ? theme::Font::Strong : theme::Font::Body,
                    selected ? theme::kText : theme::kTextDim,
                    kTextFlags | DT_END_ELLIPSIS);
    }

    theme::CanvasBlit(dis->hDC, dis->rcItem.left, dis->rcItem.top, c);
    theme::CanvasEnd(&c);
}

// --- hover -----------------------------------------------------------------

// BS_OWNERDRAW buttons swallow their own mouse messages, so hover has to be
// observed from inside each control. Only the two cells whose state actually
// changed are invalidated - never the parent - so a mouse sweep across the
// segmented bar costs one WM_DRAWITEM per cell entered and one per cell left.
LRESULT CALLBACK HoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                           UINT_PTR id, DWORD_PTR)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (g.hover != hwnd) {
            HWND previous = g.hover;
            g.hover       = hwnd;
            RepaintControl(previous);
            RepaintControl(hwnd);

            TRACKMOUSEEVENT track;
            track.cbSize      = static_cast<DWORD>(sizeof(track));
            track.dwFlags     = TME_LEAVE;
            track.hwndTrack   = hwnd;
            track.dwHoverTime = 0;
            TrackMouseEvent(&track);
        }
        break;

    case WM_MOUSELEAVE:
        if (g.hover == hwnd) {
            g.hover = nullptr;
            RepaintControl(hwnd);
        }
        break;

    case WM_NCDESTROY:
        if (g.hover == hwnd)
            g.hover = nullptr;
        RemoveWindowSubclass(hwnd, HoverProc, id);
        break;

    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// The EDIT paints its own client, so the placeholder has to be drawn from
// inside it. It only ever shows when the control is empty and unfocused, i.e.
// when the control has nothing of its own to draw, so owning the whole paint
// costs nothing.
LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                          DWORD_PTR)
{
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, EditProc, id);

    if (msg == WM_PAINT && g.editEmpty && !g.editFocused) {
        PAINTSTRUCT ps;
        ZeroMemory(&ps, sizeof(ps));
        HDC dc = BeginPaint(hwnd, &ps);
        if (dc) {
            RECT rc;
            GetClientRect(hwnd, &rc);

            theme::Canvas c;
            if (theme::CanvasBegin(dc, Width(rc), Height(rc), &c)) {
                const RECT box = MakeRect(0, 0, Width(rc), Height(rc));
                theme::Fill(c, box, theme::kSurface);
                theme::Text(c, kPlaceholder, box, theme::Font::Body, theme::kTextFaint,
                            kTextFlags);
                theme::CanvasBlit(dc, 0, 0, c);
                theme::CanvasEnd(&c);
            } else if (HBRUSH brush = EnsureBrush(g.surfaceBrush, theme::kSurface)) {
                FillRect(dc, &ps.rcPaint, brush);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// --- model <-> view --------------------------------------------------------

// The listbox's own WS_VISIBLE bit, not IsWindowVisible: the whole ancestor
// chain is hidden while the settings window itself is, and the empty-state
// logic below has to work before the window is ever shown.
bool ListShown()
{
    return g.list && (GetWindowLongPtrW(g.list, GWL_STYLE) & WS_VISIBLE) != 0;
}

void UpdateRemoveEnabled()
{
    if (!g.remove || !g.list)
        return;

    const bool enable = ListShown() &&
                        SendMessageW(g.list, LB_GETCURSEL, 0, 0) != LB_ERR;
    if (!enable && GetFocus() == g.remove)
        SetFocus(ListShown() ? g.list : g.edit);   // never park focus on a disabled control
    EnableWindow(g.remove, enable ? TRUE : FALSE);
}

// An empty listbox is hidden so the parent's empty-state text is visible
// inside the card: the control is opaque and WS_CLIPCHILDREN would otherwise
// stop the parent from drawing anything in its rectangle.
void SyncListVisibility(bool hasItems)
{
    if (!g.list)
        return;

    const bool visible = ListShown();
    if (visible == hasItems)
        return;

    if (!hasItems && GetFocus() == g.list)
        SetFocus(g.edit ? g.edit : g.hwnd);
    ShowWindow(g.list, hasItems ? SW_SHOWNA : SW_HIDE);
    Repaint(g.r.listCard);
}

// Rebuilds the list from the visible kind's entries. `select` is clamped into
// range; pass -1 for "no selection". The listbox is unsorted, so its indices
// stay 1:1 with the wordlist vector.
void RefreshList(int select)
{
    if (!g.list)
        return;

    SendMessageW(g.list, WM_SETREDRAW, static_cast<WPARAM>(FALSE), 0);
    SendMessageW(g.list, LB_RESETCONTENT, 0, 0);

    const std::vector<std::wstring>& entries = VisibleEntries();
    for (const std::wstring& entry : entries) {
        SendMessageW(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
    }

    SendMessageW(g.list, WM_SETREDRAW, static_cast<WPARAM>(TRUE), 0);
    InvalidateRect(g.list, nullptr, TRUE);

    const int count = static_cast<int>(entries.size());
    int sel = select;
    if (count == 0 || sel < 0)
        sel = -1;
    else if (sel >= count)
        sel = count - 1;
    SendMessageW(g.list, LB_SETCURSEL, static_cast<WPARAM>(static_cast<INT_PTR>(sel)), 0);

    SyncListVisibility(count > 0);
    UpdateRemoveEnabled();

    // Either list can feed the preview, so a mutation has to re-run it.
    RefreshCountText();
    RebuildSpecimen();
    Repaint(g.r.listEyebrow);
    Repaint(g.r.listCard);
    Repaint(g.r.specimen);
}

// Writes the visible list to disk. Reports a failure once per failure streak: a
// modal box on every keystroke-driven mutation would be unusable.
void Persist()
{
    if (uni::wordlist::Save(g.listKind)) {
        g.saveWarned = false;
        return;
    }

    const DWORD error = GetLastError();
    if (g.saveWarned)
        return;
    g.saveWarned = true;

    std::wstring message = L"UniPaste could not save the ";
    message += KindNoun(g.listKind);
    message += L".\n\n";
    message += uni::wordlist::FilePath(g.listKind);
    message += L"\n\nError code: ";
    message += std::to_wstring(error);
    MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

// Only the active cell of a bar is a tab stop, so Tab into it lands on the
// current selection instead of always on the first cell. WS_GROUP stays on
// cell 0 - it marks where the group begins and has nothing to do with the tab
// order.
void SyncBarTabStops(HWND* cells, int count, int active)
{
    for (int i = 0; i < count; ++i) {
        if (!cells[i])
            continue;

        const LONG_PTR style  = GetWindowLongPtrW(cells[i], GWL_STYLE);
        const LONG_PTR wanted = (i == active)
                                    ? (style | WS_TABSTOP)
                                    : (style & ~static_cast<LONG_PTR>(WS_TABSTOP));
        if (wanted != style)
            SetWindowLongPtrW(cells[i], GWL_STYLE, wanted);
    }
}

// Pulls mode, options and auto-convert from the owner and refreshes everything
// that depends on any of them. Never calls back into a setter, so it is safe
// from NotifyStateChanged() however that was reached.
void SyncState()
{
    int mode = g.cb.getMode ? static_cast<int>(g.cb.getMode()) : 0;
    if (mode < 0 || mode >= kModeCount)
        mode = 0;

    uni::policy::Options options;
    if (g.cb.getOptions)
        options = g.cb.getOptions();
    const bool autoConvert = g.cb.getAutoConvert ? g.cb.getAutoConvert() : false;

    // The visible tab is the active policy, always: one concept, one control.
    const Kind kind        = options.blacklistMode ? Kind::Only : Kind::Never;
    const bool kindChanged = (kind != g.listKind) || !g.synced;
    g.synced               = true;

    g.mode        = mode;
    g.options     = options;
    g.autoConvert = autoConvert;
    g.listKind    = kind;

    SyncBarTabStops(g.segment, kModeCount, g.mode);
    SyncBarTabStops(g.kind, kKindCount, KindIndex(g.listKind));

    if (kindChanged) {
        EnsureLoaded(kind);
        g.pathText = uni::wordlist::FilePath(kind);
        RefreshList(-1);       // rebuilds the count and the specimen with it
    } else {
        RefreshCountText();
        RebuildSpecimen();
    }

    for (int i = 0; i < kModeCount; ++i)
        RepaintControl(g.segment[i]);
    for (int i = 0; i < kKindCount; ++i)
        RepaintControl(g.kind[i]);
    RepaintControl(g.toggleLinks);
    RepaintControl(g.toggleAuto);

    Repaint(g.r.modeDesc);
    Repaint(g.r.specimen);
    Repaint(g.r.listEyebrow);
    Repaint(g.r.listCard);
    Repaint(g.r.policyLine);
    Repaint(g.r.keysCard);
    Repaint(g.r.pathLine);
}

void OnModeClicked(int index)
{
    if (index < 0 || index >= kModeCount || !g.cb.setMode)
        return;
    if (g.applying || index == g.mode)
        return;

    // The setter calls straight back through NotifyStateChanged; the guard
    // keeps that round trip one level deep.
    g.applying = true;
    g.cb.setMode(static_cast<uni::Mode>(index));
    g.applying = false;

    SyncState();   // in case the owner clamped or ignored the request
}

// Picking a tab picks the policy: "Never convert" is the normal policy,
// "Only convert" is blacklist mode. Selecting the list and selecting the policy
// are the same act.
void OnKindClicked(int index)
{
    if (index < 0 || index >= kKindCount || !g.cb.setOptions)
        return;
    const bool wanted = (KindAt(index) == Kind::Only);
    if (g.applying || wanted == g.options.blacklistMode)
        return;

    uni::policy::Options options = g.options;
    options.blacklistMode        = wanted;

    g.applying = true;
    g.cb.setOptions(options);
    g.applying = false;

    SyncState();
}

void OnToggleLinks()
{
    if (g.applying || !g.cb.setOptions)
        return;

    uni::policy::Options options = g.options;
    options.protectLinks         = !options.protectLinks;

    g.applying = true;
    g.cb.setOptions(options);
    g.applying = false;

    SyncState();
}

void OnToggleAuto()
{
    if (g.applying || !g.cb.setAutoConvert)
        return;

    g.applying = true;
    g.cb.setAutoConvert(!g.autoConvert);
    g.applying = false;

    SyncState();
}

int IndexIn(HWND* cells, int count, HWND hwnd)
{
    if (!hwnd)
        return -1;
    for (int i = 0; i < count; ++i) {
        if (cells[i] == hwnd)
            return i;
    }
    return -1;
}

// Keyboard selection. Deliberately the same path a mouse click takes, so the
// choice reaches the owner (and therefore the registry) exactly once; the focus
// then follows the selection, keeping the amber ring on the live cell.
void SelectMode(int index)
{
    if (index < 0 || index >= kModeCount || !g.segment[index])
        return;
    OnModeClicked(index);   // no-op when that mode is already active
    SetFocus(g.segment[index]);
}

void SelectKind(int index)
{
    if (index < 0 || index >= kKindCount || !g.kind[index])
        return;
    OnKindClicked(index);
    SetFocus(g.kind[index]);
}

std::wstring EditText()
{
    if (!g.edit)
        return std::wstring();

    wchar_t buffer[kMaxEntry + 1];
    const int length = GetWindowTextW(g.edit, buffer, static_cast<int>(kMaxEntry + 1));
    if (length <= 0)
        return std::wstring();
    return std::wstring(buffer, static_cast<size_t>(length));
}

void SyncEditEmpty()
{
    if (!g.edit)
        return;

    const bool empty = (GetWindowTextLengthW(g.edit) == 0);
    if (empty == g.editEmpty)
        return;
    g.editEmpty = empty;
    RepaintControl(g.edit);   // the placeholder lives inside the edit's paint
}

void FocusEditAll()
{
    if (!g.edit)
        return;
    SetFocus(g.edit);
    SendMessageW(g.edit, EM_SETSEL, 0, static_cast<LPARAM>(-1));
}

void OnAdd()
{
    if (!g.edit)
        return;

    const std::wstring text   = EditText();
    const size_t       before = VisibleEntries().size();

    if (!uni::wordlist::Add(g.listKind, text)) {
        // Empty after trimming, or already present. A modal box for a typo is
        // obnoxious: beep and leave the text so the user can fix it.
        MessageBeep(MB_ICONWARNING);
        FocusEditAll();
        return;
    }

    SetWindowTextW(g.edit, L"");
    SyncEditEmpty();
    RefreshList(-1);

    // The entry normally lands at the end; look it up by name so a list that
    // keeps its entries in some other order still selects the right row.
    int sel = static_cast<int>(before);
    const std::wstring trimmed = Trim(text);
    const LRESULT found = SendMessageW(g.list, LB_FINDSTRINGEXACT,
                                       static_cast<WPARAM>(static_cast<INT_PTR>(-1)),
                                       reinterpret_cast<LPARAM>(trimmed.c_str()));
    if (found != LB_ERR)
        sel = static_cast<int>(found);

    const int count = static_cast<int>(VisibleEntries().size());
    if (sel >= count)
        sel = count - 1;
    SendMessageW(g.list, LB_SETCURSEL, static_cast<WPARAM>(static_cast<INT_PTR>(sel)), 0);
    UpdateRemoveEnabled();

    Persist();
    SetFocus(g.edit);
}

void OnRemove()
{
    if (!g.list)
        return;

    const LRESULT sel = SendMessageW(g.list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;

    if (!uni::wordlist::RemoveAt(g.listKind, static_cast<size_t>(sel))) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    RefreshList(static_cast<int>(sel));   // same index, clamped
    Persist();
    if (ListShown())
        SetFocus(g.list);
    else if (g.edit)
        SetFocus(g.edit);
}

// Double-click pulls an entry back into the edit box and drops it from the
// list: the usual "edit in place" idiom for a plain add/remove list.
void OnEditInPlace()
{
    if (!g.list || !g.edit)
        return;

    const LRESULT sel = SendMessageW(g.list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR)
        return;

    const size_t                     index   = static_cast<size_t>(sel);
    const std::vector<std::wstring>& entries = VisibleEntries();
    if (index >= entries.size())
        return;

    const std::wstring text = entries[index];   // copy: RemoveAt invalidates it
    if (!uni::wordlist::RemoveAt(g.listKind, index)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    SetWindowTextW(g.edit, text.c_str());
    SyncEditEmpty();
    RefreshList(static_cast<int>(sel));
    Persist();
    FocusEditAll();
}

void HideSelf()
{
    if (g.hwnd)
        ShowWindow(g.hwnd, SW_HIDE);
}

// --- window ----------------------------------------------------------------

constexpr UINT_PTR kSubclassHover = 1;
constexpr UINT_PTR kSubclassEdit  = 3;

HWND MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                 DWORD exStyle, int id)
{
    return CreateWindowExW(exStyle, cls, text, style | WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           g.hInst, nullptr);
}

void ApplyFonts()
{
    HFONT body       = theme::Get(theme::Font::Body);
    HWND  controls[] = { g.segment[0], g.segment[1],  g.segment[2], g.segment[3],
                         g.kind[0],    g.kind[1],     g.list,       g.edit,
                         g.add,        g.remove,      g.toggleLinks, g.toggleAuto,
                         g.close };
    for (HWND control : controls) {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
    }
    if (g.list)
        SendMessageW(g.list, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(S(kListItemH)));
}

// Creation order is the tab order: mode bar -> list bar -> list -> edit -> Add
// -> Remove -> the two switches -> Close, which is also the reading order.
//
// Every button is a plain BS_OWNERDRAW. BS_OWNERDRAW is a *type* (0x0B) in
// BS_TYPEMASK, not a modifier, so OR-ing it with BS_RADIOBUTTON (0x04),
// BS_CHECKBOX (0x02) or BS_DEFPUSHBUTTON (0x01) yields another type entirely
// and the parent stops receiving WM_DRAWITEM. The behaviour those styles would
// have bought is provided explicitly instead: HandleDialogMessage() drives the
// bars' select-on-arrow radio semantics from the message loop, the switches
// keep their own state in g, and DM_GETDEFID below routes Enter.
bool CreateControls(HWND parent)
{
    for (int i = 0; i < kModeCount; ++i) {
        // Cell 0 opens the group; the tab stop is handed to whichever cell is
        // active (SyncBarTabStops), so Tab lands on the current selection.
        const DWORD style = BS_OWNERDRAW | (i == 0 ? (WS_GROUP | WS_TABSTOP) : 0u);
        g.segment[i] = MakeControl(parent, L"BUTTON",
                                   uni::ModeName(static_cast<uni::Mode>(i)), style, 0,
                                   kIdSegBase + i);
    }
    for (int i = 0; i < kKindCount; ++i) {
        const DWORD style = BS_OWNERDRAW | (i == 0 ? (WS_GROUP | WS_TABSTOP) : 0u);
        g.kind[i] = MakeControl(parent, L"BUTTON", KindLabel(i), style, 0, kIdKindBase + i);
    }

    g.list = MakeControl(parent, L"LISTBOX", nullptr,
                         LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_HASSTRINGS |
                             LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                         0, kIdList);
    g.edit = MakeControl(parent, L"EDIT", nullptr,
                         ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, 0, kIdEdit);
    g.add    = MakeControl(parent, L"BUTTON", kAddText, BS_OWNERDRAW | WS_TABSTOP, 0, kIdAdd);
    g.remove = MakeControl(parent, L"BUTTON", kRemoveText, BS_OWNERDRAW | WS_TABSTOP, 0,
                           kIdRemove);
    // Each switch is its own group: arrow keys must not walk between them the
    // way they walk a segmented bar.
    g.toggleLinks = MakeControl(parent, L"BUTTON", kToggleLinksText,
                                BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, 0, kIdToggleLinks);
    g.toggleAuto  = MakeControl(parent, L"BUTTON", kToggleAutoText,
                                BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, 0, kIdToggleAuto);
    g.close = MakeControl(parent, L"BUTTON", kCloseText,
                          BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, 0, kIdClose);

    if (!g.list || !g.edit || !g.add || !g.remove || !g.toggleLinks || !g.toggleAuto ||
        !g.close) {
        return false;
    }
    for (int i = 0; i < kModeCount; ++i) {
        if (!g.segment[i])
            return false;
    }
    for (int i = 0; i < kKindCount; ++i) {
        if (!g.kind[i])
            return false;
    }

    // LBS_NOINTEGRALHEIGHT: the listbox must keep the exact height the layout
    // gave it, or its frame would no longer line up with the card drawn around
    // it. DarkMode_Explorer is what makes the scrollbar dark on 1809+; it is a
    // no-op on older builds.
    SetWindowTheme(g.list, L"DarkMode_Explorer", nullptr);

    SendMessageW(g.edit, EM_SETLIMITTEXT, static_cast<WPARAM>(kMaxEntry), 0);
    SetWindowSubclass(g.edit, EditProc, kSubclassEdit, 0);

    for (int i = 0; i < kModeCount; ++i)
        SetWindowSubclass(g.segment[i], HoverProc, kSubclassHover, 0);
    for (int i = 0; i < kKindCount; ++i)
        SetWindowSubclass(g.kind[i], HoverProc, kSubclassHover, 0);
    SetWindowSubclass(g.add, HoverProc, kSubclassHover, 0);
    SetWindowSubclass(g.remove, HoverProc, kSubclassHover, 0);
    SetWindowSubclass(g.toggleLinks, HoverProc, kSubclassHover, 0);
    SetWindowSubclass(g.toggleAuto, HoverProc, kSubclassHover, 0);
    SetWindowSubclass(g.close, HoverProc, kSubclassHover, 0);
    return true;
}

void ApplyWindowIcons(HWND hwnd)
{
    if (HICON big = appicon::Get(GetSystemMetrics(SM_CXICON))) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    }
    if (HICON smallIcon = appicon::Get(GetSystemMetrics(SM_CXSMICON))) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        AdoptDpi(QueryDpi(hwnd));
        theme::DarkTitleBar(hwnd);
        ApplyWindowIcons(hwnd);
        if (!CreateControls(hwnd))
            return -1;
        ApplyFonts();
        EnsureLoaded(Kind::Never);
        EnsureLoaded(Kind::Only);
        SyncState();
        RefreshList(-1);
        return 0;

    case WM_ERASEBKGND:
        // WM_PAINT owns every pixel of the client area.
        return 1;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_SIZE:
        Layout(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (mmi) {
            int w = 0;
            int h = 0;
            WindowSizeFor(g.dpi, kMinClientW, kMinClientH, w, h);
            mmi->ptMinTrackSize.x = w;
            mmi->ptMinTrackSize.y = h;
        }
        return 0;
    }

    case WM_DPICHANGED: {
        AdoptDpi(static_cast<int>(LOWORD(wParam)));
        theme::DarkTitleBar(hwnd);
        ApplyFonts();

        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         Width(*suggested), Height(*suggested),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutFromClient();
        return 0;
    }

    case WM_SETTINGCHANGE:
        // A theme or personalisation change can drop the immersive dark frame.
        theme::DarkTitleBar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis && mis->CtlType == ODT_LISTBOX) {
            mis->itemHeight = static_cast<UINT>(S(kListItemH));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!dis)
            break;
        if (dis->CtlType == ODT_LISTBOX) {
            DrawListItem(dis);
            return TRUE;
        }
        if (dis->CtlType == ODT_BUTTON) {
            const int id = static_cast<int>(dis->CtlID);
            if (id >= kIdSegBase && id < kIdSegBase + kModeCount) {
                const int index = id - kIdSegBase;
                DrawSegment(dis, index, kModeCount, g.mode,
                            uni::ModeName(static_cast<uni::Mode>(index)));
                return TRUE;
            }
            if (id >= kIdKindBase && id < kIdKindBase + kKindCount) {
                const int index = id - kIdKindBase;
                DrawSegment(dis, index, kKindCount, KindIndex(g.listKind),
                            KindLabel(index));
                return TRUE;
            }
            switch (id) {
            case kIdAdd:
                DrawButton(dis, true, kAddText);
                return TRUE;
            case kIdRemove:
                DrawButton(dis, false, kRemoveText);
                return TRUE;
            case kIdClose:
                DrawButton(dis, false, kCloseText);
                return TRUE;
            case kIdToggleLinks:
                DrawToggle(dis, g.options.protectLinks, kToggleLinksText, nullptr);
                return TRUE;
            case kIdToggleAuto:
                DrawToggle(dis, g.autoConvert, kToggleAutoText, kToggleAutoSub);
                return TRUE;
            default:
                break;
            }
        }
        break;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, theme::kText);
        SetBkColor(dc, theme::kSurface);
        return reinterpret_cast<LRESULT>(EnsureBrush(g.surfaceBrush, theme::kSurface));
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, theme::kText);
        SetBkColor(dc, theme::kSurface);
        return reinterpret_cast<LRESULT>(EnsureBrush(g.surfaceBrush, theme::kSurface));
    }

    // Mirror the dialog manager: Enter activates the focused button when one
    // has focus and the Add button otherwise. Without this, IsDialogMessageW
    // would blindly fire IDOK (Add) even when Close, Remove or a switch has it.
    case DM_GETDEFID: {
        int  id    = kIdAdd;
        HWND focus = GetFocus();
        if (focus && IsChild(hwnd, focus)) {
            // An owner-drawn button reports a bare DLGC_BUTTON - no
            // DLGC_UNDEFPUSHBUTTON - so the focused-button test cannot rely on
            // the dialog code alone; our own buttons are recognised by handle.
            const LRESULT code = SendMessageW(focus, WM_GETDLGCODE, 0, 0);
            const bool    push =
                (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) != 0 ||
                focus == g.add || focus == g.remove || focus == g.close ||
                focus == g.toggleLinks || focus == g.toggleAuto;
            if (push)
                id = GetDlgCtrlID(focus);
        }
        return MAKELRESULT(id, DC_HASDEFID);
    }

    case WM_SETFOCUS:
        // Plain windows do not hand focus to a child the way dialogs do.
        if (g.edit) {
            SetFocus(g.edit);
            return 0;
        }
        break;

    case WM_COMMAND: {
        const int id   = static_cast<int>(LOWORD(wParam));
        const int code = static_cast<int>(HIWORD(wParam));
        if (id >= kIdSegBase && id < kIdSegBase + kModeCount) {
            if (code == BN_CLICKED)
                OnModeClicked(id - kIdSegBase);
            return 0;
        }
        if (id >= kIdKindBase && id < kIdKindBase + kKindCount) {
            if (code == BN_CLICKED)
                OnKindClicked(id - kIdKindBase);
            return 0;
        }
        switch (id) {
        case kIdAdd:
            if (code == BN_CLICKED)
                OnAdd();
            return 0;
        case kIdRemove:
            if (code == BN_CLICKED)
                OnRemove();
            return 0;
        case kIdToggleLinks:
            if (code == BN_CLICKED)
                OnToggleLinks();
            return 0;
        case kIdToggleAuto:
            if (code == BN_CLICKED)
                OnToggleAuto();
            return 0;
        case kIdClose:
            if (code == BN_CLICKED)
                HideSelf();
            return 0;
        case kIdList:
            if (code == LBN_SELCHANGE)
                UpdateRemoveEnabled();
            else if (code == LBN_DBLCLK)
                OnEditInPlace();
            return 0;
        case kIdEdit:
            if (code == EN_CHANGE) {
                SyncEditEmpty();
            } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                g.editFocused = (code == EN_SETFOCUS);
                Repaint(g.r.field);         // the stroke turns amber on focus
                RepaintControl(g.edit);     // ...and the placeholder appears
            }
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_CLOSE:
        // Hide, never destroy: the tray app outlives this window.
        HideSelf();
        return 0;

    case WM_DESTROY:
        // Deliberately no PostQuitMessage - this window is not the app.
        g.hwnd        = nullptr;
        g.list        = nullptr;
        g.edit        = nullptr;
        g.add         = nullptr;
        g.remove      = nullptr;
        g.toggleLinks = nullptr;
        g.toggleAuto  = nullptr;
        g.close       = nullptr;
        g.hover       = nullptr;
        for (int i = 0; i < kModeCount; ++i)
            g.segment[i] = nullptr;
        for (int i = 0; i < kKindCount; ++i)
            g.kind[i] = nullptr;
        return 0;

    case WM_NCDESTROY:
        // Last message of the whole tree: a child repainting on its way out
        // can still ask for the cached brushes, so they die here, not in
        // WM_DESTROY.
        FreeBrushes();
        break;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------

bool Init(HINSTANCE hInst, const Callbacks& cb)
{
    g.cb = cb;
    if (g.initialised)
        return true;

    g.hInst = hInst;
    AdoptDpi(SystemDpi());

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = static_cast<DWORD>(sizeof(icc));
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize      = static_cast<UINT>(sizeof(wc));
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance   = hInst;
    wc.hCursor     = LoadCursorW(nullptr, IDC_ARROW);
    // No background brush: WM_ERASEBKGND is swallowed and WM_PAINT composes the
    // whole client area, so a class brush could only ever flash the wrong colour.
    wc.hbrBackground = nullptr;
    wc.hIcon         = appicon::Get(GetSystemMetrics(SM_CXICON));
    wc.hIconSm       = appicon::Get(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = kClassName;

    if (RegisterClassExW(&wc) == 0) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    } else {
        g.classRegistered = true;
    }

    g.initialised = true;
    return true;
}

void Show()
{
    if (!g.initialised)
        return;

    if (!g.hwnd) {
        // Land on the monitor the user is actually looking at.
        HWND     foreground = GetForegroundWindow();
        HMONITOR mon        = nullptr;
        if (foreground) {
            mon = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
        } else {
            POINT cursor;
            if (!GetCursorPos(&cursor)) {
                cursor.x = 0;
                cursor.y = 0;
            }
            mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        }

        // Created hidden and unsized; WM_CREATE builds the controls, then the
        // window is grown to the real client size and centred. Landing on a
        // monitor with a different DPI simply raises WM_DPICHANGED.
        HWND hwnd = CreateWindowExW(kExStyle, kClassName, kTitle, kStyle, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr,
                                    nullptr, g.hInst, nullptr);
        if (!hwnd || !g.hwnd)
            return;

        CentreOn(mon);
        LayoutFromClient();
    }

    SyncState();
    RefreshList(-1);
    SyncEditEmpty();
    InvalidateRect(g.hwnd, nullptr, FALSE);

    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
    if (g.edit)
        SetFocus(g.edit);
}

void NotifyStateChanged()
{
    if (!g.hwnd)
        return;
    SyncState();
}

void Shutdown()
{
    if (g.hwnd) {
        HWND hwnd = g.hwnd;
        g.hwnd = nullptr;      // helpers reached from WM_DESTROY become no-ops
        DestroyWindow(hwnd);
    }
    // theme::Shutdown() is the application's call to make, not ours: the tray
    // icon and the toast overlay draw with the same font cache.
    FreeBrushes();

    const HINSTANCE inst       = g.hInst;
    const bool      registered = g.classRegistered;

    g = State();               // full reset: a second Shutdown() is a no-op

    if (registered && inst)
        UnregisterClassW(kClassName, inst);
}

bool HandleDialogMessage(MSG* msg)
{
    if (!msg || !g.hwnd || !IsWindow(g.hwnd))
        return false;
    if (!(msg->hwnd == g.hwnd || IsChild(g.hwnd, msg->hwnd)))
        return false;

    if (msg->message == WM_KEYDOWN) {
        if (msg->wParam == VK_ESCAPE) {
            HideSelf();
            return true;
        }
        if (msg->hwnd == g.list && msg->wParam == VK_DELETE) {
            OnRemove();
            return true;
        }

        // Both segmented bars are keyboard-driven from here rather than from
        // the buttons: they are BS_OWNERDRAW, so USER32 gives them no radio
        // behaviour at all, and IsDialogMessageW's group navigation would only
        // move the focus without ever selecting. Intercepting ahead of it keeps
        // one path - the same handler a mouse click takes - and stops the
        // dialog manager moving the focus a second time.
        const int mode = IndexIn(g.segment, kModeCount, msg->hwnd);
        const int kind = (mode >= 0) ? -1 : IndexIn(g.kind, kKindCount, msg->hwnd);
        const int cell = (mode >= 0) ? mode : kind;
        if (cell >= 0) {
            const int count  = (mode >= 0) ? kModeCount : kKindCount;
            int       target = -1;
            switch (msg->wParam) {
            case VK_RIGHT:
            case VK_DOWN:
                target = (cell + 1) % count;
                break;
            case VK_LEFT:
            case VK_UP:
                target = (cell + count - 1) % count;
                break;
            case VK_HOME:
                target = 0;
                break;
            case VK_END:
                target = count - 1;
                break;
            case VK_SPACE:
                target = cell;
                break;
            default:
                break;
            }
            if (target >= 0) {
                if (mode >= 0)
                    SelectMode(target);
                else
                    SelectKind(target);
                return true;
            }
        }
    }

    // Tab navigation, arrow keys inside groups and Enter -> the default button.
    return IsDialogMessageW(g.hwnd, msg) != FALSE;
}

}  // namespace settings
