param([string]$Addrs = "0x82F21190:32,0x83015D94:8")
$HOST_OFFSET = 0x110000000L
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
$proc = Get-Process xenia_canary -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "xenia not running"; exit 1 }
$handle = [WinMem]::OpenProcess(0x1F0FFF, $false, $proc.Id)
if ($handle -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed (need admin?)"; exit 1 }
function DumpMem([long]$g, [int]$sz) {
    $hostAddr = $g + $HOST_OFFSET
    $buf = New-Object byte[] $sz
    $read = 0
    $ok = [WinMem]::ReadProcessMemory($handle, [IntPtr]$hostAddr, $buf, $sz, [ref]$read)
    Write-Host ("[0x{0:X8}] read={1}/{2}" -f $g, $read, $sz)
    for ($i = 0; $i -lt $sz; $i += 16) {
        $end = [Math]::Min($i+15, $sz-1)
        $hex = ($buf[$i..$end] | ForEach-Object { $_.ToString('X2') }) -join ' '
        Write-Host ("  +{0:X2}  {1}" -f $i, $hex)
    }
}
foreach ($spec in $Addrs.Split(',')) {
    $parts = $spec.Split(':')
    $g = [convert]::ToInt64($parts[0].TrimStart('0','x','X'), 16)
    $sz = if ($parts.Length -gt 1) { [int]$parts[1] } else { 32 }
    DumpMem $g $sz
}
[WinMem]::CloseHandle($handle) | Out-Null
