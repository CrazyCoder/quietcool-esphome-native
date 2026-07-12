@echo off
rem Host-side unit-test runner for HttpFlashLogic.
rem Uses MSYS2 mingw64 toolchain (GCC 16.1+, C++17 ready).

setlocal
pushd "%~dp0"

set "MINGW=c:\tools\msys64\mingw64\bin"
if not exist "%MINGW%\g++.exe" (
    echo *** g++ not found at %MINGW%\g++.exe ***
    popd
    exit /b 1
)
set "PATH=%MINGW%;%PATH%"

echo === Compile ===
g++ -std=c++17 -Wall -Wextra -Wpedantic -I.. test_http_flash_logic.cpp -o test_http_flash_logic.exe
if errorlevel 1 (
    echo *** compile failed ***
    popd
    exit /b 1
)

echo.
echo === Run ===
.\test_http_flash_logic.exe
set "RC=%ERRORLEVEL%"

popd
endlocal & exit /b %RC%
