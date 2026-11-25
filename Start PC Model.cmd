@echo off
setlocal
cd /d "%~dp0"
python tools\pc_model.py --model models\v4\model.json %*
if errorlevel 1 pause
