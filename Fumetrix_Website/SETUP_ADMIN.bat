@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Run SETUP_AND_START.bat first.
  pause
  exit /b 1
)
.venv\Scripts\python.exe setup_admin.py
pause
