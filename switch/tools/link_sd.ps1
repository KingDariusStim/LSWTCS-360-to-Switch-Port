# Populate Eden's emulated SD card (%APPDATA%\eden\sdmc\lswtcs) for the Switch build WITHOUT
# copying game data: files become hard links, folders directory junctions, pointing at the user's
# existing data (game files from their own disc). env.txt is copied from the repo (no game data).
# Usage: powershell -File switch/tools/link_sd.ps1      (re-runnable; existing entries are kept)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$run  = Join-Path $repo 'LSWTCSRuntime\build'
$data = Join-Path $repo 'Convert 360\LSWTCS'
$sd   = Join-Path $env:APPDATA 'eden\sdmc\lswtcs'
New-Item -ItemType Directory -Force $sd | Out-Null

$files = @(
    @{ src = Join-Path $run 'game.DAT' },  @{ src = Join-Path $run 'game1.DAT' },
    @{ src = Join-Path $run 'SMBPATH.TXT' },
    @{ src = Join-Path $data 'image.bin' }, @{ src = Join-Path $data 'pristine.bin' }
) + (Get-ChildItem (Join-Path $run 'perm*.dds') | ForEach-Object { @{ src = $_.FullName } })
$dirs = @( (Join-Path $run 'SHADERS'), (Join-Path $data 'Audio') )

foreach ($f in $files) {
    $dst = Join-Path $sd (Split-Path $f.src -Leaf)
    if (Test-Path $dst) { "keep  $dst"; continue }
    if (-not (Test-Path $f.src)) { "MISSING source $($f.src)"; continue }
    New-Item -ItemType HardLink -Path $dst -Target $f.src | Out-Null; "link  $dst"
}
foreach ($d in $dirs) {
    $dst = Join-Path $sd (Split-Path $d -Leaf)
    if (Test-Path $dst) { "keep  $dst"; continue }
    if (-not (Test-Path $d)) { "MISSING source $d"; continue }
    New-Item -ItemType Junction -Path $dst -Target $d | Out-Null; "junction $dst"
}
Copy-Item (Join-Path $repo 'switch\sd\lswtcs\env.txt') (Join-Path $sd 'env.txt') -Force; "copy  env.txt"
