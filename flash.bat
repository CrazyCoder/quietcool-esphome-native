@echo off
rem Build + OTA-flash the ESPHome replacement firmware to the live hub.
rem Run from anywhere — script chdir's to its own directory.
rem
rem Stage 1: `esphome compile` produces .esphome\build\quietcool-atticfan\
rem          .pioenvs\quietcool-atticfan\firmware.bin (ESPHome's "factory" image).
rem Stage 2: `esphome upload` does an mDNS lookup of
rem          quietcool-atticfan-<mac6>.local, then runs the ESPHome OTA
rem          handshake using ota.password from secrets.yaml.
rem
rem Override the discovery target by passing a host as the first arg,
rem e.g.  flash.bat 192.168.1.42

setlocal
pushd "%~dp0"

set "YAML=quietcool-atticfan.yaml"
rem The shipped config sets `name_add_mac_suffix: true`, so ESPHome's mDNS
rem hostname is quietcool-atticfan-<mac6>.local, where <mac6> is the last 3 bytes
rem of the ESP32 *WiFi/STA* MAC (e.g. STA MAC 84:1f:e8:AB:CD:EF -> abcdef).
rem On ESP32 the BLE MAC differs from the STA MAC by 2 (its last byte is +2),
rem so don't read the suffix off the BLE advert. Fill in your device's <mac6>
rem below, or override per-run by passing the host/IP as the first arg
rem (e.g.  flash.bat quietcool-atticfan-abcdef.local  or  flash.bat 192.168.1.42).
set "DEVICE=quietcool-atticfan-xxxxxx.local"
if not "%~1"=="" set "DEVICE=%~1"

echo === [1/2] Compile ===
esphome compile %YAML%
if errorlevel 1 (
    echo *** compile failed ***
    popd
    exit /b 1
)

echo.
echo === [2/2] OTA upload to %DEVICE% ===
esphome upload %YAML% --device %DEVICE%
if errorlevel 1 (
    echo *** upload failed ***
    popd
    exit /b 1
)

echo.
echo === Done ===
popd
endlocal
