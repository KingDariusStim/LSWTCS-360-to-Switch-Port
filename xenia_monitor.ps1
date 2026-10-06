# xenia_monitor.ps1
# LSWTCS Xenia memory monitor for Xbox 360 port debugging
#
# Polls key guest memory every N ms and prints heap/array state.
# Saves binary snapshots to $DumpDir whenever state changes.
#
# Usage:
#   .\xenia_monitor.ps1                          # attach to running Xenia
#   .\xenia_monitor.ps1 -Launch                  # start Xenia first
#   .\xenia_monitor.ps1 -IntervalMs 16           # ~60fps polling
#   .\xenia_monitor.ps1 -DumpAll                 # dump every tick, not just on change
#   .\xenia_monitor.ps1 -WatchAddr 0x840001D8    # override buffer address to watch

param(
    [int]    $IntervalMs  = 33,
    [string] $DumpDir     = "$PSScriptRoot\..\xenia_dumps",
    [switch] $Launch,
    [switch] $DumpAll,
    [string] $XeniaPath   = "$PSScriptRoot\..\xenia\xenia_canary.exe",
    [string] $XexPath     = "$PSScriptRoot\Convert 360\LSWTCS\Default.xex",
    [string] $WatchAddr   = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Win32 P/Invoke ─────────────────────────────────────────────────────────────
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class WinMem {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr hProc, IntPtr addr, byte[] buf, int size, out int read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
"@

# ── Address translation ────────────────────────────────────────────────────────
# Derived from: Xenia maps guest 0x82000000 → host 0x192000000
# So: host = (uint64)guest + 0x110000000
$GUEST_OFFSET = [int64]0x110000000

function ToHost([uint32]$g) { [IntPtr]([int64]$g + $GUEST_OFFSET) }

# ── Memory read helpers ────────────────────────────────────────────────────────
function Read-Guest([IntPtr]$h, [uint32]$addr, [int]$size) {
    $buf = New-Object byte[] $size
    $n   = 0
    $ok  = [WinMem]::ReadProcessMemory($h, (ToHost $addr), $buf, $size, [ref]$n)
    if (-not $ok -or $n -ne $size) { return $null }
    return $buf
}

# Xbox 360 is big-endian — all multi-byte reads need byte reversal
function U8  ([byte[]]$b, [int]$i) { [uint32]$b[$i] }
function U16 ([byte[]]$b, [int]$i) { (([uint16]$b[$i]) -shl 8) -bor ([uint16]$b[$i+1]) }
function U32 ([byte[]]$b, [int]$i) {
    ([uint32]$b[$i] -shl 24) -bor ([uint32]$b[$i+1] -shl 16) -bor ([uint32]$b[$i+2] -shl 8) -bor [uint32]$b[$i+3]
}
function U64 ([byte[]]$b, [int]$i) {
    ([uint64](U32 $b $i) -shl 32) -bor [uint64](U32 $b ($i+4))
}

function RotL32([uint32]$v, [int]$n) { ($v -shl $n) -bor ($v -shr (32 - $n)) }

# ── Block header parser ────────────────────────────────────────────────────────
# Reads 32 bytes before and 8 bytes after a user pointer.
# Returns a PSCustomObject with parsed fields + both capacity formulas.
function Parse-BlockHdr([IntPtr]$h, [uint32]$userPtr) {
    # Read 40 bytes: 32 before user pointer + 8 after
    $buf = Read-Guest $h ($userPtr - 32) 40
    if ($null -eq $buf) { return $null }
    # Offsets within $buf (userPtr is at index 32):
    #   [32-24]=8  → related  (uint32, [-24])
    #   [32-16]=16 → sizeField (uint16, [-16])
    #   [32-11]=21 → flags    (uint8,  [-11])
    #   [32-10]=22 → adj      (uint8,  [-10])
    $related   = U32 $buf 8
    $sizeField = U16 $buf 16
    $flags     = U8  $buf 21
    $adj       = U8  $buf 22

    $inUse     = ($flags -band 1) -ne 0

    # Capacity formula A (normal path): rotlwi(sizeField_as_u32, 4) - adj
    $capA = (RotL32 ([uint32]$sizeField) 4) - $adj

    # Capacity formula B (bit-28 path, where related stores block end):
    # [ptr-24] - [ptr-16] - 48  (all as uint32)
    $capB = $related - [uint32]$sizeField - 48

    return [PSCustomObject]@{
        UserPtr   = $userPtr
        Flags     = $flags
        InUse     = $inUse
        SizeField = $sizeField
        Adj       = $adj
        Related   = $related
        CapA      = $capA    # normal path
        CapB      = $capB    # bit-28 path
    }
}

# ── Snapshot writer ────────────────────────────────────────────────────────────
$script:snapIdx = 0

function Save-Snapshot([IntPtr]$h, [uint32]$bufStart, [uint32]$bufEnd, [string]$reason) {
    $script:snapIdx++
    $ts   = Get-Date -Format "HHmmss_fff"
    $file = Join-Path $DumpDir ("snap_{0:D5}_{1}_{2}.bin" -f $script:snapIdx, $ts, ($reason -replace '\s+','_'))

    $ms = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($ms)

    # Write a simple header so the file is self-describing
    $writer.Write([byte[]][System.Text.Encoding]::ASCII.GetBytes("XSNAP001"))
    $writer.Write([uint32]$bufStart)
    $writer.Write([uint32]$bufEnd)
    $writer.Write([int64](Get-Date).Ticks)

    $regions = @(
        [pscustomobject]@{ Addr=[uint32]0x82D04900; Size=0x20;   Tag="globals"    },
        [pscustomobject]@{ Addr=[uint32]0x84000000; Size=0x100;  Tag="heap_obj"   },
        [pscustomobject]@{ Addr=($bufStart - 64);   Size=([int]($bufEnd - $bufStart) + 128); Tag="buf_region" },
        [pscustomobject]@{ Addr=[uint32]0x835FF800; Size=0x800;  Tag="stack_top"  }
    )

    foreach ($r in $regions) {
        $d = Read-Guest $h $r.Addr $r.Size
        if ($null -ne $d) {
            $writer.Write([byte[]][System.Text.Encoding]::ASCII.GetBytes($r.Tag.PadRight(16)))
            $writer.Write([uint32]$r.Addr)
            $writer.Write([uint32]$r.Size)
            $writer.Write($d)
        }
    }

    $writer.Flush()
    [System.IO.File]::WriteAllBytes($file, $ms.ToArray())
    Write-Host "  SNAP → $file ($reason)" -ForegroundColor Cyan
}

# ── Launch Xenia ───────────────────────────────────────────────────────────────
if ($Launch) {
    if (-not (Test-Path $XeniaPath)) { Write-Error "Xenia not found at: $XeniaPath"; exit 1 }
    if (-not (Test-Path $XexPath))   { Write-Error "XEX not found at: $XexPath";   exit 1 }
    Write-Host "[*] Starting Xenia..." -ForegroundColor Cyan
    Start-Process -FilePath $XeniaPath -ArgumentList "`"$XexPath`""
    Write-Host "[*] Waiting 10s for Xenia to initialize..." -ForegroundColor Cyan
    Start-Sleep -Seconds 10
}

# ── Attach ─────────────────────────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null

$proc   = $null
$handle = [IntPtr]::Zero

Write-Host "[*] Waiting for xenia_canary process..." -ForegroundColor Yellow
while ($true) {
    $p = Get-Process xenia_canary -ErrorAction SilentlyContinue
    if ($p) { $proc = $p[0]; break }
    Start-Sleep -Milliseconds 500
}

$handle = [WinMem]::OpenProcess(0x1F0FFF, $false, $proc.Id)
if ($handle -eq [IntPtr]::Zero) { Write-Error "OpenProcess failed for PID $($proc.Id)"; exit 1 }

Write-Host "[+] Attached to xenia_canary  PID=$($proc.Id)" -ForegroundColor Green
Write-Host "[*] Poll interval: ${IntervalMs}ms  DumpDir: $DumpDir" -ForegroundColor Cyan
Write-Host "[*] Ctrl+C to stop" -ForegroundColor Cyan
Write-Host ""
Write-Host ("  {0,-12} {1,-10} {2,-10} {3,-10} {4,-10} {5,-10} {6}" -f "Time","bufStart","bufEnd","Used","CapA","CapB","InUse") -ForegroundColor White
Write-Host ("  " + ("-" * 80)) -ForegroundColor DarkGray

# ── Override watch address ─────────────────────────────────────────────────────
$overrideAddr = [uint32]0
if ($WatchAddr -ne "") {
    $overrideAddr = [uint32][Convert]::ToUInt32($WatchAddr.TrimStart("0x"), 16)
    Write-Host "[*] Overriding buffer watch address to 0x{0:X8}" -f $overrideAddr -ForegroundColor Yellow
}

# ── State tracking ─────────────────────────────────────────────────────────────
$prevBufStart = [uint32]0
$prevBufEnd   = [uint32]0
$prevCapA     = [uint32]0
$prevInUse    = $true
$tick         = 0

# ── Log file ───────────────────────────────────────────────────────────────────
$logFile = Join-Path $DumpDir ("monitor_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))
"tick,time_ms,bufStart,bufEnd,used,capA,capB,inUse,flags,sizeField,adj,related" | Out-File -FilePath $logFile -Encoding utf8

# ── Main polling loop ──────────────────────────────────────────────────────────
try {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    while ($true) {
        $tick++
        $elapsed = $sw.ElapsedMilliseconds

        # Read globals: 0x82D04910 = buf start ptr, 0x82D04914 = buf current end ptr
        $globBuf = Read-Guest $handle ([uint32]0x82D04900) 0x20
        if ($null -eq $globBuf) {
            Write-Host "  [tick $tick] Cannot read guest memory — game may not be initialized yet" -ForegroundColor DarkYellow
            Start-Sleep -Milliseconds $IntervalMs
            continue
        }

        $bufStart = U32 $globBuf 0x10    # offset 0x10 into read = address 0x82D04910
        $bufEnd   = U32 $globBuf 0x14    # offset 0x14 = address 0x82D04914

        # Allow override
        if ($overrideAddr -ne 0 -and $bufStart -eq 0) { $bufStart = $overrideAddr }

        if ($bufStart -eq 0) {
            Write-Host "  [tick $tick] Globals zero — not yet initialized" -ForegroundColor DarkGray
            Start-Sleep -Milliseconds $IntervalMs
            continue
        }

        # Parse block header at bufStart
        $hdr = Parse-BlockHdr $handle $bufStart
        $capA   = if ($null -ne $hdr) { $hdr.CapA }   else { [uint32]0 }
        $capB   = if ($null -ne $hdr) { $hdr.CapB }   else { [uint32]0 }
        $inUse  = if ($null -ne $hdr) { $hdr.InUse }  else { $true }
        $flags  = if ($null -ne $hdr) { $hdr.Flags }  else { [byte]0 }
        $sfld   = if ($null -ne $hdr) { $hdr.SizeField } else { [uint16]0 }
        $adj    = if ($null -ne $hdr) { $hdr.Adj }    else { [byte]0 }
        $rel    = if ($null -ne $hdr) { $hdr.Related } else { [uint32]0 }
        $used   = $bufEnd - $bufStart

        # Detect changes
        $changed = ($bufStart -ne $prevBufStart) -or
                   ($bufEnd   -ne $prevBufEnd)   -or
                   ($capA     -ne $prevCapA)      -or
                   ($inUse    -ne $prevInUse)

        # Color coding
        $color = if ($changed) { "Yellow" } else { "DarkGray" }
        if (-not $inUse) { $color = "Red" }

        $ts = Get-Date -Format "HH:mm:ss.fff"
        $line = "  {0,-12} 0x{1:X8} 0x{2:X8} 0x{3:X6} 0x{4:X6} 0x{5:X6} {6}" -f `
            $ts, $bufStart, $bufEnd, $used, $capA, $capB, $(if ($inUse) { "yes" } else { "FREED!" })

        Write-Host $line -ForegroundColor $color

        # Append to log CSV
        "{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11}" -f `
            $tick, $elapsed, ("0x{0:X8}"-f$bufStart), ("0x{0:X8}"-f$bufEnd), `
            $used, $capA, $capB, $inUse, $flags, $sfld, $adj, ("0x{0:X8}"-f$rel) |
            Out-File -FilePath $logFile -Encoding utf8 -Append

        if ($changed -or $DumpAll) {
            $reason = if ($changed) { "change" } else { "tick_$tick" }
            if (-not $inUse)               { $reason = "FREED" }
            elseif ($bufStart -ne $prevBufStart) { $reason = "realloc_0x{0:X8}" -f $bufStart }
            elseif ($capA -ne $prevCapA)   { $reason = "cap_grew_0x{0:X}" -f $capA }
            elseif ($changed)              { $reason = "ptr_moved" }
            Save-Snapshot $handle $bufStart $bufEnd $reason
        }

        $prevBufStart = $bufStart
        $prevBufEnd   = $bufEnd
        $prevCapA     = $capA
        $prevInUse    = $inUse

        # Adaptive sleep: subtract time spent this iteration
        $spent = $sw.ElapsedMilliseconds - $elapsed
        $wait  = [Math]::Max(0, $IntervalMs - [int]$spent)
        if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
    }
}
finally {
    [WinMem]::CloseHandle($handle) | Out-Null
    Write-Host "`n[*] Detached. Log: $logFile" -ForegroundColor Cyan
}
