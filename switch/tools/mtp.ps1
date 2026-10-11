# Copy files to/from the Switch's SD card over USB (MTP: the console shows up as "Nintendo Switch" while an MTP
# responder such as DBI runs), so the SD card can stay in the console.
#   mtp.ps1 pull <sd-dir> <local-dir> <name>...   e.g.  mtp.ps1 pull lswtcs switch/hwlogs/run stdout.log trace.log
#   mtp.ps1 push <local-file> <sd-dir>            e.g.  mtp.ps1 push switch/build/lswtcs.nro lswtcs
#   mtp.ps1 ls <sd-dir>
# <sd-dir> is relative to the SD root, '/'-separated. MTP has no overwrite-in-place: push renames an existing file of
# the same name to <name>_prev_<time><ext> first (the user deletes old copies). Never use this to delete anything on
# the SD card. Verify a push by pulling it back and comparing hashes.
param([Parameter(Mandatory)][string]$Cmd, [string]$A, [string]$B, [Parameter(ValueFromRemainingArguments)][string[]]$Names)
$ErrorActionPreference = 'Stop'
$sh = New-Object -ComObject Shell.Application
$dev = $sh.NameSpace(17).Items() | Where-Object { $_.Name -eq 'Nintendo Switch' }
if (-not $dev) { throw 'Nintendo Switch not connected over USB (start the MTP responder, e.g. DBI)' }
$sd = $dev.GetFolder.Items() | Where-Object { $_.Name -eq 'SD Card' }
if (-not $sd) { throw 'no "SD Card" storage on the device' }
function Get-Size($item) { [long]$item.ExtendedProperty('System.Size') }   # .Size reads 0 over MTP
function Get-SdFolder([string]$path) {
    $f = $sd.GetFolder
    foreach ($part in ($path -split '/' | Where-Object { $_ })) {
        $item = $f.Items() | Where-Object { $_.Name -eq $part }
        if (-not $item) { throw "SD folder not found: $path" }
        $f = $item.GetFolder
    }
    $f
}
# Shell copies are asynchronous: wait until the target exists with the source's size.
function Wait-Copy([scriptblock]$size, [long]$want, [string]$what) {
    for ($t = 0; $t -lt 1200; $t++) {
        Start-Sleep -Milliseconds 250
        $have = & $size
        if ($have -ne $null -and $have -eq $want) { return }
    }
    throw "timed out copying $what"
}
switch ($Cmd) {
    'ls' {
        (Get-SdFolder $A).Items() | ForEach-Object { '{0,12}  {1}' -f (Get-Size $_), $_.Name }
    }
    'pull' {
        $src = Get-SdFolder $A
        $dst = (New-Item -ItemType Directory -Force $B).FullName
        $dstNs = $sh.NameSpace($dst)
        foreach ($n in $Names) {
            $item = $src.Items() | Where-Object { $_.Name -eq $n }
            if (-not $item) { "missing on SD: $A/$n"; continue }
            $local = Join-Path $dst $n
            $dstNs.CopyHere($item, 4 + 16 + 1024)   # no progress UI, yes to all, no error UI
            $want = Get-Size $item
            Wait-Copy { if (Test-Path $local) { (Get-Item $local).Length } } $want $n
            "pulled $A/$n -> $local ($want bytes)"
        }
    }
    'push' {
        $file = (Get-Item $A).FullName
        $dstF = Get-SdFolder $B
        $name = Split-Path $file -Leaf
        # Over MTP, CopyHere onto an existing file is silently skipped (2026-10-09: same-size build "pushed", SD kept
        # the old one). Rename the existing file out of the way (never delete on the SD card), then copy.
        $old = $dstF.Items() | Where-Object { $_.Name -eq $name }
        if ($old) {
            $prev = [IO.Path]::GetFileNameWithoutExtension($name) + '_prev_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + [IO.Path]::GetExtension($name)
            $old.Name = $prev
            Start-Sleep -Milliseconds 500
            if ($dstF.Items() | Where-Object { $_.Name -eq $name }) { throw "could not rename the existing $B/$name out of the way" }
            "renamed existing $B/$name -> $B/$prev"
        }
        $dstF.CopyHere($file, 4 + 16 + 1024)
        $want = (Get-Item $file).Length
        Wait-Copy { $i = $dstF.Items() | Where-Object { $_.Name -eq $name }; if ($i) { Get-Size $i } } $want $name
        "pushed $file -> $B/$name ($want bytes)"
    }
    default { throw "unknown command $Cmd (pull, push, ls)" }
}
