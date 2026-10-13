param([Parameter(Mandatory=$true)][string]$JobPath,[Parameter(Mandatory=$true)][string]$CleanupNamespace)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'cleanup-jobs.ps1')
$mutex=$null;$held=$false
try{
 $store=Get-CleanupStore $CleanupNamespace
 $jobFile=[IO.Path]::GetFullPath($JobPath)
 $directory=Split-Path -Parent $jobFile
 if((Split-Path -Parent $directory) -ne $store -or (Split-Path -Leaf $jobFile) -ne 'job.json' -or $directory -ne $PSScriptRoot){throw 'Invalid cleanup job location'}
 if(!(Test-Path -LiteralPath $jobFile)){exit 0}
 Assert-CleanupSecurity $directory;Assert-CleanupSecurity $jobFile
 $job=Get-Content -LiteralPath $jobFile -Raw -Encoding UTF8|ConvertFrom-Json
 $root=Get-CleanupRoot ([string]$job.Root);$hash=Get-CleanupRootHash $root
 if($job.Version -ne 1 -or $job.Namespace -ne $CleanupNamespace -or $job.RootHash -ne $hash -or $job.Id -notmatch '^[a-f0-9]{32}$' -or
  (Split-Path -Leaf $directory) -ne ($hash+'-'+$job.Id) -or $job.TaskName -ne ('Cleanup-'+$hash+'-'+$job.Id) -or $job.Kind -notin @('upgrade','uninstall')){throw 'Invalid cleanup job identity'}
 $mutex=[Threading.Mutex]::new($false,(Get-CleanupLockName $root $CleanupNamespace))
 try{$held=$mutex.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$held=$true}
 if(!$held){exit 0} # The next trigger retries after the installation transaction.
 if(!(Test-Path -LiteralPath $jobFile)){exit 0}
 if(Invoke-CleanupJob $directory $job){exit 0}
 [IO.File]::WriteAllText((Join-Path $directory 'last-result.txt'),([DateTimeOffset]::Now.ToString('o')+' Pending locked or replaced files.'))
 exit 2
}catch{
 Write-Error $_
 exit 1
}finally{
 if($held){$mutex.ReleaseMutex()};if($mutex){$mutex.Dispose()}
}
