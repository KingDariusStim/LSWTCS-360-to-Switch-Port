param([long]$Start = 0x82E70000L, [long]$End = 0x83460000L, [string]$Out = "xenia_data.bin")
# Read-only dump of Xenia guest memory [Start, End) to a raw file (host address = guest + 0x110000000, see xdump.ps1).
$HOST_OFFSET = 0x110000000L
$Start = $Start -band 0xFFFFFFFFL; $End = $End -band 0xFFFFFFFFL   # 0x8xxxxxxx literals arrive as negative Int32
$code = @"
using System;
using System.Runtime.InteropServices;
public class WinMemB {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int sz, out int read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
try { Add-Type -TypeDefinition $code -ErrorAction Stop } catch {}
$proc = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Host "xenia not running"; exit 1 }
$handle = [WinMemB]::OpenProcess(0x0410, $false, $proc.Id)   # PROCESS_VM_READ | PROCESS_QUERY_INFORMATION
if ($handle -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed"; exit 1 }
$fs = [System.IO.File]::Create($Out)
$chunk = 0x10000; $bad = 0
for ($g = $Start; $g -lt $End; $g += $chunk) {
    $sz = [int][Math]::Min($chunk, $End - $g)
    $buf = New-Object byte[] $sz
    $read = 0
    if (-not [WinMemB]::ReadProcessMemory($handle, [IntPtr]($g + $HOST_OFFSET), $buf, $sz, [ref]$read)) { $bad++ }
    $fs.Write($buf, 0, $sz)
}
$fs.Close()
[WinMemB]::CloseHandle($handle) | Out-Null
Write-Host ("dumped 0x{0:X8}..0x{1:X8} to {2}, {3} unreadable chunks" -f $Start, $End, $Out, $bad)
