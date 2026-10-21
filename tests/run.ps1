param()
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
$pio = Join-Path $PWD '.venv\Scripts\pio.exe'
if (-not (Test-Path -LiteralPath $pio)) {
    $command = Get-Command pio -ErrorAction SilentlyContinue
    if (-not $command) { throw 'PlatformIO Core is required.' }
    $pio = $command.Source
}
New-Item -ItemType Directory -Path .pio -Force | Out-Null
& $pio pkg exec --package platformio/toolchain-gccmingw32@1.50100.0 -- g++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/test_control.cpp -o .pio/test_control.exe
if ($LASTEXITCODE -ne 0) { throw 'Control test compilation failed.' }
& $pio pkg exec --package platformio/toolchain-gccmingw32@1.50100.0 -- .pio/test_control.exe
if ($LASTEXITCODE -ne 0) { throw 'Control tests failed.' }
