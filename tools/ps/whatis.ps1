# What IS in that window? PrintWindow can silently return a stale or
Add-Type -AssemblyName System.Windows.Forms
# composited surface, and three different mods producing a byte-identical
# capture proves the capture is not the live render.
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class C2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x,int y,int cx,int cy,uint f);
}
'@
Add-Type -TypeDefinition $sig -Language CSharp
$p = Get-Process teardown -EA SilentlyContinue
if (-not $p) { 'no process'; exit 0 }
$h = $p.MainWindowHandle
"handle=0x$($h.ToInt64().ToString('x'))  visible=$([C2]::IsWindowVisible($h))  iconic=$([C2]::IsIconic($h))"
"style=0x$(([C2]::GetWindowLongW($h,-16)).ToString('x'))  exstyle=0x$(([C2]::GetWindowLongW($h,-20)).ToString('x'))"
$wr = New-Object C2+RECT; [void][C2]::GetWindowRect($h,[ref]$wr)
$cr = New-Object C2+RECT; [void][C2]::GetClientRect($h,[ref]$cr)
"window  $($wr.L),$($wr.T) .. $($wr.R),$($wr.B)  = $($wr.R-$wr.L)x$($wr.B-$wr.T)"
"client  $($cr.R-$cr.L)x$($cr.B-$cr.T)"
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
"virtual $($vs.Width)x$($vs.Height)"
# Force it to the foreground and full-window on the real desktop, then grab
# the SCREEN rather than the window DC. A composited game surface does not
# always appear via PrintWindow.
[void][C2]::BringWindowToTop($h)
[void][C2]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 1200
$wr2 = New-Object C2+RECT; [void][C2]::GetWindowRect($h,[ref]$wr2)
"after focus: $($wr2.L),$($wr2.T) .. $($wr2.R),$($wr2.B)"
$b = New-Object System.Drawing.Bitmap($vs.Width,$vs.Height)
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($vs.X,$vs.Y,0,0,$b.Size)
$g.Dispose()
$b.Save('C:\tdvr\screen.png',[System.Drawing.Imaging.ImageFormat]::Png)
$b.Dispose()
'saved C:\tdvr\screen.png'
