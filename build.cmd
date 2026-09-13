@echo off
setlocal

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "WINSDK=10.0.26100.0"

if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" %WINSDK% >nul
if errorlevel 1 (
    echo ERROR: failed to initialise the MSVC environment.
    exit /b 1
)

cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

set "CFLAGS=/nologo /W4 /O2 /MT /std:c++20 /EHsc /DUNICODE /D_UNICODE /Fo:obj\\"

echo === building hotspotd.exe ===
cl %CFLAGS% src\hotspotd.cpp src\hotspot.cpp /Fe:bin\hotspotd.exe ^
   /link /SUBSYSTEM:CONSOLE windowsapp.lib advapi32.lib
if errorlevel 1 goto :fail

if exist src\blackout.cpp (
    echo === building blackout.exe ===
    cl %CFLAGS% src\blackout.cpp /Fe:bin\blackout.exe ^
       /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib powrprof.lib advapi32.lib
    if errorlevel 1 goto :fail
)

echo.
echo === build OK : output in %~dp0bin ===
exit /b 0

:fail
echo.
echo === BUILD FAILED ===
exit /b 1
