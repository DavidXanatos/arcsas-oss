@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo This must be run as Administrator.
    pause
    exit /b 1
)

echo ============================================================
echo  srcraid install  (test-signed; testsigning must be ON)
echo ============================================================

echo.
echo [1/3] Trusting the test-signing certificate...
certutil -addstore -f Root             srcraid_test.cer >nul
certutil -addstore -f TrustedPublisher srcraid_test.cer >nul

echo [2/3] Staging driver package into the driver store...
pnputil /add-driver srcraid.inf
if errorlevel 1 (
    echo    pnputil /add-driver failed.
    pause
    exit /b 1
)

echo [3/3] Force-binding srcraid onto the Series 7 controller...
echo        (overrides the WHQL-ranked arcsas)
devcon.exe update srcraid.inf "PCI\VEN_9005&DEV_028C"
if errorlevel 1 (
    echo.
    echo    devcon update did not match. Hardware IDs present:
    devcon.exe hwids "PCI\VEN_9005*"
    echo    Re-run: devcon.exe update srcraid.inf "<one of the IDs above>"
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  Installed. In KD / DebugView (filter: srcraid) look for:
echo     srcraid: FindAdapter FOUND 9005:028c ...
echo     srcraid: HwInitialize ... OMR0=0x00010080
echo  The controller will show driver 'srcraid' with no disks
echo  (expected - LUN support arrives in a later phase).
echo.
echo  To return to arcsas (restore the test volume): run rollback.bat
echo ============================================================
pause
