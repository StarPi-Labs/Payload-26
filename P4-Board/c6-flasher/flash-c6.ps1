<#
.SYNOPSIS
    Build and flash the ESP-NOW receiver firmware onto the ESP32-C6.

.DESCRIPTION
    Wraps the idf.py build/flash for ../espnow-link-test/c6-receiver with the
    right target and a safety confirmation, because this OVERWRITES the C6's
    esp-hosted slave firmware and takes the P4's Wi-Fi and Bluetooth with it
    until restore-c6.ps1 is run.

    Run verify-link.ps1 first. This script refuses to guess at wiring.

.PARAMETER Port
    Serial port of your adapter, e.g. COM7.

.PARAMETER Force
    Skip the confirmation prompt.

.EXAMPLE
    .\flash-c6.ps1 -Port COM7
#>
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$projectDir = Join-Path $PSScriptRoot "..\espnow-link-test\c6-receiver"
if (-not (Test-Path $projectDir)) {
    Write-Host "[FAIL] project not found: $projectDir" -ForegroundColor Red
    exit 1
}
$projectDir = (Resolve-Path $projectDir).Path

Write-Host ""
Write-Host "About to flash the ESP32-C6" -ForegroundColor Cyan
Write-Host "  project : $projectDir"
Write-Host "  port    : $Port"
Write-Host ""
Write-Host "THIS OVERWRITES THE FACTORY esp-hosted SLAVE FIRMWARE." -ForegroundColor Yellow
Write-Host "While the ESP-NOW firmware is loaded, the ESP32-P4 has NO Wi-Fi" -ForegroundColor Yellow
Write-Host "and NO Bluetooth. Reversible with restore-c6.ps1." -ForegroundColor Yellow
Write-Host ""

if (-not $Force) {
    $reply = Read-Host "Type 'yes' to continue"
    if ($reply -ne 'yes') {
        Write-Host "Aborted - nothing was written." -ForegroundColor DarkGray
        exit 0
    }
}

$probe = Get-Command idf.py -ErrorAction SilentlyContinue
if ($null -eq $probe) {
    Write-Host "[FAIL] idf.py not found. Open an ESP-IDF terminal first." -ForegroundColor Red
    exit 1
}

Push-Location $projectDir
try {
    Write-Host ""
    Write-Host "--- set-target esp32c6 ---" -ForegroundColor Cyan
    # Regenerates sdkconfig from sdkconfig.defaults, which is what forces the
    # console onto UART0 so you can actually see the receiver's output.
    & idf.py set-target esp32c6
    if (-not $?) { throw "set-target failed" }

    Write-Host ""
    Write-Host "--- build ---" -ForegroundColor Cyan
    & idf.py build
    if (-not $?) { throw "build failed" }

    Write-Host ""
    Write-Host "--- flash ---" -ForegroundColor Cyan
    & idf.py -p $Port flash
    if (-not $?) { throw "flash failed" }

    Write-Host ""
    Write-Host "[OK] C6 flashed." -ForegroundColor Green
    Write-Host ""
    Write-Host "Now watch it receive:" -ForegroundColor Cyan
    Write-Host "    idf.py -p $Port monitor"
    Write-Host ""
    Write-Host "and power up the S3 transmitter from ../espnow-link-test/s3-sender."
}
catch {
    Write-Host ""
    Write-Host "[FAIL] $_" -ForegroundColor Red
    Write-Host "Run .\verify-link.ps1 -Port $Port to check the wiring." -ForegroundColor Yellow
    exit 1
}
finally {
    Pop-Location
}
