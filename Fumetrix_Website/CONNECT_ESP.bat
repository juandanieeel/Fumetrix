@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  py -3.11 -m venv .venv
  if errorlevel 1 goto failed
)
.venv\Scripts\python.exe -m pip install -r requirements.txt
if errorlevel 1 goto failed
.venv\Scripts\python.exe configure.py
if errorlevel 1 goto failed
echo Stop the old website server with Ctrl+C. Keep Cloudflared running.
pause
.venv\Scripts\python.exe app.py
pause
exit /b 0
:failed
echo Setup failed. Please send the error above.
pause
exit /b 1
