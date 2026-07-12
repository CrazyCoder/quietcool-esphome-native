@echo off
rem Host-side unit-test runner for LedStateMachineLogic.
rem Uses MSYS2 mingw64 toolchain (GCC 16.1+, C++17 ready).
rem Runs from anywhere — script chdir's to its own directory.
rem
rem Re-run after every edit to led_state_machine_logic.h or test_*.cpp.

setlocal
pushd "%~dp0"

rem Locate g++: honor a pre-set MINGW env var, else check common MSYS2
rem install locations, else fall back to g++ already on PATH.
if not defined MINGW (
    for %%D in ("C:\msys64\mingw64\bin" "C:\tools\msys64\mingw64\bin") do (
        if not defined MINGW if exist "%%~D\g++.exe" set "MINGW=%%~D"
    )
)
if defined MINGW set "PATH=%MINGW%;%PATH%"
where g++ >nul 2>nul
if errorlevel 1 (
    echo *** g++ not found - install MSYS2 mingw64 or set MINGW to your mingw64\bin ***
    popd
    exit /b 1
)

echo === Compile ===
g++ -std=c++17 -Wall -Wextra -Wpedantic -I.. test_led_state_machine_logic.cpp -o test_led_state_machine_logic.exe
if errorlevel 1 (
    echo *** compile failed ***
    popd
    exit /b 1
)

echo.
echo === Run ===
.\test_led_state_machine_logic.exe
set "RC=%ERRORLEVEL%"

popd
endlocal & exit /b %RC%
