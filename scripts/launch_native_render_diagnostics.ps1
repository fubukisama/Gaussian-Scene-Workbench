#requires -Version 5.1
<#
.SYNOPSIS
Manually launch a separate native installation with local F12 frame diagnostics.
.DESCRIPTION
Developer/manual CLI; see docs/RENDER_DIAGNOSTICS.md for instructions in
zh_CN, en_US and ja_JP. This script never closes an existing application,
changes persistent environment/settings, opens captures, or uploads data.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $ExecutablePath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $CaptureDirectory
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-AbsoluteLocalPath {
    param([string] $Value, [string] $ParameterName)
    if ($Value -notmatch '^[A-Za-z]:[\\/]') {
        throw "$ParameterName must be an absolute local drive path (not a relative or network path)."
    }
    return [System.IO.Path]::GetFullPath($Value)
}

function Test-DirectoryWritable {
    param([string] $Directory)
    if (-not [System.IO.Directory]::Exists($Directory)) { return $false }
    $probe = [System.IO.Path]::Combine($Directory, '.gsw-diagnostic-write-probe-' + [guid]::NewGuid().ToString('N'))
    $stream = $null
    try {
        $stream = [System.IO.FileStream]::new(
            $probe, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None, 1, [System.IO.FileOptions]::DeleteOnClose)
        return $true
    }
    catch { return $false }
    finally { if ($null -ne $stream) { $stream.Dispose() } }
}

$executable = Get-AbsoluteLocalPath $ExecutablePath 'ExecutablePath'
$capture = Get-AbsoluteLocalPath $CaptureDirectory 'CaptureDirectory'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf) -or
    -not [string]::Equals([System.IO.Path]::GetExtension($executable), '.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'ExecutablePath must identify an existing diagnostic .exe file.'
}

$processName = [System.IO.Path]::GetFileNameWithoutExtension($executable)
$existing = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    [string]::Equals($_.ProcessName, $processName, [System.StringComparison]::OrdinalIgnoreCase) -or
    [string]::Equals($_.ProcessName, 'Gaussian Scene Workbench', [System.StringComparison]::OrdinalIgnoreCase)
})
if ($existing.Count -gt 0) {
    throw ('Save your project and manually close all Gaussian Scene Workbench windows before launching diagnostics. Existing process IDs: ' + (($existing | Select-Object -ExpandProperty Id) -join ', '))
}

$captureRoot = [System.IO.Path]::GetPathRoot($capture)
$systemRoot = [System.IO.Path]::GetPathRoot($env:WINDIR)
$nonSystemRoots = @()
foreach ($drive in @(Get-PSDrive -PSProvider FileSystem)) {
    if ($drive.Root -notmatch '^[A-Za-z]:[\\/]$' -or
        [string]::Equals($drive.Root, $systemRoot, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    try {
        $info = [System.IO.DriveInfo]::new($drive.Root)
        if (-not $info.IsReady -or $info.DriveType -ne [System.IO.DriveType]::Fixed) { continue }
        $writable = Test-DirectoryWritable $drive.Root
        if (-not $writable -and
            [string]::Equals([System.IO.Path]::GetPathRoot($PSScriptRoot), $drive.Root, [System.StringComparison]::OrdinalIgnoreCase)) {
            $writable = Test-DirectoryWritable $PSScriptRoot
        }
        if ($writable) { $nonSystemRoots += $drive.Root }
    }
    catch { continue }
}
if ([string]::Equals($captureRoot, $systemRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
    $nonSystemRoots.Count -gt 0) {
    throw ('Choose CaptureDirectory on a writable non-system drive: ' + ($nonSystemRoots -join ', '))
}

$ancestor = $capture
while (-not (Test-Path -LiteralPath $ancestor)) {
    $parent = [System.IO.Path]::GetDirectoryName($ancestor)
    if ([string]::IsNullOrEmpty($parent) -or $parent -eq $ancestor) {
        throw 'CaptureDirectory has no existing local parent directory.'
    }
    $ancestor = $parent
}
if (-not (Test-Path -LiteralPath $ancestor -PathType Container)) {
    throw 'A CaptureDirectory component is a file rather than a directory.'
}
# Do not let a directory junction silently redirect local captures to another drive.
$component = Get-Item -LiteralPath $ancestor -Force
while ($null -ne $component) {
    if (($component.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'CaptureDirectory must not pass through a symbolic link or directory junction.'
    }
    $component = $component.Parent
}
if (-not (Test-DirectoryWritable $ancestor)) { throw 'CaptureDirectory parent is not writable.' }
[void][System.IO.Directory]::CreateDirectory($capture)
if (-not (Test-DirectoryWritable $capture)) { throw 'CaptureDirectory is not writable.' }

$variable = 'GSW_RENDER_DIAGNOSTICS_DIR'
$previous = [System.Environment]::GetEnvironmentVariable($variable, [System.EnvironmentVariableTarget]::Process)
try {
    [System.Environment]::SetEnvironmentVariable($variable, $capture, [System.EnvironmentVariableTarget]::Process)
    $workingDirectory = [System.IO.Path]::GetDirectoryName($executable)
    if ([string]::Equals([System.IO.Path]::GetFileName($workingDirectory), 'bin', [System.StringComparison]::OrdinalIgnoreCase)) {
        $workingDirectory = [System.IO.Path]::GetDirectoryName($workingDirectory)
    }
    # Visible launch is intentional: the user runs this script to reproduce manually.
    $started = Start-Process -FilePath $executable -WorkingDirectory $workingDirectory -WindowStyle Normal -PassThru
    Write-Output ('Diagnostic process started. PID: ' + $started.Id)
    Write-Output ('Local captures: ' + $capture)
}
finally {
    [System.Environment]::SetEnvironmentVariable($variable, $previous, [System.EnvironmentVariableTarget]::Process)
}
