param(
    [string]$Start = "0x82000000",
    [long]$Length = 0x14000000,
    [string]$Out = "xenia_guest.bin"
)
$HOST_OFFSET = 0x110000000L
$code = @"
using System;
using System.Runtime.InteropServices;
public class WinMem2 {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int sz, out int read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
try { Add-Type -TypeDefinition $code -ErrorAction Stop } catch {}
$proc = Get-Process xenia_canary -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "xenia not running"; exit 1 }
$handle = [WinMem2]::OpenProcess(0x1F0FFF, $false, $proc.Id)
if ($handle -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed"; exit 1 }
$g0 = [convert]::ToInt64($Start.Substring(2), 16)
$fs = [System.IO.File]::Create($Out)
$chunk = 1MB
$buf = New-Object byte[] $chunk
$zero = New-Object byte[] $chunk
$okBytes = 0
for ($off = 0L; $off -lt $Length; $off += $chunk) {
    $sz = [int][Math]::Min($chunk, $Length - $off)
    $read = 0
    $ok = [WinMem2]::ReadProcessMemory($handle, [IntPtr]($g0 + $HOST_OFFSET + $off), $buf, $sz, [ref]$read)
    if ($ok -and $read -gt 0) { $fs.Write($buf, 0, $read); if ($read -lt $sz) { $fs.Write($zero, 0, $sz - $read) }; $okBytes += $read }
    else { $fs.Write($zero, 0, $sz) }
}
$fs.Close()
[WinMem2]::CloseHandle($handle) | Out-Null
Write-Host ("dumped 0x{0:X} bytes from 0x{1:X8} ({2} MB readable) -> {3}" -f $Length, $g0, [int]($okBytes/1MB), $Out)
