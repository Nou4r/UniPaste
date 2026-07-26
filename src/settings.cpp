// UniPaste settings window: a plain, system-themed Win32 window that manages
// the never-convert whitelist and mirrors the active conversion mode.
//
// Deliberately boring: stock USER32 controls only - no owner-draw, no custom
// painting, no colour scheme of our own - so the window inherits whatever
// theme, font and colours the user's Windows is running.
//
// The window is created lazily on the first Show() and then reused forever;
// closing it only hides it, so the tray application keeps running.

#include "settings.h"

#include <commctrl.h>

#include <cwctype>
#include <string>
#include <vector>

#include "whitelist.h"

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

constexpr wchar_t kClassName[] = L"UniPasteSettings";
constexpr wchar_t kTitle[]     = L"UniPaste Settings";
constexpr wchar_t kAppName[]   = L"UniPaste";

constexpr DWORD kStyle   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kExStyle = WS_EX_APPWINDOW;

constexpr int kModeCount = 4;   // uni::Mode 0..3
constexpr int kMaxEntry  = 512; // characters accepted from the edit control

// Control identifiers. Add is IDOK on purpose: IsDialogMessageW turns an
// unhandled Enter into WM_COMMAND(IDOK), which is exactly the Enter-to-add
// behaviour we want without subclassing the edit control.
constexpr int kIdAdd       = IDOK;
constexpr int kIdClose     = IDCANCEL;
constexpr int kIdModeLabel = 1101;
constexpr int kIdModeCombo = 1102;
constexpr int kIdListLabel = 1103;
constexpr int kIdList      = 1104;
constexpr int kIdEdit      = 1105;
constexpr int kIdRemove    = 1106;
constexpr int kIdPath      = 1107;
constexpr int kIdHint      = 1108;

// Logical (96-dpi) metrics; every one of these goes through S() before use.
constexpr int kClientW    = 470;
constexpr int kClientH    = 330;
constexpr int kMargin     = 12;
constexpr int kGap        = 8;
constexpr int kRowGap     = 10;
constexpr int kLabelH     = 17;
constexpr int kControlH   = 24;
constexpr int kButtonH    = 26;
constexpr int kButtonW    = 84;
constexpr int kModeLabelW = 112;
constexpr int kComboW     = 170;
constexpr int kComboDropH = 160;  // total height incl. the dropped-down list
constexpr int kNoteH      = 16;
constexpr int kMinListH   = 60;
constexpr int kMinEditW   = 60;

constexpr wchar_t kModeLabelText[] = L"Conversion mode:";
constexpr wchar_t kListLabelText[] = L"Never convert these words (whitelist):";
constexpr wchar_t kAddText[]       = L"Add";
constexpr wchar_t kRemoveText[]    = L"Remove";
constexpr wchar_t kCloseText[]     = L"Close";
constexpr wchar_t kHintText[]      =
    L"Shift+Numpad9 converts and pastes  |  Shift+Numpad8 cycles mode";

// Resolved at runtime so this TU never depends on the project's WINVER level.
using GetDpiForWindowFn          = UINT(WINAPI*)(HWND);
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);

struct State {
    HINSTANCE hInst           = nullptr;
    GetModeFn getMode         = nullptr;
    SetModeFn setMode         = nullptr;
    bool      initialised     = false;
    bool      classRegistered = false;
    bool      saveWarned      = false;  // one modal per failure streak, not per keystroke

    HWND hwnd      = nullptr;
    HWND modeLabel = nullptr;
    HWND combo     = nullptr;
    HWND listLabel = nullptr;
    HWND list      = nullptr;
    HWND edit      = nullptr;
    HWND add       = nullptr;
    HWND remove    = nullptr;
    HWND path      = nullptr;
    HWND hint      = nullptr;
    HWND close     = nullptr;

    HFONT font    = nullptr;
    int   fontDpi = 0;
    int   dpi     = 96;

    bool                       apisResolved  = false;
    GetDpiForWindowFn          dpiForWindow  = nullptr;
    AdjustWindowRectExForDpiFn adjustForDpi  = nullptr;
    SystemParametersInfoForDpiFn spiForDpi   = nullptr;
};

State g;

// --- small helpers ---------------------------------------------------------

inline int S(int logical) { return MulDiv(logical, g.dpi, 96); }

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
    g.spiForDpi = reinterpret_cast<SystemParametersInfoForDpiFn>(
        GetProcAddress(user32, "SystemParametersInfoForDpi"));
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

// --- font ------------------------------------------------------------------

BOOL CALLBACK ApplyFontProc(HWND child, LPARAM lParam)
{
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lParam), static_cast<LPARAM>(TRUE));
    return TRUE;
}

// Builds the shell's message font at `dpi` and pushes it onto every control.
// Owns exactly one HFONT at a time; Shutdown deletes it.
bool EnsureFont(int dpi)
{
    if (g.font && g.fontDpi == dpi)
        return true;

    ResolveApis();

    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = static_cast<UINT>(sizeof(ncm));

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));

    bool haveMetrics = false;
    if (g.spiForDpi) {
        // Windows 10 1607+: metrics come back already scaled for `dpi`.
        haveMetrics = (g.spiForDpi(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0,
                                   static_cast<UINT>(dpi)) != FALSE);
        if (haveMetrics)
            lf = ncm.lfMessageFont;
    }
    if (!haveMetrics) {
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = static_cast<UINT>(sizeof(ncm));
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0)) {
            lf = ncm.lfMessageFont;
            const int sysDpi = SystemDpi();
            if (sysDpi > 0 && sysDpi != dpi)
                lf.lfHeight = MulDiv(lf.lfHeight, dpi, sysDpi);
            haveMetrics = true;
        }
    }
    if (!haveMetrics) {
        lf.lfHeight  = -MulDiv(9, dpi, 72);   // 9 pt
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        const wchar_t* face = L"Segoe UI";
        int i = 0;
        for (; i < LF_FACESIZE - 1 && face[i]; ++i)
            lf.lfFaceName[i] = face[i];
        lf.lfFaceName[i] = L'\0';
    }

    HFONT font = CreateFontIndirectW(&lf);
    if (!font)
        return g.font != nullptr;   // keep the old one rather than going fontless

    if (g.font)
        DeleteObject(g.font);
    g.font    = font;
    g.fontDpi = dpi;

    if (g.hwnd)
        EnumChildWindows(g.hwnd, ApplyFontProc, reinterpret_cast<LPARAM>(g.font));
    return true;
}

// --- geometry --------------------------------------------------------------

void WindowSizeFor(int dpi, int& outW, int& outH)
{
    const int clientW = MulDiv(kClientW, dpi, 96);
    const int clientH = MulDiv(kClientH, dpi, 96);

    RECT r = { 0, 0, clientW, clientH };
    ResolveApis();

    BOOL ok = FALSE;
    if (g.adjustForDpi)
        ok = g.adjustForDpi(&r, kStyle, FALSE, kExStyle, static_cast<UINT>(dpi));
    if (!ok) {
        r.left   = 0;
        r.top    = 0;
        r.right  = clientW;
        r.bottom = clientH;
        AdjustWindowRectEx(&r, kStyle, FALSE, kExStyle);
    }

    outW = r.right - r.left;
    outH = r.bottom - r.top;
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
    WindowSizeFor(g.dpi, w, h);

    RECT work;
    WorkAreaOf(mon, &work);

    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    if (x < work.left)
        x = work.left;
    if (y < work.top)
        y = work.top;

    SetWindowPos(g.hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Single source of truth for the control geometry: called from WM_SIZE, from
// WM_DPICHANGED and once right after creation. Idempotent.
void Layout(int width, int height)
{
    if (!g.hwnd || !g.list || width <= 0 || height <= 0)
        return;

    const int m       = S(kMargin);
    const int gap     = S(kGap);
    const int rowGap  = S(kRowGap);
    const int lblH    = S(kLabelH);
    const int ctlH    = S(kControlH);
    const int btnH    = S(kButtonH);
    const int btnW    = S(kButtonW);
    const int lblW    = S(kModeLabelW);
    const int comboW  = S(kComboW);
    const int drop    = S(kComboDropH);
    const int noteH   = S(kNoteH);
    const int gapS    = S(4);
    const int gapNote = S(6);

    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;

    // --- mode row (top) ----------------------------------------------------
    int y = m;

    // A combo box snaps its own window height to the closed size and remembers
    // the height we pass as the dropped-down height, so read the real height
    // back and centre the label against that.
    SetWindowPos(g.combo, nullptr, m + lblW + gap, y, comboW, drop, flags);
    int comboH = ctlH;
    RECT cr;
    if (GetWindowRect(g.combo, &cr)) {
        const int actual = static_cast<int>(cr.bottom - cr.top);
        if (actual > 0 && actual <= drop)
            comboH = actual;
    }
    const int rowH = comboH > ctlH ? comboH : ctlH;
    SetWindowPos(g.modeLabel, nullptr, m, y, lblW, rowH, flags);
    y += rowH + rowGap;

    SetWindowPos(g.listLabel, nullptr, m, y, width - 2 * m, lblH, flags);
    y += lblH + gapS;
    const int listTop = y;

    // --- notes, Close and the add row (bottom, laid out upwards) -----------
    int bottom = height - m;

    SetWindowPos(g.hint, nullptr, m, bottom - noteH, width - 2 * m, noteH, flags);
    bottom -= noteH + gapNote;

    const int closeX = width - m - btnW;
    const int closeY = bottom - btnH;
    SetWindowPos(g.close, nullptr, closeX, closeY, btnW, btnH, flags);

    int pathW = closeX - gap - m;
    if (pathW < 0)
        pathW = 0;
    SetWindowPos(g.path, nullptr, m, closeY, pathW, btnH, flags);
    bottom = closeY - rowGap;

    const int rowTop  = bottom - btnH;
    const int removeX = width - m - btnW;
    const int addX    = removeX - gap - btnW;
    SetWindowPos(g.remove, nullptr, removeX, rowTop, btnW, btnH, flags);
    SetWindowPos(g.add, nullptr, addX, rowTop, btnW, btnH, flags);

    int editW = addX - gap - m;
    if (editW < S(kMinEditW))
        editW = S(kMinEditW);
    SetWindowPos(g.edit, nullptr, m, rowTop + (btnH - ctlH) / 2, editW, ctlH, flags);
    bottom = rowTop - rowGap;

    // --- the list takes whatever is left -----------------------------------
    int listH = bottom - listTop;
    if (listH < S(kMinListH))
        listH = S(kMinListH);
    SetWindowPos(g.list, nullptr, m, listTop, width - 2 * m, listH, flags);
}

void LayoutFromClient()
{
    if (!g.hwnd)
        return;
    RECT rc;
    if (GetClientRect(g.hwnd, &rc))
        Layout(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
}

// --- model <-> view --------------------------------------------------------

void UpdateRemoveEnabled()
{
    if (!g.remove || !g.list)
        return;

    const bool enable = SendMessageW(g.list, LB_GETCURSEL, 0, 0) != LB_ERR;
    if (!enable && GetFocus() == g.remove)
        SetFocus(g.list);   // never leave the focus on a disabled control
    EnableWindow(g.remove, enable ? TRUE : FALSE);
}

// Rebuilds the list from uni::whitelist::Entries(). `select` is clamped into
// range; pass -1 for "no selection". The listbox is unsorted, so its indices
// stay 1:1 with the whitelist vector.
void RefreshList(int select)
{
    if (!g.list)
        return;

    SendMessageW(g.list, WM_SETREDRAW, static_cast<WPARAM>(FALSE), 0);
    SendMessageW(g.list, LB_RESETCONTENT, 0, 0);

    const std::vector<std::wstring>& entries = uni::whitelist::Entries();
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

    UpdateRemoveEnabled();
}

// Writes the whitelist to disk. Reports a failure once per failure streak: a
// modal box on every keystroke-driven mutation would be unusable.
void Persist()
{
    if (uni::whitelist::Save()) {
        g.saveWarned = false;
        return;
    }

    const DWORD error = GetLastError();
    if (g.saveWarned)
        return;
    g.saveWarned = true;

    std::wstring message = L"UniPaste could not save the whitelist.\n\n";
    message += uni::whitelist::FilePath();
    message += L"\n\nError code: ";
    message += std::to_wstring(error);
    MessageBoxW(g.hwnd, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

void PopulateModes()
{
    if (!g.combo)
        return;

    SendMessageW(g.combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kModeCount; ++i) {
        const wchar_t* name = uni::ModeName(static_cast<uni::Mode>(i));
        SendMessageW(g.combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name ? name : L""));
    }
}

// CB_SETCURSEL does not raise CBN_SELCHANGE, so this can never loop back into
// OnModeSelected.
void SyncMode()
{
    if (!g.combo || !g.getMode)
        return;

    int index = static_cast<int>(g.getMode());
    if (index < 0 || index >= kModeCount)
        index = 0;
    SendMessageW(g.combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

void OnModeSelected()
{
    if (!g.combo || !g.setMode)
        return;

    const LRESULT sel = SendMessageW(g.combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR)
        return;

    const int index = static_cast<int>(sel);
    if (index < 0 || index >= kModeCount)
        return;
    g.setMode(static_cast<uni::Mode>(index));
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

    const std::wstring text  = EditText();
    const size_t       before = uni::whitelist::Entries().size();

    if (!uni::whitelist::Add(text)) {
        // Empty after trimming, or already present. A modal box for a typo is
        // obnoxious: beep and leave the text so the user can fix it.
        MessageBeep(MB_ICONWARNING);
        FocusEditAll();
        return;
    }

    SetWindowTextW(g.edit, L"");
    RefreshList(-1);

    // The entry normally lands at the end; look it up by name so a whitelist
    // that keeps its entries in some other order still selects the right row.
    int sel = static_cast<int>(before);
    const std::wstring trimmed = Trim(text);
    const LRESULT found = SendMessageW(g.list, LB_FINDSTRINGEXACT,
                                       static_cast<WPARAM>(static_cast<INT_PTR>(-1)),
                                       reinterpret_cast<LPARAM>(trimmed.c_str()));
    if (found != LB_ERR)
        sel = static_cast<int>(found);

    const int count = static_cast<int>(uni::whitelist::Entries().size());
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

    if (!uni::whitelist::RemoveAt(static_cast<size_t>(sel))) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    RefreshList(static_cast<int>(sel));   // same index, clamped
    Persist();
    SetFocus(g.list);
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

    const size_t index = static_cast<size_t>(sel);
    const std::vector<std::wstring>& entries = uni::whitelist::Entries();
    if (index >= entries.size())
        return;

    const std::wstring text = entries[index];   // copy: RemoveAt invalidates it
    if (!uni::whitelist::RemoveAt(index)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    SetWindowTextW(g.edit, text.c_str());
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

HWND MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text,
                 DWORD style, DWORD exStyle, int id)
{
    return CreateWindowExW(exStyle, cls, text, style | WS_CHILD | WS_VISIBLE,
                           0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           g.hInst, nullptr);
}

// Creation order is the tab order: combo -> list -> edit -> Add -> Remove -> Close.
bool CreateControls(HWND parent)
{
    constexpr DWORD kStaticStyle = SS_LEFT | SS_NOPREFIX;

    g.modeLabel = MakeControl(parent, L"STATIC", kModeLabelText,
                              kStaticStyle | SS_CENTERIMAGE, 0, kIdModeLabel);
    g.combo     = MakeControl(parent, L"COMBOBOX", nullptr,
                              CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, kIdModeCombo);
    g.listLabel = MakeControl(parent, L"STATIC", kListLabelText,
                              kStaticStyle, 0, kIdListLabel);
    g.list      = MakeControl(parent, L"LISTBOX", nullptr,
                              LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT |
                                  WS_VSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, kIdList);
    g.edit      = MakeControl(parent, L"EDIT", nullptr,
                              ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, kIdEdit);
    g.add       = MakeControl(parent, L"BUTTON", kAddText,
                              BS_DEFPUSHBUTTON | WS_TABSTOP, 0, kIdAdd);
    g.remove    = MakeControl(parent, L"BUTTON", kRemoveText,
                              BS_PUSHBUTTON | WS_TABSTOP, 0, kIdRemove);
    g.path      = MakeControl(parent, L"STATIC", nullptr,
                              kStaticStyle | SS_CENTERIMAGE | SS_PATHELLIPSIS, 0, kIdPath);
    g.hint      = MakeControl(parent, L"STATIC", kHintText,
                              kStaticStyle, 0, kIdHint);
    g.close     = MakeControl(parent, L"BUTTON", kCloseText,
                              BS_PUSHBUTTON | WS_TABSTOP, 0, kIdClose);

    if (!g.modeLabel || !g.combo || !g.listLabel || !g.list || !g.edit ||
        !g.add || !g.remove || !g.path || !g.hint || !g.close)
    {
        return false;
    }

    SendMessageW(g.edit, EM_SETLIMITTEXT, static_cast<WPARAM>(kMaxEntry), 0);
    return true;
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        g.dpi  = QueryDpi(hwnd);
        EnsureFont(g.dpi);
        if (!CreateControls(hwnd))
            return -1;
        if (g.font)
            EnumChildWindows(hwnd, ApplyFontProc, reinterpret_cast<LPARAM>(g.font));
        PopulateModes();
        SyncMode();
        RefreshList(-1);
        SetWindowTextW(g.path, uni::whitelist::FilePath().c_str());
        return 0;

    case WM_SIZE:
        Layout(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        break;

    case WM_DPICHANGED: {
        const int dpi = static_cast<int>(LOWORD(wParam));
        g.dpi = (dpi >= 72) ? dpi : SystemDpi();
        EnsureFont(g.dpi);

        int w = 0;
        int h = 0;
        WindowSizeFor(g.dpi, w, h);

        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, w, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(hwnd, nullptr, 0, 0, w, h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutFromClient();
        return 0;
    }

    // Mirror the dialog manager: Enter activates the focused push button when
    // one has focus and the Add button otherwise. Without this, IsDialogMessageW
    // would blindly fire IDOK (Add) even when Close or Remove has the focus.
    case DM_GETDEFID: {
        int id = kIdAdd;
        HWND focus = GetFocus();
        if (focus && IsChild(hwnd, focus)) {
            const LRESULT code = SendMessageW(focus, WM_GETDLGCODE, 0, 0);
            if (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))
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

    case WM_CTLCOLORSTATIC: {
        HDC       dc  = reinterpret_cast<HDC>(wParam);
        const int id  = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        const int col = (id == kIdPath || id == kIdHint) ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(col));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_COMMAND: {
        const int id   = static_cast<int>(LOWORD(wParam));
        const int code = static_cast<int>(HIWORD(wParam));
        switch (id) {
        case kIdAdd:
            if (code == BN_CLICKED)
                OnAdd();
            return 0;
        case kIdRemove:
            if (code == BN_CLICKED)
                OnRemove();
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
        case kIdModeCombo:
            if (code == CBN_SELCHANGE)
                OnModeSelected();
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
        g.hwnd      = nullptr;
        g.modeLabel = nullptr;
        g.combo     = nullptr;
        g.listLabel = nullptr;
        g.list      = nullptr;
        g.edit      = nullptr;
        g.add       = nullptr;
        g.remove    = nullptr;
        g.path      = nullptr;
        g.hint      = nullptr;
        g.close     = nullptr;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------

bool Init(HINSTANCE hInst, GetModeFn getMode, SetModeFn setMode)
{
    g.getMode = getMode;
    g.setMode = setMode;
    if (g.initialised)
        return true;

    g.hInst = hInst;
    g.dpi   = SystemDpi();

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = static_cast<DWORD>(sizeof(icc));
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = static_cast<UINT>(sizeof(wc));
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = wc.hIcon;
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
        POINT cursor;
        if (!GetCursorPos(&cursor)) {
            cursor.x = 0;
            cursor.y = 0;
        }
        HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

        // Created hidden and unsized; WM_CREATE builds the controls, then the
        // window is grown to the real client size and centred. Landing on a
        // monitor with a different DPI simply raises WM_DPICHANGED.
        HWND hwnd = CreateWindowExW(kExStyle, kClassName, kTitle, kStyle,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                    nullptr, nullptr, g.hInst, nullptr);
        if (!hwnd || !g.hwnd)
            return;

        CentreOn(mon);
        LayoutFromClient();
    }

    SyncMode();
    RefreshList(-1);
    SetWindowTextW(g.path, uni::whitelist::FilePath().c_str());

    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
    if (g.edit)
        SetFocus(g.edit);
}

void NotifyModeChanged()
{
    if (!g.hwnd)
        return;
    SyncMode();
}

void Shutdown()
{
    if (g.hwnd) {
        HWND hwnd = g.hwnd;
        g.hwnd = nullptr;      // helpers reached from WM_DESTROY become no-ops
        DestroyWindow(hwnd);
    }
    if (g.font) {
        DeleteObject(g.font);
        g.font = nullptr;
    }

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
    }

    // Tab navigation, arrow keys inside groups and Enter -> the default button.
    return IsDialogMessageW(g.hwnd, msg) != FALSE;
}

}  // namespace settings
