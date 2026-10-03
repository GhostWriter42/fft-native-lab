@echo off
rem Double-click to play the native build (Docker Desktop must be running). See HOW-TO-PLAY.md for the options (two players, HD, ...).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0port\native\play.ps1" %*
pause
