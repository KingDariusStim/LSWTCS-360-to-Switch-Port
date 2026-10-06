param([int]$DurationSec = 60, [int]$IntervalMs = 80, [string]$OutDir = "wincaps", [int]$Threshold = 60, [int]$ProcId = 0)
# Standalone window capturer for LSWTCSRuntime's D3D12 window. Captures the
# on-screen region (works for flip-model swapchains) and saves only COLORFUL
# frames (saturated title-card colors), so it catches the intermittent glitchy
# title render without touching the game's timing.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class WCap {
  public delegate bool Cb(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Cb cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
Add-Type -AssemblyName System.Drawing

# Poll for the window so we can start the capturer EARLY (before the window
# exists) and begin grabbing the instant it appears — the title-card palette
# flashes during the early black-screen phase, so we must not miss the start.
$hwnd = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(90)
while ($hwnd -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    if ($ProcId -ne 0) {
        $script:found = [IntPtr]::Zero
        $cb = [WCap+Cb]{ param($h,$p) $pp=0; [WCap]::GetWindowThreadProcessId($h,[ref]$pp)|Out-Null;
            if ($pp -eq $script:ProcId -and [WCap]::IsWindowVisible($h)) { $script:found = $h; return $false }; return $true }
        [WCap]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
        $hwnd = $script:found
    } else {
        $hwnd = [WCap]::FindWindow("LSWTCSWnd", $null)
    }
    if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "[wincap] window not found (ProcId=$ProcId) after 90s"; exit 1 }
Write-Host "[wincap] window appeared - capturing immediately"
# Pin the game window TOPMOST so it is never occluded by the terminal/chat —
# CopyFromScreen grabs the screen region, so the game must stay on top.
$HWND_TOPMOST = [IntPtr](-1); $SWP = 0x13  # NOMOVE|NOSIZE|NOACTIVATE
[WCap]::SetWindowPos($hwnd, $HWND_TOPMOST, 0,0,0,0, $SWP) | Out-Null
[WCap]::BringWindowToTop($hwnd) | Out-Null
[WCap]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 300
$rect = New-Object WCap+RECT
[WCap]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { Write-Host "[wincap] bad window rect"; exit 1 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Write-Host "[wincap] hwnd=$hwnd rect=($($rect.Left),$($rect.Top)) ${w}x${h}  capturing ${DurationSec}s..."

$end = (Get-Date).AddSeconds($DurationSec)
$frames = 0; $saved = 0; $maxColor = 0
while ((Get-Date) -lt $end) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try { $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($w, $h))) } catch {}
    $g.Dispose()
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0,0,$w,$h), [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $len = $data.Stride * $h
    $bytes = New-Object byte[] $len
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $len)
    $bmp.UnlockBits($data)
    $colorful = 0
    for ($y = 0; $y -lt $h; $y += 32) {
        $row = $y * $data.Stride
        for ($x = 0; $x -lt $w; $x += 32) {
            $i = $row + $x * 4
            $b = $bytes[$i]; $gr = $bytes[$i+1]; $rr = $bytes[$i+2]
            $mx = [Math]::Max($b, [Math]::Max($gr, $rr)); $mn = [Math]::Min($b, [Math]::Min($gr, $rr))
            if (($mx - $mn) -gt 45 -and $mx -gt 55) { $colorful++ }
        }
    }
    if ($colorful -gt $maxColor) { $maxColor = $colorful }
    if ($colorful -gt $Threshold) {
        $name = "{0}\cap_{1:D3}_c{2}.png" -f $OutDir, $saved, $colorful
        $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Host "[wincap] SAVED $name (colorful=$colorful)"
        $saved++
    }
    $bmp.Dispose()
    $frames++
    Start-Sleep -Milliseconds $IntervalMs
}
Write-Host "[wincap] done: $frames frames scanned, $saved colorful saved, peak colorful=$maxColor"
