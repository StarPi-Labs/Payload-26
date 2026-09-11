<#
.SYNOPSIS
    Dot-source this to get a working ESP-IDF environment in a plain PowerShell.

.DESCRIPTION
    ESP-IDF's own export.ps1 picks whichever `python` is first on PATH, then
    looks for a virtualenv named after THAT python's version. If the first
    python on PATH is not the one ESP-IDF was installed with, it hunts for a
    venv that was never created and fails with:

        ERROR: ESP-IDF Python virtual environment
        "...\python_env\idfX.Y_pyA.B_env\Scripts\python.exe" not found.

    On this machine that happens because `python` resolves to Inkscape's bundled
    Python 3.12, while the ESP-IDF venv was built with ESP-IDF's own managed
    Python 3.11.

    This script finds the managed python whose version actually matches an
    existing venv, puts it first on PATH, and only then dot-sources export.ps1.

    USE:  . .\env.ps1

    Note the leading dot-space. Without it the environment is set inside a child
    scope and vanishes the moment the script ends.

.PARAMETER IdfPath
    ESP-IDF install to activate. Defaults to the v5.5.1 tree, which is the one
    with a working virtualenv on this machine. v5.5 is >= 5.4 and therefore
    fine for both esp32c6 and esp32s3 targets.

.PARAMETER ToolsPath
    ESP-IDF tools directory. Defaults to ~/.espressif.
#>
param(
    [string]$IdfPath   = "C:\Users\Samuele\esp\v5.5.1\esp-idf",
    [string]$ToolsPath = "$env:USERPROFILE\.espressif"
)

function Write-Step($t) { Write-Host $t -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host "  [OK]   $t" -ForegroundColor Green }
function Write-Bad($t)  { Write-Host "  [FAIL] $t" -ForegroundColor Red }
function Write-Note($t) { Write-Host "         $t" -ForegroundColor DarkGray }

Write-Step "Locating a usable ESP-IDF python"

$envRoot = Join-Path $ToolsPath "python_env"
if (-not (Test-Path $envRoot)) {
    Write-Bad "no python_env at $envRoot - ESP-IDF tools were never installed."
    Write-Note "Run: $IdfPath\install.ps1"
    return
}

# Existing venvs are named like 'idf5.5_py3.11_env'. The pyX.Y in that name is
# the only python version that will satisfy export.ps1 without a reinstall.
$wanted = @()
Get-ChildItem $envRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    if (-not (Test-Path (Join-Path $_.FullName "Scripts\python.exe"))) { return }
    # Several IDF versions can share one python version (idf5.5_py3.11_env and
    # idf6.0_py3.11_env both want 3.11) - we only care about distinct versions.
    if ($_.Name -match 'py(\d+\.\d+)_env' -and $wanted -notcontains $Matches[1]) {
        $wanted += $Matches[1]
    }
}

if ($wanted.Count -eq 0) {
    Write-Bad "no complete virtualenv found under $envRoot"
    Write-Note "Run: $IdfPath\install.ps1"
    return
}
Write-Note ("virtualenvs present for python: " + ($wanted -join ", "))

$pyDir = $null
foreach ($v in $wanted) {
    # Managed pythons live in tools\idf-python\<full version>\python.exe
    $cand = Get-ChildItem (Join-Path $ToolsPath "tools\idf-python") -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "$v.*" -and (Test-Path (Join-Path $_.FullName "python.exe")) } |
            Sort-Object Name -Descending |
            Select-Object -First 1
    if ($null -ne $cand) { $pyDir = $cand.FullName; break }
}

if ($null -eq $pyDir) {
    Write-Bad "no managed python matching those venvs under $ToolsPath\tools\idf-python"
    Write-Note "Run: $IdfPath\install.ps1"
    return
}
Write-Ok "using $pyDir\python.exe"

# Prepend so export.ps1 sees this one rather than Inkscape's / the Store's.
$env:PATH = "$pyDir;" + $env:PATH

Write-Step "Activating ESP-IDF"
$exportScript = Join-Path $IdfPath "export.ps1"
if (-not (Test-Path $exportScript)) {
    Write-Bad "export.ps1 not found at $exportScript"
    return
}
. $exportScript

Write-Host ""
if ([string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
    Write-Bad "IDF_PATH still empty - activation did not take."

    # A very common half-installed state: install.ps1 creates the virtualenv
    # directory early but populates it last, so a venv that exists but has no
    # 'rich' package means the install never finished (or is still running).
    $verMatch = [regex]::Match($IdfPath, 'v(\d+\.\d+)')
    if ($verMatch.Success) {
        $venv = Join-Path $envRoot ("idf{0}_py*_env" -f $verMatch.Groups[1].Value)
        $hit  = Get-ChildItem $venv -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $hit) {
            $hasRich = Test-Path (Join-Path $hit.FullName "Lib\site-packages\rich")
            if (-not $hasRich) {
                Write-Note ""
                Write-Note "The virtualenv $($hit.Name) exists but its packages are missing"
                Write-Note "(no 'rich' module). install.ps1 has not finished populating it."
                Write-Note ""
                Write-Note "If an install is running right now, just wait for it."
                Write-Note "Otherwise run:  $IdfPath\install.ps1"
            }
        }
    }
    return
}

Write-Ok "IDF_PATH = $env:IDF_PATH"
& python -m esptool version *> $null
if ($?) {
    Write-Ok "esptool reachable via 'python -m esptool'"
    Write-Host ""
    Write-Host "Ready. Next:  .\verify-link.ps1 -Port COM13" -ForegroundColor Green
} else {
    Write-Bad "esptool still not reachable."
}
