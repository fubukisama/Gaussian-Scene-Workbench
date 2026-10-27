#requires -Version 7.2
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
  [Parameter(Mandatory=$true)][string]$ShortcutFixture,
  [Parameter(Mandatory=$true)][string]$TestRoot
)
# Remote CI only. Fixtures and shortcut folders are owned by this test; no
# application GUI, user shortcut, taskbar registry, project or setting is touched.
$ErrorActionPreference='Stop'
Set-StrictMode -Version 3.0
$scripts=$PSScriptRoot
$root=[IO.Path]::GetFullPath($TestRoot)
if(Test-Path -LiteralPath $root){throw 'Refusing to overwrite an existing test directory.'}
[IO.Directory]::CreateDirectory($root)|Out-Null
function Check([bool]$Good,[string]$Message){if(-not $Good){throw $Message}}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Json([string]$Path,$Value){[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))}
function Plan([string]$Launcher){
  $start=[Diagnostics.ProcessStartInfo]::new($Launcher);$start.UseShellExecute=$false;$start.CreateNoWindow=$true
  $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true;$start.ArgumentList.Add('--print-launch-plan')
  $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
  $process=[Diagnostics.Process]::Start($start)
  try{Check ($process.WaitForExit(10000)) 'Launch-plan query timed out.';Check ($process.ExitCode -eq 0) 'Launch-plan query failed.';return ($process.StandardOutput.ReadToEnd()|ConvertFrom-Json)}finally{$process.Dispose()}
}
function ReadShortcut([string]$Path){
  $start=[Diagnostics.ProcessStartInfo]::new($launcher);$start.UseShellExecute=$false;$start.CreateNoWindow=$true
  $start.RedirectStandardOutput=$true;$start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
  $start.ArgumentList.Add('--read-shortcut');$start.ArgumentList.Add($Path)
  $process=[Diagnostics.Process]::Start($start)
  try{Check ($process.WaitForExit(10000)) 'Native shortcut query timed out.';Check ($process.ExitCode -eq 0) 'Native shortcut query failed.';return ($process.StandardOutput.ReadToEnd()|ConvertFrom-Json)}finally{$process.Dispose()}
}
function Publish([string]$Package,[string]$Manifest,[string]$Proof,[string]$Evidence){
  & (Join-Path $scripts 'publish_native_installation.ps1') -InstallRoot $installation -PackageRoot $Package -FilesManifest $Manifest -AcceptanceProof $Proof -SynchronizeShortcuts -ShortcutDirectories @($shortcuts) -EvidenceDirectory (Join-Path $root $Evidence)
}
$installation=Join-Path $root 'managed install 日本語 中文'
$shortcuts=Join-Path $root 'owned links';[IO.Directory]::CreateDirectory($shortcuts)|Out-Null
$package=Join-Path $root 'release one';[IO.Directory]::CreateDirectory((Join-Path $package 'bin'))|Out-Null
Get-ChildItem -LiteralPath $RuntimeDirectory -Force|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $package 'bin') -Recurse}
$app=Join-Path $package 'bin/Gaussian Scene Workbench.exe'
[IO.File]::WriteAllText($app,'not executable - owned release one fixture')
[IO.Directory]::CreateDirectory((Join-Path $package 'native'))|Out-Null
[IO.File]::WriteAllText((Join-Path $package 'native/LICENSE.md'),'owned GPL fixture')
Json (Join-Path $package 'source-manifest.json') @{schema='gsw-native-corresponding-source-v1';license='GPL-3.0-or-later';executableSha256=(Hash $app)}
$proof1=Join-Path $root 'acceptance1.json';$manifest1=Join-Path $root 'files1.json'
Json $proof1 @{sourceCommit=('1'*40);fullAcceptance=$true;compiledExecutableSha256=(Hash $app);scope='owned CI fixture only, not product acceptance'}
& (Join-Path $scripts 'write_native_release_manifest.ps1') -PackageRoot $package -SourceCommit ('1'*40) -AcceptanceProof $proof1 -OutputPath $manifest1
$linkPath=Join-Path $shortcuts 'GSW 日本語 中文.lnk'
$arguments='--project "name with spaces & ;.gsw"'
& $ShortcutFixture $linkPath $app $arguments
Check ($LASTEXITCODE -eq 0) 'Unicode shortcut fixture creation failed.'
Publish $package $manifest1 $proof1 'publish-one'
$pointer=Join-Path $installation 'current.json';$first=Get-Content -LiteralPath $pointer -Raw|ConvertFrom-Json
$launcher=Join-Path $installation 'bin/Gaussian Scene Workbench Launcher.exe'
$bootstrapHash=Hash $launcher
$plan1=Plan $launcher
Check ($plan1.valid -and $plan1.version -ceq $first.version) 'First version was not resolved.'
$link=ReadShortcut $linkPath
Check ($link.target -ieq $launcher -and $link.arguments -ceq $arguments -and $link.description -ceq 'retained user description' -and $link.showCommand -eq 3) 'Migrated shortcut lost its stable target or user options.'
Check (@(Get-ChildItem -LiteralPath (Join-Path $root 'publish-one') -Filter *.lnk.bak -File).Count -eq 1) 'Original shortcut was not backed up.'
# An unrelated executable with the same filename and no native source identity
# must not be treated as our application.
$unrelated=Join-Path $root 'unrelated/Gaussian Scene Workbench.exe'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($unrelated))|Out-Null
[IO.File]::WriteAllText($unrelated,'unrelated owned fixture')
$otherPath=Join-Path $shortcuts 'unrelated.lnk'
& $ShortcutFixture $otherPath $unrelated ''
Check ($LASTEXITCODE -eq 0) 'Unrelated shortcut fixture creation failed.'
$otherHash=Hash $otherPath
# Open the old executable for reading exactly as a running slot remains in use.
# Updating must not overwrite or remove it.
$held=[IO.File]::Open($plan1.program,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try{
  $package2=Join-Path $root 'release two';[IO.Directory]::CreateDirectory($package2)|Out-Null
  Get-ChildItem -LiteralPath $package -Force|ForEach-Object{Copy-Item -LiteralPath $_.FullName -Destination $package2 -Recurse}
  $app2=Join-Path $package2 'bin/Gaussian Scene Workbench.exe';[IO.File]::WriteAllText($app2,'not executable - owned release two fixture')
  Json (Join-Path $package2 'source-manifest.json') @{schema='gsw-native-corresponding-source-v1';license='GPL-3.0-or-later';executableSha256=(Hash $app2)}
  $proof2=Join-Path $root 'acceptance2.json';$manifest2=Join-Path $root 'files2.json'
  Json $proof2 @{sourceCommit=('2'*40);fullAcceptance=$true;compiledExecutableSha256=(Hash $app2);scope='owned CI fixture only, not product acceptance'}
  & (Join-Path $scripts 'write_native_release_manifest.ps1') -PackageRoot $package2 -SourceCommit ('2'*40) -AcceptanceProof $proof2 -OutputPath $manifest2
  Publish $package2 $manifest2 $proof2 'publish-two'
  $second=Get-Content -LiteralPath $pointer -Raw|ConvertFrom-Json;$plan2=Plan $launcher
  Check ($second.version -cne $first.version -and $plan2.program -cne $plan1.program -and $plan2.version -ceq $second.version) 'Update did not switch exactly to the new slot.'
  Check ((Hash $launcher) -ceq $bootstrapHash -and (Test-Path -LiteralPath $plan1.program)) 'Stable bootstrap or old in-use slot changed.'
  Check ((Hash $otherPath) -ceq $otherHash) 'An unrelated shortcut was changed.'
  $beforeFailure=Hash $pointer
  [IO.File]::WriteAllText($app2,'corrupt after manifest generation')
  $rejected=$false
  try{Publish $package2 $manifest2 $proof2 'rejected-corruption'}catch{$rejected=$true}
  Check ($rejected -and (Hash $pointer) -ceq $beforeFailure) 'Corrupt update changed the current pointer.'
  $rejected=$false
  try{& (Join-Path $scripts 'publish_native_installation.ps1') -InstallRoot $installation -ActivateVersion '../outside' -EvidenceDirectory (Join-Path $root 'rejected-traversal')}catch{$rejected=$true}
  Check ($rejected -and (Hash $pointer) -ceq $beforeFailure) 'Traversal update changed the current pointer.'
  & (Join-Path $scripts 'publish_native_installation.ps1') -InstallRoot $installation -ActivateVersion $first.version -EvidenceDirectory (Join-Path $root 'rollback')
  Check ((Plan $launcher).program -ceq $plan1.program) 'Rollback did not restore the selected old slot.'
  Check ((Test-Path -LiteralPath $plan2.program) -and (Hash $launcher) -ceq $bootstrapHash) 'Rollback removed a slot or changed the bootstrap.'
}finally{$held.Dispose()}
Json (Join-Path $root 'acceptance.json') @{schema='gsw.native_installation_test/1';accepted=$true;contracts=@('stable-entry','unicode-shortcuts','retained-shortcut-options','immutable-in-use-slot','exact-version-activation','corruption-rejected','traversal-rejected','rollback','unrelated-link-preserved');realUserApplicationsStarted=0;realUserApplicationsStopped=0}
Write-Output 'Native managed installation contracts PASS.'
