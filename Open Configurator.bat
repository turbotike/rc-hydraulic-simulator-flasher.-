@echo off
cd /d "%~dp0"
start "" "http://localhost:8080"
python configure.py --no-browser
pause
