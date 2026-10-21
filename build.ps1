param()
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$pio = Join-Path $PSScriptRoot '.venv\Scripts\pio.exe'
if (-not (Test-Path -LiteralPath $pio)) {
    $command = Get-Command pio -ErrorAction SilentlyContinue
    if (-not $command) { throw 'Install PlatformIO Core (python -m pip install platformio), or create .venv and install it there.' }
    $pio = $command.Source
}
if (-not (Test-Path -LiteralPath 'include\secrets.h')) { throw 'Copy include/secrets.example.h to include/secrets.h and set Wi-Fi credentials.' }
& $pio run -e esp32dev
if ($LASTEXITCODE -ne 0) { throw 'ESP32 build failed.' }
Write-Output "Built $PSScriptRoot\.pio\build\esp32dev\firmware.bin"
