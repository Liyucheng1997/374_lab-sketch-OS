@echo off
rem SketchOS: run in QEMU on Windows (calls run-qemu.ps1 next to this file).
rem Extra arguments are passed through, e.g.:  -NoGui   -Wsl   -DryRun
rem This file is kept pure ASCII on purpose: cmd.exe reads .bat files in the
rem console code page, so non-ASCII text inside it could be garbled.
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1" %*
if errorlevel 1 pause
