@echo off
rem Double-click: opens the launcher (choose the program, resolution, fast effects, a save state to start from, ...). See HOW-TO-PLAY.md.
rem The command-line way still works: powershell -File port\native\play.ps1 -Native -Hd 2 [-FastEffects] [-Filter] ...
if not exist "%~dp0port\build\venv\Scripts\pythonw.exe" (
  echo The viewer needs its Python environment first:  python -m venv port\build\venv  and  port\build\venv\Scripts\pip install moderngl glfw numpy pillow
  pause
  exit /b 1
)
start "" "%~dp0port\build\venv\Scripts\pythonw.exe" "%~dp0port\native\launcher.pyw"
