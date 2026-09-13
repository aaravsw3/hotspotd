// hotspotd.exe
// dont toyuch power_request core unless u wanna deal with a potential watchdog timeout

#include "common.h"
#include "hotspot.h"

#include <string>

#pragma comment(lib, "advapi32.lib")

namespace hg {

static const wchar_t* kServiceName = L"hotspotd";
static const wchar_t* kDescription =
    L"keeps the windows hotspot running continuously and blocks system sleep";

static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
static SERVICE_STATUS        g_status{};
static HANDLE                g_stopEvent = nullptr;
static HANDLE                g_wakeEvent = nullptr;   // signalled on resume-from-suspend
static HANDLE                g_powerRequest = INVALID_HANDLE_VALUE;


static void AcquirePowerRequests()
{
    REASON_CONTEXT rc{};
    rc.Version = POWER_REQUEST_CONTEXT_VERSION;
    rc.Flags   = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    rc.Reason.SimpleReasonString = const_cast<LPWSTR>(L"HotspotGuard: Mobile Hotspot must stay online");

    g_powerRequest = PowerCreateRequest(&rc);
    if (g_powerRequest == INVALID_HANDLE_VALUE) {
        Log(L"[power] PowerCreateRequest failed err=%lu", GetLastError());
    } else {
        if (!PowerSetRequest(g_powerRequest, PowerRequestSystemRequired))
            Log(L"[power] SystemRequired failed err=%lu", GetLastError());
        else
            Log(L"[power] SystemRequired held - idle sleep blocked");

        // Away mode turns any forced sleep into "screen off, machine still running",
        // which is exactly what we want when the user hits a sleep button.
        if (!PowerSetRequest(g_powerRequest, PowerRequestAwayModeRequired))
            Log(L"[power] AwayModeRequired unavailable err=%lu (non-fatal)", GetLastError());
        else
            Log(L"[power] AwayModeRequired held");
    }

    // Belt and braces: legacy per-thread execution state.
    if (!SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED))
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
}

static void ReleasePowerRequests()
{
    if (g_powerRequest != INVALID_HANDLE_VALUE) {
        PowerClearRequest(g_powerRequest, PowerRequestAwayModeRequired);
        PowerClearRequest(g_powerRequest, PowerRequestSystemRequired);
        CloseHandle(g_powerRequest);
        g_powerRequest = INVALID_HANDLE_VALUE;
    }
    SetThreadExecutionState(ES_CONTINUOUS);
}

// ---------------------------------------------------------------- support services

// The hotspot is backed by icssvc (Internet Connection Sharing) and WlanSvc.
// If either is stopped, tethering cannot come up.
static void EnsureSupportServices()
{
    static const wchar_t* names[] = { L"WlanSvc", L"icssvc" };

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;

    for (const wchar_t* name : names) {
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_START);
        if (!svc) continue;

        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
            if (ssp.dwCurrentState == SERVICE_STOPPED) {
                Log(L"[svc] %s is stopped - starting", name);
                if (!StartServiceW(svc, 0, nullptr))
                    Log(L"[svc] StartService(%s) failed err=%lu", name, GetLastError());
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

// ---------------------------------------------------------------- watchdog

static DWORD IntervalForFailures(int fails)
{
    if (fails <= 2)  return 5000;
    if (fails <= 5)  return 15000;
    if (fails <= 10) return 30000;
    return 60000;
}

static void WatchdogLoop()
{
    WinRtInitThread();
    AcquirePowerRequests();

    int         fails = 0;
    TetherState last  = TetherState::Unknown;
    bool        first = true;
    ULONGLONG   lastHeartbeat = GetTickCount64();

    HANDLE waits[2] = { g_stopEvent, g_wakeEvent };

    for (;;) {
        EnsureSupportServices();

        HotspotStatus st = HotspotQuery();

        if (!st.queried) {
            ++fails;
            Log(L"[watch] query failed (%d in a row): %s", fails, st.error.c_str());
        } else {
            if (first || st.state != last) {
                Log(L"[watch] state=%s clients=%u/%u uplink=%s",
                    StateName(st.state), st.clients, st.maxClients, st.profile.c_str());
                last  = st.state;
                first = false;
            }

            if (st.state == TetherState::On) {
                fails = 0;
            } else if (st.state != TetherState::InTransition) {
                std::wstring err;
                Log(L"[watch] hotspot is %s - restarting", StateName(st.state));
                if (HotspotStart(err)) {
                    Log(L"[watch] hotspot restarted OK");
                    last  = TetherState::On;
                    fails = 0;
                } else {
                    ++fails;
                    Log(L"[watch] restart failed (%d in a row): %s", fails, err.c_str());
                }
            }
        }

        if (GetTickCount64() - lastHeartbeat > 10ull * 60ull * 1000ull) {
            Log(L"[watch] heartbeat state=%s clients=%u", StateName(last), st.clients);
            lastHeartbeat = GetTickCount64();
        }

        DWORD w = WaitForMultipleObjects(2, waits, FALSE, IntervalForFailures(fails));
        if (w == WAIT_OBJECT_0) break;                 // stop requested
        if (w == WAIT_OBJECT_0 + 1) {                  // resumed from suspend
            ResetEvent(g_wakeEvent);
            Log(L"[watch] resume detected - forcing immediate hotspot check");
            fails = 0;
            first = true;
            AcquirePowerRequests();                    // re-arm after resume
        }
    }

    ReleasePowerRequests();
}

// ---------------------------------------------------------------- service plumbing

static void ReportStatus(DWORD state, DWORD waitHint = 0)
{
    static DWORD checkPoint = 1;

    g_status.dwCurrentState  = state;
    g_status.dwWin32ExitCode = NO_ERROR;
    g_status.dwWaitHint      = waitHint;
    g_status.dwControlsAccepted =
        (state == SERVICE_START_PENDING)
            ? 0
            : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT);

    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;

    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

static DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD eventType, LPVOID, LPVOID)
{
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            Log(L"[svc] stop requested");
            ReportStatus(SERVICE_STOP_PENDING, 5000);
            if (g_stopEvent) SetEvent(g_stopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_POWEREVENT:
            if (eventType == PBT_APMRESUMEAUTOMATIC || eventType == PBT_APMRESUMESUSPEND) {
                if (g_wakeEvent) SetEvent(g_wakeEvent);
            }
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            ReportStatus(g_status.dwCurrentState);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    LogInit(L"hotspotd", false);

    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
    if (!g_statusHandle) return;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ReportStatus(SERVICE_START_PENDING, 10000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent || !g_wakeEvent) {
        ReportStatus(SERVICE_STOPPED);
        return;
    }

    Log(L"[svc] HotspotGuard starting");
    ReportStatus(SERVICE_RUNNING);

    WatchdogLoop();

    Log(L"[svc] HotspotGuard stopped");
    ReportStatus(SERVICE_STOPPED);
}

// ---------------------------------------------------------------- install / uninstall

static std::wstring ExePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static int InstallService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { wprintf(L"OpenSCManager failed err=%lu (run elevated)\n", GetLastError()); return 1; }

    std::wstring bin = L"\"" + ExePath() + L"\"";

    SC_HANDLE svc = CreateServiceW(
        scm, kServiceName, kServiceName,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS) {
            svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
            if (svc) {
                ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                     SERVICE_ERROR_NORMAL, bin.c_str(),
                                     nullptr, nullptr, nullptr, nullptr, nullptr, kServiceName);
                wprintf(L"Service already existed - configuration updated.\n");
            }
        } else {
            wprintf(L"CreateService failed err=%lu\n", e);
            CloseServiceHandle(scm);
            return 1;
        }
    }
    if (!svc) { CloseServiceHandle(scm); return 1; }

    SERVICE_DESCRIPTIONW desc{ const_cast<LPWSTR>(kDescription) };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Fail-safe: if the process ever dies, SCM restarts it after 5s, indefinitely.
    SC_ACTION acts[3];
    acts[0].Type = SC_ACTION_RESTART; acts[0].Delay = 5000;
    acts[1].Type = SC_ACTION_RESTART; acts[1].Delay = 5000;
    acts[2].Type = SC_ACTION_RESTART; acts[2].Delay = 5000;

    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions      = 3;
    fa.lpsaActions   = acts;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    // Restart even when the process exits with a success code.
    SERVICE_FAILURE_ACTIONS_FLAG faf{ TRUE };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &faf);

    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING)
            wprintf(L"StartService failed err=%lu\n", e);
    }

    wprintf(L"Installed and started service '%s'.\n", kServiceName);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int UninstallService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { wprintf(L"OpenSCManager failed err=%lu (run elevated)\n", GetLastError()); return 1; }

    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        wprintf(L"Service not installed.\n");
        CloseServiceHandle(scm);
        return 0;
    }

    SERVICE_STATUS ss{};
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    Sleep(1000);

    if (!DeleteService(svc)) wprintf(L"DeleteService failed err=%lu\n", GetLastError());
    else                     wprintf(L"Service '%s' removed.\n", kServiceName);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// ---------------------------------------------------------------- diagnostics

// Stops then restarts tethering, proving the full start path works in whatever
// security context this process is running under (notably LocalSystem / session 0).
static int SelfTestCycle()
{
    LogInit(L"hotspotd-selftest", true);

    wchar_t user[256]{}; DWORD ulen = 256;
    GetUserNameW(user, &ulen);
    Log(L"[cycle] identity = %s  session = %lu", user, WTSGetActiveConsoleSessionId());

    WinRtInitThread();
    EnsureSupportServices();

    std::wstring err;
    if (!HotspotStop(err)) { Log(L"[cycle] STOP FAILED: %s", err.c_str()); return 2; }
    Log(L"[cycle] stopped; state=%s", StateName(HotspotQuery().state));

    if (!HotspotStart(err)) { Log(L"[cycle] START FAILED: %s", err.c_str()); return 3; }

    HotspotStatus st = HotspotQuery();
    Log(L"[cycle] restarted; state=%s clients=%u uplink=%s",
        StateName(st.state), st.clients, st.profile.c_str());

    if (st.state != TetherState::On) { Log(L"[cycle] FAILED - not On after start"); return 4; }
    Log(L"[cycle] SUCCESS - tethering start works in this context");
    return 0;
}

static int SelfTest(bool tryStart)
{
    LogInit(L"hotspotd-selftest", true);
    Log(L"[test] running as PID %lu", GetCurrentProcessId());

    wchar_t user[256]{}; DWORD ulen = 256;
    if (GetUserNameW(user, &ulen)) Log(L"[test] identity = %s", user);

    WinRtInitThread();
    EnsureSupportServices();

    HotspotStatus st = HotspotQuery();
    if (!st.queried) {
        Log(L"[test] QUERY FAILED: %s", st.error.c_str());
        return 2;
    }
    Log(L"[test] state=%s clients=%u/%u uplink=%s",
        StateName(st.state), st.clients, st.maxClients, st.profile.c_str());

    if (tryStart && st.state != TetherState::On) {
        std::wstring err;
        if (HotspotStart(err)) Log(L"[test] start OK");
        else { Log(L"[test] START FAILED: %s", err.c_str()); return 3; }
    }

    Log(L"[test] success");
    return 0;
}

static int RunConsole()
{
    LogInit(L"hotspotd", true);
    Log(L"[svc] running in console mode - Ctrl+C to stop");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WatchdogLoop();
    return 0;
}

} // namespace hg

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1) {
        std::wstring a = argv[1];
        if (a == L"--install")   return hg::InstallService();
        if (a == L"--uninstall") return hg::UninstallService();
        if (a == L"--selftest")  return hg::SelfTest(false);
        if (a == L"--selftest-start") return hg::SelfTest(true);
        if (a == L"--selftest-cycle") return hg::SelfTestCycle();
        if (a == L"--console")   return hg::RunConsole();

        wprintf(L"hotspotd.exe [--install|--uninstall|--selftest|--selftest-start|--console]\n");
        return 1;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(hg::kServiceName), hg::ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            wprintf(L"Not started by the SCM. Use --console to run interactively,\n"
                    L"or --install to register the service.\n");
            return 1;
        }
        return 1;
    }
    return 0;
}
