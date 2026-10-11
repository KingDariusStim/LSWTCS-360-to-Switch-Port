param([string]$Out = "snap")
# Read-only snapshot of Xenia guest memory for a before/after diff: the game heap (guest 0xE0000000-0xFFFFFFFF, where
# Xenia places the game's physical allocations) as <Out>_heap.bin (512 MB, unreadable / uncommitted parts left zero)
# and the game's globals 0x82E70000-0x83460000 as <Out>_data.bin. Host address = guest + 0x110000000 (xdump.ps1).
$HOST_OFFSET = 0x110000000L
$code = @"
using System;
using System.Runtime.InteropServices;
public class WinMemS {
    [StructLayout(LayoutKind.Sequential)] public struct MBI {
        public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect; public uint pad0;
        public IntPtr RegionSize; public uint State; public uint Protect; public uint Type; public uint pad1; }
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int sz, out int read);
    [DllImport("kernel32.dll")] public static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI info, IntPtr len);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
try { Add-Type -TypeDefinition $code -ErrorAction Stop } catch {}
$proc = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Host "xenia not running"; exit 1 }
$h = [WinMemS]::OpenProcess(0x0410, $false, $proc.Id)
if ($h -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed"; exit 1 }
function Dump([long]$gStart, [long]$gEnd, [string]$file) {
    $fs = [System.IO.File]::Create($file); $fs.SetLength($gEnd - $gStart)
    $g = $gStart; $copied = 0L
    while ($g -lt $gEnd) {
        $mbi = New-Object WinMemS+MBI
        if ([WinMemS]::VirtualQueryEx($h, [IntPtr]($g + $HOST_OFFSET), [ref]$mbi, [IntPtr]48) -eq [IntPtr]::Zero) { break }
        $rEnd = [Math]::Min($mbi.BaseAddress.ToInt64() + $mbi.RegionSize.ToInt64() - $HOST_OFFSET, $gEnd)
        if ($mbi.State -eq 0x1000) {   # MEM_COMMIT
            for ($a = $g; $a -lt $rEnd; $a += 0x100000) {
                $sz = [int][Math]::Min(0x100000, $rEnd - $a)
                $buf = New-Object byte[] $sz; $read = 0
                if ([WinMemS]::ReadProcessMemory($h, [IntPtr]($a + $HOST_OFFSET), $buf, $sz, [ref]$read)) {
                    $fs.Position = $a - $gStart; $fs.Write($buf, 0, $sz); $copied += $sz }
            }
        }
        $g = $rEnd
    }
    $fs.Close()
    Write-Host ("{0}: 0x{1:X8}..0x{2:X8}, {3} MB committed copied" -f $file, $gStart, $gEnd, [int]($copied / 1MB))
}
Dump 0xE0000000L 0x100000000L "$($Out)_heap.bin"
Dump 0x82E70000L 0x83460000L "$($Out)_data.bin"
[WinMemS]::CloseHandle($h) | Out-Null
