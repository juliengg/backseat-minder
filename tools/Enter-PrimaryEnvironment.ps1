# Dot-source this file in PowerShell before running idf.py:
# . ./tools/Enter-PrimaryEnvironment.ps1
param(
    [string]$IdfPath = (Join-Path $env:USERPROFILE 'esp/esp-idf-v5.5.4'),
    [string]$WhoPath = (Join-Path $env:USERPROFILE 'esp/esp-who-legacy'),
    [string]$ToolsPath = (Join-Path $env:USERPROFILE '.espressif')
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath (Join-Path $IdfPath 'export.ps1'))) {
    throw "ESP-IDF not found at $IdfPath"
}
if (!(Test-Path -LiteralPath (Join-Path $WhoPath 'components'))) {
    throw "Legacy ESP-WHO not found at $WhoPath"
}
# Use the workspace's portable Git when a system Git is unavailable.
if (!(Get-Command git -ErrorAction SilentlyContinue)) {
    $portableGit = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) '.tools/PortableGit/cmd'
    if (Test-Path -LiteralPath (Join-Path $portableGit 'git.exe')) {
        $env:PATH = $portableGit + ';' + $env:PATH
    }
}
# PlatformIO's existing base Python can bootstrap the official IDF environment.
$bootstrapPython = Join-Path $env:USERPROFILE '.platformio/python3'
if (Test-Path -LiteralPath (Join-Path $bootstrapPython 'python.exe')) {
    $env:PATH = $bootstrapPython + ';' + $env:PATH
}
$env:IDF_PATH = $IdfPath
$env:ESP_WHO_PATH = $WhoPath
$env:IDF_TOOLS_PATH = $ToolsPath
. (Join-Path $IdfPath 'export.ps1')
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF environment activation failed.' }
Write-Host "ESP-WHO: $WhoPath"
Write-Host 'Environment ready. Run: idf.py build'
