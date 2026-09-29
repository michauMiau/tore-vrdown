# Click a window position given as a fraction of the window rect, and
# capture afterwards. Fractions come from OCR tsv output, never guessed.
param([double]$FX = 0.5, [double]$FY = 0.5, [string]$Tag = 'click', [int]$WaitMs = 1500)
$ErrorActionPreference='Continue'
Add-Type -AssemblyName System.Drawing
$sig = @'
using System; using System.Runtime.InteropServices;
public class C {
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
}
'@
Add-Type -TypeDefinition $sig -Language CSharp
$p=Get-Process teardown -EA SilentlyContinue; if(-not $p){'no process';exit 0}
$h=$p.MainWindowHandle
$q=0;$t=[C]::GetWindowThreadProcessId($h,[ref]$q);$c=[C]::GetCurrentThreadId()
[void][C]::AttachThreadInput($c,$t,$true);[void][C]::ShowWindow($h,9)
[void][C]::BringWindowToTop($h);[void][C]::SetForegroundWindow($h)
[void][C]::AttachThreadInput($c,$t,$false);Start-Sleep -Milliseconds 400
$r=New-Object C+RECT;[void][C]::GetWindowRect($h,[ref]$r)
$W=$r.R-$r.L;$H=$r.B-$r.T
[void][C]::SetCursorPos([int]($r.L+$W*$FX),[int]($r.T+$H*$FY))
Start-Sleep -Milliseconds 300
[C]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 90
[C]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
Start-Sleep -Milliseconds $WaitMs
$b=New-Object System.Drawing.Bitmap($W,$H)
$g=[System.Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.L,$r.T,0,0,$b.Size);$g.Dispose()
$b.Save("C:\tdvr\cf_$Tag.png",[System.Drawing.Imaging.ImageFormat]::Png);$b.Dispose()
"clicked frac $FX,$FY -> C:\tdvr\cf_$Tag.png  cpu=" + [math]::Round((Get-Process teardown).CPU,2)
