@echo off
setlocal DisableDelayedExpansion
set "ps=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
"%ps%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\windows.ps1" %*
set "rc=%ERRORLEVEL%"
if "%~1"=="" pause
exit /b %rc%
