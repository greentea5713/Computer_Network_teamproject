param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio 2022 with Desktop development with C++ and MFC.' }
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2022 C++ build tools were not found.' }
if (!(Get-ChildItem "$vs\VC\Tools\MSVC\*\atlmfc\include\afxwin.h" -ErrorAction SilentlyContinue)) {
    throw 'MFC is missing. In Visual Studio Installer, add C++ MFC for v143 build tools (x86 & x64).'
}
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
Push-Location $PSScriptRoot
try {
    & $msbuild .\MACFileTransfer.sln /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x86 /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Application build failed.' }
    if ($Test) {
        & $msbuild .\MACFileTransfer\ProtocolTests.vcxproj /m /t:Build "/p:Configuration=$Configuration" /p:Platform=Win32 /nologo /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
        & .\tests\bin\ProtocolTests.exe
        if ($LASTEXITCODE -ne 0) { throw 'Protocol tests failed.' }
    }
} finally {
    Pop-Location
}
