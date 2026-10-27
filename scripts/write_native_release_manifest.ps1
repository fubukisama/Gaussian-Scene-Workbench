#requires -Version 7.2
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$PackageRoot,
  [Parameter(Mandatory=$true)][ValidatePattern('^[a-f0-9]{40}$')][string]$SourceCommit,
  [Parameter(Mandatory=$true)][string]$AcceptanceProof,
  [Parameter(Mandatory=$true)][string]$OutputPath
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 3.0
$root=(Resolve-Path -LiteralPath $PackageRoot).Path.TrimEnd([char]92)
$output=[IO.Path]::GetFullPath($OutputPath)
if($output.StartsWith($root+[char]92,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)){
  throw 'Use a fresh manifest path outside the immutable package.'
}
$proof=Get-Content -LiteralPath $AcceptanceProof -Raw|ConvertFrom-Json
if($proof.sourceCommit -cne $SourceCommit -or $proof.fullAcceptance -isnot [bool] -or -not $proof.fullAcceptance){
  throw 'Exact-source full remote acceptance is required; scoped/pending runs cannot be published.'
}
$exe=Join-Path $root 'bin/Gaussian Scene Workbench.exe'
$exeHash=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
if($proof.compiledExecutableSha256 -cne $exeHash){throw 'Accepted executable differs from the package.'}
$files=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force|Sort-Object FullName|ForEach-Object{
  [ordered]@{path=$_.FullName.Substring($root.Length+1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest=[ordered]@{schema='gsw.native_release_files/1';sourceCommit=$SourceCommit;acceptanceProofSha256=(Get-FileHash -LiteralPath $AcceptanceProof -Algorithm SHA256).Hash.ToLowerInvariant();files=$files}
$stream=[IO.File]::Open($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($manifest|ConvertTo-Json -Depth 8));$stream.Write($bytes);$stream.Flush($true)}finally{$stream.Dispose()}
Write-Output ('NATIVE RELEASE FILES: '+$files.Count)
