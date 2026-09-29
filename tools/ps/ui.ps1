# Drive the game UI by measured screen fractions. Values live in ui_target.txt
# so run_in_session1.ps1 does not have to forward arguments (it collapses
# them into a single string -- verified repeatedly).
#   line 1: FX   line 2: FY   line 3: TAG   line 4: WAITMS
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class U {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
}
'@
Add-Type -TypeDefinition $sig -Language CSharp
$cfg = Get-Content 'C:\tdvr\ui_target.txt'
$FX=[double]$cfg[0]; $FY=[double]$cfg[1]; $TAG=$cfg[2]; $WAIT=[int]$cfg[3]
$KEY=$cfg[4]   # optional: a VK code to send instead of a click
function Focus($h){ $q=0;$t=[U]::GetWindowThreadProcessId($h,[ref]$q);$c=[U]::GetCurrentThreadId()
  [void][U]::AttachThreadInput($c,$t,$true);[void][U]::ShowWindow($h,9)
  [void][U]::BringWindowToTop($h);[void][U]::SetForegroundWindow($h)
  [void][U]::AttachThreadInput($c,$t,$false);Start-Sleep -Milliseconds 400 }
function Shot($t){ $p=Get-Process teardown;$h=$p.MainWindowHandle
  $r=New-Object U+RECT;[void][U]::GetWindowRect($h,[ref]$r)
  $b=New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.L,$r.T,0,0,$b.Size);$g.Dispose()
  $b.Save("C:\tdvr\ui_$t.png",[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose() }
$p=Get-Process teardown -EA SilentlyContinue; if(-not $p){'no process';exit 0}
$h=$p.MainWindowHandle
Focus $h
if ($KEY -and $KEY -ne '0') {
  [U]::keybd_event([byte]$KEY,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 70
  [U]::keybd_event([byte]$KEY,0,2,[IntPtr]::Zero)
  Start-Sleep -Milliseconds $WAIT
} else {
  $r=New-Object U+RECT;[void][U]::GetWindowRect($h,[ref]$r)
  $W=$r.R-$r.L;$H=$r.B-$r.T
  [void][U]::SetCursorPos([int]($r.L+$W*$FX),[int]($r.T+$H*$FY))
  Start-Sleep -Milliseconds 300
  [U]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 90
  [U]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds $WAIT
}
Shot $TAG
"ui: frac=$FX,$FY key=$KEY -> ui_$TAG.png  cpu=" + [math]::Round((Get-Process teardown).CPU,2)
