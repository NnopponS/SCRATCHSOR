@echo off
setlocal
cd /d "%~dp0"
title SCRATCHSOR - Flash Board
echo SCRATCHSOR firmware and dashboard flasher
echo.
echo Connect the board by USB. Reset and restart are automatic.
echo Close the USB Dashboard and any serial monitor first.
echo Uses the existing built firmware and dashboard images.
echo.
python tools\flash_board.py %*
set "FLASH_EXIT=%errorlevel%"
echo.
if not "%FLASH_EXIT%"=="0" goto failed
echo Flash complete. The device restarts automatically.
echo Wi-Fi/password: SCRATCHSOR-9BD8   Dashboard: http://192.168.4.1
goto done
:failed
echo Flash failed. Check the error above.
echo Check the USB connection and run this file again.
:done
pause
exit /b %FLASH_EXIT%
