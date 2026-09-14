param([string]$Executable = "$PSScriptRoot\..\cmake-build-release\PcTool.exe")
$ErrorActionPreference='Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HotkeyProbe {
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="FindWindowW")] private static extern IntPtr NativeFindWindow(string cls,string title);
 public static IntPtr FindWindow(string cls,string ignored){return NativeFindWindow(cls,null);}
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w,uint m,IntPtr wp,IntPtr lp);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w,uint m,IntPtr wp,IntPtr lp);
 [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr w,int id,uint modifiers,uint key);
 [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr w,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
 [StructLayout(LayoutKind.Sequential)] public struct Input {public uint type;public Data data;}
 [StructLayout(LayoutKind.Explicit)] public struct Data {[FieldOffset(0)] public Keyboard key;[FieldOffset(0)] public Mouse mouse;}
 [StructLayout(LayoutKind.Sequential)] public struct Keyboard {public ushort vk,scan;public uint flags,time;public UIntPtr extra;}
 [StructLayout(LayoutKind.Sequential)] public struct Mouse {public int x,y;public uint data,flags,time;public UIntPtr extra;}
 [DllImport("user32.dll")] public static extern uint SendInput(uint count,Input[] inputs,int size);
 public static void Chord(){ChordKey(0x78);}
 public static void ChordKey(ushort target){ushort[] keys={0x11,0x12,0x10,target,target,0x10,0x12,0x11};var inputs=new Input[8];for(int i=0;i<8;i++){inputs[i].type=1;inputs[i].data.key.vk=keys[i];inputs[i].data.key.flags=i>=4?2u:0u;}if(SendInput(8,inputs,Marshal.SizeOf(typeof(Input)))!=8)throw new Exception("SendInput failed");}
}
'@
function Stop-TestApp {
 $running=@(Get-Process PcTool -ErrorAction SilentlyContinue)
 Start-Process -FilePath $Executable -ArgumentList '--exit-existing' -WindowStyle Hidden -Wait
 foreach($item in $running){if(!$item.WaitForExit(15000)){throw 'Shutdown timed out'}}
}
function Wait-Window([string]$class,[bool]$visible=$false) {
 $end=[DateTime]::UtcNow.AddSeconds(8)
 do {$w=[HotkeyProbe]::FindWindow($class,$null);if($w -ne [IntPtr]::Zero -and (!$visible -or [HotkeyProbe]::IsWindowVisible($w))){return $w};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $end)
 throw "Window absent: $class"
}
$settingsPath=Join-Path (Split-Path $Executable) 'Data/settings.ini'
$original=if(Test-Path -LiteralPath $settingsPath){[IO.File]::ReadAllBytes($settingsPath)}else{$null}
function Read-Hotkey([int]$index){$text=[IO.File]::ReadAllText($settingsPath);$match=[regex]::Match($text,"(?m)^Hotkey$index=(\d+)");if(!$match.Success){throw 'Missing setting'};return [int]$match.Groups[1].Value}
try {
 Stop-TestApp
 Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $Executable) -WindowStyle Hidden
 $controller=Wait-Window 'PcTool.BackgroundController'
 [void][HotkeyProbe]::PostMessage($controller,0x806c,[IntPtr]::Zero,[IntPtr]::Zero)
 $editor=Wait-Window 'PcTool.HotkeySettings'
 $viewport=[HotkeyProbe]::GetDlgItem($editor,250)
 for($i=0;$i -lt 9;$i++){[void][HotkeyProbe]::SendMessage([HotkeyProbe]::GetDlgItem($viewport,300+$i),0x401,[IntPtr](0x700+0x77+$i),[IntPtr]::Zero)}
 # Reserve the proposed clipboard binding in this independent process.
 if(![HotkeyProbe]::RegisterHotKey([IntPtr]::Zero,920,7,0x78)){throw 'Test key already occupied'}
 [void][HotkeyProbe]::SendMessage($editor,0x111,[IntPtr]1,[IntPtr]::Zero)
 if(![HotkeyProbe]::IsWindowVisible($editor)){throw 'Conflict was accepted'}
 [void][HotkeyProbe]::UnregisterHotKey([IntPtr]::Zero,920)
 [void][HotkeyProbe]::SendMessage($editor,0x111,[IntPtr]1,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 300
 if(![HotkeyProbe]::IsWindowVisible($editor)){throw 'Save closed editor'}
 for($i=0;$i -lt 9;$i++){if((Read-Hotkey $i) -ne (0x70000+0x77+$i)){throw 'Persistence mismatch'}}
 for($i=5;$i -lt 9;$i++){if([HotkeyProbe]::RegisterHotKey([IntPtr]::Zero,930+$i,7,0x77+$i)){[void][HotkeyProbe]::UnregisterHotKey([IntPtr]::Zero,930+$i);throw 'New optional action was not registered'}}
 [HotkeyProbe]::Chord()
 $history=Wait-Window 'PcTool.ClipboardHistory' $true
 if(![HotkeyProbe]::IsWindowVisible($history)){throw 'New shortcut did not show clipboard'}
 Stop-TestApp
 Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $Executable) -WindowStyle Hidden
 $controller=Wait-Window 'PcTool.BackgroundController'
 Start-Sleep -Milliseconds 300
 [HotkeyProbe]::Chord()
 $history=Wait-Window 'PcTool.ClipboardHistory' $true
 if(![HotkeyProbe]::IsWindowVisible($history)){throw 'Shortcut did not survive restart'}
 [HotkeyProbe]::ChordKey(0x7f)
 $editor=Wait-Window 'PcTool.HotkeySettings' $true
 $viewport=[HotkeyProbe]::GetDlgItem($editor,250)
 # Merely browsing settings must leave global shortcuts usable.
 [void][HotkeyProbe]::SendMessage($history,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 [HotkeyProbe]::Chord()
 $history=Wait-Window 'PcTool.ClipboardHistory' $true
 if(![HotkeyProbe]::IsWindowVisible($editor)){throw 'Global shortcut closed settings'}
 [void][HotkeyProbe]::SendMessage($history,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 # Only the focused binding field consumes a chord; Escape restores it and releases capture.
 $field=[HotkeyProbe]::GetDlgItem($viewport,308)
 [void][HotkeyProbe]::SendMessage($field,0x201,[IntPtr]1,[IntPtr](12 -bor (12 -shl 16)))
 if(![HotkeyProbe]::RegisterHotKey([IntPtr]::Zero,950,7,0x78)){throw 'Binding focus did not pause globals'}
 [void][HotkeyProbe]::UnregisterHotKey([IntPtr]::Zero,950)
 [HotkeyProbe]::Chord()
 Start-Sleep -Milliseconds 150
 if([HotkeyProbe]::IsWindowVisible($history)){throw 'Recording a binding triggered clipboard'}
 if([HotkeyProbe]::SendMessage($field,0x402,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 0x778){throw 'Focused binding did not capture real chord'}
 [void][HotkeyProbe]::SendMessage($field,0x100,[IntPtr]27,[IntPtr]::Zero)
 [HotkeyProbe]::Chord()
 $history=Wait-Window 'PcTool.ClipboardHistory' $true
 [void][HotkeyProbe]::SendMessage($history,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 for($i=0;$i -lt 9;$i++){[void][HotkeyProbe]::SendMessage([HotkeyProbe]::GetDlgItem($viewport,300+$i),0x401,[IntPtr]::Zero,[IntPtr]::Zero)}
 [void][HotkeyProbe]::SendMessage($editor,0x111,[IntPtr]1,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 300
 for($i=0;$i -lt 9;$i++){if((Read-Hotkey $i) -ne 0){throw 'Unbound value not persisted'};if(![HotkeyProbe]::RegisterHotKey([IntPtr]::Zero,930+$i,7,0x77+$i)){throw 'Unbound shortcut still registered'};[void][HotkeyProbe]::UnregisterHotKey([IntPtr]::Zero,930+$i)}
 Write-Output 'PASS: nine persisted bindings, external conflict rejected, optional actions, restart, globals usable while settings open, focused binding capture only, Escape restores globals, clear all releases registrations'
} finally {
 [void][HotkeyProbe]::UnregisterHotKey([IntPtr]::Zero,920)
 Stop-TestApp
 if($null -eq $original){Remove-Item -LiteralPath $settingsPath -ErrorAction SilentlyContinue}else{[IO.File]::WriteAllBytes($settingsPath,$original)}
}
