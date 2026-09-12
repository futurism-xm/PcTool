param([string]$Executable="$PSScriptRoot\..\cmake-build-release\PcTool.exe",[int]$Cycles=30,[double]$LimitMs=1000,[string]$ReportPath='')
$ErrorActionPreference='Stop'
Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Diagnostics;using System.Threading;
public static class SwitchProbe {
 public static Guid Identity;
 public static void SetIdentity(string path){ulong hash=14695981039346656037UL;foreach(char c in path.ToLowerInvariant())hash=unchecked((hash^c)*1099511628211UL);byte[] bytes=BitConverter.GetBytes(hash);bytes[0]=(byte)((bytes[0]&63)|128);Identity=new Guid(0x739c00a1,unchecked((short)0x882e),0x48cc,bytes);}
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] static extern bool PostMessage(IntPtr w,uint m,IntPtr wp,IntPtr lp);
 [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr w,uint m,IntPtr wp,IntPtr lp);
 [DllImport("user32.dll")] static extern bool GetMenuItemRect(IntPtr w,IntPtr menu,uint item,out Rect r);
 [DllImport("user32.dll")] static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr w);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w,out Rect r);
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct NotifyData {public uint size;public IntPtr hwnd;public uint id,flags,callback;public IntPtr icon;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=128)]public string tip;public uint state,mask;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)]public string info;public uint version;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=64)]public string title;public uint infoFlags;public Guid guid;public IntPtr balloon;}
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)] static extern bool Shell_NotifyIcon(uint message,ref NotifyData data);
 public static bool Tray(){var data=new NotifyData{size=(uint)Marshal.SizeOf(typeof(NotifyData)),hwnd=Controller(),id=1,flags=32,guid=Identity};return Shell_NotifyIcon(1,ref data);}
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr w,EnumProc callback,IntPtr data);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr w,StringBuilder s,int n);
 delegate bool EnumProc(IntPtr w,IntPtr data);
 [StructLayout(LayoutKind.Sequential)] struct Rect{public int left,top,right,bottom;}
 [StructLayout(LayoutKind.Sequential)] struct Input {public uint type;public Data data;}
 [StructLayout(LayoutKind.Explicit)] struct Data {[FieldOffset(0)]public Mouse mouse;[FieldOffset(0)]public Keyboard key;}
 [StructLayout(LayoutKind.Sequential)] struct Mouse {public int x,y;public uint data,flags,time;public UIntPtr extra;}
 [StructLayout(LayoutKind.Sequential)] struct Keyboard {public ushort vk,scan;public uint flags,time;public UIntPtr extra;}
 [DllImport("user32.dll")] static extern uint SendInput(uint count,Input[] data,int size);
 public static IntPtr Controller(){return FindWindow("PcTool.BackgroundController",null);}
 public static IntPtr Monitor(){IntPtr found=IntPtr.Zero;EnumChildWindows(FindWindow("Shell_TrayWnd",null),(w,d)=>{var s=new StringBuilder(256);GetClassName(w,s,256);if(s.ToString()=="PcTool.TaskbarMonitor"&&IsWindowVisible(w))found=w;return true;},IntPtr.Zero);return found;}
 public static double Toggle(bool taskbar){
  var control=Controller();var monitor=Monitor();SetCursorPos(400,350);
  if(monitor!=IntPtr.Zero){Rect bounds;GetWindowRect(monitor,out bounds);SetCursorPos((bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2);var mouse=new Input[2];mouse[0].data.mouse.flags=8;mouse[1].data.mouse.flags=16;SendInput(2,mouse,Marshal.SizeOf(typeof(Input)));}
  else PostMessage(control,0x806b,IntPtr.Zero,new IntPtr((1<<16)|0x7b));
  var wait=Stopwatch.StartNew();IntPtr menu=IntPtr.Zero;Rect r=new Rect();
  while(wait.ElapsedMilliseconds<5000){var popup=FindWindow("#32768",null);if(popup!=IntPtr.Zero){menu=SendMessage(popup,0x1e1,IntPtr.Zero,IntPtr.Zero);if(menu!=IntPtr.Zero&&GetMenuItemRect(IntPtr.Zero,menu,0,out r))break;}Thread.Sleep(10);}
  if(menu==IntPtr.Zero||r.right<=r.left)throw new Exception("Menu absent");
  Console.WriteLine("menu rect="+r.left+","+r.top+","+r.right+","+r.bottom);
  SetCursorPos(r.left+65,(r.top+r.bottom)/2);var input=new Input[2];input[0].data.mouse.flags=2;input[1].data.mouse.flags=4;
  var timer=Stopwatch.StartNew();if(SendInput(2,input,Marshal.SizeOf(typeof(Input)))!=2)throw new Exception("Click failed");
  while(((Monitor()!=IntPtr.Zero)!=taskbar||Tray()==taskbar)&&timer.ElapsedMilliseconds<20000)Thread.Sleep(5);
  double elapsed=timer.Elapsed.TotalMilliseconds;
  Console.WriteLine("switch target="+taskbar+" elapsed_ms="+elapsed.ToString("F1")+" menu_still_visible="+(FindWindow("#32768",null)!=IntPtr.Zero));
  Escape();if((Monitor()!=IntPtr.Zero)!=taskbar||Tray()==taskbar)throw new Exception("Placement did not change or duplicate entry");return elapsed;
 }
 public static void Escape(){var input=new Input[2];for(int i=0;i<2;i++){input[i].type=1;input[i].data.key.vk=0x1b;input[i].data.key.flags=i==1?2u:0u;}SendInput(2,input,Marshal.SizeOf(typeof(Input)));Thread.Sleep(100);}
}
'@
[SwitchProbe]::SetIdentity((Resolve-Path -LiteralPath $Executable).ProviderPath)
$key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\PcTool');$original=$key.GetValue('MonitorInTaskbar',$null)
[void][SwitchProbe]::SetThreadDpiAwarenessContext([IntPtr](-4))
function Stop-TestApp {$running=@(Get-Process PcTool -ErrorAction SilentlyContinue);Start-Process -FilePath $Executable -ArgumentList '--exit-existing' -WindowStyle Hidden -Wait;foreach($p in $running){if(!$p.WaitForExit(15000)){throw 'Shutdown timeout'}}}
try {
 Stop-TestApp;$key.SetValue('MonitorInTaskbar',1,[Microsoft.Win32.RegistryValueKind]::DWord)
 Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $Executable) -WindowStyle Hidden
 $end=[DateTime]::UtcNow.AddSeconds(10);while([SwitchProbe]::Monitor() -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $end){Start-Sleep -Milliseconds 50}
 $durations=@();for($i=0;$i -lt $Cycles;$i++){$durations+=[SwitchProbe]::Toggle(($i%2) -eq 1)}
 $max=($durations|Measure-Object -Maximum).Maximum;Write-Output "MAX_MS=$max"
 if($ReportPath){[ordered]@{cycles=$Cycles;maximumMs=$max;durationsMs=$durations}|ConvertTo-Json|Set-Content -LiteralPath $ReportPath -Encoding utf8}
 if($max -gt $LimitMs){throw "Switch exceeded $LimitMs ms"}
}finally{[SwitchProbe]::Escape();Stop-TestApp;$leaked=[SwitchProbe]::Tray();if($null -eq $original){$key.DeleteValue('MonitorInTaskbar',$false)}else{$key.SetValue('MonitorInTaskbar',$original,[Microsoft.Win32.RegistryValueKind]::DWord)};$key.Dispose();if($leaked){throw 'Tray registration survived final exit'}}
