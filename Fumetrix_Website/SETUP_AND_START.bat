@echo off
setlocal
cd /d "%~dp0"
echo Fumetrix - first-time setup and start
if not exist "app.py" goto missing
if not exist "requirements.txt" goto missing
py -3.11 --version
if errorlevel 1 goto failed
if not exist ".venv\Scripts\python.exe" (
  py -3.11 -m venv .venv
  if errorlevel 1 goto failed
)
.venv\Scripts\python.exe -m pip install -r requirements.txt
if errorlevel 1 goto failed
echo.
echo Stop your old Hello World server with Ctrl+C before continuing.
echo Keep the Cloudflared service running.
pause
.venv\Scripts\python.exe app.py
if errorlevel 1 goto failed
exit /b 0
:missing
echo Extract the ZIP fully. This file must be beside app.py and requirements.txt.
pause
exit /b 1
:failed
echo Setup or startup failed. Send the error above to troubleshoot.
pause
exit /b 1
