# Dismiss whatever modal prompt is up, trying a spread of activators and
# reporting after each so the one that works is recorded, not guessed.
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class D {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
}
'@
Add-Type -TypeDefinition $sig -Language CSharp
$VK=@{ SPACE=0x20; ENTER=0x0D; ESC=0x1B; TAB=0x09; A=0x41; E=0x45; B=0x42; F=0x46 }
function Focus($h){ $q=0;$t=[D]::GetWindowThreadProcessId($h,[ref]$q);$c=[D]::GetCurrentThreadId()
  [void][D]::AttachThreadInput($c,$t,$true); [void][D]::ShowWindow($h,9)
  [void][D]::BringWindowToTop($h); [void][D]::SetForegroundWindow($h)
  [void][D]::AttachThreadInput($c,$t,$false); Start-Sleep -Milliseconds 500 }
function Key($n){ $vk=[byte]$VK[$n]; [D]::keybd_event($vk,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 70; [D]::keybd_event($vk,0,2,[IntPtr]::Zero); Start-Sleep -Milliseconds 1000 }
function Shot($t){ $p=Get-Process teardown;$h=$p.MainWindowHandle
  $r=New-Object D+RECT;[void][D]::GetWindowRect($h,[ref]$r)
  $b=New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.L,$r.T,0,0,$b.Size);$g.Dispose()
  $b.Save("C:\tdvr\d_$t.png",[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose() }
$p=Get-Process teardown -EA SilentlyContinue; if(-not $p){'no process';exit 0}
$h=$p.MainWindowHandle
foreach ($k in @('SPACE','ENTER','ESC','A','B','E','F')) {
  Focus $h
  Key $k
  Shot $k
  "$k sent  cpu=" + [math]::Round((Get-Process teardown).CPU,2)
}
# also try a click in the middle of the prompt area
Focus $h
$r=New-Object D+RECT;[void][D]::GetWindowRect($h,[ref]$r)
[void][D]::SetCursorPos([int](($r.L+$r.R)/2),[int](($r.T+$r.B)/2))
Start-Sleep -Milliseconds 300
[D]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
[D]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
Start-Sleep -Milliseconds 1200
Shot 'CLICK'
'clicked centre'
