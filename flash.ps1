param([string]$Port = 'COM4', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$pio = Join-Path $PSScriptRoot '.venv\Scripts\pio.exe'
if (-not (Test-Path -LiteralPath $pio)) {
    $command = Get-Command pio -ErrorAction SilentlyContinue
    if (-not $command) { throw 'PlatformIO Core is missing.' }
    $pio = $command.Source
}
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') }
& $pio run -e esp32dev -t upload --upload-port $Port
if ($LASTEXITCODE -ne 0) { throw 'ESP32 upload failed.' }
Write-Output "Flashed ESP32 on $Port. Open serial at 115200 baud to see its web address."
