<#
.SYNOPSIS
    Prove the wiring to the ESP32-C6 works BEFORE attempting a real flash.

.DESCRIPTION
    Runs an esptool handshake against the C6 through the PROG_C6 header and
    interprets the result. Every failure mode maps to a specific wiring or
    procedural mistake, so you get "your TX and RX are swapped" instead of a
    raw stack trace.

    Run this first. Always. A failed handshake costs you five seconds; a failed
    flash can leave the C6 half-written and the P4 with no Wi-Fi until you
    recover it.

.PARAMETER Port
    Serial port of your adapter, e.g. COM7. Omit to list candidates and exit.

.PARAMETER Baud
    Handshake baud rate. Default 115200, which is the most forgiving.

.EXAMPLE
    .\verify-link.ps1
    .\verify-link.ps1 -Port COM7
#>
param(
    [string]$Port,
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Continue'

function Write-Head($text) {
    Write-Host ""
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

function Write-Ok($text)   { Write-Host "  [OK]   $text" -ForegroundColor Green }
function Write-Bad($text)  { Write-Host "  [FAIL] $text" -ForegroundColor Red }
function Write-Info($text) { Write-Host "         $text" -ForegroundColor DarkGray }

# ---------------------------------------------------------------- esptool ---
Write-Head "Locating esptool"

# Prefer the module form. ESP-IDF 5.5 exports `esptool.py` as a PowerShell
# FUNCTION rather than an executable, so it is invisible to any child process
# (cmd.exe, another shell). 'python -m esptool' is a real process and behaves
# predictably wherever it is called from.
$useModule = $false
& python -m esptool version *> $null
if ($?) {
    $useModule = $true
    Write-Ok "using 'python -m esptool'"
} else {
    $probe = Get-Command esptool.py -ErrorAction SilentlyContinue
    if ($null -ne $probe) {
        Write-Ok "using the esptool.py $($probe.CommandType.ToString().ToLower())"
    } else {
        Write-Bad "esptool not found."
        Write-Info "Activate ESP-IDF first. In this folder:"
        Write-Info ""
        Write-Info "    . .\env.ps1"
        Write-Info ""
        Write-Info "(note the leading dot-space - it must be dot-sourced)"
        Write-Info ""
        Write-Info "Plain export.ps1 may fail here: it picks whichever python is"
        Write-Info "first on PATH, which on this machine is Inkscape's 3.12, and"
        Write-Info "then looks for a virtualenv that was never created. env.ps1"
        Write-Info "puts ESP-IDF's own managed python first to avoid that."
        exit 1
    }
}

# ------------------------------------------------------------------- port ---
Write-Head "Serial ports"

$ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if ($ports.Count -eq 0) {
    Write-Bad "no serial ports present at all."
    Write-Info "Your USB-UART adapter (or bridge ESP32) is not plugged in,"
    Write-Info "or its driver is missing."
    exit 1
}
foreach ($p in $ports) { Write-Info $p }

if ([string]::IsNullOrWhiteSpace($Port)) {
    Write-Host ""
    Write-Host "Re-run with the adapter's port, e.g.:" -ForegroundColor Yellow
    Write-Host "    .\verify-link.ps1 -Port $($ports[0])" -ForegroundColor Yellow
    Write-Info ""
    Write-Info "Not sure which is the adapter? Unplug it, re-run this script,"
    Write-Info "and see which port disappeared."
    exit 0
}

if ($ports -notcontains $Port) {
    Write-Bad "$Port is not in the list above."
    exit 1
}
Write-Ok "using $Port"

# -------------------------------------------------------------- handshake ---
Write-Head "Handshake with the C6"
Write-Info "Chip should auto-enter download mode via Boot(IO9) + EN."
Write-Host ""

$esptoolArgs = @('-p', $Port, '-b', "$Baud", 'chip_id')
if ($useModule) {
    $output = & python -m esptool @esptoolArgs 2>&1
} else {
    $output = & esptool.py @esptoolArgs 2>&1
}
$joined = ($output | Out-String)
Write-Host $joined -ForegroundColor DarkGray

# ---------------------------------------------------------------- verdict ---
Write-Head "Verdict"

if ($joined -match 'ESP32-C6') {
    Write-Ok "C6 responded. Wiring is correct and download mode works."
    if ($joined -match 'MAC:\s*([0-9a-fA-F:]+)') {
        Write-Info "MAC: $($Matches[1])"
    }
    Write-Host ""
    Write-Host "  Safe to proceed:  .\flash-c6.ps1 -Port $Port" -ForegroundColor Green
    exit 0
}

if ($joined -match 'ESP32-S3|ESP32-S2|ESP32-C3|ESP8266|ESP32\b') {
    Write-Bad "something answered, but it is NOT the C6."
    Write-Info "You are almost certainly talking to your bridge/adapter board"
    Write-Info "itself, or to the P4, rather than through to the C6."
    Write-Info "Check that TX0/RX0 go to the PROG_C6 header, not elsewhere."
    exit 1
}

if ($joined -match 'Failed to connect|No serial data received|Wrong boot mode') {
    Write-Bad "no response from the C6."
    Write-Host ""
    Write-Host "  Work through these in order:" -ForegroundColor Yellow
    Write-Info "1. TX/RX CROSSOVER - the single most common mistake."
    Write-Info "     PROG_C6 TX0  ->  adapter RX"
    Write-Info "     PROG_C6 RX0  ->  adapter TX"
    Write-Info "   If in doubt, swap those two wires and re-run."
    Write-Info ""
    Write-Info "2. GND not connected. Signals need a shared reference."
    Write-Info ""
    Write-Info "3. Auto-reset lines:"
    Write-Info "     PROG_C6 EN   ->  adapter RTS"
    Write-Info "     PROG_C6 Boot ->  adapter DTR"
    Write-Info "   If your adapter has no DTR/RTS, it cannot trigger download"
    Write-Info "   mode. Hold Boot to GND, tap EN to GND, release EN, then"
    Write-Info "   release Boot, and re-run this script immediately."
    Write-Info ""
    Write-Info "4. P4 not held in bootloader mode - it can drive shared lines."
    Write-Info "   Hold the P4 BOOT button, tap RESET, release both."
    Write-Info ""
    Write-Info "5. Board unpowered. The P4 board needs its own USB power;"
    Write-Info "   do NOT feed VCC from the adapter."
    exit 1
}

if ($joined -match 'could not open port|Access is denied|PermissionError') {
    Write-Bad "cannot open $Port - something else is holding it."
    Write-Info "Close any open serial monitor, then re-run."
    exit 1
}

Write-Bad "unrecognised result - read the esptool output above."
exit 1
