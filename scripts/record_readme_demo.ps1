#Requires -Version 7.0
<#
.SYNOPSIS
Records real native desktop input on a disposable GitHub-hosted Windows runner.
.DESCRIPTION
Never run this on a user's desktop. This does not train, install, publish,
promote, alter application code, or invoke an in-process smoke-test driver.
The supplied PLYs must be actual training snapshots with hash-bound provenance.
The raw recording is continuous; replay is explicitly labelled in its sidecar.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$TrainingRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$PackageReceipt,
    [Parameter(Mandatory)][string]$PythonExecutable,
    [string]$FfmpegExecutable = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows -or $env:GITHUB_ACTIONS -cne 'true' -or
    $env:RUNNER_ENVIRONMENT -cne 'github-hosted') {
    throw 'Video input and capture are restricted to disposable GitHub-hosted Windows runners. No local GUI fallback exists.'
}
if (-not $FfmpegExecutable) {
    $FfmpegExecutable = (& $PythonExecutable -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())').Trim()
    if ($LASTEXITCODE -ne 0) { throw 'The pinned imageio-ffmpeg runtime is unavailable.' }
}
$arguments = @(
    '-I', '-B', (Join-Path $PSScriptRoot 'record_readme_demo.py'),
    '--package-root', ([IO.Path]::GetFullPath($PackageRoot)),
    '--training-root', ([IO.Path]::GetFullPath($TrainingRoot)),
    '--output', ([IO.Path]::GetFullPath($OutputDirectory)),
    '--package-receipt', ([IO.Path]::GetFullPath($PackageReceipt)),
    '--ffmpeg', ([IO.Path]::GetFullPath($FfmpegExecutable))
)
& $PythonExecutable @arguments
if ($LASTEXITCODE -ne 0) {
    throw 'Remote desktop recording failed; inspect recording-evidence.json, raw video, screenshots and logs. Do not substitute a synthetic animation.'
}
