#pragma once
#include <string>

namespace hg {

// Must match Windows.Networking.NetworkOperators.TetheringOperationalState exactly.
// Note the ordering: On is 1 and Off is 2, not the other way round.
enum class TetherState { Unknown = 0, On = 1, Off = 2, InTransition = 3 };

struct HotspotStatus {
    bool         queried = false;   // the query itself succeeded
    TetherState  state   = TetherState::Unknown;
    unsigned     clients = 0;
    unsigned     maxClients = 0;
    std::wstring profile;
    std::wstring error;
};

const wchar_t* StateName(TetherState s);

// Reads current Mobile Hotspot state from the shared internet connection profile.
HotspotStatus HotspotQuery();

// Starts tethering. Returns true if tethering is On when the call returns.
bool HotspotStart(std::wstring& err);

// Stops tethering. Only used by --selftest-cycle to verify the start path end to end.
bool HotspotStop(std::wstring& err);

// Must be called once per thread that uses the two functions above.
void WinRtInitThread();

} // namespace hg
