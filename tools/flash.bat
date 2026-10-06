@echo off
rem AGV Wi-Fi Bridge 전체 이미지 굽기 (Windows)
rem 사용: flash.bat COM5 [merged.bin]
rem esptool 탐색 순서: Arduino IDE ESP32 코어 내장 esptool.exe → PATH 의 esptool → python -m esptool
setlocal enabledelayedexpansion
set "PORT=%~1"
if "%PORT%"=="" (
    echo usage: flash.bat COMx [merged.bin]
    exit /b 1
)
set "BIN=%~2"
if "%BIN%"=="" set "BIN=%~dp0agv_wifi_bridge_merged.bin"
if not exist "%BIN%" (
    echo not found: %BIN%
    exit /b 1
)

set "ESPTOOL="
for /d %%d in ("%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\*") do (
    if exist "%%d\esptool.exe" set ESPTOOL="%%d\esptool.exe"
)
if not defined ESPTOOL (
    where esptool >nul 2>&1 && set "ESPTOOL=esptool"
)
if not defined ESPTOOL (
    python -m esptool version >nul 2>&1 && set "ESPTOOL=python -m esptool"
)
if not defined ESPTOOL (
    echo esptool not found. install: pip install esptool  ^(or install ESP32 board in Arduino IDE^)
    exit /b 1
)

echo using: !ESPTOOL!
!ESPTOOL! --chip esp32s3 --port %PORT% --baud 921600 --before default_reset --after hard_reset write_flash 0x0 "%BIN%"
if errorlevel 1 (
    echo.
    echo flash failed. if the port is busy, close Arduino IDE serial monitor / idf.py monitor.
    echo boards without auto-reset: hold BOOT, press RESET, release BOOT, then retry.
    exit /b 1
)
echo done. open a terminal at 115200 on the UART-side USB for the bridge^> console.
