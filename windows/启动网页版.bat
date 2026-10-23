@echo off
rem SketchOS web version launcher: starts a local web server and opens the browser.
rem This file is ASCII on purpose (cmd.exe reads .bat files in the console code page);
rem all messages come from serve-web.ps1. Extra arguments are passed through, e.g. -Build.
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve-web.ps1" %*
if errorlevel 1 pause
endlocal
