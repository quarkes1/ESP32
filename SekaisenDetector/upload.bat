@echo off
setlocal enabledelayedexpansion

set "SKETCH_DIR=C:\Users\19444\Desktop\ESP32\SekaisenDetector"
set "ARDUINO_CLI=D:\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set "FLASHER=C:\Users\19444\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.10\tools\flasher.exe"
set "ESPTOOL=C:\Users\19444\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\5.3.0\esptool.exe"
set "BOARD_FQBN=esp32:esp32:esp32:UploadSpeed=115200,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default"
set "BOOT_APP0=C:\Users\19444\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.10\tools\partitions\boot_app0.bin"

REM Extract sketch name
for %%I in ("%SKETCH_DIR%") do set "SKETCH_NAME=%%~nxI"

REM Kill serial-discovery to free COM ports
echo [1/3] Killing serial-discovery...
taskkill /F /IM serial-discovery.exe >nul 2>&1

REM Compile only (no upload)
echo [2/3] Compiling...
"%ARDUINO_CLI%" compile --fqbn "%BOARD_FQBN%" "%SKETCH_DIR%"
if %ERRORLEVEL% neq 0 (
    echo COMPILE FAILED!
    pause
    exit /b 1
)

REM Find build directory (most recently modified)
set "BUILD_DIR="
for /f "delims=" %%d in ('powershell -NoProfile -Command "Get-ChildItem 'C:\Users\19444\AppData\Local\arduino\sketches' -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object { $_.FullName }"') do set "BUILD_DIR=%%d"

if not defined BUILD_DIR (
    echo Could not find build directory!
    pause
    exit /b 1
)

REM Auto-detect ESP32 COM port
set "ESP_PORT="
for /f "delims=" %%p in ('powershell -NoProfile -Command "$p = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' -and $_.Name -notmatch 'Standard|Bluetooth' } | ForEach-Object { if($_.Name -match 'COM(\d+)') { $matches[1] } } | ForEach-Object { $com = $_; $port = New-Object System.IO.Ports.SerialPort "COM$com",115200,None,8,One; try { $port.Open(); $port.Close(); Write-Host "COM$com"; break } catch {} }; if(-not $p){ Write-Host 'NOT_FOUND' }"') do set "ESP_PORT=%%p"

if "%ESP_PORT%"=="NOT_FOUND" (
    echo.
    echo ========================================
    echo  NO ESP32 FOUND!
    echo  Available ports:
    powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { Write-Host '   ' $_ }"
    echo.
    echo  Please:
    echo   1. Check USB cable and board connection
    echo   2. Try a different USB port
    echo   3. Check Device Manager for 'USB Serial' errors
    echo ========================================
    pause
    exit /b 1
)

echo Build dir: %BUILD_DIR%
echo ESP32 Port: COM%ESP_PORT%

REM Upload directly via flasher (bypasses arduino-cli's broken COM5 handling)
echo [3/3] Uploading...
"%FLASHER%" --esptool "%ESPTOOL%" --build-dir "%BUILD_DIR%" --no-fast-flash --chip esp32 --port "COM%ESP_PORT%" --baud 115200 --before default-reset --after hard-reset write-flash -z --flash-mode keep --flash-freq keep --flash-size keep 0x1000 "%BUILD_DIR%/%SKETCH_NAME%.ino.bootloader.bin" 0x8000 "%BUILD_DIR%/%SKETCH_NAME%.ino.partitions.bin" 0xe000 "%BOOT_APP0%" 0x10000 "%BUILD_DIR%/%SKETCH_NAME%.ino.bin"

if %ERRORLEVEL% equ 0 (
    echo.
    echo ============ UPLOAD SUCCESS! ============
) else (
    echo.
    echo ============ UPLOAD FAILED ============
)
pause
