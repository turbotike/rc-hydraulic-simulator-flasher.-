@echo off
setlocal

:: Flash a sound pack to the ESP32's sound partition
:: Usage: flash_sounds.bat COM8 [soundpack.bin]

set PORT=%1
set PACKFILE=%2
if "%PACKFILE%"=="" set PACKFILE=soundpack.bin

if "%PORT%"=="" (
    echo.
    echo  Sound Pack Flash Tool
    echo  =====================
    echo  Usage: flash_sounds.bat COMx [soundpack.bin]
    echo.
    echo  Example: flash_sounds.bat COM8
    echo           flash_sounds.bat COM8 my_sounds.bin
    echo.
    exit /b 1
)

set PIO_PYTHON=%USERPROFILE%\.platformio\penv\Scripts\python.exe
set ESPTOOL=%USERPROFILE%\.platformio\packages\tool-esptoolpy\esptool.py

if not exist "%PIO_PYTHON%" (
    echo ERROR: PlatformIO Python not found.
    pause
    exit /b 1
)

if not exist "%PACKFILE%" (
    echo ERROR: Sound pack file not found: %PACKFILE%
    echo Use the Web UI Sound Pack Builder to generate one,
    echo or run: python configure.py
    pause
    exit /b 1
)

echo.
echo  Flashing sound pack to %PORT%...
echo  File: %PACKFILE%
echo.

"%PIO_PYTHON%" "%ESPTOOL%" --chip esp32 --port %PORT% --baud 921600 ^
    --before default_reset --after hard_reset ^
    write_flash 0x190000 "%PACKFILE%"

if errorlevel 1 (
    echo.
    echo  FLASH FAILED!
    pause
    exit /b 1
)

echo.
echo  Sound pack flashed! ESP32 will restart with new sounds.
pause
