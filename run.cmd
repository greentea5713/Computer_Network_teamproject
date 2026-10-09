@echo off
setlocal
cd /d "%~dp0"
rem This launcher runs the x86 build with the installed 32-bit Npcap runtime.
if exist "%SystemRoot%\SysWOW64\Npcap\wpcap.dll" (
    set "PATH=%SystemRoot%\SysWOW64\Npcap;%PATH%"
) else if exist "%SystemRoot%\System32\Npcap\wpcap.dll" (
    set "PATH=%SystemRoot%\System32\Npcap;%PATH%"
)
if not exist "Debug\ARP.exe" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
    if errorlevel 1 (
        pause
        exit /b 1
    )
)
start "" "%~dp0Debug\ARP.exe"
