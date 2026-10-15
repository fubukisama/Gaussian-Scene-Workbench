#requires -Version 7.2
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$PackageRoot,
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [string]$PythonExecutable='python',
  [string]$InnoCompiler='',
  [switch]$TestBuild,
  [string]$CertificateThumbprint=''
)
$ErrorActionPreference='Stop'
$sourceRoot=Split-Path -Parent $PSScriptRoot
$PackageRoot=(Resolve-Path -LiteralPath $PackageRoot).Path
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $InnoCompiler){$InnoCompiler=$env:GSW_INNO_COMPILER}
if(-not $InnoCompiler -and (Test-Path 'D:/Apps/InnoSetup/7.1.0/ISCC.exe')){$InnoCompiler='D:/Apps/InnoSetup/7.1.0/ISCC.exe'}
if(-not $InnoCompiler){$command=Get-Command ISCC.exe -ErrorAction SilentlyContinue;if($command){$InnoCompiler=$command.Source}}
if(-not $InnoCompiler -or -not(Test-Path -LiteralPath $InnoCompiler -PathType Leaf)){throw 'Inno Setup 7.1.0 is required. Run scripts/bootstrap_inno_setup.ps1 first.'}
$compilerVersion=(& $InnoCompiler '--version') -join "`n"
if($LASTEXITCODE -ne 0 -or $compilerVersion -notmatch '(?m)^7\.1\.0(?:\s|$)'){throw "Expected pinned Inno Setup 7.1.0; got $compilerVersion"}
$work=Join-Path $OutputDirectory ('build-'+[guid]::NewGuid().ToString('N'))
$arguments=@('-I','-B',(Join-Path $PSScriptRoot 'prepare_windows_installer.py'),'--package-root',$PackageRoot,
  '--work-root',$work,'--output-root',$OutputDirectory,'--source-root',$sourceRoot)
if($TestBuild){$arguments+='--test-build'}
& $PythonExecutable @arguments
if($LASTEXITCODE -ne 0){throw 'Native installer preparation failed.'}
$record=Get-Content -LiteralPath (Join-Path $work 'installer-build.json') -Raw|ConvertFrom-Json
if(Test-Path -LiteralPath $record.setupPath){throw 'Installer output already exists; choose a fresh output directory.'}
if(-not $record.installerFixture){
  $crt=(& (Join-Path $PSScriptRoot 'stage_native_crt.ps1') -PackageRoot $record.payloadRoot -VerifyOnly) -join "`n"
  [IO.File]::WriteAllText((Join-Path $record.payloadRoot 'native-crt-manifest.json'),$crt,[Text.UTF8Encoding]::new($false))
}
& (Join-Path $PSScriptRoot 'write_native_source_manifest.ps1') -PackageRoot $record.payloadRoot
if($LASTEXITCODE -and $LASTEXITCODE -ne 0){throw 'Installer source manifest failed.'}
& $PythonExecutable -I -B (Join-Path $PSScriptRoot 'verify_native_backend.py') --root $record.payloadRoot --package
if($LASTEXITCODE -ne 0){throw 'Staged installer package boundary failed.'}
$files=@(Get-ChildItem -LiteralPath $record.payloadRoot -Recurse -File|Sort-Object FullName|ForEach-Object{
  [ordered]@{path=$_.FullName.Substring($record.payloadRoot.Length+1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$ownedPaths=@($files.path)+@('gsw-owned-files.txt','installer-payload.json')
[IO.File]::WriteAllLines((Join-Path $record.payloadRoot 'gsw-owned-files.txt'),[string[]]$ownedPaths,[Text.UTF8Encoding]::new($true))
$files=@(Get-ChildItem -LiteralPath $record.payloadRoot -Recurse -File|Sort-Object FullName|ForEach-Object{
  [ordered]@{path=$_.FullName.Substring($record.payloadRoot.Length+1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$payloadManifest=[ordered]@{schema='gsw-installer-payload-v1';executableSha256=$record.executableSha256;files=$files}
[IO.File]::WriteAllText((Join-Path $record.payloadRoot 'installer-payload.json'),($payloadManifest|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
& $InnoCompiler '--quiet-progress' (Join-Path $work 'gsw.iss')
if($LASTEXITCODE -ne 0){throw 'Inno Setup compilation failed.'}
if(-not(Test-Path -LiteralPath $record.setupPath -PathType Leaf)){throw 'Setup EXE was not produced.'}
if($CertificateThumbprint){
  & (Join-Path $PSScriptRoot 'sign_windows_artifacts.ps1') -Path $record.setupPath -CertificateThumbprint $CertificateThumbprint
}
$hash=(Get-FileHash -LiteralPath $record.setupPath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($record.setupPath+'.sha256'),("$hash  "+[IO.Path]::GetFileName($record.setupPath)+"`n"),[Text.UTF8Encoding]::new($false))
$record|Add-Member -NotePropertyName setupSha256 -NotePropertyValue $hash
$record|Add-Member -NotePropertyName setupBytes -NotePropertyValue (Get-Item -LiteralPath $record.setupPath).Length
$record|Add-Member -NotePropertyName authenticodeStatus -NotePropertyValue ([string](Get-AuthenticodeSignature -LiteralPath $record.setupPath).Status)
$record|Add-Member -NotePropertyName builtAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
[IO.File]::WriteAllText(($record.setupPath+'.json'),($record|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
Write-Output "Native Setup ready: $($record.setupPath)"
Write-Output "SHA256: $hash; signature: $($record.authenticodeStatus); no application was launched."
