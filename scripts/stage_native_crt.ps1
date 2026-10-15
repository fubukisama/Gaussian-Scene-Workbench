#requires -Version 7.2
# GPL-3.0-or-later. This script does not change the Microsoft DLL licenses.
# App-local redistribution is conditional on the applicable Visual Studio terms:
# https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution
# https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files
# Never source DLLs from System32, WinSxS, debug_nonredist or a global runtime.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$PackageRoot,
  [string]$DumpBinPath='',
  [string]$RedistRoot='',
  [switch]$VerifyOnly
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT){throw 'Native MSVC runtime staging requires Windows.'}
$runtimePattern='^(?:msvcp|msvcr|vcruntime|concrt|vcomp|vccorlib)[0-9][a-z0-9_]*\.dll$'
$debugPattern='^(?:msvcp|msvcr|vcruntime|concrt|vcomp|vccorlib)[0-9]+(?:d(?:_[a-z0-9_]+)?|_[0-9]+d)\.dll$'
$minimumRuntimeNames=@('MSVCP140.dll','MSVCP140_1.dll','MSVCP140_2.dll','VCRUNTIME140.dll','VCRUNTIME140_1.dll')

function Assert-NoReparsePath([string]$Path){
  $cursor=[IO.Path]::GetFullPath($Path)
  while($cursor){
    if(Test-Path -LiteralPath $cursor){
      if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){
        throw "Reparse paths are not permitted for CRT inputs or outputs: $cursor"
      }
    }
    $parent=[IO.Path]::GetDirectoryName($cursor)
    if(-not $parent -or $parent -eq $cursor){break};$cursor=$parent
  }
}
function File-Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Assert-MicrosoftSignature([string]$Path){
  $signature=Get-AuthenticodeSignature -LiteralPath $Path
  if($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate -or
      $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)'){
    throw "A valid Microsoft Authenticode signature is required: $Path ($($signature.Status))"
  }
  return $signature.SignerCertificate.Subject
}
function Pe-Machine([string]$Path){
  $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  $reader=[IO.BinaryReader]::new($stream)
  try{
    if($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D){throw "Not a PE file: $Path"}
    [void]$stream.Seek(0x3C,[IO.SeekOrigin]::Begin);$offset=$reader.ReadUInt32()
    if($offset -gt $stream.Length-6){throw "Invalid PE header offset: $Path"}
    [void]$stream.Seek($offset,[IO.SeekOrigin]::Begin)
    if($reader.ReadUInt32() -ne 0x4550){throw "Invalid PE signature: $Path"}
    return $reader.ReadUInt16()
  }finally{$reader.Dispose()}
}
function VisualStudio-Roots{
  $roots=[Collections.Generic.List[string]]::new()
  if($env:VCINSTALLDIR){$roots.Add((Split-Path -Parent $env:VCINSTALLDIR.TrimEnd('\','/')))}
  $vswhere=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
  if(Test-Path -LiteralPath $vswhere -PathType Leaf){
    Assert-NoReparsePath $vswhere;[void](Assert-MicrosoftSignature $vswhere)
    $text=& $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
    if($LASTEXITCODE -ne 0){throw 'Official Visual Studio discovery failed.'}
    foreach($installation in @(($text -join "`n")|ConvertFrom-Json)){
      if($installation.installationPath){$roots.Add([string]$installation.installationPath)}
    }
  }
  return @($roots|Select-Object -Unique)
}
function Resolve-DumpBin([string]$Requested){
  $candidates=[Collections.Generic.List[string]]::new()
  if($Requested){$candidates.Add($Requested)}else{
    if($env:GSW_DUMPBIN_PATH){$candidates.Add($env:GSW_DUMPBIN_PATH)}
    $command=Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if($command){$candidates.Add($command.Source)}
    if($env:VCToolsInstallDir){$candidates.Add((Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/dumpbin.exe'))}
    foreach($root in VisualStudio-Roots){
      $tools=Join-Path $root 'VC/Tools/MSVC'
      if(Test-Path -LiteralPath $tools -PathType Container){
        foreach($version in Get-ChildItem -LiteralPath $tools -Directory|Sort-Object Name -Descending){
          $candidates.Add((Join-Path $version.FullName 'bin/Hostx64/x64/dumpbin.exe'))
        }
      }
    }
    # Existing portable Microsoft tools are a read-only inspector fallback on
    # this D-drive workstation, not a source of redistributable DLLs.
    $portableTools='D:/Apps/PortableMSVC-2022/msvc/VC/Tools/MSVC'
    if(Test-Path -LiteralPath $portableTools -PathType Container){
      foreach($version in Get-ChildItem -LiteralPath $portableTools -Directory|Sort-Object Name -Descending){
        $candidates.Add((Join-Path $version.FullName 'bin/Hostx64/x64/dumpbin.exe'))
      }
    }
  }
  foreach($candidate in $candidates){
    if(-not(Test-Path -LiteralPath $candidate -PathType Leaf)){continue}
    Assert-NoReparsePath $candidate
    if([IO.Path]::GetFileName($candidate) -ine 'dumpbin.exe'){throw 'The PE inspector must be the official dumpbin.exe.'}
    [void](Assert-MicrosoftSignature $candidate)
    if([IO.Path]::GetFileName([string](Get-Item -LiteralPath $candidate).VersionInfo.OriginalFilename) -ine 'dumpbin.exe'){
      throw 'The signed PE inspector original filename must identify Microsoft dumpbin.exe.'
    }
    return (Resolve-Path -LiteralPath $candidate).Path
  }
  throw 'Official MSVC dumpbin.exe is required; pass -DumpBinPath or use a Visual Studio build environment.'
}
function Assert-RedistRoot([string]$Path){
  $full=[IO.Path]::GetFullPath($Path).TrimEnd('\','/')
  if([IO.Path]::GetFileName($full) -ine 'Microsoft.VC143.CRT' -or
      $full -match '(?i)(?:^|[\\/])(?:system32|syswow64|winsxs|debug_nonredist)(?:[\\/]|$)'){
    throw 'RedistRoot must be a Microsoft.VC143.CRT distribution folder, never a Windows/global/debug runtime folder.'
  }
  if(-not(Test-Path -LiteralPath $full -PathType Container)){throw "MSVC redistribution folder is missing: $full"}
  Assert-NoReparsePath $full
  return (Resolve-Path -LiteralPath $full).Path
}
function Resolve-RedistRoots([string]$Requested){
  if($Requested){return ,(Assert-RedistRoot $Requested)}
  $candidates=[Collections.Generic.List[string]]::new()
  if($env:VCToolsRedistDir){$candidates.Add((Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'))}
  foreach($vsRoot in VisualStudio-Roots){
    $parent=Join-Path $vsRoot 'VC/Redist/MSVC'
    if(Test-Path -LiteralPath $parent -PathType Container){
      foreach($version in Get-ChildItem -LiteralPath $parent -Directory|Sort-Object Name -Descending){
        $candidates.Add((Join-Path $version.FullName 'x64/Microsoft.VC143.CRT'))
      }
    }
  }
  $roots=[Collections.Generic.List[string]]::new()
  foreach($candidate in $candidates|Select-Object -Unique){
    if(Test-Path -LiteralPath $candidate -PathType Container){$roots.Add((Assert-RedistRoot $candidate))}
  }
  return @($roots)
}
function Runtime-Facts([string]$Path,[string]$SourceType){
  Assert-NoReparsePath $Path
  $item=Get-Item -LiteralPath $Path -Force
  if($item.Name -match $debugPattern){throw "Debug MSVC runtime cannot be redistributed: $Path"}
  if((Pe-Machine $Path) -ne 0x8664){throw "MSVC runtime must be an x64 PE: $Path"}
  $publisher=Assert-MicrosoftSignature $Path
  if(-not $item.VersionInfo.FileVersion){throw "MSVC runtime has no file version: $Path"}
  if([IO.Path]::GetFileName([string]$item.VersionInfo.OriginalFilename) -ine $item.Name){
    throw "Microsoft CRT original-filename identity differs from its package filename: $Path"
  }
  return [ordered]@{name=$item.Name;version=$item.VersionInfo.FileVersion;sha256=File-Hash $Path
    bytes=$item.Length;architecture='x64';authenticodeStatus='Valid';publisher=$publisher
    originalFilename=$item.VersionInfo.OriginalFilename
    sourceType=$SourceType;source=$item.FullName;sourceDirectory=$item.DirectoryName}
}
function Pe-Imports([string]$Path){
  [void](Pe-Machine $Path)
  $lines=@(& $script:Inspector /nologo /dependents $Path 2>&1)
  if($LASTEXITCODE -ne 0){throw "PE import inspection failed: $Path`n$($lines -join "`n")"}
  $imports=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach($line in $lines){
    if([string]$line -match '^\s+([A-Za-z0-9_.+\-]+\.dll)\s*$'){[void]$imports.Add($Matches[1])}
  }
  return @($imports|Sort-Object)
}

$PackageRoot=(Resolve-Path -LiteralPath $PackageRoot).Path
Assert-NoReparsePath $PackageRoot
$bin=Join-Path $PackageRoot 'bin'
if(-not(Test-Path -LiteralPath $bin -PathType Container)){throw 'PackageRoot/bin is required.'}
Assert-NoReparsePath $bin
$all=@(Get-ChildItem -LiteralPath $bin -Recurse -Force)
if(@($all|Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count){throw 'The package bin tree contains a reparse point.'}
$binaries=@($all|Where-Object {-not $_.PSIsContainer -and $_.Extension -in '.exe','.dll'})
if(-not $binaries.Count){throw 'The package contains no EXE/DLL files to inspect.'}
$script:Inspector=Resolve-DumpBin $DumpBinPath
if($RedistRoot){$RedistRoot=Assert-RedistRoot $RedistRoot}
$facts=@{};$planned=@{};$required=@{};$nonCrt=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$queue=[Collections.Generic.Queue[string]]::new()
foreach($binary in $binaries){
  $queue.Enqueue($binary.FullName)
  if($binary.Name -match $runtimePattern){
    if(-not $binary.DirectoryName.Equals($bin,[StringComparison]::OrdinalIgnoreCase)){
      throw "MSVC runtime must be app-local in the executable bin directory: $($binary.FullName)"
    }
    $facts[$binary.Name]=Runtime-Facts $binary.FullName 'existing-app-local-origin-not-asserted'
  }
}
$processed=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$resolution=@{redistRoots=$null}
function Require-Runtime([string]$Dependency,[string]$Importer){
  if($Dependency -match $debugPattern){throw "A production PE imports a nonredistributable debug runtime: $Dependency"}
  if(-not $required.ContainsKey($Dependency)){$required[$Dependency]=[Collections.Generic.List[string]]::new()}
  $required[$Dependency].Add($Importer)
  if($facts.ContainsKey($Dependency) -or $planned.ContainsKey($Dependency)){return}
  if($VerifyOnly){throw "Required MSVC runtime is not app-local in bin: $Dependency (system installations do not satisfy this gate)."}
  if($null -eq $resolution.redistRoots){$resolution.redistRoots=@(Resolve-RedistRoots $RedistRoot)}
  $source=$null
  foreach($root in $resolution.redistRoots){
    $candidate=Join-Path $root $Dependency
    if(Test-Path -LiteralPath $candidate -PathType Leaf){$source=$candidate;break}
  }
  if(-not $source){throw "Required MSVC runtime closure is unavailable in official redistribution folders: $Dependency. No files have been copied."}
  $sourceType=if($RedistRoot){'explicit-redist-subject-to-visual-studio-license'}else{'visual-studio-redist-subject-to-license'}
  $planned[$Dependency]=Runtime-Facts $source $sourceType
  $queue.Enqueue($source)
}
# Keep the package's stable five-file baseline even when a particular Qt build
# happens not to import one member. Discover all additional numeric CRT imports.
foreach($name in $minimumRuntimeNames){Require-Runtime $name 'minimum-native-package-contract'}
while($queue.Count){
  $binary=$queue.Dequeue();if(-not $processed.Add($binary)){continue}
  foreach($dependency in Pe-Imports $binary){
    if($dependency -notmatch $runtimePattern){[void]$nonCrt.Add($dependency);continue}
    Require-Runtime $dependency $binary
  }
}
# Resolve and authenticate the entire transitive CRT closure before copying any
# bytes. Existing app-local files are validated above and are never overwritten.
foreach($name in @($planned.Keys|Sort-Object)){
  $record=$planned[$name]
  if((File-Hash $record.source) -ne $record.sha256){throw "MSVC source changed during preflight: $($record.source)"}
  $destination=Join-Path $bin $name
  if(Test-Path -LiteralPath $destination){throw "A CRT destination appeared during staging; refusing to overwrite: $destination"}
  [IO.File]::Copy($record.source,$destination,$false)
  if((File-Hash $destination) -ne $record.sha256){throw "Copied MSVC runtime hash differs: $name"}
  $facts[$name]=$record
}
$files=@(foreach($name in @($facts.Keys|Sort-Object)){
  $record=$facts[$name];$record['path']='bin/'+$record.name
  $record['requiredBy']=if($required.ContainsKey($name)){@($required[$name]|Sort-Object -Unique)}else{@()}
  $record
})
$manifest=[ordered]@{schema='gsw-native-crt-v1';verified=$true;verifyOnly=[bool]$VerifyOnly
  scope='MSVC redistributable import closure of every staged EXE/DLL; Windows OS imports are not copied'
  architecture='x64';peFilesInspected=$processed.Count;runtimeFilesCopied=$planned.Count
  minimumRuntimeNames=$minimumRuntimeNames;requiredRuntimeNames=@($required.Keys|Sort-Object);files=$files;nonCrtImports=@($nonCrt|Sort-Object)
  dumpBin=$script:Inspector;checkedAtUtc=[DateTime]::UtcNow.ToString('o')
  licenseNotice='Microsoft runtime files are unmodified and retain their own license. Redistribution requires the applicable Visual Studio rights; this report does not grant them.'
  references=@('https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution',
    'https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files')}
$json=$manifest|ConvertTo-Json -Depth 12
if(-not $VerifyOnly){
  $target=Join-Path $PackageRoot 'native-crt-manifest.json'
  if(Test-Path -LiteralPath $target){Assert-NoReparsePath $target}
  $temporary=Join-Path $PackageRoot ('.native-crt-manifest.'+[guid]::NewGuid().ToString('N')+'.tmp')
  try{
    [IO.File]::WriteAllText($temporary,$json,[Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary,$target,$true)
  }finally{if(Test-Path -LiteralPath $temporary -PathType Leaf){Remove-Item -LiteralPath $temporary}}
}
Write-Output $json
