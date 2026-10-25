# Grab just the Unreal editor window into PNG frames - no desktop capture, no topmost pinning.
#
# Why PrintWindow(PW_RENDERFULLCONTENT=2) instead of a desktop grabber: ffmpeg's ddagrab records the
# whole desktop, so whatever floats above the editor lands in the recording (measured: a chat window did,
# and pinning the editor topmost is a desktop takeover we do not want). PrintWindow asks DWM for that one
# window's own composition, so it works while the window is covered, unfocused and un-topmost.
#
#   powershell -NoProfile -File <skill>/scripts/ue_window_grab.ps1 -Seconds 20 -Fps 30 -OutDir Saved\rec
# Then encode (use the ACHIEVED fps it prints):  ffmpeg -framerate <achieved> -i f%04d.jpg -c:v libx264 -pix_fmt yuv420p out.mp4
# Load the matching skill before running this: unreal-video-capture.
param(
    [int]$Seconds = 20,
    [int]$Fps = 15,
    [string]$OutDir = "Saved\rec",
    [string]$Prefix = "f"
)

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class WinGrab {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);

    public static Bitmap Grab(IntPtr hwnd, out int w, out int h) {
        RECT r;
        if (!GetWindowRect(hwnd, out r)) { w = 0; h = 0; return null; }
        w = r.Right - r.Left; h = r.Bottom - r.Top;
        if (w <= 0 || h <= 0) { w = 0; h = 0; return null; }
        Bitmap bmp = new Bitmap(w, h);
        using (Graphics g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            PrintWindow(hwnd, hdc, 2);          // PW_RENDERFULLCONTENT
            g.ReleaseHdc(hdc);
        }
        return bmp;
    }
}
'@

$proc = Get-Process UnrealEditor -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "editor-not-running"; exit 1 }
$hwnd = $proc.MainWindowHandle
$root = Join-Path (Split-Path -Parent $PSScriptRoot) $OutDir
if (-not (Test-Path $root)) { New-Item -ItemType Directory -Path $root | Out-Null }
Get-ChildItem -Path $root -Filter "$Prefix*.jpg" -ErrorAction SilentlyContinue | Remove-Item -Force

$interval = [Math]::Max(1, [int](1000 / $Fps))
$count = $Seconds * $Fps
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$made = 0
for ($i = 0; $i -lt $count; $i++) {
    $frameSw = [System.Diagnostics.Stopwatch]::StartNew()
    $w = 0; $h = 0
    $bmp = [WinGrab]::Grab($hwnd, [ref]$w, [ref]$h)
    if ($bmp) {
        $made++
        # JPEG: ~10x faster to write than PNG at this size, and 20 s of frames stays around 50 MB
        $bmp.Save((Join-Path $root ("{0}{1:d4}.jpg" -f $Prefix, $made)), [System.Drawing.Imaging.ImageFormat]::Jpeg)
        $bmp.Dispose()
    }
    $wait = $interval - $frameSw.ElapsedMilliseconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
}
$sw.Stop()
$achieved = $made / [Math]::Max(0.001, $sw.Elapsed.TotalSeconds)
Write-Output ("frames={0} size={1}x{2} wall={3:N1}s achieved_fps={4:N2} -> {5}" -f $made, $w, $h, $sw.Elapsed.TotalSeconds, $achieved, $root)
