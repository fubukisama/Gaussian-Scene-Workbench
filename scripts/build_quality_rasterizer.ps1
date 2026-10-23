param(
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string]$Python = 'D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe',
    [switch]$Compile
)
$ErrorActionPreference = 'Stop'
$Repository = Split-Path -Parent $PSScriptRoot
$Arguments = @((Join-Path $Repository 'training_kit/quality_rasterizer/build.py'), '--output-root', $OutputRoot)
if ($Compile) { $Arguments += '--compile' }
& $Python @Arguments
if ($LASTEXITCODE -ne 0) { throw ('Quality rasterizer build entry failed: ' + $LASTEXITCODE) }
