// blackout.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>

#include "common.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "powrprof.lib")

using hg::Log;

// GUID_CONSOLE_DISPLAY_STATE - defined locally so we do not depend on initguid.h.
static const GUID kConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

static const UINT WM_TRAYICON = WM_APP + 1;
static const UINT kHotkeyId   = 1;
static const UINT kMenuSleep  = 100;
static const UINT kMenuWake   = 101;
static const UINT kMenuLog    = 102;
static const UINT kMenuExit   = 103;

static HINSTANCE g_inst      = nullptr;
static HWND      g_main      = nullptr;
static HWND      g_overlay   = nullptr;
static HWND      g_prevFocus = nullptr;
static HPOWERNOTIFY g_dispNotify = nullptr;
static NOTIFYICONDATAW g_tray{};

static bool g_idle = false;          // currently in fake sleep

// config
static bool g_cfgMute    = true;
static bool g_cfgOverlay = true;
static bool g_cfgPlan    = true;
static UINT g_cfgMods    = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
static UINT g_cfgVk      = 'S';
static GUID g_idleScheme{};
static bool g_haveIdleScheme = false;

// saved state to restore on wake
static GUID g_prevScheme{};
static bool g_havePrevScheme = false;
static BOOL g_prevMute = FALSE;
static bool g_didMute  = false;


static std::wstring IniPath() { return hg::DataDir() + L"\\blackout.ini"; }

static void ParseHotkey(const std::wstring& s)
{
    UINT mods = 0;
    std::wstring key;
    size_t start = 0;
    while (start <= s.size()) {
        size_t plus = s.find(L'+', start);
        std::wstring tok = s.substr(start, (plus == std::wstring::npos) ? std::wstring::npos : plus - start);
        while (!tok.empty() && iswspace(tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && iswspace(tok.back()))  tok.pop_back();
        for (auto& c : tok) c = (wchar_t)towlower(c);

        if      (tok == L"ctrl" || tok == L"control") mods |= MOD_CONTROL;
        else if (tok == L"alt")                       mods |= MOD_ALT;
        else if (tok == L"shift")                     mods |= MOD_SHIFT;
        else if (tok == L"win")                       mods |= MOD_WIN;
        else if (!tok.empty())                        key = tok;

        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }

    if (!key.empty()) {
        if (key.size() == 1) g_cfgVk = (UINT)towupper(key[0]);
        else if (key[0] == L'f' && key.size() <= 3) {
            int n = _wtoi(key.c_str() + 1);
            if (n >= 1 && n <= 24) g_cfgVk = VK_F1 + (n - 1);
        }
    }
    if (mods) g_cfgMods = mods;
}

static void LoadConfig()
{
    std::wstring ini = IniPath();

    g_cfgMute    = GetPrivateProfileIntW(L"blackout", L"mute",        1, ini.c_str()) != 0;
    g_cfgOverlay = GetPrivateProfileIntW(L"blackout", L"overlay",     1, ini.c_str()) != 0;
    g_cfgPlan    = GetPrivateProfileIntW(L"blackout", L"switch_plan", 1, ini.c_str()) != 0;

    wchar_t buf[128]{};
    GetPrivateProfileStringW(L"blackout", L"hotkey", L"ctrl+alt+shift+s", buf, 128, ini.c_str());
    ParseHotkey(buf);

    wchar_t g[128]{};
    GetPrivateProfileStringW(L"blackout", L"idle_scheme", L"", g, 128, ini.c_str());
    if (g[0] && SUCCEEDED(CLSIDFromString(g, &g_idleScheme)))
        g_haveIdleScheme = true;
}


static void SetSystemMute(BOOL mute, BOOL* prev)
{
    IMMDeviceEnumerator*  pEnum = nullptr;
    IMMDevice*            pDev  = nullptr;
    IAudioEndpointVolume* pVol  = nullptr;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) && pEnum) {
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDev)) && pDev) {
            if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                         nullptr, (void**)&pVol)) && pVol) {
                if (prev) pVol->GetMute(prev);
                pVol->SetMute(mute, nullptr);
                pVol->Release();
            }
            pDev->Release();
        }
        pEnum->Release();
    }
}


static void SwitchToIdleScheme()
{
    if (!g_cfgPlan || !g_haveIdleScheme) return;

    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
        g_prevScheme     = *active;
        g_havePrevScheme = true;
        LocalFree(active);
    }
    if (PowerSetActiveScheme(nullptr, &g_idleScheme) != ERROR_SUCCESS)
        Log(L"[plan] failed to activate idle power scheme");
}

static void RestoreScheme()
{
    if (!g_havePrevScheme) return;
    PowerSetActiveScheme(nullptr, &g_prevScheme);
    g_havePrevScheme = false;
}


static void MonitorOff()
{
    SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)2,
                        SMTO_ABORTIFHUNG, 2000, nullptr);
}

static void MonitorOn()
{
    SendMessageTimeoutW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)-1,
                        SMTO_ABORTIFHUNG, 2000, nullptr);
    // Nudge the input stack so the panel actually relights on every driver.
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = 0;
    in.mi.dy = 0;
    SendInput(1, &in, sizeof(in));
}

static void PositionOverlay()
{
    if (!g_overlay) return;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
}

static void Wake();   // fwd

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_ERASEBKGND: {
            RECT rc; GetClientRect(h, &rc);
            FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;

        // Any real user input wakes. Mouse movement alone does not.
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            Wake();
            return 0;

        case WM_DISPLAYCHANGE:
            PositionOverlay();
            return 0;

        case WM_DESTROY:
            g_overlay = nullptr;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowOverlay()
{
    if (!g_cfgOverlay || g_overlay) return;

    g_prevFocus = GetForegroundWindow();

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HotspotGuardBlackout", L"", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, g_inst, nullptr);

    if (!g_overlay) { Log(L"[overlay] creation failed err=%lu", GetLastError()); return; }

    PositionOverlay();
    ShowWindow(g_overlay, SW_SHOW);
    SetForegroundWindow(g_overlay);
    SetFocus(g_overlay);
    UpdateWindow(g_overlay);
}

static void HideOverlay()
{
    if (!g_overlay) return;
    DestroyWindow(g_overlay);
    g_overlay = nullptr;

    if (g_prevFocus && IsWindow(g_prevFocus)) SetForegroundWindow(g_prevFocus);
    g_prevFocus = nullptr;
}


static void EnterIdle()
{
    if (g_idle) return;
    g_idle = true;

    if (g_cfgMute) { SetSystemMute(TRUE, &g_prevMute); g_didMute = true; }
    SwitchToIdleScheme();
    ShowOverlay();

    Log(L"[idle] entered fake sleep (screen off, audio muted, low-power plan)");
}

static void ExitIdle()
{
    if (!g_idle) return;
    g_idle = false;

    RestoreScheme();
    if (g_didMute) { SetSystemMute(g_prevMute, nullptr); g_didMute = false; }
    HideOverlay();

    Log(L"[idle] woke up");
}

static void RequestSleep()
{
    Log(L"[idle] sleep requested");
    // Overlay first: it takes the foreground, which makes SC_MONITORPOWER reliable,
    // and guarantees black pixels on any panel that ignores DPMS.
    ShowOverlay();
    Sleep(200);
    EnterIdle();
    MonitorOff();
}

static void Wake()
{
    MonitorOn();
    ExitIdle();
}


static void TrayAdd(HWND h)
{
    g_tray.cbSize           = sizeof(g_tray);
    g_tray.hWnd             = h;
    g_tray.uID              = 1;
    g_tray.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAYICON;
    g_tray.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"Hotspot Guard - blackout");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

static void TrayRemove() { Shell_NotifyIconW(NIM_DELETE, &g_tray); }

static void ShowTrayMenu(HWND h)
{
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuSleep, L"Sleep now (black screen)");
    AppendMenuW(menu, MF_STRING, kMenuWake,  L"Wake");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuLog,   L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit,  L"Exit");

    SetForegroundWindow(h);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(menu);
}


static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_HOTKEY:
            if (w == kHotkeyId) { if (g_idle) Wake(); else RequestSleep(); }
            return 0;

        case WM_POWERBROADCAST:
            if (w == PBT_POWERSETTINGCHANGE) {
                auto* s = reinterpret_cast<POWERBROADCAST_SETTING*>(l);
                if (s && IsEqualGUID(s->PowerSetting, kConsoleDisplayState) && s->DataLength >= 1) {
                    DWORD state = s->Data[0];   // 0 = off, 1 = on, 2 = dimmed
                    if (state == 0)      EnterIdle();
                    else if (state == 1) ExitIdle();
                }
            }
            return TRUE;

        case WM_TRAYICON:
            if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP) ShowTrayMenu(h);
            return 0;

        case WM_COMMAND:
            switch (LOWORD(w)) {
                case kMenuSleep: RequestSleep(); return 0;
                case kMenuWake:  Wake();         return 0;
                case kMenuLog:
                    ShellExecuteW(nullptr, L"open", hg::DataDir().c_str(), nullptr, nullptr, SW_SHOW);
                    return 0;
                case kMenuExit:  DestroyWindow(h); return 0;
            }
            return 0;

        case WM_DISPLAYCHANGE:
            PositionOverlay();
            return 0;

        case WM_DESTROY:
            ExitIdle();
            TrayRemove();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;
    hg::LogInit(L"blackout", false);

    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\HotspotGuardBlackout");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadConfig();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"HotspotGuardBlackoutMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW oc{};
    oc.cbSize        = sizeof(oc);
    oc.lpfnWndProc   = OverlayProc;
    oc.hInstance     = inst;
    oc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    oc.hCursor       = nullptr;
    oc.lpszClassName = L"HotspotGuardBlackout";
    RegisterClassExW(&oc);

    g_main = CreateWindowExW(0, L"HotspotGuardBlackoutMain", L"Hotspot Guard",
                             0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_main) return 1;

    TrayAdd(g_main);

    if (!RegisterHotKey(g_main, kHotkeyId, g_cfgMods | MOD_NOREPEAT, g_cfgVk))
        Log(L"[hotkey] registration failed err=%lu (another app may own it)", GetLastError());
    else
        Log(L"[hotkey] registered mods=0x%X vk=0x%X", g_cfgMods, g_cfgVk);

    g_dispNotify = RegisterPowerSettingNotification(g_main, &kConsoleDisplayState,
                                                    DEVICE_NOTIFY_WINDOW_HANDLE);
    Log(L"[blackout] running");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_dispNotify) UnregisterPowerSettingNotification(g_dispNotify);
    UnregisterHotKey(g_main, kHotkeyId);
    CoUninitialize();
    if (once) CloseHandle(once);
    return 0;
}
