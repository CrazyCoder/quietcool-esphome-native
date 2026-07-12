@echo off
rem Host-side unit-test runner for OemNvsReaderLogic.
rem Uses MSYS2 mingw64 toolchain (GCC 16.1+, C++17 ready).
rem Runs from anywhere — script chdir's to its own directory.
rem
rem Re-run after every edit to oem_nvs_reader_logic.h or test_*.cpp.

setlocal
pushd "%~dp0"

set "MINGW=c:\tools\msys64\mingw64\bin"
if not exist "%MINGW%\g++.exe" (
    echo *** g++ not found at %MINGW%\g++.exe ***
    echo *** install msys2 mingw64 or edit MINGW path in run_tests.bat ***
    popd
    exit /b 1
)

set "PATH=%MINGW%;%PATH%"

echo === Compile ===
g++ -std=c++17 -Wall -Wextra -Wpedantic -I.. test_oem_nvs_reader_logic.cpp -o test_oem_nvs_reader_logic.exe
if errorlevel 1 (
    echo *** compile failed ***
    popd
    exit /b 1
)

echo.
echo === Run ===
.\test_oem_nvs_reader_logic.exe
set "RC1=%ERRORLEVEL%"

echo.
echo === Fixture: group_history_keys() vs real NVS dump ===
python verify_group_keys_fixture.py
set "RC2=%ERRORLEVEL%"

set /a RC=RC1+RC2
popd
endlocal & exit /b %RC%
