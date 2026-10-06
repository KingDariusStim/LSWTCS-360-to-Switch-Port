param([int]$Frames = 20, [int]$IntervalMs = 100)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class G {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int l,t,r,b; }
  public struct POINT { public int x,y; }
}
"@
[G]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$p = Get-Process LSWTCSRuntime -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -First 1
if (-not $p) { Write-Output "NO PROC"; exit }
$h = $p.MainWindowHandle
# Move the window fully on-screen and topmost so CopyFromScreen grabs it.
[G]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x41) | Out-Null   # TOPMOST, NOSIZE|NOMOVE? -> use 0x41=NOSIZE? we want move
[G]::SetWindowPos($h, [IntPtr](-1), 0, 0, 1296, 759, 0x40) | Out-Null  # SWP_SHOWWINDOW, move to 0,0
[G]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$cr = New-Object G+RECT; [G]::GetClientRect($h, [ref]$cr) | Out-Null
$o = New-Object G+POINT; $o.x=0; $o.y=0; [G]::ClientToScreen($h, [ref]$o) | Out-Null
$w = $cr.r - $cr.l; $ht = $cr.b - $cr.t
Write-Output "client ${w}x${ht} at screen ($($o.x),$($o.y))"
$best = -1; $bestname = ""
for ($i=0; $i -lt $Frames; $i++) {
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  try { $g.CopyFromScreen($o.x, $o.y, 0, 0, (New-Object System.Drawing.Size($w,$ht))) } catch {}
  $g.Dispose()
  # crude colorfulness score (sample center area)
  $score = 0
  for ($y=50; $y -lt $ht-50; $y+=40) { for ($x=50; $x -lt $w-50; $x+=40) {
    $px = $bmp.GetPixel($x,$y); $mx=[Math]::Max($px.R,[Math]::Max($px.G,$px.B)); $mn=[Math]::Min($px.R,[Math]::Min($px.G,$px.B))
    if (($mx-$mn) -gt 40 -and $mx -gt 50) { $score++ } } }
  $name = "$PSScriptRoot\LSWTCSRuntime\build\grab_$i.png"
  $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  if ($score -gt $best) { $best = $score; $bestname = $name }
  Start-Sleep -Milliseconds $IntervalMs
}
Write-Output "BEST=$bestname score=$best"
