$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio 2022 first.' }
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -property installationPath
if (!$vs) { throw 'Visual Studio 2022 was not found.' }
$arguments = 'modify --installPath "' + $vs + '" --add Microsoft.VisualStudio.Component.VC.ATLMFC --quiet --norestart'
Write-Host 'Installing MFC. Accept the Windows administrator prompt. Downloading may take several minutes.'
$installation = Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
if ($installation.ExitCode -notin @(0, 3010)) {
    throw "MFC installation failed (exit $($installation.ExitCode)). Check the latest dd_installer logs in your TEMP folder and your connection to download.visualstudio.microsoft.com."
}
if ($installation.ExitCode -eq 3010) { Write-Host 'Windows restart is required before building.' }
else {
    if (!(Get-ChildItem "$vs\VC\Tools\MSVC\*\atlmfc\include\afxwin.h" -ErrorAction SilentlyContinue)) {
        throw 'MFC was not installed. Close any existing Visual Studio Installer window and retry. Check the newest dd_installer log if this persists.'
    }
    Write-Host 'MFC installation verified. Run build.ps1 -Test to verify the application.'
}
