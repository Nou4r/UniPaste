// UniPaste - native Win32 tray application.
//
// Shift + Numpad9 reads the clipboard, runs the Unicode homoglyph substitution
// from spoof.cpp over it (skipping anything the whitelist protects), writes the
// result back to the clipboard, synthesises Ctrl+V into the foreground window
// and flashes a small transient toast in the top-right corner of the screen.
//
// Shift + Numpad8 cycles the conversion mode and only shows the toast - it
// never touches the clipboard and never pastes.
//
// The tray icon exposes the same mode choice plus a settings window (whitelist
// editor + mode combo), which is also what a double-click on the icon opens.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "appicon.h"
#include "overlay.h"
#include "settings.h"
#include "spoof.h"
#include "theme.h"
#include "whitelist.h"

// Present in the Windows 10 1703+ SDK headers only; define a compatible
// fallback so the translation unit builds against older SDKs too. The value is
// consumed through a dynamically resolved entry point, so an older *runtime*
// simply skips the call.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))
#endif

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr wchar_t kClassName[] = L"UniPasteMain";
constexpr wchar_t kAppName[]   = L"UniPaste";
constexpr wchar_t kMutexName[] = L"Local\\UniPaste_SingleInstance";
constexpr wchar_t kRegPath[]   = L"Software\\UniPaste";
constexpr wchar_t kRegValue[]  = L"Mode";
constexpr wchar_t kTrayTip[]   = L"UniPaste - Shift+Numpad9 convert, Shift+Numpad8 mode";

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kTrayIconId   = 1;

// Fallback path only. Shift+Numpad9 cannot be caught with RegisterHotKey while
// NumLock is ON: the keyboard layer fabricates a Shift *release* immediately
// before the numpad key ("shift cancels NumLock"), so MOD_SHIFT never matches
// and the hotkey silently never fires. The primary trigger is the low-level
// keyboard hook further down; these registrations remain as a fallback for the
// case where the hook cannot be installed, where they work with NumLock OFF.
//
// The mode chord deliberately has no such fallback: numpad 8 reports as VK_UP
// under both NumLock states once Shift is held, so a working registration would
// have to claim Shift+Up globally and break line-wise text selection in every
// application. Without the hook the mode is still reachable from the tray menu
// and the settings window.
constexpr int kHotkeyNumpad9 = 1;
constexpr int kHotkeyPrior   = 2;

constexpr UINT kIdModeBase = 1000;  // 1000..1003 map onto uni::Mode 0..3
constexpr UINT kIdExit     = 1100;
constexpr UINT kIdSettings = 1101;
constexpr int  kModeCount  = 4;

// Stamped on every injected keyboard event so our own input is distinguishable
// from real hardware input in traces / hooks.
constexpr ULONG_PTR kInjectTag = 0x554E4950;  // 'UNIP'

// Posted by the keyboard hook once a chord is recognised.
constexpr UINT kMsgTrigger   = WM_APP + 2;  // Shift+Numpad9: convert and paste
constexpr UINT kMsgCycleMode = WM_APP + 3;  // Shift+Numpad8: next mode

// Scan codes of the physical numpad 9 / numpad 8 keys. The grey PageUp and Up
// keys are 0xE0 0x49 and 0xE0 0x48, i.e. the same scan codes plus the extended
// flag, which is what keeps them out of our way.
constexpr DWORD kNumpad9Scan = 0x49;
constexpr DWORD kNumpad8Scan = 0x48;

// The NumLock-cancelling Shift release/press the keyboard layer fabricates
// around numpad keys carries this bit in its scan code; real Shift input does
// not. Tracking the physical Shift state means ignoring those.
constexpr DWORD kFakeShiftScanBit = 0x200;

constexpr int   kClipboardTries   = 10;
constexpr DWORD kClipboardRetryMs = 20;
constexpr DWORD kSettleMs         = 30;  // let the modifier releases land

// ---------------------------------------------------------------------------
// State (POD only - no global objects with nontrivial destructors)
// ---------------------------------------------------------------------------

HWND      g_hwnd            = nullptr;
uni::Mode g_mode            = uni::Mode::Basic;
UINT      g_taskbarCreated  = 0;
bool      g_hotkeyNumpad9   = false;
bool      g_hotkeyPrior     = false;
// Touched only by the hook thread (g_shiftDown, g_swallowUpVk) or only before
// that thread starts and after it is joined - no synchronisation needed.
HHOOK     g_keyHook         = nullptr;
HANDLE    g_hookThread      = nullptr;
HANDLE    g_hookReady       = nullptr;
DWORD     g_hookThreadId    = 0;
bool      g_shiftDown       = false;
WORD      g_swallowUpVk     = 0;
bool      g_trayAdded       = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void ShowError(const wchar_t* text)
{
    MessageBoxW(nullptr, text, kAppName, MB_OK | MB_ICONERROR);
}

void EnablePerMonitorDpi()
{
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;

    auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (!setContext)
        return;  // pre-1703: run DPI-unaware, which is harmless here

    setContext(reinterpret_cast<HANDLE>(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
}

uni::Mode LoadMode()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return uni::Mode::Basic;

    DWORD   value = 0;
    DWORD   type  = 0;
    DWORD   size  = sizeof(value);
    const LSTATUS status = RegQueryValueExW(key, kRegValue, nullptr, &type,
                                            reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);

    if (status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value) &&
        value < static_cast<DWORD>(kModeCount))
    {
        return static_cast<uni::Mode>(value);
    }
    return uni::Mode::Basic;
}

void SaveMode(uni::Mode mode)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    const DWORD value = static_cast<DWORD>(mode);
    RegSetValueExW(key, kRegValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), static_cast<DWORD>(sizeof(value)));
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

void FillTrayData(NOTIFYICONDATAW& nid, HWND hwnd)
{
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = static_cast<DWORD>(sizeof(nid));
    nid.hWnd             = hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon            = appicon::Get(GetSystemMetrics(SM_CXSMICON));

    static_assert(sizeof(kTrayTip) <= sizeof(nid.szTip), "tray tip too long");
    CopyMemory(nid.szTip, kTrayTip, sizeof(kTrayTip));
}

void AddTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid;
    FillTrayData(nid, hwnd);
    g_trayAdded = (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE);
}

void RemoveTrayIcon(HWND hwnd)
{
    if (!g_trayAdded)
        return;

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd   = hwnd;
    nid.uID    = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void ShowTrayMenu(HWND hwnd)
{
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kIdSettings, L"Settings...");
    // By position, so the item renders bold as the menu's default action.
    SetMenuDefaultItem(menu, 0, TRUE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (int i = 0; i < kModeCount; ++i)
    {
        AppendMenuW(menu, MF_STRING, kIdModeBase + static_cast<UINT>(i),
                    uni::ModeName(static_cast<uni::Mode>(i)));
    }
    CheckMenuRadioItem(menu, kIdModeBase, kIdModeBase + (kModeCount - 1),
                       kIdModeBase + static_cast<UINT>(g_mode), MF_BYCOMMAND);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"Exit");

    // Documented workaround: the menu only dismisses correctly when the owner
    // is foreground, and it needs a nudge afterwards to repaint/close.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

// Another process very commonly owns the clipboard for a few milliseconds;
// retry instead of failing on the first refusal.
bool OpenClipboardWithRetry(HWND hwnd)
{
    for (int attempt = 0; attempt < kClipboardTries; ++attempt)
    {
        if (OpenClipboard(hwnd))
            return true;
        Sleep(kClipboardRetryMs);
    }
    return false;
}

bool ReadClipboardText(HWND hwnd, std::wstring& out, const wchar_t*& error)
{
    out.clear();

    if (!OpenClipboardWithRetry(hwnd))
    {
        error = L"Clipboard is busy";
        return false;
    }

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle)
    {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle));
        if (text)
        {
            const size_t maxChars = GlobalSize(handle) / sizeof(wchar_t);
            size_t length = 0;
            while (length < maxChars && text[length] != L'\0')
                ++length;
            out.assign(text, length);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();

    if (out.empty())
    {
        error = L"Clipboard has no text";
        return false;
    }
    return true;
}

bool WriteClipboardText(HWND hwnd, const std::wstring& text, const wchar_t*& error)
{
    if (!OpenClipboardWithRetry(hwnd))
    {
        error = L"Clipboard is busy";
        return false;
    }

    if (!EmptyClipboard())
    {
        CloseClipboard();
        error = L"Could not clear the clipboard";
        return false;
    }

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem)
    {
        CloseClipboard();
        error = L"Out of memory";
        return false;
    }

    void* dst = GlobalLock(mem);
    if (!dst)
    {
        GlobalFree(mem);
        CloseClipboard();
        error = L"Out of memory";
        return false;
    }
    CopyMemory(dst, text.c_str(), bytes);  // includes the terminating NUL
    GlobalUnlock(mem);

    if (!SetClipboardData(CF_UNICODETEXT, mem))
    {
        GlobalFree(mem);  // ownership was not transferred
        CloseClipboard();
        error = L"Could not write to the clipboard";
        return false;
    }

    // On success the clipboard owns `mem`; freeing it here would be a bug.
    CloseClipboard();
    return true;
}

// ---------------------------------------------------------------------------
// Synthetic input
// ---------------------------------------------------------------------------

bool IsExtendedVk(WORD vk)
{
    // Only the keys this app actually injects are considered. Note VK_PRIOR is
    // deliberately absent: the physical key being released is the numpad 9
    // (scan code 0x49, non-extended), not the grey PageUp (0xE0 0x49).
    return vk == VK_LWIN || vk == VK_RWIN;
}

INPUT MakeKeyEvent(WORD vk, bool keyUp)
{
    INPUT input{};
    input.type           = INPUT_KEYBOARD;
    input.ki.wVk         = vk;
    input.ki.wScan       = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags     = (keyUp ? KEYEVENTF_KEYUP : 0u) |
                           (IsExtendedVk(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    input.ki.time        = 0;
    input.ki.dwExtraInfo = kInjectTag;
    return input;
}

bool IsKeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The trigger fires while Shift (and possibly the numpad key) are physically
// held, so a naive Ctrl+V would reach the target as Ctrl+Shift+V.
//
// Releasing Shift in a separate SendInput call is not enough: when the numpad
// key goes up, the keyboard layer restores the Shift it had cancelled by
// injecting a fresh Shift *press*, which can land between our release and our
// Ctrl+V and silently turn it back into Ctrl+Shift+V. SendInput guarantees one
// array is inserted serially without foreign events interleaved, so the
// releases and the paste have to travel in a single batch.
//
// A `triggerVk` of 0 means the trigger key needs no release: the hook already
// swallowed both its down and its up.
void SendPaste(WORD triggerVk)
{
    INPUT batch[12];
    UINT  count = 0;

    batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_LSHIFT), true);
    batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_RSHIFT), true);
    batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_SHIFT), true);
    if (triggerVk != 0)
        batch[count++] = MakeKeyEvent(triggerVk, true);
    if (IsKeyDown(VK_LWIN))
        batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_LWIN), true);
    if (IsKeyDown(VK_RWIN))
        batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_RWIN), true);
    if (IsKeyDown(VK_MENU))
        batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_MENU), true);

    batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_CONTROL), false);
    batch[count++] = MakeKeyEvent(static_cast<WORD>('V'), false);
    batch[count++] = MakeKeyEvent(static_cast<WORD>('V'), true);
    batch[count++] = MakeKeyEvent(static_cast<WORD>(VK_CONTROL), true);

    Sleep(kSettleMs);  // let the user's own key-up storm drain first
    SendInput(count, batch, static_cast<int>(sizeof(INPUT)));
}

// ---------------------------------------------------------------------------
// Hotkey handler
// ---------------------------------------------------------------------------

void OnConvertHotkey(HWND hwnd, WORD triggerVk)
{
    const wchar_t* error = nullptr;

    std::wstring text;
    if (!ReadClipboardText(hwnd, text, error))
    {
        overlay::Show(error, overlay::Kind::Error);
        return;
    }

    // Whitelisted words and phrases are pinned to their original code units.
    std::vector<bool> mask;
    uni::whitelist::Mark(text, mask);

    std::wstring out = uni::Convert(text, g_mode, mask);

    if (!WriteClipboardText(hwnd, out, error))
    {
        overlay::Show(error, overlay::Kind::Error);
        return;
    }

    SendPaste(triggerVk);
    overlay::Show(L"Conversion successful");
}

// ---------------------------------------------------------------------------
// Low-level keyboard hook (primary trigger)
// ---------------------------------------------------------------------------

// True when `ev` is the physical numpad key identified by `scan`, whichever of
// its two virtual-key faces it happens to be wearing.
//
// With NumLock ON and Shift held the numpad key reports as its navigation twin
// (numpad 9 -> VK_PRIOR, numpad 8 -> VK_UP) but keeps the numpad's own
// non-extended scan code. The grey navigation keys are the extended variants of
// those same scan codes and must keep working normally, so the extended flag is
// the whole discriminator.
bool IsNumpadChord(const KBDLLHOOKSTRUCT& ev, DWORD numpadVk, DWORD navVk, DWORD scan)
{
    if (ev.vkCode == numpadVk)
        return true;

    return ev.vkCode == navVk && ev.scanCode == scan &&
           (ev.flags & LLKHF_EXTENDED) == 0;
}

bool IsNumpad9Event(const KBDLLHOOKSTRUCT& ev)
{
    return IsNumpadChord(ev, VK_NUMPAD9, VK_PRIOR, kNumpad9Scan);
}

bool IsNumpad8Event(const KBDLLHOOKSTRUCT& ev)
{
    return IsNumpadChord(ev, VK_NUMPAD8, VK_UP, kNumpad8Scan);
}

// True when `ev` releases the key whose key-down we swallowed.
//
// Matched by chord identity rather than by raw vk: the paste synthesis releases
// Shift, so by the time the user lifts the key the layer can hand us the other
// virtual-key face of the very key we recorded.
bool IsPendingChordUp(const KBDLLHOOKSTRUCT& ev)
{
    switch (g_swallowUpVk)
    {
    case VK_NUMPAD9:
    case VK_PRIOR:
        return IsNumpad9Event(ev);
    case VK_NUMPAD8:
    case VK_UP:
        return IsNumpad8Event(ev);
    default:
        return false;  // including 0: nothing pending
    }
}

// Runs on the UI thread for every keystroke in the session, so it does nothing
// but bookkeeping and a PostMessage - the clipboard work happens in WndProc.
LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION)
        return CallNextHookEx(nullptr, code, wParam, lParam);

    const KBDLLHOOKSTRUCT& ev = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool keyUp   = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

    if (ev.dwExtraInfo == kInjectTag)  // our own paste synthesis
        return CallNextHookEx(nullptr, code, wParam, lParam);

    if (ev.vkCode == VK_LSHIFT || ev.vkCode == VK_RSHIFT || ev.vkCode == VK_SHIFT)
    {
        if ((ev.scanCode & kFakeShiftScanBit) == 0)
        {
            if (keyDown)
                g_shiftDown = true;
            else if (keyUp)
                g_shiftDown = false;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (keyDown && g_shiftDown)
    {
        if (IsNumpad9Event(ev))
        {
            g_swallowUpVk = static_cast<WORD>(ev.vkCode);
            PostMessageW(g_hwnd, kMsgTrigger, 0, 0);
            return 1;  // swallow: the target must not receive a stray 9 / PageUp
        }
        if (IsNumpad8Event(ev))
        {
            g_swallowUpVk = static_cast<WORD>(ev.vkCode);
            PostMessageW(g_hwnd, kMsgCycleMode, 0, 0);
            return 1;  // swallow: no stray 8 / Up either
        }
    }

    if (keyUp && IsPendingChordUp(ev))
    {
        g_swallowUpVk = 0;
        return 1;  // and no orphan key-up either
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// The hook MUST NOT live on the UI thread. Windows silently unhooks a low-level
// hook whose callback cannot run within LowLevelHooksTimeout (~300 ms), and the
// UI thread blocks for exactly that long while it retries the clipboard, waits
// out kSettleMs and pushes the paste through SendInput - the very events the
// hook is asked to inspect. A dedicated thread that does nothing but pump
// messages is always available to answer, so the hook survives.
DWORD WINAPI HookThreadProc(LPVOID param)
{
    g_shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    g_keyHook   = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook,
                                    static_cast<HINSTANCE>(param), 0);
    g_hookThreadId = GetCurrentThreadId();

    // Force the queue to exist before the installer is told we are ready, so a
    // PostThreadMessage sent immediately after cannot be dropped.
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(g_hookReady);

    if (!g_keyHook)
        return 1;

    BOOL got = 0;
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_keyHook);
    g_keyHook = nullptr;
    return 0;
}

bool InstallKeyboardHook(HINSTANCE hInst)
{
    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hookReady)
        return false;

    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, hInst, 0, nullptr);
    if (!g_hookThread)
    {
        CloseHandle(g_hookReady);
        g_hookReady = nullptr;
        return false;
    }

    WaitForSingleObject(g_hookReady, INFINITE);
    CloseHandle(g_hookReady);
    g_hookReady = nullptr;
    return g_keyHook != nullptr;
}

void RemoveKeyboardHook()
{
    if (!g_hookThread)
        return;

    if (g_hookThreadId != 0)
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hookThread, 2000);
    CloseHandle(g_hookThread);
    g_hookThread   = nullptr;
    g_hookThreadId = 0;
}

// The single funnel for every mode change: tray menu, settings combo and the
// Shift+Numpad8 chord all land here.
void ApplyMode(uni::Mode mode)
{
    g_mode = mode;
    SaveMode(mode);
    // Re-entrant only in the harmless direction: the settings window selects
    // the combo item programmatically, which raises no change notification.
    settings::NotifyModeChanged();

    std::wstring message = L"Mode: ";
    message += uni::ModeName(mode);
    overlay::Show(message.c_str());
}

// Plain functions rather than lambdas so their addresses match the
// settings::GetModeFn / settings::SetModeFn pointer types exactly.
static uni::Mode GetModeCallback()
{
    return g_mode;
}

static void ApplyModeCallback(uni::Mode mode)
{
    ApplyMode(mode);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Explorer restarted: the shell dropped every tray icon, so re-add ours.
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated)
    {
        g_trayAdded = false;
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case kMsgTrigger:
        OnConvertHotkey(hwnd, 0);
        return 0;

    case kMsgCycleMode:
        // Mode-only chord: no clipboard access, no paste.
        ApplyMode(uni::NextMode(g_mode));
        return 0;

    case WM_HOTKEY:
    {
        const int hotkeyId = static_cast<int>(wParam);
        if (hotkeyId == kHotkeyNumpad9)
            OnConvertHotkey(hwnd, static_cast<WORD>(VK_NUMPAD9));
        else if (hotkeyId == kHotkeyPrior)
            OnConvertHotkey(hwnd, static_cast<WORD>(VK_PRIOR));
        return 0;
    }

    case kTrayCallback:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
            settings::Show();
        return 0;

    case WM_COMMAND:
    {
        const UINT id = LOWORD(wParam);
        if (id == kIdSettings)
        {
            settings::Show();
            return 0;
        }
        if (id >= kIdModeBase && id < kIdModeBase + static_cast<UINT>(kModeCount))
        {
            ApplyMode(static_cast<uni::Mode>(id - kIdModeBase));
            return 0;
        }
        if (id == kIdExit)
        {
            PostQuitMessage(0);
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        RemoveKeyboardHook();
        if (g_hotkeyNumpad9)
        {
            UnregisterHotKey(hwnd, kHotkeyNumpad9);
            g_hotkeyNumpad9 = false;
        }
        if (g_hotkeyPrior)
        {
            UnregisterHotKey(hwnd, kHotkeyPrior);
            g_hotkeyPrior = false;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    EnablePerMonitorDpi();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr,
                    L"UniPaste is already running.\n\nLook for its icon in the notification area.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        return 0;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize        = static_cast<UINT>(sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kClassName;
    wc.hIcon         = appicon::Get(GetSystemMetrics(SM_CXICON));
    wc.hIconSm       = appicon::Get(GetSystemMetrics(SM_CXSMICON));
    if (!RegisterClassExW(&wc))
    {
        ShowError(L"Could not register the UniPaste window class.");
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    // Real (never shown) window rather than message-only: the tray icon needs
    // an HWND the shell can talk to, and broadcast messages such as
    // TaskbarCreated are not delivered to message-only windows.
    g_hwnd = CreateWindowExW(0, kClassName, kAppName, WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd)
    {
        ShowError(L"Could not create the UniPaste window.");
        UnregisterClassW(kClassName, hInst);
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    if (!overlay::Init(hInst))
    {
        ShowError(L"Could not create the UniPaste overlay window.");
        DestroyWindow(g_hwnd);
        UnregisterClassW(kClassName, hInst);
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    g_mode = LoadMode();

    // Loaded before the settings window exists so its list is populated the
    // first time it is shown. A missing or unreadable file is not fatal - the
    // app then simply runs with an empty list, and the settings window is where
    // save/load errors surface.
    (void)uni::whitelist::Load();

    if (!settings::Init(hInst, &GetModeCallback, &ApplyModeCallback))
    {
        ShowError(L"Could not initialise the UniPaste settings window.");
        overlay::Shutdown();
        DestroyWindow(g_hwnd);
        UnregisterClassW(kClassName, hInst);
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    const bool hookInstalled = InstallKeyboardHook(hInst);

    DWORD numpadError = 0;
    DWORD priorError  = 0;
    if (!hookInstalled)
    {
        const UINT kMods = MOD_SHIFT | MOD_NOREPEAT;
        g_hotkeyNumpad9 = (RegisterHotKey(g_hwnd, kHotkeyNumpad9, kMods, VK_NUMPAD9) != FALSE);
        numpadError     = g_hotkeyNumpad9 ? 0u : GetLastError();
        g_hotkeyPrior   = (RegisterHotKey(g_hwnd, kHotkeyPrior, kMods, VK_PRIOR) != FALSE);
        priorError      = g_hotkeyPrior ? 0u : GetLastError();
    }

    if (!hookInstalled && !g_hotkeyNumpad9 && !g_hotkeyPrior)
    {
        std::wstring message = L"Could not capture the Shift+Numpad9 hotkey.\n\n"
                               L"Another application is probably using it.\n\n"
                               L"Error codes: ";
        message += std::to_wstring(numpadError);
        message += L" / ";
        message += std::to_wstring(priorError);
        ShowError(message.c_str());

        settings::Shutdown();
        overlay::Shutdown();
        DestroyWindow(g_hwnd);
        UnregisterClassW(kClassName, hInst);
        if (mutex)
            CloseHandle(mutex);
        return 1;
    }

    AddTrayIcon(g_hwnd);

    MSG msg{};
    for (;;)
    {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1)
            break;
        // The modeless settings dialog gets first refusal so Tab / Esc / Enter
        // and the edit control's own keys reach IsDialogMessage intact.
        if (settings::HandleDialogMessage(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(g_hwnd);  // WM_DESTROY removes the icon and the hotkeys
    g_hwnd = nullptr;
    settings::Shutdown();
    overlay::Shutdown();
    theme::Shutdown();
    appicon::Shutdown();
    UnregisterClassW(kClassName, hInst);
    if (mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    return static_cast<int>(msg.wParam);
}
