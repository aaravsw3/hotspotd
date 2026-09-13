#include "hotspot.h"
#include "common.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

using namespace winrt;
using namespace winrt::Windows::Networking::Connectivity;
using namespace winrt::Windows::Networking::NetworkOperators;

namespace hg {

const wchar_t* StateName(TetherState s)
{
    switch (s) {
        case TetherState::Off:          return L"Off";
        case TetherState::On:           return L"On";
        case TetherState::InTransition: return L"InTransition";
        default:                        return L"Unknown";
    }
}

void WinRtInitThread()
{
    // Multi-threaded apartment so blocking .get() on WinRT async calls is legal.
    try { init_apartment(apartment_type::multi_threaded); }
    catch (hresult_error const&) { /* already initialised on this thread */ }
}

static NetworkOperatorTetheringManager MakeManager(std::wstring& profileName, std::wstring& err)
{
    auto prof = NetworkInformation::GetInternetConnectionProfile();
    if (!prof) {
        err = L"no internet connection profile (uplink down?)";
        return nullptr;
    }
    profileName = prof.ProfileName().c_str();
    return NetworkOperatorTetheringManager::CreateFromConnectionProfile(prof);
}

// StartTetheringAsync/StopTetheringAsync report Success before the radio has finished
// changing state, so callers must wait for the state to settle before trusting a query.
static bool WaitForState(NetworkOperatorTetheringManager const& mgr,
                         TetheringOperationalState want,
                         DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        auto now = mgr.TetheringOperationalState();
        if (now == want) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(500);
    }
}

HotspotStatus HotspotQuery()
{
    HotspotStatus s;
    try {
        auto mgr = MakeManager(s.profile, s.error);
        if (!mgr) return s;

        s.state      = static_cast<TetherState>(mgr.TetheringOperationalState());
        s.clients    = mgr.ClientCount();
        s.maxClients = mgr.MaxClientCount();
        s.queried    = true;
    } catch (hresult_error const& e) {
        s.error = e.message().c_str();
    }
    return s;
}

bool HotspotStart(std::wstring& err)
{
    try {
        std::wstring profile;
        auto mgr = MakeManager(profile, err);
        if (!mgr) return false;

        if (mgr.TetheringOperationalState() == TetheringOperationalState::On)
            return true;

        auto result = mgr.StartTetheringAsync().get();
        if (result.Status() == TetheringOperationStatus::Success) {
            if (WaitForState(mgr, TetheringOperationalState::On, 20000))
                return true;
            err = L"StartTethering reported success but state never reached On";
            return false;
        }

        err = L"StartTethering status=" + std::to_wstring(static_cast<int>(result.Status()));
        auto extra = result.AdditionalErrorMessage();
        if (!extra.empty()) err += L" detail=" + std::wstring(extra.c_str());
        return false;
    } catch (hresult_error const& e) {
        err = e.message().c_str();
        return false;
    }
}

bool HotspotStop(std::wstring& err)
{
    try {
        std::wstring profile;
        auto mgr = MakeManager(profile, err);
        if (!mgr) return false;

        if (mgr.TetheringOperationalState() == TetheringOperationalState::Off)
            return true;

        auto result = mgr.StopTetheringAsync().get();
        if (result.Status() == TetheringOperationStatus::Success) {
            if (WaitForState(mgr, TetheringOperationalState::Off, 20000))
                return true;
            err = L"StopTethering reported success but state never reached Off";
            return false;
        }

        err = L"StopTethering status=" + std::to_wstring(static_cast<int>(result.Status()));
        auto extra = result.AdditionalErrorMessage();
        if (!extra.empty()) err += L" detail=" + std::wstring(extra.c_str());
        return false;
    } catch (hresult_error const& e) {
        err = e.message().c_str();
        return false;
    }
}

} // namespace hg
