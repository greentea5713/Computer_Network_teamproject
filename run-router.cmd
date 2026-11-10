@echo off
setlocal
cd /d "%~dp0"
set "NPCAP_DIR=%SystemRoot%\SysWOW64\Npcap"
if not exist "%NPCAP_DIR%\Packet.dll" set "NPCAP_DIR=%SystemRoot%\System32\Npcap"
if not exist "%NPCAP_DIR%\Packet.dll" (
    echo Npcap runtime was not found. Install Npcap from https://npcap.com/#download
    echo Then run this file again. The Npcap SDK alone is not sufficient.
    pause
    exit /b 1
)
if not exist "%NPCAP_DIR%\wpcap.dll" (
    echo Npcap installation is incomplete. Please reinstall Npcap.
    pause
    exit /b 1
)
if not exist "%~dp0Release\ARP.exe" (
    echo Release\ARP.exe was not found. Build the Release configuration first.
    pause
    exit /b 1
)
set "PATH=%NPCAP_DIR%;%PATH%"
start "" "%~dp0Release\ARP.exe"
