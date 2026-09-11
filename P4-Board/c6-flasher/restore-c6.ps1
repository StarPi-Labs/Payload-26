<#
.SYNOPSIS
    Restore the factory esp-hosted slave firmware to the ESP32-C6, giving the
    ESP32-P4 back its Wi-Fi and Bluetooth.

.DESCRIPTION
    Rebuilds Espressif's own esp_hosted:slave example from source and flashes it
    through the PROG_C6 header. Because it is rebuilt from upstream, this works
    even though you never took a backup of the image the board shipped with.

    The board ships with slave firmware v0.0.6; this will produce whatever
    version your ESP-IDF component manager resolves, which is normally newer and
    fine. The P4 host component and the C6 slave should be from compatible
    versions - if the P4 later complains about a version mismatch, update the
    esp_hosted component on the P4 side to match.

.PARAMETER Port
    Serial port of your adapter, e.g. COM7.

.PARAMETER WorkDir
    Where to create the slave project. Defaults to a 'restore' folder here.

.EXAMPLE
    .\restore-c6.ps1 -Port COM7
#>
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$WorkDir
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($WorkDir)) {
    $WorkDir = Join-Path $PSScriptRoot "restore"
}

$probe = Get-Command idf.py -ErrorAction SilentlyContinue
if ($null -eq $probe) {
    Write-Host "[FAIL] idf.py not found. Open an ESP-IDF terminal first." -ForegroundColor Red
    exit 1
}

$slaveDir = Join-Path $WorkDir "slave"

if (-not (Test-Path $slaveDir)) {
    Write-Host "--- fetching esp_hosted:slave example ---" -ForegroundColor Cyan
    if (-not (Test-Path $WorkDir)) {
        New-Item -ItemType Directory -Path $WorkDir | Out-Null
    }
    Push-Location $WorkDir
    try {
        & idf.py create-project-from-example "espressif/esp_hosted:slave"
        if (-not $?) { throw "could not fetch the slave example" }
    }
    finally {
        Pop-Location
    }
} else {
    Write-Host "--- reusing existing $slaveDir ---" -ForegroundColor DarkGray
}

Push-Location $slaveDir
try {
    Write-Host ""
    Write-Host "--- set-target esp32c6 ---" -ForegroundColor Cyan
    & idf.py set-target esp32c6
    if (-not $?) { throw "set-target failed" }

    Write-Host ""
    Write-Host "IMPORTANT: menuconfig is about to open." -ForegroundColor Yellow
    Write-Host "Set the transport to SDIO - that is how the C6 is wired to the" -ForegroundColor Yellow
    Write-Host "P4 on this board. The default may be SPI, which will build fine" -ForegroundColor Yellow
    Write-Host "and then silently never talk to the P4." -ForegroundColor Yellow
    Write-Host ""
    Read-Host "Press Enter to open menuconfig"

    & idf.py menuconfig
    if (-not $?) { throw "menuconfig failed" }

    Write-Host ""
    Write-Host "--- build ---" -ForegroundColor Cyan
    & idf.py build
    if (-not $?) { throw "build failed" }

    Write-Host ""
    Write-Host "--- flash ---" -ForegroundColor Cyan
    & idf.py -p $Port flash
    if (-not $?) { throw "flash failed" }

    Write-Host ""
    Write-Host "[OK] factory-equivalent slave firmware restored." -ForegroundColor Green
    Write-Host "Power-cycle the whole board, then the P4 should have Wi-Fi again."
}
catch {
    Write-Host ""
    Write-Host "[FAIL] $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
