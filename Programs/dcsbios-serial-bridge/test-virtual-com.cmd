@echo off
REM ============================================================
REM  DCS-BIOS Serial Bridge - Virtual COM Pair Test
REM
REM  Prerequisites:
REM    - com0com virtual pair already created (e.g. COM10 / COM11)
REM    - Bridge built:  build\Release\dcsbios-serial-bridge.exe
REM    - Python + pyserial:  pip install pyserial
REM
REM  Usage:
REM    test-virtual-com.cmd              (uses COM10/COM11 defaults)
REM    test-virtual-com.cmd 10 11        (explicit write/read ports)
REM ============================================================

setlocal

set WRITE_PORT=%1
set READ_PORT=%2
if "%WRITE_PORT%"=="" set WRITE_PORT=10
if "%READ_PORT%"==""  set READ_PORT=11

set BRIDGE="%~dp0build\Release\dcsbios-serial-bridge.exe"
set MONITOR="%~dp0serial-monitor.py"

echo.
echo [INFO] Test setup
echo        Bridge writes to : COM%WRITE_PORT%
echo        Monitor reads from: COM%READ_PORT%
echo.

REM ------------------------------------------------------------------
REM  Check bridge binary
REM ------------------------------------------------------------------
if not exist %BRIDGE% (
    echo [ERROR] Bridge not built.  Run build-package.cmd first.
    exit /b 1
)

REM ------------------------------------------------------------------
REM  Check pyserial
REM ------------------------------------------------------------------
py -3 -c "import serial" 2>nul
if errorlevel 1 (
    echo [INFO] pyserial not found - installing ...
    py -3 -m pip install pyserial
    if errorlevel 1 (
        echo [ERROR] Could not install pyserial.
        exit /b 1
    )
)

REM ------------------------------------------------------------------
REM  Start serial monitor in a separate window
REM ------------------------------------------------------------------
echo [INFO] Starting serial monitor on COM%READ_PORT% ...
start "DCS-BIOS Monitor COM%READ_PORT%" cmd /k py -3 %MONITOR% %READ_PORT%
timeout /t 2 /nobreak >nul

REM ------------------------------------------------------------------
REM  Start bridge against WRITE_PORT with autostart
REM ------------------------------------------------------------------
echo [INFO] Starting bridge on COM%WRITE_PORT% ...
start "DCS-BIOS Bridge COM%WRITE_PORT%" %BRIDGE% --autostart --udp --ports=%WRITE_PORT%
timeout /t 2 /nobreak >nul

REM ------------------------------------------------------------------
REM  Send a known UDP test packet via PowerShell
REM ------------------------------------------------------------------
echo [INFO] Injecting UDP test packet to 239.255.50.10:5010 ...
powershell -NoProfile -Command ^
  "$c = New-Object System.Net.Sockets.UdpClient;" ^
  "$c.MulticastLoopback = $true;" ^
  "$ep = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Parse('239.255.50.10')),5010;" ^
  "[byte[]]$b = 0x55,0xAA,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E;" ^
  "[void]$c.Send($b,$b.Length,$ep); $c.Close();" ^
  "Write-Host 'UDP packet sent (16 bytes).'"

echo.
echo [INFO] Check the monitor window for READ_BYTES output.
echo        Expected: 16 bytes with hex  55 AA 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E
echo.
echo [INFO] Close the bridge and monitor windows when done.
echo        Or press any key to close this helper window.
pause >nul

endlocal
