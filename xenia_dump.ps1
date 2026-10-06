# xenia_dump.ps1 — on-demand guest memory dump from a running Xenia process
#
# Usage:
#   .\xenia_dump.ps1 [-Launch] [-WaitSec 8]
#   .\xenia_dump.ps1 -GuestAddr 0x840001D8 -Size 64 -Label "buf header"
#   .\xenia_dump.ps1 -Preset crash     # stack save area + heap block header
#   .\xenia_dump.ps1 -Preset heap      # heap object first 0x100 bytes
#   .\xenia_dump.ps1 -Preset block     # 64 bytes around dynamic array block header
#   .\xenia_dump.ps1 -Preset globals   # array start/end pointers
#   .\xenia_dump.ps1 -Preset all       # all of the above
#
# Run as Administrator for ReadProcessMemory access.

param(
    [switch]$Launch,
    [int]$WaitSec = 8,
    [string]$GuestAddr = "",
    [int]$Size = 64,
    [string]$Label = "",
    [string]$Preset = "",
    [switch]$ParseBlock   # parse heap block header fields at GuestAddr
)

$XENIA_EXE   = "$PSScriptRoot\..\xenia\xenia_canary.exe"
$GAME_XEX    = "$PSScriptRoot\Convert 360\LSWTCS\Default.xex"
$DUMP_DIR    = "$PSScriptRoot\..\xenia_dumps"
$HOST_OFFSET = 0x110000000L   # guest_addr + HOST_OFFSET = host_addr

# ── Interop ────────────────────────────────────────────────────────────────────
$code = @"
using System;
using System.Runtime.InteropServices;
public class WinMem {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int sz, out int read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
try { Add-Type -TypeDefinition $code -ErrorAction Stop } catch {}

# ── Optional launch ────────────────────────────────────────────────────────────
if ($Launch) {
    Write-Host "[xenia_dump] Launching Xenia..."
    Start-Process -FilePath $XENIA_EXE -ArgumentList "`"$GAME_XEX`""
    Write-Host "[xenia_dump] Waiting ${WaitSec}s for Xenia to start..."
    Start-Sleep -Seconds $WaitSec
}

# ── Find process ───────────────────────────────────────────────────────────────
$proc = Get-Process xenia_canary -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Host "[xenia_dump] ERROR: xenia_canary not running. Use -Launch or start it first."
    exit 1
}
Write-Host "[xenia_dump] Found xenia_canary PID=$($proc.Id)"

$handle = [WinMem]::OpenProcess(0x1F0FFF, $false, $proc.Id)
if ($handle -eq [IntPtr]::Zero) {
    Write-Host "[xenia_dump] ERROR: OpenProcess failed — try running as Administrator."
    exit 1
}

# ── Core read helper ──────────────────────────────────────────────────────────
function Read-GuestMem([long]$guestAddr, [int]$sz) {
    $hostAddr = $guestAddr + $HOST_OFFSET
    $buf = New-Object byte[] $sz
    $read = 0
    $ok = [WinMem]::ReadProcessMemory($handle, [IntPtr]$hostAddr, $buf, $sz, [ref]$read)
    if (-not $ok -or $read -lt $sz) {
        Write-Host "[xenia_dump] WARNING: read at 0x$($guestAddr.ToString('X')) returned $read/$sz bytes"
    }
    return $buf
}

function Swap16([byte[]]$b, [int]$off) {
    return ([int]$b[$off] -shl 8) -bor $b[$off+1]
}
function Swap32([byte[]]$b, [int]$off) {
    return ([long]$b[$off] -shl 24) -bor ([long]$b[$off+1] -shl 16) -bor ([long]$b[$off+2] -shl 8) -bor $b[$off+3]
}
function Swap64([byte[]]$b, [int]$off) {
    return (([long](Swap32 $b $off) -shl 32) -bor ([long](Swap32 $b ($off+4)) -band 0xFFFFFFFFL))
}

function Format-HexDump([long]$baseGuest, [byte[]]$data, [string]$lbl) {
    Write-Host ""
    Write-Host "=== $lbl (guest 0x$($baseGuest.ToString('X8')), $($data.Length) bytes) ==="
    for ($i = 0; $i -lt $data.Length; $i += 16) {
        $addr = "0x" + ($baseGuest + $i).ToString('X8')
        $hex  = ($data[$i..([Math]::Min($i+15,$data.Length-1))] | ForEach-Object { $_.ToString('X2') }) -join ' '
        $asc  = ($data[$i..([Math]::Min($i+15,$data.Length-1))] | ForEach-Object { if ($_ -ge 0x20 -and $_ -le 0x7E) { [char]$_ } else { '.' } }) -join ''
        Write-Host "  $addr  $($hex.PadRight(47))  $asc"
    }
}

function Parse-BlockHeader([long]$userPtr, [byte[]]$data, [int]$dataOffset) {
    # data[dataOffset] corresponds to guest userPtr
    # Block header fields are at negative offsets from userPtr
    # Offsets relative to userPtr (all big-endian on PPC):
    #   [-24]: related ptr (4 bytes)
    #   [-16]: size/class field (2 bytes, little note: stored BE)
    #   [-11]: flags byte (bit0 = in-use)
    #   [-10]: adjustment byte
    # Capacity formula (non-bit28 path): rotlwi([-16], 4) - [-10]
    # Capacity formula (bit28 path):     [ptr-24] - [ptr-16] - 48
    $base = $dataOffset  # index into $data where userPtr starts
    if ($base -lt 24) { Write-Host "  [ParseBlock] not enough leading bytes to parse header"; return }

    $rel_m24 = Swap32 $data ($base - 24)
    $rel_m16 = Swap16 $data ($base - 16)
    $rel_m11 = $data[$base - 11]
    $rel_m10 = $data[$base - 10]

    $inuse   = $rel_m11 -band 1
    $bit28   = ($rel_m11 -shr 4) -band 1

    if ($bit28) {
        $cap = $rel_m24 - $rel_m16 - 48
    } else {
        # rotlwi(x, 4) = (x << 4) | (x >> 28), treating as 32-bit
        $rotated = (([long]$rel_m16 -shl 4) -bor ([long]$rel_m16 -shr 28)) -band 0xFFFFFFFFL
        $cap = $rotated - $rel_m10
    }

    Write-Host ""
    Write-Host "  [Block Header @ userPtr=0x$($userPtr.ToString('X8'))]"
    Write-Host "    [-24] related  = 0x$($rel_m24.ToString('X8'))"
    Write-Host "    [-16] size cls = 0x$($rel_m16.ToString('X4'))"
    Write-Host "    [-11] flags    = 0x$($rel_m11.ToString('X2'))  (in-use=$inuse, bit28=$bit28)"
    Write-Host "    [-10] adjust   = 0x$($rel_m10.ToString('X2'))"
    Write-Host "    => capacity    = 0x$($cap.ToString('X'))  ($cap bytes)"
}

# ── Save helper ────────────────────────────────────────────────────────────────
function Save-Dump([long]$guestAddr, [byte[]]$data, [string]$lbl) {
    if (-not (Test-Path $DUMP_DIR)) { New-Item -ItemType Directory -Path $DUMP_DIR | Out-Null }
    $ts   = Get-Date -Format "yyyyMMdd_HHmmss"
    $name = "$ts`_$($lbl -replace '[^A-Za-z0-9_]','_').bin"
    [System.IO.File]::WriteAllBytes("$DUMP_DIR\$name", $data)
    Write-Host "  [saved] $DUMP_DIR\$name"
}

# ── Presets ────────────────────────────────────────────────────────────────────
function Dump-Preset([string]$p) {
    switch ($p.ToLower()) {

        "globals" {
            # 0x82D04910 = buf_start ptr, 0x82D04914 = write_pos ptr
            $data = Read-GuestMem 0x82D04910L 8
            Format-HexDump 0x82D04910L $data "globals [0x82D04910..17]"
            $start = Swap32 $data 0
            $pos   = Swap32 $data 4
            Write-Host "  buf_start = 0x$($start.ToString('X8'))  write_pos = 0x$($pos.ToString('X8'))  used = 0x$(($pos - $start).ToString('X')) bytes"
        }

        "block" {
            # Block header: read 32 bytes before + 32 bytes after 0x840001D8
            $base  = 0x840001D8L - 32
            $data  = Read-GuestMem $base 64
            Format-HexDump $base $data "buf block header [-32..+32] around 0x840001D8"
            Parse-BlockHeader 0x840001D8L $data 32
        }

        "heap" {
            $data = Read-GuestMem 0x84000000L 0x100
            Format-HexDump 0x84000000L $data "heap object [0x84000000, 256 bytes]"
        }

        "stack" {
            # Stack save area: [r1_old - 112 .. r1_old] = [0x835FFB20..0x835FFB90]
            $data = Read-GuestMem 0x835FFB20L 0x70
            Format-HexDump 0x835FFB20L $data "stack save area [0x835FFB20..0x835FFB8F]"
            Write-Host ""
            Write-Host "  Expected saved regs at offsets from r1_old=0x835FFB90:"
            $labels = @{-112="r19"; -104="r20"; -96="r21"; -88="r22"; -80="r23"; -72="r24"; -64="r25"; -56="r26"; -48="r27"; -40="r28"; -32="r29"; -24="r30"; -16="r31"; -8="LR(r12)"}
            foreach ($off in ($labels.Keys | Sort-Object)) {
                $idx = ($off + 112)  # offset into $data (which starts at r1-112)
                if ($off -eq -8) {
                    $val = Swap32 $data $idx
                    Write-Host ("  [r1{0:+0;-0}] {1,-4} = 0x{2:X8}" -f $off, $labels[$off], $val)
                } else {
                    $val = Swap64 $data $idx
                    Write-Host ("  [r1{0:+0;-0}] {1,-4} = 0x{2:X16}" -f $off, $labels[$off], $val)
                }
            }
        }

        "crash" {
            Dump-Preset "globals"
            Dump-Preset "block"
            Dump-Preset "stack"
        }

        "all" {
            Dump-Preset "globals"
            Dump-Preset "heap"
            Dump-Preset "block"
            Dump-Preset "stack"
        }

        default {
            Write-Host "[xenia_dump] Unknown preset '$p'. Use: globals, block, heap, stack, crash, all"
        }
    }
}

# ── Dispatch ───────────────────────────────────────────────────────────────────
if ($Preset) {
    Dump-Preset $Preset
} elseif ($GuestAddr) {
    $addr = [long]([convert]::ToInt64($GuestAddr.TrimStart('0x'), 16))
    $data = Read-GuestMem $addr $Size
    $lbl  = if ($Label) { $Label } else { "0x$($addr.ToString('X8'))" }
    Format-HexDump $addr $data $lbl
    if ($ParseBlock) { Parse-BlockHeader $addr $data 0 }
    Save-Dump $addr $data $lbl
} else {
    Write-Host "[xenia_dump] No action specified. Use -Preset, -GuestAddr, or -Launch."
    Write-Host "  Presets: globals, block, heap, stack, crash, all"
}

[WinMem]::CloseHandle($handle) | Out-Null
Write-Host ""
Write-Host "[xenia_dump] Done."
