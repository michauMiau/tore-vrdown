# Click the real menu items, measured by OCR rather than guessed:
#   "Graj"        frac 0.3389,0.1581
#   "Gra wieloosobowa" frac ~0.4283..0.4868,0.1583
#   "Opcje"       frac 0.7343,0.1581
# The window is at (88,29)-(1592,998) on a 1680x1050 capture, so the capture
# fractions map straight onto the window rect.
param([int]$Rounds = 3)
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class P {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
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
function Focus($h){ $q=0;$t=[P]::GetWindowThreadProcessId($h,[ref]$q);$c=[P]::GetCurrentThreadId()
  [void][P]::AttachThreadInput($c,$t,$true); [void][P]::ShowWindow($h,9)
  [void][P]::BringWindowToTop($h); [void][P]::SetForegroundWindow($h)
  [void][P]::AttachThreadInput($c,$t,$false); Start-Sleep -Milliseconds 400 }
function Key($vk){ [P]::keybd_event([byte]$vk,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 70; [P]::keybd_event([byte]$vk,0,2,[IntPtr]::Zero); Start-Sleep -Milliseconds 700 }
function Click($fx,$fy){ $p=Get-Process teardown; $h=$p.MainWindowHandle
  $r=New-Object P+RECT; [void][P]::GetWindowRect($h,[ref]$r)
  $W=$r.R-$r.L; $H=$r.B-$r.T
  [void][P]::SetCursorPos([int]($r.L+$W*$fx),[int]($r.T+$H*$fy))
  Start-Sleep -Milliseconds 250
  [P]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
  [P]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 1400 }
function Shot($t){ $p=Get-Process teardown;$h=$p.MainWindowHandle
  $r=New-Object P+RECT;[void][P]::GetWindowRect($h,[ref]$r)
  $b=New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.L,$r.T,0,0,$b.Size);$g.Dispose()
  $b.Save("C:\tdvr\play_$t.png",[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose()
  "shot play_$t" }
$p=Get-Process teardown -EA SilentlyContinue; if(-not $p){'no process';exit 0}
$h=$p.MainWindowHandle
# dismiss the two modal prompts first
Focus $h; Key 0x20
Focus $h; Click 0.5 0.5
Focus $h; Key 0x20
Shot '00menu'
# "Graj"
Focus $h; Click 0.3389 0.1581
Shot '01play'
for ($i=0; $i -lt $Rounds; $i++) {
  Focus $h
  Click 0.5 0.5      # campaign is roughly centred; a click accepts the default
  "after click $i  cpu=" + [math]::Round((Get-Process teardown).CPU,2)
  Shot ('0%dclick%d' -f (2+$i),$i)
}
