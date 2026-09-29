# A sequence of measured UI steps, read from seq.txt (one step per line:
# "fracX fracY" or "key VK"). Captures after each step as seq_<n>.png.
# run_in_session1.ps1 collapses forwarded arguments into one string, so the
# whole script has to be data-driven from a file.
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class S {
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
function Focus($h){ $q=0;$t=[S]::GetWindowThreadProcessId($h,[ref]$q);$c=[S]::GetCurrentThreadId()
  [void][S]::AttachThreadInput($c,$t,$true);[void][S]::ShowWindow($h,9)
  [void][S]::BringWindowToTop($h);[void][S]::SetForegroundWindow($h)
  [void][S]::AttachThreadInput($c,$t,$false);Start-Sleep -Milliseconds 350 }
function Rect($h){ $r=New-Object S+RECT;[void][S]::GetWindowRect($h,[ref]$r);return $r }
function Shot($t){ $p=Get-Process teardown;$h=$p.MainWindowHandle;$r=Rect $h
  $b=New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.L,$r.T,0,0,$b.Size);$g.Dispose()
  $b.Save("C:\tdvr\seq_$t.png",[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose() }
$steps = Get-Content 'C:\tdvr\seq.txt' | Where-Object { $_.Trim() -ne '' -and -not $_.StartsWith('#') }
$p=Get-Process teardown -EA SilentlyContinue; if(-not $p){'no process';exit 0}
$h=$p.MainWindowHandle
$i=0
foreach ($s in $steps) {
  $i++
  $t=$s.Trim()
  Focus $h
  if ($t.StartsWith('key')) {
    $vk=[byte]$t.Split(' ')[1]
    [S]::keybd_event($vk,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 70
    [S]::keybd_event($vk,0,2,[IntPtr]::Zero)
  } else {
    $f=$t.Split(' ')
    $r=Rect $h; $W=$r.R-$r.L; $H=$r.B-$r.T
    [void][S]::SetCursorPos([int]($r.L+$W*[double]$f[0]),[int]($r.T+$H*[double]$f[1]))
    Start-Sleep -Milliseconds 250
    [S]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
    [S]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
  }
  Start-Sleep -Milliseconds 2200
  Shot ("{0:d2}" -f $i)
  "step $i : $t   cpu=" + [math]::Round((Get-Process teardown).CPU,2)
}
