@echo off
setlocal

:: HydraulicController Flash Script
:: Flashes pre-compiled firmware to ESP32 using PlatformIO
:: Usage: flash.bat COM8

set PORT=%1
if "%PORT%"=="" (
    echo.
    echo  HydraulicController Flash Tool
    echo  ================================
    echo  Usage: flash.bat COMx
    echo.
    echo  Example: flash.bat COM8
    echo.
    echo  Find your port in Device Manager ^> Ports ^(COM ^& LPT^)
    exit /b 1
)

set PIO=%USERPROFILE%\.platformio\penv\Scripts\platformio.exe

if not exist "%PIO%" (
    echo ERROR: PlatformIO not found at %PIO%
    echo Install PlatformIO first: https://platformio.org/install/ide?install=vscode
    pause
    exit /b 1
)

echo.
echo  Flashing HydraulicController to %PORT%...
echo  ==========================================
echo.

"%PIO%" run --target upload --upload-port %PORT%

if errorlevel 1 (
    echo.
    echo  FLASH FAILED! Check:
    echo  - Is %PORT% correct?
    echo  - Is the ESP32 plugged in?
    echo  - Close any serial monitors first
    pause
    exit /b 1
)

echo.
echo  ==========================================
echo  SUCCESS! Firmware flashed to %PORT%.
echo  The ESP32 will restart automatically.
echo.
echo  Next steps:
echo  - Open Web UI: python configure.py
echo  - Push channels/settings via Channels tab
echo  - Change sounds via Sound Technician tab
echo  ==========================================
pause
