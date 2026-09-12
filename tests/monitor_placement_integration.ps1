param([string]$Executable = "$PSScriptRoot\..\cmake-build-release\PcTool.exe")
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class MonitorPlacementProbe {
 public static Guid Identity;
 public static void SetIdentity(string path){ulong hash=14695981039346656037UL;foreach(char c in path.ToLowerInvariant())hash=unchecked((hash^c)*1099511628211UL);byte[] bytes=BitConverter.GetBytes(hash);bytes[0]=(byte)((bytes[0]&63)|128);Identity=new Guid(0x739c00a1,unchecked((short)0x882e),0x48cc,bytes);}
 [StructLayout(LayoutKind.Sequential)] public struct IconId { public uint size; public IntPtr hwnd; public uint id; public Guid guid; }
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left,top,right,bottom; }
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
 [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref IconId id,out Rect rect);
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct NotifyData { public uint size; public IntPtr hwnd; public uint id,flags,callback;public IntPtr icon;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)]public string tip;public uint state,mask;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)]public string info;public uint version;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=64)]public string title;public uint infoFlags;public Guid guid;public IntPtr balloon; }
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)] public static extern bool Shell_NotifyIcon(uint message,ref NotifyData data);
 public delegate bool EnumProc(IntPtr hwnd,IntPtr data);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent,EnumProc callback,IntPtr data);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd,StringBuilder text,int count);
 public static bool HasMonitor() { bool found=false;EnumChildWindows(FindWindow("Shell_TrayWnd",null),(w,d)=>{var text=new StringBuilder(256);GetClassName(w,text,256);if(text.ToString()=="PcTool.TaskbarMonitor")found=true;return true;},IntPtr.Zero);return found; }
 // A collapsed overflow icon need not expose a rectangle. A zero-flag modify
 // verifies registration without changing any icon fields.
 public static bool HasTray() {var data=new NotifyData{size=(uint)Marshal.SizeOf(typeof(NotifyData)),hwnd=FindWindow("PcTool.BackgroundController",null),id=1,flags=32,guid=Identity};return Shell_NotifyIcon(1,ref data);}
}
'@
[MonitorPlacementProbe]::SetIdentity((Resolve-Path -LiteralPath $Executable).ProviderPath)
$key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\PcTool')
$original = $key.GetValue('MonitorInTaskbar', $null)
function Stop-TestApp {
 $running = @(Get-Process PcTool -ErrorAction SilentlyContinue)
 Start-Process -FilePath $Executable -ArgumentList '--exit-existing' -WindowStyle Hidden -Wait
 foreach ($item in $running) {if (!$item.WaitForExit(15000)) {throw 'PcTool did not finish shutdown'}}
}
try {
 foreach ($mode in @(1,0,0,1)) {
  Stop-TestApp
  if([MonitorPlacementProbe]::HasTray()){throw 'Tray registration survived process exit'}
  $key.SetValue('MonitorInTaskbar',$mode,[Microsoft.Win32.RegistryValueKind]::DWord)
  $process = Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $Executable) -WindowStyle Hidden -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  do {
   Start-Sleep -Milliseconds 200
   $monitor = [MonitorPlacementProbe]::HasMonitor()
   $tray = [MonitorPlacementProbe]::HasTray()
  } while (($monitor -ne ($mode -eq 1) -or $tray -ne ($mode -eq 0)) -and [DateTime]::UtcNow -lt $deadline)
  if ($monitor -ne ($mode -eq 1) -or $tray -ne ($mode -eq 0)) {throw "Placement mismatch: mode=$mode monitor=$monitor tray=$tray"}
  Write-Output "PASS mode=$mode taskbar=$monitor tray=$tray"
 }
} finally {
 Stop-TestApp
 $leaked=[MonitorPlacementProbe]::HasTray()
 if ($null -eq $original) {$key.DeleteValue('MonitorInTaskbar',$false)} else {$key.SetValue('MonitorInTaskbar',$original,[Microsoft.Win32.RegistryValueKind]::DWord)}
 $key.Dispose()
 if($leaked){throw 'Tray registration survived final exit'}
}
