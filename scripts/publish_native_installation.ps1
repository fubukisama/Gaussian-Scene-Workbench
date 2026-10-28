#requires -Version 7.2
[CmdletBinding(DefaultParameterSetName='Publish')]
param(
  [Parameter(Mandatory=$true)][string]$InstallRoot,
  # Legacy directory inputs are admitted only for the first trusted bootstrap.
  [Parameter(ParameterSetName='Publish')][string]$PackageRoot,
  [Parameter(ParameterSetName='Publish')][string]$FilesManifest,
  [Parameter(ParameterSetName='Publish')][string]$AcceptanceProof,
  [Parameter(Mandatory=$true,ParameterSetName='Activate')][string]$ActivateVersion,
  [string]$Archive,
  [string]$Offer,
  [string]$DeveloperPublisher,
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
  $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false);$start.StandardErrorEncoding=[Text.UTF8Encoding]::new($false)
  foreach($argument in $Arguments){$start.ArgumentList.Add($argument)}
  $process=[Diagnostics.Process]::Start($start)
  $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
  try{Require ($process.WaitForExit(60000)) 'native_maintenance_timed_out';Require ($process.ExitCode -eq 0) ('native_maintenance_failed: '+$stdout.GetAwaiter().GetResult()+' '+$stderr.GetAwaiter().GetResult());return $stdout.GetAwaiter().GetResult()}finally{$process.Dispose()}
}
function NoTrustRoot([string]$Root){
  foreach($relative in @('.update-trust-anchor','updates/trust')){
    try{$null=[IO.File]::GetAttributes((Join-Path $Root $relative));throw 'developer_trusted_root_forbidden'}
    catch [IO.FileNotFoundException],[IO.DirectoryNotFoundException]{}
  }
}

# Missing migration inputs are rejected before creating the installation,
# evidence, locks or any pointer. Never silently use the retired PS activator.
Require (-not [string]::IsNullOrEmpty($Archive) -and -not [string]::IsNullOrEmpty($Offer) -and -not [string]::IsNullOrEmpty($DeveloperPublisher)) 'native_publisher_inputs_required | 请提供 Archive、Offer、DeveloperPublisher；旧脚本激活已停用。 | Supply Archive, Offer and DeveloperPublisher; the old script activator is retired. | Archive・Offer・DeveloperPublisher が必要です。旧スクリプトによる切替は廃止されました。'
$archivePath=FullLocal $Archive;$offerPath=FullLocal $Offer;$publisherPath=FullLocal $DeveloperPublisher
foreach($inputPath in @($archivePath,$offerPath,$publisherPath)){NoLinks $inputPath;Require (Test-Path -LiteralPath $inputPath -PathType Leaf) 'native_publisher_input_missing'}
$descriptor=Get-Content -LiteralPath $offerPath -Raw -Encoding utf8|ConvertFrom-Json
Require ($descriptor.schema -ceq 'gsw.update_offer/1') 'native_publisher_offer_required'
if($PSCmdlet.ParameterSetName -eq 'Activate'){
  Require ($descriptor.buildId -ceq $ActivateVersion) 'native_publisher_archive_version_mismatch | 回退须提供该版本的原始完整 ZIP 和 Offer。 | Re-activation requires that version''s original complete ZIP and Offer. | 再切替には対象版の元の完全 ZIP と Offer が必要です。'
}

$root=FullLocal $InstallRoot;NoLinks $root
NoTrustRoot $root
if(-not $EvidenceDirectory){$EvidenceDirectory=Join-Path $root ('update-records/'+[guid]::NewGuid().ToString('N'))}
$evidence=FullLocal $EvidenceDirectory;NoLinks $evidence
Require (-not(Test-Path -LiteralPath $evidence)) 'Evidence directory already exists; preserving it.'
[IO.Directory]::CreateDirectory($root)|Out-Null
[IO.Directory]::CreateDirectory($evidence)|Out-Null
$previous=$null;$pointer=$null;$activated=$false;$publication=$null
$changed=[Collections.Generic.List[object]]::new()
try{
  $launcher=Join-Path $root 'bin/Gaussian Scene Workbench Launcher.exe'
  $current=Join-Path $root 'current.json';$previous=$null
  if(Test-Path -LiteralPath $current){
    Copy-Item -LiteralPath $current -Destination (Join-Path $evidence 'current-before.json')
    try{$previous=Get-Content -LiteralPath $current -Raw|ConvertFrom-Json}catch{$previous=$null}
  }
  if(-not(Test-Path -LiteralPath $launcher)){
    Require ($PSCmdlet.ParameterSetName -eq 'Publish' -and $PackageRoot -and $FilesManifest -and $AcceptanceProof) 'native_first_bootstrap_inputs_required'
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
    # This lock belongs only to first bootstrap. Release it before the native
    # publisher takes the one shared lock for its whole activation operation.
    $bootstrapLock=[IO.FileStream]::new((Join-Path $root '.publish.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try{
      NoTrustRoot $root
      Require (-not(Test-Path -LiteralPath (Join-Path $root 'bootstrap.json'))) 'native_bootstrap_already_exists'
      Require (-not(Test-Path -LiteralPath (Join-Path $root 'bin'))) 'Unrecognized existing bootstrap directory.'
      [IO.Directory]::CreateDirectory((Join-Path $root 'bin'))|Out-Null
      $bootstrapRows=@($rows|Where-Object{$_.path.StartsWith('bin/') -and $_.path -cne 'bin/Gaussian Scene Workbench.exe'})
      foreach($row in $bootstrapRows){$destination=Child $root $row.path;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))|Out-Null;Copy-Item -LiteralPath (Child $package $row.path) -Destination $destination}
      VerifyFiles $root $bootstrapRows
      NewJson (Join-Path $root 'bootstrap.json') ([ordered]@{schema='gsw.native_bootstrap/1';protocol=1;files=$bootstrapRows})
    }finally{$bootstrapLock.Dispose()}
  }
  # No PowerShell version-slot copy, receipt admission, pointer writer or second
  # operation-wide lock remains. Core independently validates every ZIP byte.
  $publication=(Maintenance $publisherPath @('--publish-unsigned-local',$root,$archivePath,$offerPath))|ConvertFrom-Json
  Require ($publication.schema -ceq 'gsw.native_developer_publication/1' -and $publication.ok -eq $true -and
    $publication.publisherAuthenticated -eq $false -and $publication.admission -ceq 'local-developer-unsigned') 'native_publisher_response_invalid'
  $activated=$true
  $pointer=Get-Content -LiteralPath $current -Raw|ConvertFrom-Json
  $version=$publication.buildId
  Require ($pointer.version -ceq $version) 'native_publisher_pointer_readback_differs'
  if($SynchronizeShortcuts){
    if(-not $ShortcutDirectories){$ShortcutDirectories=@([Environment]::GetFolderPath('Desktop'),[Environment]::GetFolderPath('Programs'),(Join-Path $env:APPDATA 'Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar'))}
    foreach($directory in $ShortcutDirectories){
      if(-not(Test-Path -LiteralPath $directory -PathType Container)){continue}
      NoLinks $directory
      foreach($file in Get-ChildItem -LiteralPath $directory -Filter *.lnk -File -Recurse -ErrorAction SilentlyContinue){
        NoLinks $file.FullName
        $link=(Maintenance $launcher @('--read-shortcut',$file.FullName))|ConvertFrom-Json
        $target=[string]$link.target;$arguments=[string]$link.arguments
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
        $null=Retry {Maintenance $launcher @('--write-shortcut',$file.FullName,$arguments)}
        $after=(Maintenance $launcher @('--read-shortcut',$file.FullName))|ConvertFrom-Json
        Require ($after.target -ieq $launcher -and $after.arguments -ceq $arguments -and
          $after.description -ceq $link.description -and $after.hotkey -eq $link.hotkey -and $after.showCommand -eq $link.showCommand) 'Stable shortcut readback differs.'
        $changed.Add([ordered]@{before=$before;target=$after.target;arguments=$after.arguments;sha256=(Retry {Hash $file.FullName})})
      }
    }
  }
  NewJson (Join-Path $evidence 'publication.json') ([ordered]@{sourceCommit=$descriptor.sourceCommit;installRoot=$root;previous=$previous;current=$pointer;nativePublication=$publication;publisherAuthenticated=$false;changedShortcuts=$changed.ToArray();oldVersionsRemoved=0;userApplicationsStarted=0;userApplicationsStopped=0;publishedAt=(Get-Date).ToString('o')})
  Write-Output ($publication|ConvertTo-Json -Compress)
}catch{
  # A failed entry-point migration must not be reported as a failed activation
  # (nor as complete success). The old links and exact backups remain available.
  NewJson (Join-Path $evidence 'publication-failed.json') ([ordered]@{installRoot=$root;activated=$activated;current=$pointer;changedShortcuts=$changed.ToArray();error=$_.Exception.Message})
  throw
}
