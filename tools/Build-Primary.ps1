# Build only. This script never flashes or opens a serial port.
param(
    [string]$BuildDirectory = (Join-Path $env:USERPROFILE 'esp/build-backseat-minder'),
    [string]$IdfPath = (Join-Path $env:USERPROFILE 'esp/esp-idf-v5.5.4'),
    [string]$WhoPath = (Join-Path $env:USERPROFILE 'esp/esp-who-legacy')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-PrimaryEnvironment.ps1') -IdfPath $IdfPath -WhoPath $WhoPath
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    python (Join-Path $env:IDF_PATH 'tools/idf.py') -B $BuildDirectory -D "ESP_WHO_PATH=$env:ESP_WHO_PATH" build
    if ($LASTEXITCODE -ne 0) { throw "Primary build failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}
