$ErrorActionPreference = 'Stop'
$hostsPath = Join-Path $env:WINDIR 'System32\drivers\etc\hosts'
$original = [IO.File]::ReadAllText($hostsPath)
$pattern = '(?m)^[ \t]*192\.229\.232\.200[ \t]+download\.visualstudio\.microsoft\.com[^\r\n]*'
if ($original -match $pattern) {
    $backupPath = Join-Path $PSScriptRoot ('hosts-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
    Copy-Item -LiteralPath $hostsPath -Destination $backupPath
    $updated = [regex]::Replace($original, $pattern, '# Disabled stale Visual Studio download mapping: $0')
    [IO.File]::WriteAllText($hostsPath, $updated, [Text.UTF8Encoding]::new($false))
    Write-Output "Hosts entry disabled. Backup: $backupPath"
} else {
    Write-Output 'No matching stale hosts entry found.'
}
ipconfig /flushdns
