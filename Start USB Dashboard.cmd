@echo off
setlocal
cd /d "%~dp0"
set "USB_PYTHON=python"
if exist ".pio\usb-env\Scripts\python.exe" set "USB_PYTHON=%~dp0.pio\usb-env\Scripts\python.exe"
"%USB_PYTHON%" tools\usb_dashboard.py --http-port 8766 %*
if errorlevel 1 pause
