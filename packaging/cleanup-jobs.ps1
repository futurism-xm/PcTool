# Shared by the elevated installer and its isolated, temporary SYSTEM tasks.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
 # A PowerShell 7 parent can pass Core module paths to Windows PowerShell.
 # Load its own ACL cmdlets explicitly instead of inheriting that lookup order.
 Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
}
# Callers of installer operations hold Get-CleanupLockName across the transaction.
Set-StrictMode -Version 2
$script:PendingMappedImage=$false
function Test-CleanupNamespace([string]$Namespace) {
 if($Namespace -notmatch '^(PcTool|PcToolTest-[a-fA-F0-9]{32})$'){throw 'Invalid cleanup namespace'}
}
function Get-CleanupRoot([string]$Root) {
 $path=[IO.Path]::GetFullPath($Root).TrimEnd('\')
 if($path -notmatch '^[A-Za-z]:\\' -or $path -eq [IO.Path]::GetPathRoot($path).TrimEnd('\')){throw 'Invalid cleanup root'}
 for($item=$path;$item;$item=Split-Path -Parent $item){
  if((Test-Path -LiteralPath $item) -and ((Get-Item -LiteralPath $item -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Cleanup root traverses a reparse point'}
 }
 return $path
}
function Get-CleanupRootHash([string]$Root) {
 $sha=[Security.Cryptography.SHA256]::Create()
 try{return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::Unicode.GetBytes((Get-CleanupRoot $Root).ToUpperInvariant())))).Replace('-','')}
 finally{$sha.Dispose()}
}
function Get-CleanupLockName([string]$Root,[string]$Namespace) {
 Test-CleanupNamespace $Namespace
 return 'Global\'+$Namespace+'-Install-'+(Get-CleanupRootHash $Root)
}
function Set-CleanupSecurity([string]$Path,[bool]$Directory) {
 if($Directory){$acl=New-Object Security.AccessControl.DirectorySecurity}else{$acl=New-Object Security.AccessControl.FileSecurity}
 $acl.SetAccessRuleProtection($true,$false)
 $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
 foreach($sid in @('S-1-5-18','S-1-5-32-544')){
  $identity=[Security.Principal.SecurityIdentifier]::new($sid)
  if($Directory){$rule=[Security.AccessControl.FileSystemAccessRule]::new($identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')}
  else{$rule=[Security.AccessControl.FileSystemAccessRule]::new($identity,'FullControl','Allow')}
  [void]$acl.AddAccessRule($rule)
 }
 Set-Acl -LiteralPath $Path -AclObject $acl
}
function Assert-CleanupSecurity([string]$Path) {
 $item=Get-Item -LiteralPath $Path -Force
 if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Cleanup store cannot be a reparse point'}
 $acl=Get-Acl -LiteralPath $Path
 if($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-18','S-1-5-32-544')){throw 'Cleanup store has an untrusted owner'}
 $write=[Security.AccessControl.FileSystemRights]::Write -bor [Security.AccessControl.FileSystemRights]::Delete -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
 foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
  if($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $write) -and $rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')){throw 'Cleanup store permits untrusted writes'}
 }
}
function Get-CleanupStore([string]$Namespace,[switch]$Create) {
 Test-CleanupNamespace $Namespace
 $path=[Environment]::GetFolderPath('CommonApplicationData')
 foreach($part in @($Namespace,'CleanupJobs')){
  $path=Join-Path $path $part
  if(Test-Path -LiteralPath $path){Assert-CleanupSecurity $path}
  elseif($Create){[void](New-Item -ItemType Directory -Path $path);Set-CleanupSecurity $path $true}
 }
 return $path
}
function Resolve-CleanupPath([string]$Root,[string]$Relative) {
 if([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or $Relative.Contains(':')){throw 'Invalid cleanup relative path'}
 $full=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
 if(!$full.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup path escaped installation root'}
 return $full
}
function Initialize-CleanupNative {
 if('PcToolCleanup.Native' -as [type]){return}
 Add-Type @'
using System;using System.IO;using System.Collections.Generic;using System.ComponentModel;using System.Runtime.InteropServices;using Microsoft.Win32.SafeHandles;
namespace PcToolCleanup {
 public static class Native {
  public static void Replace(string source,string target){File.Replace(source,target,null);}
  public static bool SupportsRetirement(string path){
   // Inspect PE exports without loading/executing a DLL (also works cross-bitness).
   try{byte[] b=File.ReadAllBytes(path);using(var r=new BinaryReader(new MemoryStream(b))){
    if(r.ReadUInt16()!=0x5a4d)return false;r.BaseStream.Position=0x3c;uint pe=r.ReadUInt32();r.BaseStream.Position=pe;
    if(r.ReadUInt32()!=0x4550)return false;r.ReadUInt16();ushort count=r.ReadUInt16();r.BaseStream.Position=pe+20;ushort size=r.ReadUInt16();
    long optional=pe+24;r.BaseStream.Position=optional;ushort magic=r.ReadUInt16();if(magic!=0x10b&&magic!=0x20b)return false;
    r.BaseStream.Position=optional+(magic==0x20b?112:96);uint exports=r.ReadUInt32();if(exports==0)return false;
    var sections=new List<uint[]>();r.BaseStream.Position=optional+size;
    for(int i=0;i<count;i++){r.BaseStream.Position+=8;uint virtualSize=r.ReadUInt32(),address=r.ReadUInt32(),rawSize=r.ReadUInt32(),raw=r.ReadUInt32();sections.Add(new uint[]{address,Math.Max(virtualSize,rawSize),raw});r.BaseStream.Position+=16;}
    Func<uint,long> offset=delegate(uint address){foreach(var s in sections){if(address>=s[0]&&(ulong)address<(ulong)s[0]+s[1])return (long)s[2]+address-s[0];}throw new InvalidDataException();};
    r.BaseStream.Position=offset(exports)+24;uint names=r.ReadUInt32();r.ReadUInt32();uint table=r.ReadUInt32();if(names>65536)return false;
    for(uint i=0;i<names;i++){r.BaseStream.Position=offset(table)+i*4;r.BaseStream.Position=offset(r.ReadUInt32());var text=new System.Text.StringBuilder();byte c;while((c=r.ReadByte())!=0&&text.Length<128)text.Append((char)c);if(text.ToString()=="PcToolSupportsRetirement")return true;}
   }}catch(IOException){}catch(ArgumentException){}catch(OverflowException){}
   return false;
  }
  [StructLayout(LayoutKind.Sequential)] struct Info {public uint Attributes,CreateLow,CreateHigh,AccessLow,AccessHigh,WriteLow,WriteHigh,Volume,SizeHigh,SizeLow,Links,IndexHigh,IndexLow;}
  [StructLayout(LayoutKind.Sequential)] struct Disposition {[MarshalAs(UnmanagedType.Bool)] public bool Delete;}
  [StructLayout(LayoutKind.Sequential)] struct BasicInfo {public long Creation,Access,Write,Change;public uint Attributes;}
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct RenameInfo {public uint Replace;public IntPtr Root;public uint Length;public char Name;}
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFile(string path,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle file,out Info info);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetFileInformationByHandle(SafeFileHandle file,int kind,ref Disposition info,uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetFileInformationByHandle(SafeFileHandle file,int kind,ref BasicInfo info,uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetFileInformationByHandle(SafeFileHandle file,int kind,IntPtr info,uint size);
  static string Id(Info i){return i.Volume.ToString("X8")+":"+i.IndexHigh.ToString("X8")+i.IndexLow.ToString("X8")+":"+i.CreateHigh.ToString("X8")+i.CreateLow.ToString("X8");}
  static List<SafeFileHandle> Parents(string path){
   var paths=new List<string>();for(string p=Path.GetDirectoryName(path);!String.IsNullOrEmpty(p);p=Path.GetDirectoryName(p))paths.Add(p);
   paths.Reverse();var handles=new List<SafeFileHandle>();
   try {foreach(string p in paths){var h=CreateFile(p,0,3,IntPtr.Zero,3,0x02200000,IntPtr.Zero);if(h.IsInvalid){h.Dispose();throw new Win32Exception(Marshal.GetLastWin32Error());}handles.Add(h);Info i;if(!GetFileInformationByHandle(h,out i))throw new Win32Exception(Marshal.GetLastWin32Error());if((i.Attributes&0x400)!=0)throw new IOException("Reparse parent rejected: "+p);}return handles;}
   catch{foreach(var h in handles)h.Dispose();throw;}
  }
  public static string Identity(string path){
   var parents=Parents(path);try{using(var h=CreateFile(path,0,7,IntPtr.Zero,3,0x02200000,IntPtr.Zero)){if(h.IsInvalid)throw new Win32Exception(Marshal.GetLastWin32Error());Info i;if(!GetFileInformationByHandle(h,out i))throw new Win32Exception(Marshal.GetLastWin32Error());return Id(i);}}finally{foreach(var h in parents)h.Dispose();}
  }
  static bool Locked(int code){return code==32||code==33||code==1224;}
  public static int Rename(string source,string destination,string identity){
   var from=Parents(source);try{var to=Parents(destination);try{
    using(var h=CreateFile(source,0x10080,7,IntPtr.Zero,3,0x02200000,IntPtr.Zero)){
     if(h.IsInvalid)return Marshal.GetLastWin32Error();Info info;if(!GetFileInformationByHandle(h,out info))return Marshal.GetLastWin32Error();
     if(Id(info)!=identity||(info.Attributes&0x410)!=0x10)return 1306;
     byte[] name=System.Text.Encoding.Unicode.GetBytes(destination+"\0");int offset=Marshal.OffsetOf(typeof(RenameInfo),"Name").ToInt32();
     IntPtr buffer=Marshal.AllocHGlobal(offset+name.Length);try{
      for(int i=0;i<offset;i++)Marshal.WriteByte(buffer,i,0);
      Marshal.WriteInt32(buffer,Marshal.OffsetOf(typeof(RenameInfo),"Length").ToInt32(),name.Length-2);Marshal.Copy(name,0,IntPtr.Add(buffer,offset),name.Length);
      return SetFileInformationByHandle(h,3,buffer,(uint)(offset+name.Length))?0:Marshal.GetLastWin32Error();
     }finally{Marshal.FreeHGlobal(buffer);}
    }
   }finally{foreach(var h in to)h.Dispose();}}finally{foreach(var h in from)h.Dispose();}
  }
  public static int Delete(string path,string identity,bool directory,out bool retryable){
   retryable=false;
   List<SafeFileHandle> parents;try{parents=Parents(path);}catch(Win32Exception e){retryable=Locked(e.NativeErrorCode);return e.NativeErrorCode;}
   try{using(var h=CreateFile(path,0x10100,7,IntPtr.Zero,3,0x02200000,IntPtr.Zero)){
    if(h.IsInvalid){int error=Marshal.GetLastWin32Error();retryable=Locked(error);return error;}Info i;if(!GetFileInformationByHandle(h,out i))return Marshal.GetLastWin32Error();
    if(Id(i)!=identity||(((i.Attributes&0x10)!=0)!=directory))return 1306;
    if((i.Attributes&1)!=0){var basic=new BasicInfo{Attributes=i.Attributes&~1u};if(basic.Attributes==0)basic.Attributes=0x80;if(!SetFileInformationByHandle(h,0,ref basic,(uint)Marshal.SizeOf(typeof(BasicInfo))))return Marshal.GetLastWin32Error();}
    var disposition=new Disposition{Delete=true};if(SetFileInformationByHandle(h,4,ref disposition,4))return 0;
    int code=Marshal.GetLastWin32Error();string name=Path.GetFileName(path);
    bool image=name.EndsWith(".dll",StringComparison.OrdinalIgnoreCase)||name.EndsWith(".exe",StringComparison.OrdinalIgnoreCase)||System.Text.RegularExpressions.Regex.IsMatch(name,@"^7-zip(32)?\.dll\.\d+\.old$",System.Text.RegularExpressions.RegexOptions.IgnoreCase);
    retryable=Locked(code)||(code==5&&!directory&&image);return code;
   }}finally{foreach(var h in parents)h.Dispose();}
  }
 }
}
'@
}
function New-CleanupEntry([string]$Root,[string]$Relative) {
 $path=Resolve-CleanupPath $Root $Relative
 if(!(Test-Path -LiteralPath $path)){return}
 Initialize-CleanupNative
 $item=Get-Item -LiteralPath $path -Force
 [pscustomobject]@{Path=$Relative;Identity=[PcToolCleanup.Native]::Identity($path);Directory=[bool]$item.PSIsContainer}
}
function Remove-CleanupEntry([string]$Root,$Entry) {
 $path=Resolve-CleanupPath $Root ([string]$Entry.Path)
 if([string]$Entry.Identity -notmatch '^[0-9A-F]{8}:[0-9A-F]{16}:[0-9A-F]{16}$'){throw 'Invalid cleanup file identity'}
 Initialize-CleanupNative
 $retryable=$false
 $errorCode=[PcToolCleanup.Native]::Delete($path,[string]$Entry.Identity,[bool]$Entry.Directory,[ref]$retryable)
 if($errorCode -in @(0,2,3)){return $true}
 if($Entry.Directory -and $errorCode -eq 145){return $true} # Unrelated children are retained.
 if(!$retryable){throw "Cannot remove owned path: $path (Win32 $errorCode; identity, permissions or path changed)"}
 if(!$Entry.Directory -and $errorCode -eq 5 -and ([string]$Entry.Path -match '(?i)\.(dll|exe)(\.\d+\.old)?$')){$script:PendingMappedImage=$true}
 Write-Output "Pending cleanup: $path (Win32 $errorCode)" | Out-Host
 return $false
}
function Write-CleanupJson([string]$Path,$Value) {
 $temporary=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
 try{
  [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
  Set-CleanupSecurity $temporary $false
  if(Test-Path -LiteralPath $Path){Initialize-CleanupNative;[PcToolCleanup.Native]::Replace($temporary,$Path)}
  else{[IO.File]::Move($temporary,$Path)}
 }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
function Read-CleanupInstance([string]$Root,[string]$ExpectedRoot=$Root) {
 $path=Join-Path $Root '.pctool-install-instance.json'
 if(!(Test-Path -LiteralPath $path)){throw 'Installation instance is missing'}
 Assert-CleanupSecurity $path
 $instance=Get-Content -LiteralPath $path -Raw -Encoding UTF8|ConvertFrom-Json
 if($instance.Version -ne 1 -or $instance.Id -notmatch '^[a-f0-9]{32}$' -or $instance.Root -ne $ExpectedRoot){throw 'Invalid installation instance'}
 return $instance
}
function Get-RetiredStore([string]$Root,[string]$Namespace,[switch]$Create){
 Test-CleanupNamespace $Namespace
 $path=[IO.Path]::GetPathRoot((Get-CleanupRoot $Root))
 foreach($part in @('.PcToolCleanup',$Namespace)){
  $path=Join-Path $path $part
  if(Test-Path -LiteralPath $path){Assert-CleanupSecurity $path}
  elseif($Create){[void](New-Item -ItemType Directory -Path $path);Set-CleanupSecurity $path $true}
 }
 return $path
}
function Get-JobPhysicalRoot($Job){
 $root=Get-CleanupRoot ([string]$Job.Root)
 if($Job.PSObject.Properties['RetiredRoot'] -and $Job.RetiredRoot){
  $expected=Join-Path (Get-RetiredStore $root $Job.Namespace) ($Job.RootHash+'-'+$Job.Id)
  if($Job.RetiredRoot -cne $expected){throw 'Retired cleanup path is invalid'}
  if(Test-Path -LiteralPath $expected){
   $expected=Get-CleanupRoot $expected
   Initialize-CleanupNative
   if([PcToolCleanup.Native]::Identity($expected) -ne $Job.RetiredIdentity){throw 'Retired directory identity changed'}
   return $expected
  }
 }
 return $root
}
function Retire-CleanupRoot([string]$Directory,$Job){
 if($Job.Kind -ne 'uninstall' -or (Get-JobPhysicalRoot $Job) -ne $Job.Root){return $false}
 $root=Get-CleanupRoot $Job.Root
 $instance=Read-CleanupInstance $root
 if($instance.Id -ne $Job.Instance){return $false}
 # Move only a complete, verified set of remaining managed objects. Never move
 # unrelated user files just to make an installation directory disappear.
 $owned=@{}
 foreach($entry in @($Job.Entries)+@($Job.Marker)+@(New-CleanupEntry $root '.pctool-install-instance.json')){if($entry){$owned[[string]$entry.Path.Replace('/','\')]=$entry}}
 $directories=[Collections.Generic.Stack[string]]::new();$directories.Push($root)
 while($directories.Count){foreach($item in @(Get-ChildItem -LiteralPath $directories.Pop() -Force)){
  if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Retired cleanup cannot traverse a reparse point'}
  $relative=$item.FullName.Substring($root.Length+1)
  if(!$owned.ContainsKey($relative)){return $false}
  $entry=$owned[$relative]
  if($entry.Identity -ne [PcToolCleanup.Native]::Identity($item.FullName) -or [bool]$entry.Directory -ne [bool]$item.PSIsContainer){throw 'Retired object identity changed'}
  if(!$item.PSIsContainer -and $relative -match '^modules\\archive\\7-zip(32)?\.dll(\.\d+\.old)?$'){
   if(![PcToolCleanup.Native]::SupportsRetirement($item.FullName)){
    # Baseline shipped before the capability export, but its fail-closed COM
    # behavior was independently verified. Unknown older DLLs keep their guard.
    $known=@('6607588424F56726A0789FD7C84BA013E736A9AB762C3F39A4559F9B559EF384','069667F01CD430DC3856E208CBEF6FB9AC17224460716D21665387A4B916E919')
    if((Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash -notin $known){return $false}
   }
  }
  if($item.PSIsContainer){$directories.Push($item.FullName)}
 }}
 $store=Get-RetiredStore $root $Job.Namespace -Create
 $destination=Join-Path $store ($Job.RootHash+'-'+$Job.Id)
 $identity=[PcToolCleanup.Native]::Identity($root)
 $Job|Add-Member -NotePropertyName RetiredRoot -NotePropertyValue $destination -Force
 $Job|Add-Member -NotePropertyName RetiredIdentity -NotePropertyValue $identity -Force
 # Write intent first: after a crash the worker can find either the original
 # tree or the renamed tree, using the same instance and file identities.
 Write-CleanupJson (Join-Path $Directory 'job.json') $Job
 $code=[PcToolCleanup.Native]::Rename($root,$destination,$identity)
 if($code -eq 0){Write-Host 'Mapped extension moved to protected same-volume cleanup storage.';return $true}
 $Job.RetiredRoot='';Write-CleanupJson (Join-Path $Directory 'job.json') $Job
 if($code -in @(5,32,33,1224)){return $false}
 throw "Cannot retire installation directory (Win32 $code)"
}
function Remove-CleanupRoot([string]$Root,$Instance,[byte[]]$RetryUninstaller=$null) {
 try{[IO.Directory]::Delete($Root,$false);return $true}catch{
  $failure=$_
  # A nonempty root contains unrelated files, which are intentionally retained.
  if((Test-Path -LiteralPath $Root) -and @(Get-ChildItem -LiteralPath $Root -Force).Count){return $true}
  # Keep authoritative state even when the final empty directory, rather than
  # a DLL, is held by an external process. Never silently swallow this failure.
  Write-CleanupJson (Join-Path $Root '.pctool-install-instance.json') $Instance
  [IO.File]::WriteAllText((Join-Path $Root '.pctool-uninstalling'),'')
  if($null -ne $RetryUninstaller){[IO.File]::WriteAllBytes((Join-Path $Root 'Uninstall.exe'),$RetryUninstaller)}
  $code=$failure.Exception.GetBaseException().HResult -band 65535
  if($code -in @(32,33,1224)){return $false}
  throw $failure
 }
}
function Open-CleanupScheduler([string]$Namespace,[switch]$Create) {
 $scheduler=New-Object -ComObject Schedule.Service;$scheduler.Connect()
 try{$folder=$scheduler.GetFolder('\'+$Namespace)}catch{
  if($_.Exception.GetBaseException().HResult -notin @(-2147024894,-2147024893)){throw}
  if(!$Create){return $null}
  $folder=$scheduler.GetFolder('\').CreateFolder($Namespace,'O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)')
 }
 if($Create){$folder.SetSecurityDescriptor('O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)',0)}
 return [pscustomobject]@{Service=$scheduler;Folder=$folder}
}
function Remove-CleanupJob([string]$Directory,$Job) {
 $scheduler=Open-CleanupScheduler $Job.Namespace
 if($scheduler){try{$scheduler.Folder.DeleteTask([string]$Job.TaskName,0)}catch{if($_.Exception.GetBaseException().HResult -ne -2147024894){throw}}}
 foreach($name in @('job.json','retry-cleanup.ps1','cleanup-jobs.ps1','last-result.txt')){
  $file=Join-Path $Directory $name;if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file -Force}
 }
 if(Test-Path -LiteralPath $Directory){
  foreach($file in @(Get-ChildItem -LiteralPath $Directory -File -Force|Where-Object {$_.Name -match '^job\.json\.[a-f0-9]{32}\.tmp$'})){Remove-Item -LiteralPath $file.FullName -Force}
  if(!@(Get-ChildItem -LiteralPath $Directory -Force).Count){[IO.Directory]::Delete($Directory,$false)}
  else{Write-Warning "Unknown files retained in cleanup metadata directory: $Directory"}
 }
 $retiredStore=Get-RetiredStore ([string]$Job.Root) $Job.Namespace
 foreach($path in @($retiredStore,(Split-Path -Parent $retiredStore))){
  if((Test-Path -LiteralPath $path) -and !@(Get-ChildItem -LiteralPath $path -Force).Count){
   Assert-CleanupSecurity $path
   try{[IO.Directory]::Delete($path,$false)}catch{if(($_.Exception.GetBaseException().HResult -band 65535) -notin @(2,3,145)){throw}}
  }
 }
}
function Stop-CleanupJobs([string]$Root,[string]$Namespace,[string]$ExceptJobId='') {
 $store=Get-CleanupStore $Namespace
 if(!(Test-Path -LiteralPath $store)){return}
 $hash=Get-CleanupRootHash $Root
 foreach($directory in @(Get-ChildItem -LiteralPath $store -Directory -Force|Where-Object {$_.Name -cmatch ('^'+$hash+'-[a-f0-9]{32}$')})){
  if($directory.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Cleanup job directory is a reparse point'}
  # Only administrators/SYSTEM can create children in the validated store.
  # Repair a crash before the new child's explicit ACL/job.json was written.
  Set-CleanupSecurity $directory.FullName $true
  $jobFile=Join-Path $directory.FullName 'job.json'
  if(Test-Path -LiteralPath $jobFile){
   Assert-CleanupSecurity $jobFile
   $existing=Get-Content -LiteralPath $jobFile -Raw|ConvertFrom-Json
   if($existing.PSObject.Properties['RetiredRoot'] -and $existing.RetiredRoot -and (Get-JobPhysicalRoot $existing) -ne $Root){continue}
  }
  $job=[pscustomobject]@{Namespace=$Namespace;Root=$Root;TaskName=('Cleanup-'+$directory.Name);Id=$directory.Name.Substring($hash.Length+1)}
  if($job.Id -eq $ExceptJobId){continue}
  Remove-CleanupJob $directory.FullName $job
 }
}
function Register-CleanupJob([string]$Root,[string]$Namespace,[string]$Kind,[object[]]$Entries,[hashtable]$RuntimeSource=@{}) {
 if($Kind -notin @('upgrade','uninstall')){throw 'Invalid cleanup kind'}
 $instance=Read-CleanupInstance $Root
 $store=Get-CleanupStore $Namespace -Create;$hash=Get-CleanupRootHash $Root;$id=[guid]::NewGuid().ToString('N')
 $directory=Join-Path $store ($hash+'-'+$id)
 $job=[pscustomobject]@{Version=1;Namespace=$Namespace;Root=$Root;RootHash=$hash;Instance=$instance.Id;Kind=$Kind;Id=$id;TaskName=('Cleanup-'+$hash+'-'+$id);Entries=@($Entries);Marker=$null;InstanceFile=$null}
 try{
  [void](New-Item -ItemType Directory -Path $directory);Set-CleanupSecurity $directory $true
  $job.InstanceFile=New-CleanupEntry $Root '.pctool-install-instance.json'
  if($Kind -eq 'uninstall'){$job.Marker=New-CleanupEntry $Root '.pctool-uninstalling';if(!$job.Marker){throw 'Uninstall marker is missing'}}
  foreach($scriptName in @('cleanup-jobs.ps1','retry-cleanup.ps1')){
   if($RuntimeSource.ContainsKey($scriptName)){[IO.File]::WriteAllText((Join-Path $directory $scriptName),$RuntimeSource[$scriptName],[Text.UTF8Encoding]::new($false))}
   else{Copy-Item -LiteralPath (Join-Path $PSScriptRoot $scriptName) -Destination $directory}
  }
  Write-CleanupJson (Join-Path $directory 'job.json') $job
  $scheduler=Open-CleanupScheduler $Namespace -Create;$task=$scheduler.Service.NewTask(0)
  $task.RegistrationInfo.Description='Remove only recorded files from a completed PcTool installation transaction.'
  $task.Principal.UserId='SYSTEM';$task.Principal.LogonType=5;$task.Principal.RunLevel=1
  $task.Settings.Enabled=$true;$task.Settings.Hidden=$true;$task.Settings.StartWhenAvailable=$true
  $task.Settings.DisallowStartIfOnBatteries=$false;$task.Settings.StopIfGoingOnBatteries=$false;$task.Settings.MultipleInstances=2;$task.Settings.ExecutionTimeLimit='PT10M'
  $trigger=$task.Triggers.Create(1);$trigger.StartBoundary=(Get-Date).AddSeconds(2).ToString('s');$trigger.Repetition.Interval='PT5M'
  [void]$task.Triggers.Create(9)
  $action=$task.Actions.Create(0);$action.Path=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $action.Arguments='-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+(Join-Path $directory 'retry-cleanup.ps1')+'" -JobPath "'+(Join-Path $directory 'job.json')+'" -CleanupNamespace '+$Namespace
  $registered=$scheduler.Folder.RegisterTaskDefinition($job.TaskName,$task,6,'SYSTEM',$null,5,'O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)')
  [void]$scheduler.Folder.GetTask($job.TaskName)
  [void]$registered.Run($null)
  if($Kind -eq 'uninstall'){Stop-CleanupJobs $Root $Namespace $id}
  return [pscustomobject]@{Directory=$directory;Job=$job}
 }catch{
  try{Remove-CleanupJob $directory $job}catch{}
  throw
 }
}
function Invoke-CleanupJob([string]$Directory,$Job) {
 $root=Get-JobPhysicalRoot $Job
 $state=Join-Path $root '.pctool-install-instance.json'
 if(!(Test-Path -LiteralPath $state)){Remove-CleanupJob $Directory $Job;return $true}
 $instance=Read-CleanupInstance $root ([string]$Job.Root)
 if($instance.Id -ne $Job.Instance){Remove-CleanupJob $Directory $Job;return $true}
 $currentInstanceFile=New-CleanupEntry $root '.pctool-install-instance.json'
 $pending=$false
 foreach($entry in @($Job.Entries|Where-Object {!$_.Directory})){if(!(Remove-CleanupEntry $root $entry)){$pending=$true}}
 foreach($entry in @($Job.Entries|Where-Object {$_.Directory}|Sort-Object {$_.Path.Length} -Descending)){if(!(Remove-CleanupEntry $root $entry)){$pending=$true}}
 if($pending){
  if($root -eq $Job.Root -and $script:PendingMappedImage){[void](Retire-CleanupRoot $Directory $Job)}
  return $false
 }
 if($Job.Kind -eq 'uninstall'){
  if(!(Remove-CleanupEntry $root $Job.Marker)){return $false}
  if(!(Remove-CleanupEntry $root $currentInstanceFile)){return $false}
  try{$rootRemoved=Remove-CleanupRoot $root $instance}finally{
   # Restored marker files have a new identity. Persist it for the next retry;
   # otherwise the identity guard would correctly reject our own replacement.
   if(Test-Path -LiteralPath $state){
    $Job.Marker=New-CleanupEntry $root '.pctool-uninstalling'
    Write-CleanupJson (Join-Path $Directory 'job.json') $Job
   }
  }
  if(!$rootRemoved){return $false}
 }
 Remove-CleanupJob $Directory $Job
 return $true
}
