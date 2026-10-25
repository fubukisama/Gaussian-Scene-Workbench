[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$Executable,
  [Parameter(Mandatory=$true)][string]$PythonExecutable,
  [Parameter(Mandatory=$true)][string]$TestRoot,
  [ValidateSet('windows','offscreen')][string]$Platform='windows'
)
$ErrorActionPreference='Stop'
$Root=Split-Path -Parent $PSScriptRoot
$BuildDirectory=[IO.Path]::GetFullPath($TestRoot)
if(Test-Path -LiteralPath $BuildDirectory){throw 'A fresh owned startup-test directory is required.'}
New-Item -ItemType Directory -Path $BuildDirectory | Out-Null
$PackageSmokePlatform=$Platform
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Root 'scripts/build_native.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Trusted build script parser failed.'}
$functions=@($ast.EndBlock.Statements | Where-Object {
  $_ -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'Invoke-NativeLaunchSmokeTest'
})
if($functions.Count -ne 1){throw 'Expected exactly one package launch gate.'}
. ([scriptblock]::Create($functions[0].Extent.Text))
$names=@('PATH','QT_OPENGL','QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH')
$before=@{}; foreach($name in $names){$before[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$NativeExecutable=[IO.Path]::GetFullPath($Executable)
$NativeBin=[IO.Path]::GetDirectoryName($NativeExecutable)
$SettingsRoot=Join-Path $NativeBin 'test-settings'
$SettingsRootExisted=Test-Path -LiteralPath $SettingsRoot
Invoke-NativeLaunchSmokeTest -Executable $NativeExecutable
if(-not $SettingsRootExisted -and (Test-Path -LiteralPath $SettingsRoot)){
  throw 'Actual package startup left its newly created smoke-settings parent in the payload.'
}
foreach($name in $names){
  if($before[$name] -cne [Environment]::GetEnvironmentVariable($name,'Process')){throw ('Parent environment changed after successful startup: '+$name)}
}
$rejected=$false
try {
  # The real CPU interpreter rejects the application's fixed option with exit2.
  # It does not execute any Python script, model, library import or training.
  Invoke-NativeLaunchSmokeTest -Executable ([IO.Path]::GetFullPath($PythonExecutable))
} catch {
  if($_.Exception.Message -notmatch 'exit code 2 \(0x00000002\)'){throw}
  $rejected=$true
}
if(-not $rejected){throw 'A failing process was accepted as a successful application startup.'}
foreach($name in $names){
  if($before[$name] -cne [Environment]::GetEnvironmentVariable($name,'Process')){throw ('Parent environment changed after failed startup: '+$name)}
}
# Exercise preservation with the real executable and runtime, but only in this
# test's fresh copy. Never place sentinel data in the real installed package.
if(@(Get-ChildItem -LiteralPath $NativeBin -Force -Recurse |
    Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count){
  throw 'A copied startup fixture must not contain reparse entries.'
}
$SentinelPayload=Join-Path $BuildDirectory 'preexisting-settings-payload'
$SentinelBin=Join-Path $SentinelPayload 'bin'
New-Item -ItemType Directory -Path $SentinelPayload | Out-Null
Copy-Item -LiteralPath $NativeBin -Destination $SentinelBin -Recurse
$SentinelSettings=Join-Path $SentinelBin 'test-settings'
if(Test-Path -LiteralPath $SentinelSettings){throw 'Sentinel fixture requires a fresh settings root.'}
New-Item -ItemType Directory -Path $SentinelSettings | Out-Null
$SentinelExecutable=Join-Path $SentinelBin ([IO.Path]::GetFileName($NativeExecutable))
Invoke-NativeLaunchSmokeTest -Executable $SentinelExecutable
if(-not (Test-Path -LiteralPath $SentinelSettings -PathType Container) -or
   [IO.Directory]::GetFileSystemEntries($SentinelSettings).Length -ne 0){
  throw 'Actual startup did not preserve a pre-existing empty test-settings parent.'
}
$SentinelPath=Join-Path $SentinelSettings 'preexisting-sentinel.txt'
$SentinelContent='Owned startup fixture: pre-existing settings data must survive.'
[IO.File]::WriteAllText($SentinelPath,$SentinelContent,[Text.UTF8Encoding]::new($false))
$SentinelHash=(Get-FileHash -LiteralPath $SentinelPath).Hash
Invoke-NativeLaunchSmokeTest -Executable $SentinelExecutable
if(-not (Test-Path -LiteralPath $SentinelSettings -PathType Container) -or
   -not (Test-Path -LiteralPath $SentinelPath -PathType Leaf) -or
   (Get-FileHash -LiteralPath $SentinelPath).Hash -cne $SentinelHash){
  throw 'Actual startup changed pre-existing test-settings sentinel data.'
}
foreach($name in $names){
  if($before[$name] -cne [Environment]::GetEnvironmentVariable($name,'Process')){throw ('Parent environment changed after sentinel startup: '+$name)}
}
$logs=@(Get-ChildItem -LiteralPath (Join-Path $BuildDirectory 'packaged-startup') -File)
if($logs.Count -ne 8){throw 'All four actual processes must retain stdout and stderr.'}
if(-not ($logs | Where-Object { $_.Name -like '*.stderr.log' -and (Get-Content -LiteralPath $_.FullName -Raw) -match 'unknown option' })){
  throw 'The actual negative-process diagnostic was not captured.'
}
$receipt=[ordered]@{
  schema='gsw.native_package_launch_test/1'; positiveExit=0; negativeExit=2;
  platform=$Platform; parentEnvironmentUnchanged=$true;
  freshSettingsParentAbsentAfterStartup=(-not $SettingsRootExisted -and -not (Test-Path -LiteralPath $SettingsRoot));
  preexistingEmptySettingsPreserved=$true;
  preexistingSettingsPreserved=$true; sentinelSha256=$SentinelHash.ToLowerInvariant();
  logs=@($logs | ForEach-Object {[ordered]@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}})
}
[IO.File]::WriteAllText((Join-Path $BuildDirectory 'receipt.json'),($receipt | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Write-Output 'Package startup, actual failure rejection, clean fresh settings, preserved pre-existing sentinel, captured diagnostics and unchanged parent environment passed.'
