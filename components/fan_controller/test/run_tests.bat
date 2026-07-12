@echo off
rem Host-side unit-test runner for FanControllerLogic.
rem Uses MSYS2 mingw64 toolchain (GCC 16.1+, C++17 ready).
rem Runs from anywhere — script chdir's to its own directory.
rem
rem Re-run after every edit to fan_controller_logic.h or test_*.cpp.

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

echo === Compile fan_controller_logic ===
g++ -std=c++17 -Wall -Wextra -Wpedantic -I.. test_fan_controller_logic.cpp -o test_fan_controller_logic.exe
if errorlevel 1 (
    echo *** fan_controller_logic compile failed ***
    popd
    exit /b 1
)

echo === Compile dual_gesture_tracker ===
g++ -std=c++17 -Wall -Wextra -Wpedantic -I.. test_dual_gesture_tracker.cpp -o test_dual_gesture_tracker.exe
if errorlevel 1 (
    echo *** dual_gesture_tracker compile failed ***
    popd
    exit /b 1
)

echo.
echo === Run fan_controller_logic ===
.\test_fan_controller_logic.exe
set "RC1=%ERRORLEVEL%"

echo.
echo === Run dual_gesture_tracker ===
.\test_dual_gesture_tracker.exe
set "RC2=%ERRORLEVEL%"

set /a RC=RC1+RC2
popd
endlocal & exit /b %RC%
