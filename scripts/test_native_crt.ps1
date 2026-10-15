#requires -Version 7.2
# GPL-3.0-or-later. Public CLI acceptance with test-owned, unmodified PE files.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$SourcePackageRoot,
  [Parameter(Mandatory=$true)][string]$TestRoot,
  [Parameter(Mandatory=$true)][string]$DumpBinPath
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Assert-True([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Invoke-Crt([string]$Package,[string[]]$Options=@()){
  $start=[Diagnostics.ProcessStartInfo]::new()
  $start.FileName=Join-Path $PSHOME 'pwsh.exe'
  $start.UseShellExecute=$false;$start.CreateNoWindow=$true
  $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
  foreach($argument in @('-NoLogo','-NoProfile','-NonInteractive','-File',
      (Join-Path $PSScriptRoot 'stage_native_crt.ps1'),'-PackageRoot',$Package,'-DumpBinPath',$DumpBinPath)+$Options){
    $start.ArgumentList.Add($argument)
  }
  $process=[Diagnostics.Process]::new();$process.StartInfo=$start
  try{
    [void]$process.Start()
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    Assert-True ($process.WaitForExit(60000)) 'CRT CLI timed out; no process was force-terminated.'
    [pscustomobject]@{exitCode=$process.ExitCode;stdout=$stdout.GetAwaiter().GetResult();stderr=$stderr.GetAwaiter().GetResult()}
  }finally{$process.Dispose()}
}

$SourcePackageRoot=(Resolve-Path -LiteralPath $SourcePackageRoot).Path
$TestRoot=[IO.Path]::GetFullPath($TestRoot)
Assert-True (-not (Test-Path -LiteralPath $TestRoot)) 'TestRoot must be fresh.'
New-Item -ItemType Directory -Path $TestRoot|Out-Null
$complete=Join-Path $TestRoot 'complete 程序 空白'
$bin=Join-Path $complete 'bin'
New-Item -ItemType Directory -Path $bin|Out-Null
$runtimeNames=@('MSVCP140.dll','MSVCP140_1.dll','MSVCP140_2.dll','VCRUNTIME140.dll','VCRUNTIME140_1.dll')
foreach($name in $runtimeNames){Copy-Item -LiteralPath (Join-Path $SourcePackageRoot ('bin/'+$name)) -Destination $bin}
$before=@{};foreach($name in $runtimeNames){$before[$name]=Hash (Join-Path $bin $name)}
$result=Invoke-Crt $complete @('-VerifyOnly')
Assert-True ($result.exitCode -eq 0) ('Complete signed app-local CRT verification failed: '+$result.stderr)
$report=$result.stdout|ConvertFrom-Json
Assert-True ($report.schema -eq 'gsw-native-crt-v1' -and $report.verified) 'CLI did not return a verified public CRT report.'
Assert-True ($report.peFilesInspected -eq 5 -and @($report.files).Count -eq 5) 'Not every supplied CRT PE was verified.'
Assert-True (-not(Test-Path -LiteralPath (Join-Path $complete 'native-crt-manifest.json'))) 'VerifyOnly wrote into the package.'
foreach($name in $runtimeNames){Assert-True ((Hash (Join-Path $bin $name)) -eq $before[$name]) 'VerifyOnly changed a runtime DLL.'}
Write-Output 'CRT acceptance passed: complete signed package verifies without mutation.'

function New-PartialPackage([string]$Name){
  $package=Join-Path $TestRoot $Name
  New-Item -ItemType Directory -Path (Join-Path $package 'bin')|Out-Null
  # A real, signed MSVCP140_2 PE imports MSVCP140 and VCRUNTIME; no app is run.
  Copy-Item -LiteralPath (Join-Path $SourcePackageRoot 'bin/MSVCP140_2.dll') -Destination (Join-Path $package 'bin')
  return $package
}
$missing=New-PartialPackage 'missing runtime 程序'
$result=Invoke-Crt $missing @('-VerifyOnly')
Assert-True ($result.exitCode -ne 0 -and $result.stderr -match 'not app-local') 'System-wide CRT incorrectly satisfied the app-local import gate.'
Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $missing 'bin') -File).Count -eq 1) 'Failed VerifyOnly copied a DLL.'
Assert-True (-not(Test-Path -LiteralPath (Join-Path $missing 'native-crt-manifest.json'))) 'Failed VerifyOnly wrote a success manifest.'
Write-Output 'CRT acceptance passed: missing app-local runtime is refused even on this configured developer PC.'

$incompleteRedist=Join-Path $TestRoot 'incomplete distribution/x64/Microsoft.VC143.CRT'
New-Item -ItemType Directory -Path $incompleteRedist -Force|Out-Null
foreach($name in $runtimeNames|Where-Object {$_ -ne 'VCRUNTIME140_1.dll'}){
  Copy-Item -LiteralPath (Join-Path $SourcePackageRoot ('bin/'+$name)) -Destination $incompleteRedist
}
$result=Invoke-Crt $missing @('-RedistRoot',$incompleteRedist)
Assert-True ($result.exitCode -ne 0 -and $result.stderr -match 'VCRUNTIME140_1.dll') 'Missing transitive CRT was not rejected.'
Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $missing 'bin') -File).Count -eq 1) 'An incomplete CRT closure was partially copied.'
Assert-True (-not(Test-Path -LiteralPath (Join-Path $missing 'native-crt-manifest.json'))) 'Missing closure received a success manifest.'
Write-Output 'CRT acceptance passed: incomplete transitive distribution fails before copying.'

$redist=Join-Path $TestRoot 'signed distribution/x64/Microsoft.VC143.CRT'
New-Item -ItemType Directory -Path $redist -Force|Out-Null
foreach($name in $runtimeNames){Copy-Item -LiteralPath (Join-Path $SourcePackageRoot ('bin/'+$name)) -Destination $redist}
$stage=New-PartialPackage 'stage signed closure 程序 日本語'
$originalHash=Hash (Join-Path $stage 'bin/MSVCP140_2.dll')
$result=Invoke-Crt $stage @('-RedistRoot',$redist)
Assert-True ($result.exitCode -eq 0) ('Signed CRT staging failed: '+$result.stderr)
$staged=$result.stdout|ConvertFrom-Json
Assert-True ($staged.verified -and $staged.runtimeFilesCopied -eq 4) 'The known signed fixture did not stage exactly the missing minimum five-file closure.'
$expected=$runtimeNames
foreach($name in $expected){
  $path=Join-Path $stage ('bin/'+$name)
  Assert-True (Test-Path -LiteralPath $path -PathType Leaf) ('Required staged CRT is missing: '+$name)
  Assert-True ((Hash $path) -eq (Hash (Join-Path $SourcePackageRoot ('bin/'+$name)))) ('Staging changed Microsoft bytes: '+$name)
}
Assert-True ((Hash (Join-Path $stage 'bin/MSVCP140_2.dll')) -eq $originalHash) 'Staging overwrote an existing valid CRT.'
Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $stage 'bin') -File).Count -eq 5) 'Staging copied files outside the required CRT closure.'
$manifestPath=Join-Path $stage 'native-crt-manifest.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
Assert-True ($manifest.schema -eq 'gsw-native-crt-v1' -and @($manifest.files).Count -eq 5) 'Staging did not record the actual DLL closure.'
foreach($entry in $manifest.files){
  Assert-True ($entry.version -and $entry.source -and $entry.sha256 -and $entry.authenticodeStatus -eq 'Valid') 'CRT provenance record is incomplete.'
}
$manifestHash=Hash $manifestPath
$result=Invoke-Crt $stage @('-VerifyOnly')
Assert-True ($result.exitCode -eq 0 -and (Hash $manifestPath) -eq $manifestHash) 'VerifyOnly mutated the staged provenance manifest.'
Write-Output 'CRT acceptance passed: actual signed minimum/transitive closure copied unchanged, existing DLL preserved.'

$bad=Join-Path $TestRoot 'tampered runtime/bin'
New-Item -ItemType Directory -Path $bad -Force|Out-Null
$badPath=Join-Path $bad 'MSVCP140.dll'
Copy-Item -LiteralPath (Join-Path $SourcePackageRoot 'bin/MSVCP140.dll') -Destination $badPath
$bytes=[IO.File]::ReadAllBytes($badPath);$bytes[1024]=$bytes[1024] -bxor 1;[IO.File]::WriteAllBytes($badPath,$bytes)
$result=Invoke-Crt (Split-Path -Parent $bad) @('-VerifyOnly')
Assert-True ($result.exitCode -ne 0 -and $result.stderr -match 'signature') 'A damaged Microsoft signature was accepted.'
Write-Output 'CRT acceptance passed: tampered runtime rejected.'

$renamed=Join-Path $TestRoot 'wrong signed identity/bin'
New-Item -ItemType Directory -Path $renamed -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $SourcePackageRoot 'bin/MSVCP140.dll') -Destination (Join-Path $renamed 'VCRUNTIME140.dll')
$result=Invoke-Crt (Split-Path -Parent $renamed) @('-VerifyOnly')
Assert-True ($result.exitCode -ne 0 -and $result.stderr -match 'original-filename identity') 'A signed Microsoft DLL renamed to the wrong runtime was accepted.'
Write-Output 'CRT acceptance passed: signed-but-renamed runtime identity rejected.'

$debug=Join-Path $TestRoot 'debug runtime/bin'
New-Item -ItemType Directory -Path $debug -Force|Out-Null
Copy-Item -LiteralPath (Join-Path $SourcePackageRoot 'bin/MSVCP140.dll') -Destination (Join-Path $debug 'MSVCP140d.dll')
$result=Invoke-Crt (Split-Path -Parent $debug) @('-VerifyOnly')
Assert-True ($result.exitCode -ne 0 -and $result.stderr -match 'Debug MSVC runtime') 'A debug runtime filename was accepted.'
$result=Invoke-Crt $missing @('-RedistRoot',(Join-Path $env:WINDIR 'System32'))
Assert-True ($result.exitCode -ne 0 -and $result.stderr -match 'RedistRoot must be') 'System32 was accepted as a redistribution source.'
Write-Output 'CRT acceptance passed: debug distribution and System32 sources refused.'

$evidence=[ordered]@{schema='gsw-native-crt-acceptance-v1';passed=$true;sourcePackage=$SourcePackageRoot
  completeVerifyOnly=$true;missingVerifyOnlyRejected=$true;partialClosureCopiedNothing=$true
  signedClosureStaged=$true;existingCRTUnchanged=$true;onlyRequiredClosureCopied=$true
  damagedSignatureRejected=$true;renamedMicrosoftDLLRejected=$true;debugRejected=$true;system32SourceRejected=$true
  applicationLaunched=$false;globalRuntimeChanged=$false;finishedAtUtc=[DateTime]::UtcNow.ToString('o')}
[IO.File]::WriteAllText((Join-Path $TestRoot 'crt-acceptance.json'),($evidence|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Write-Output ('CRT acceptance evidence: '+(Join-Path $TestRoot 'crt-acceptance.json'))
