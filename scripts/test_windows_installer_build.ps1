#requires -Version 7.2
# SPDX-License-Identifier: GPL-3.0-or-later
# Public wrapper rejection test using only fresh, non-application input bytes.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$TestRoot,
  [Parameter(Mandatory=$true)][string]$InnoCompiler,
  [Parameter(Mandatory=$true)][string]$PythonExecutable
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$TestRoot=[IO.Path]::GetFullPath($TestRoot)
if($TestRoot.TrimEnd('\','/') -eq [IO.Path]::GetPathRoot($TestRoot).TrimEnd('\','/')){throw 'A dedicated TestRoot below a volume root is required.'}
$cursor=$TestRoot
while($cursor){
  if((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){
    throw 'Reparse paths are forbidden for the controlled wrapper test.'
  }
  $parent=[IO.Path]::GetDirectoryName($cursor)
  if(-not $parent -or $parent -eq $cursor){break}
  $cursor=$parent
}
$InnoCompiler=(Resolve-Path -LiteralPath $InnoCompiler).Path
$PythonExecutable=(Resolve-Path -LiteralPath $PythonExecutable).Path
New-Item -ItemType Directory -Path $TestRoot -Force|Out-Null
$root=Join-Path $TestRoot ('wrapper-input-boundary-'+[guid]::NewGuid().ToString('N'))
$inputRoot=Join-Path $root 'owned input 空白 日本語'
$temp=Join-Path $root 'temp'
New-Item -ItemType Directory -Path (Join-Path $inputRoot 'existing folder'),$temp|Out-Null
[IO.File]::WriteAllText((Join-Path $inputRoot 'sentinel.txt'),'Original controlled input; never a candidate package',[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllBytes((Join-Path $inputRoot 'existing folder/sentinel.bin'),[byte[]]@(0,1,2,3,127,128,254,255))

function Get-InputSnapshot {
  if(-not(Test-Path -LiteralPath $inputRoot -PathType Container)){return @([ordered]@{path='.';kind='missing'})}
  return @(Get-ChildItem -LiteralPath $inputRoot -Force -Recurse|Sort-Object FullName|ForEach-Object{
    $record=[ordered]@{path=$_.FullName.Substring($inputRoot.Length+1).Replace('\','/');kind='directory'}
    if(($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){$record.kind='reparse'}
    elseif(-not $_.PSIsContainer){
      $record.kind='file';$record['bytes']=$_.Length
      $record['sha256']=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [pscustomobject]$record
  })
}

$before=Get-InputSnapshot
$forbidden=Join-Path $inputRoot ('forbidden-'+[guid]::NewGuid().ToString('N'))
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=Join-Path $PSHOME 'pwsh.exe'
$start.WorkingDirectory=$root
$start.UseShellExecute=$false;$start.CreateNoWindow=$true
$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
$start.Environment['TEMP']=$temp;$start.Environment['TMP']=$temp
$start.Environment['PYTHONDONTWRITEBYTECODE']='1'
foreach($argument in @('-NoProfile','-NonInteractive','-File',(Join-Path $PSScriptRoot 'build_windows_installer.ps1'),
    '-PackageRoot',$inputRoot,'-OutputDirectory',$forbidden,'-PythonExecutable',$PythonExecutable,'-InnoCompiler',$InnoCompiler)){
  $start.ArgumentList.Add($argument)
}
$record=[ordered]@{schema='gsw-installer-wrapper-input-boundary-v1';inputRoot=$inputRoot;forbiddenOutput=$forbidden
  innoCompiler=$InnoCompiler;pythonExecutable=$PythonExecutable;before=$before;passed=$false
  applicationLaunched=$false;installerLaunched=$false;realCandidateUsed=$false}
$failure=$null
$process=[Diagnostics.Process]::new();$process.StartInfo=$start
try{
  if(-not $process.Start()){throw 'Unable to start hidden wrapper process.'}
  $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
  if(-not $process.WaitForExit(30000)){throw 'Wrapper timeout; process was not force-terminated.'}
  [IO.File]::WriteAllText((Join-Path $root 'stdout.log'),$stdout.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
  $stderrText=$stderr.GetAwaiter().GetResult()
  [IO.File]::WriteAllText((Join-Path $root 'stderr.log'),$stderrText,[Text.UTF8Encoding]::new($false))
  $after=Get-InputSnapshot
  $record['exitCode']=$process.ExitCode;$record['after']=$after
  $record['inputUnchanged']=($before|ConvertTo-Json -Depth 5 -Compress) -eq ($after|ConvertTo-Json -Depth 5 -Compress)
  $record['forbiddenOutputExists']=Test-Path -LiteralPath $forbidden
  if($process.ExitCode -eq 0){throw 'Wrapper incorrectly accepted an output directory inside its immutable input.'}
  $record['rejectionReasonVerified']=$stderrText.Contains('Fresh work directory outside the immutable input payload is required')
  if(-not $record.rejectionReasonVerified){throw 'Wrapper failed for an unrelated reason instead of enforcing its immutable input boundary.'}
  if(-not $record.inputUnchanged -or $record.forbiddenOutputExists){throw 'Rejected wrapper invocation mutated its input before validation.'}
  $record.passed=$true
}catch{$failure=$_;$record['error']=$_.Exception.Message}
finally{
  $process.Dispose()
  [IO.File]::WriteAllText((Join-Path $root 'wrapper-input-boundary.json'),($record|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
Write-Output ('Wrapper boundary evidence: '+(Join-Path $root 'wrapper-input-boundary.json'))
if($failure){throw $failure}
