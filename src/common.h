#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdarg>

namespace hg {

inline std::wstring DataDir()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring d = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
    d += L"\\HotspotGuard";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

struct LogState {
    CRITICAL_SECTION cs;
    std::wstring     path;
    bool             echo;
    bool             init;
};

inline LogState& LS() { static LogState s{}; return s; }

inline void LogInit(const wchar_t* name, bool echo)
{
    LogState& s = LS();
    if (!s.init) { InitializeCriticalSection(&s.cs); s.init = true; }
    s.path = DataDir() + L"\\" + name + L".log";
    s.echo = echo;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(s.path.c_str(), GetFileExInfoStandard, &fad)) {
        ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz > 1024ull * 1024ull) {
            std::wstring prev = s.path + L".1";
            DeleteFileW(prev.c_str());
            MoveFileW(s.path.c_str(), prev.c_str());
        }
    }
}

inline void Log(const wchar_t* fmt, ...)
{
    LogState& s = LS();
    if (!s.init) LogInit(L"hotspotguard", false);

    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2304];
    _snwprintf_s(line, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d  %s\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);

    EnterCriticalSection(&s.cs);
    FILE* f = nullptr;
    if (_wfopen_s(&f, s.path.c_str(), L"a+, ccs=UTF-8") == 0 && f) {
        fputws(line, f);
        fclose(f);
    }
    if (s.echo) { fputws(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&s.cs);
}

} // namespace hg
