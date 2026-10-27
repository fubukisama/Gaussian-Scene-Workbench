#requires -Version 7.2
[CmdletBinding(DefaultParameterSetName='Publish')]
param(
  [Parameter(Mandatory=$true)][string]$InstallRoot,
  [Parameter(Mandatory=$true,ParameterSetName='Publish')][string]$PackageRoot,
  [Parameter(Mandatory=$true,ParameterSetName='Publish')][string]$FilesManifest,
  [Parameter(Mandatory=$true,ParameterSetName='Publish')][string]$AcceptanceProof,
  [Parameter(Mandatory=$true,ParameterSetName='Activate')][string]$ActivateVersion,
  [switch]$SynchronizeShortcuts,
  # Tests pass only owned fixture directories. Production defaults use actual
  # known Windows entry-point folders, never the taskbar's binary registry data.
  [string[]]$ShortcutDirectories,
  [string]$EvidenceDirectory
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version 3.0
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Require([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function FullLocal([string]$Path){
  Require ($Path -match '^[A-Za-z]:[\\/]') 'An absolute local Windows drive path is required.'
  return [IO.Path]::GetFullPath($Path).TrimEnd([char]92,[char]47)
}
function NoLinks([string]$Path){
  $cursor=$Path
  while(-not(Test-Path -LiteralPath $cursor)){$cursor=[IO.Path]::GetDirectoryName($cursor);Require (-not [string]::IsNullOrEmpty($cursor)) 'No existing path ancestor.'}
  $item=Get-Item -LiteralPath $cursor -Force
  while($null -ne $item){Require (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Installation paths must not traverse links/junctions.';$item=if($item -is [IO.FileInfo]){$item.Directory}else{$item.Parent}}
}
function Child([string]$Parent,[string]$Relative){
  Require (-not [IO.Path]::IsPathRooted($Relative) -and $Relative -notmatch '(^|[\\/])\.\.?([\\/]|$)|:') 'Unsafe manifest child path.'
  $result=[IO.Path]::GetFullPath((Join-Path $Parent $Relative))
  Require ($result.StartsWith($Parent.TrimEnd([char]92)+[char]92,[StringComparison]::OrdinalIgnoreCase)) 'Manifest path escaped its owner.'
  NoLinks $result
  return $result
}
function NewJson([string]$Path,$Value){
  $stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 12));$stream.Write($bytes);$stream.Flush($true)}finally{$stream.Dispose()}
}
function AtomicJson([string]$Path,$Value){
  $parent=[IO.Path]::GetDirectoryName($Path);$stage=Join-Path $parent ([guid]::NewGuid().ToString('N'))
  NewJson $stage $Value
  try{
    for($attempt=0;$attempt -lt 40;$attempt++){
      try{
        if(Test-Path -LiteralPath $Path){[IO.File]::Replace($stage,$Path,$null)}else{[IO.File]::Move($stage,$Path)}
        return
      }catch [IO.IOException],[UnauthorizedAccessException]{if($attempt -eq 39){throw};Start-Sleep -Milliseconds 25}
    }
  }finally{if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage}}
}
function VerifyFiles([string]$Directory,$Rows){
  foreach($row in $Rows){$file=Child $Directory $row.path;Require ((Test-Path -LiteralPath $file -PathType Leaf) -and
    (Get-Item -LiteralPath $file).Length -eq $row.size -and (Hash $file) -ceq $row.sha256) "Installation file mismatch: $($row.path)"}
}
function Retry([scriptblock]$Action){
  for($attempt=0;$attempt -lt 80;$attempt++){
    try{return (& $Action)}catch [IO.IOException],[UnauthorizedAccessException]{if($attempt -eq 79){throw};Start-Sleep -Milliseconds 100}
  }
}
function Maintenance([string]$Program,[string[]]$Arguments){
  $start=[Diagnostics.ProcessStartInfo]::new($Program);$start.UseShellExecute=$false
  $start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
  $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
  foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
  $process=[Diagnostics.Process]::Start($start)
  try{Require ($process.WaitForExit(15000)) 'Native shortcut maintenance timed out.';Require ($process.ExitCode -eq 0) ('Native shortcut maintenance failed: '+$process.StandardError.ReadToEnd())}finally{$process.Dispose()}
}

$root=FullLocal $InstallRoot;NoLinks $root
if(-not $EvidenceDirectory){$EvidenceDirectory=Join-Path $root ('update-records/'+[guid]::NewGuid().ToString('N'))}
$evidence=FullLocal $EvidenceDirectory;NoLinks $evidence
Require (-not(Test-Path -LiteralPath $evidence)) 'Evidence directory already exists; preserving it.'
[IO.Directory]::CreateDirectory($root)|Out-Null
[IO.Directory]::CreateDirectory($evidence)|Out-Null
$lock=$null;$stage=$null;$previous=$null;$pointer=$null;$activated=$false
$changed=[Collections.Generic.List[object]]::new()
try{
  $lock=[IO.FileStream]::new((Join-Path $root '.publish.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
  $launcher=Join-Path $root 'bin/Gaussian Scene Workbench Launcher.exe'
  $versions=Join-Path $root 'versions';[IO.Directory]::CreateDirectory($versions)|Out-Null
  $current=Join-Path $root 'current.json';$previous=$null
  if(Test-Path -LiteralPath $current){$previous=Get-Content -LiteralPath $current -Raw|ConvertFrom-Json;Copy-Item -LiteralPath $current -Destination (Join-Path $evidence 'current-before.json')}
  if($PSCmdlet.ParameterSetName -eq 'Publish'){
    $package=FullLocal $PackageRoot;NoLinks $package
    $manifest=Get-Content -LiteralPath $FilesManifest -Raw|ConvertFrom-Json
    $acceptance=Get-Content -LiteralPath $AcceptanceProof -Raw|ConvertFrom-Json
    Require ($manifest.schema -ceq 'gsw.native_release_files/1' -and $manifest.sourceCommit -match '^[a-f0-9]{40}$' -and $acceptance.sourceCommit -ceq $manifest.sourceCommit -and
      $acceptance.fullAcceptance -is [bool] -and $acceptance.fullAcceptance) 'A fully accepted, exact-source package is required.'
    Require ($manifest.acceptanceProofSha256 -ceq (Hash $AcceptanceProof)) 'Acceptance evidence changed after file verification.'
    $rows=@($manifest.files);Require ($rows.Count -gt 0) 'Empty payload manifest.'
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($row in $rows){Require ($seen.Add([string]$row.path)) 'Duplicate payload path.';Require ($row.sha256 -match '^[a-f0-9]{64}$') 'Invalid payload digest.'}
    Require (@(Get-ChildItem -LiteralPath $package -File -Recurse -Force).Count -eq $rows.Count) 'Payload file set changed.'
    VerifyFiles $package $rows
    Require ((Hash (Join-Path $package 'bin/Gaussian Scene Workbench.exe')) -ceq $acceptance.compiledExecutableSha256) 'Accepted executable differs from the payload.'
    Require ($seen.Contains('bin/Gaussian Scene Workbench.exe') -and $seen.Contains('bin/Gaussian Scene Workbench Launcher.exe')) 'Both native executables are required.'
    $payloadHash=Hash $FilesManifest
    $version='v1-'+$manifest.sourceCommit.Substring(0,12)+'-'+$payloadHash.Substring(0,12)
    $versionRoot=Child $versions $version
    if(Test-Path -LiteralPath $versionRoot){
      VerifyFiles $versionRoot $rows
      $receipt=Get-Content -LiteralPath (Join-Path $versionRoot 'installed-release.json') -Raw|ConvertFrom-Json
      Require ($receipt.payloadManifestSha256 -ceq $payloadHash) 'Existing immutable version identity differs.'
    }else{
      $stage=Child $versions ('stage-'+[guid]::NewGuid().ToString('N'))
      [IO.Directory]::CreateDirectory($stage)|Out-Null
      foreach($row in $rows){$destination=Child $stage $row.path;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))|Out-Null;Copy-Item -LiteralPath (Child $package $row.path) -Destination $destination}
      VerifyFiles $stage $rows
      $receipt=[ordered]@{schema='gsw.native_installed_release/1';sourceCommit=$manifest.sourceCommit;payloadManifestSha256=$payloadHash;files=$rows}
      NewJson (Join-Path $stage 'installed-release.json') $receipt
      # Both resolved absolute paths are checked inside this owned versions root
      # before moving the fresh directory; no old version is moved or removed.
      $null=Child $versions ([IO.Path]::GetFileName($stage));$null=Child $versions $version
      Move-Item -LiteralPath $stage -Destination $versionRoot;$stage=$null
    }
    $bootstrapReceipt=Join-Path $root 'bootstrap.json'
    if(-not(Test-Path -LiteralPath $launcher)){
      Require (-not(Test-Path -LiteralPath (Join-Path $root 'bin'))) 'Unrecognized existing bootstrap directory.'
      [IO.Directory]::CreateDirectory((Join-Path $root 'bin'))|Out-Null
      $bootstrapRows=@($rows|Where-Object{$_.path.StartsWith('bin/') -and $_.path -cne 'bin/Gaussian Scene Workbench.exe'})
      foreach($row in $bootstrapRows){$destination=Child $root $row.path;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))|Out-Null;Copy-Item -LiteralPath (Child $versionRoot $row.path) -Destination $destination}
      VerifyFiles $root $bootstrapRows
      NewJson $bootstrapReceipt ([ordered]@{schema='gsw.native_bootstrap/1';protocol=1;files=$bootstrapRows})
    }else{
      $bootstrap=Get-Content -LiteralPath $bootstrapReceipt -Raw|ConvertFrom-Json
      Require ($bootstrap.schema -ceq 'gsw.native_bootstrap/1' -and $bootstrap.protocol -eq 1) 'Unsupported bootstrap protocol.'
      # Bootstrap stays immutable on ordinary version updates. Versioned apps
      # keep their own Qt/runtime; a bootstrap migration is a separate operation.
      VerifyFiles $root $bootstrap.files
    }
  }else{
    Require ($ActivateVersion -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -and $ActivateVersion -notmatch '[.]$|^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:[.]|$)') 'Invalid installed version identity.'
    $version=$ActivateVersion;$versionRoot=Child $versions $version
    $receipt=Get-Content -LiteralPath (Join-Path $versionRoot 'installed-release.json') -Raw|ConvertFrom-Json
    Require ($receipt.schema -ceq 'gsw.native_installed_release/1') 'Invalid installed release.'
    VerifyFiles $versionRoot $receipt.files
    $bootstrap=Get-Content -LiteralPath (Join-Path $root 'bootstrap.json') -Raw|ConvertFrom-Json
    Require ($bootstrap.schema -ceq 'gsw.native_bootstrap/1' -and $bootstrap.protocol -eq 1) 'Unsupported bootstrap protocol.'
    VerifyFiles $root $bootstrap.files
  }
  $application=Join-Path $versionRoot 'bin/Gaussian Scene Workbench.exe'
  $pointer=[ordered]@{schema='gsw.native_active_version/1';version=$version;executableSha256=(Hash $application)}
  AtomicJson $current $pointer
  $activated=$true
  if($SynchronizeShortcuts){
    if(-not $ShortcutDirectories){$ShortcutDirectories=@([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('Programs'),(Join-Path $env:APPDATA 'Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar'))}
    $shell=New-Object -ComObject WScript.Shell
    try{
    foreach($directory in $ShortcutDirectories){
      if(-not(Test-Path -LiteralPath $directory -PathType Container)){continue}
      NoLinks $directory
      foreach($file in Get-ChildItem -LiteralPath $directory -Filter *.lnk -File -Recurse -ErrorAction SilentlyContinue){
        NoLinks $file.FullName
        $link=$shell.CreateShortcut($file.FullName);$target=[string]$link.TargetPath;$arguments=[string]$link.Arguments
        try{
        if([IO.Path]::GetFileName($target) -ine 'Gaussian Scene Workbench.exe' -and $target -ine $launcher){continue}
        $targetRoot=[IO.Path]::GetDirectoryName([IO.Path]::GetDirectoryName($target))
        $owned=$target -ieq $launcher
        if(-not $owned -and (Test-Path -LiteralPath (Join-Path $targetRoot 'source-manifest.json'))){
          $sourceManifest=Get-Content -LiteralPath (Join-Path $targetRoot 'source-manifest.json') -Raw|ConvertFrom-Json
          $owned=$sourceManifest.schema -ceq 'gsw-native-corresponding-source-v1' -and
            $sourceManifest.license -ceq 'GPL-3.0-or-later' -and
            (Test-Path -LiteralPath (Join-Path $targetRoot 'native/LICENSE.md') -PathType Leaf) -and
            (Hash $target) -ceq $sourceManifest.executableSha256
        }
        if(-not $owned){continue}
        $backup=Join-Path $evidence ([guid]::NewGuid().ToString('N')+'.lnk.bak')
        $before=[ordered]@{path=$file.FullName;target=$target;arguments=$arguments;sha256=(Retry {Hash $file.FullName});backup=$backup}
        Retry {Copy-Item -LiteralPath $file.FullName -Destination $backup}
        Require ((Retry {Hash $backup}) -ceq $before.sha256) 'Shortcut backup differs.'
        Retry {Maintenance $launcher @('--write-shortcut',$file.FullName,$arguments)}
        $after=$shell.CreateShortcut($file.FullName)
        try{
        Require ($after.TargetPath -ieq $launcher -and $after.Arguments -ceq $arguments) 'Stable shortcut readback differs.'
        $changed.Add([ordered]@{before=$before;target=$after.TargetPath;arguments=$after.Arguments;sha256=(Retry {Hash $file.FullName})})
        }finally{[Runtime.InteropServices.Marshal]::FinalReleaseComObject($after)|Out-Null}
        }finally{[Runtime.InteropServices.Marshal]::FinalReleaseComObject($link)|Out-Null}
      }
    }
    }finally{[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)|Out-Null}
  }
  NewJson (Join-Path $evidence 'publication.json') ([ordered]@{sourceCommit=$receipt.sourceCommit;installRoot=$root;previous=$previous;current=$pointer;changedShortcuts=$changed.ToArray();oldVersionsRemoved=0;userApplicationsStarted=0;userApplicationsStopped=0;publishedAt=(Get-Date).ToString('o')})
  Write-Output ('ACTIVE NATIVE VERSION: '+$version)
}catch{
  # A failed entry-point migration must not be reported as a failed activation
  # (nor as complete success). The old links and exact backups remain available.
  NewJson (Join-Path $evidence 'publication-failed.json') ([ordered]@{installRoot=$root;activated=$activated;current=$pointer;changedShortcuts=$changed.ToArray();error=$_.Exception.Message})
  throw
}finally{if($lock){$lock.Dispose()}}
