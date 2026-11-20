@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo This must be run as Administrator.
    pause
    exit /b 1
)

echo Reverting the Series 7 controller to its WHQL driver (arcsas)...
echo (srcraid stays in the driver store for the next test; PnP re-ranks to arcsas.)
devcon.exe remove "PCI\VEN_9005&DEV_028C"
devcon.exe rescan

echo.
echo Done. Device Manager should show the 71605 back on arcsas and the
echo test volume should return. If not, reboot once.
pause
