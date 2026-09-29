# The correct launch sequence, established by measurement:
#   1. the game opens on a modal "SteamVR wireless connection" prompt that
#      blocks everything behind it
#   2. a CLICK in the centre of the window dismisses it (verified: the prompt
#      is gone from the capture afterwards, while SPACE/ENTER/ESC/letter keys
#      do not remove it)
#   3. only then is the main menu reachable and mods can load
# ReShade's "press any key" splash is dismissed by keybd_event as well.
param([int]$Boots = 1)
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class B {
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
function Focus($h){ $q=0;$t=[B]::GetWindowThreadProcessId($h,[ref]$q);$c=[B]::GetCurrentThreadId()
  [void][B]::AttachThreadInput($c,$t,$true); [void][B]::ShowWindow($h,9)
  [void][B]::BringWindowToTop($h); [void][B]::SetForegroundWindow($h)
  [void][B]::AttachThreadInput($c,$t,$false); Start-Sleep -Milliseconds 500 }
function Key($vk){ [B]::keybd_event([byte]$vk,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 70; [B]::keybd_event([byte]$vk,0,2,[IntPtr]::Zero); Start-Sleep -Milliseconds 900 }
function ClickCentre {
  $p=Get-Process teardown; $h=$p.MainWindowHandle
  $r=New-Object B+RECT; [void][B]::GetWindowRect($h,[ref]$r)
  [void][B]::SetCursorPos([int](($r.L+$r.R)/2),[int](($r.T+$r.B)/2))
  Start-Sleep -Milliseconds 300
  [B]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
  [B]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 1200 }
function Shot($t){ $p=Get-Process teardown;$h=$p.MainWindowHandle
  $r=New-Object B+RECT;[void][B]::GetWindowRect($h,[ref]$r)
  $b=New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.L,$r.T,0,0,$b.Size);$g.Dispose()
  $b.Save("C:\tdvr\boot_$t.png",[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose()
  "shot boot_$t" }
$p=Get-Process teardown -EA SilentlyContinue; if(-not $p){'no process';exit 0}
$h=$p.MainWindowHandle
for ($i=0; $i -lt 3; $i++) {
  Focus $h
  Key 0x20                 # ReShade "press any key"
  Focus $h
  ClickCentre             # SteamVR prompt
  Focus $h
  Key 0x20
  "round $i done  cpu=" + [math]::Round((Get-Process teardown).CPU,2)
}
Shot 'final'
