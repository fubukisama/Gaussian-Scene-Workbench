param(
  [string]$Runtime = 'D:/Apps/GSW/runtime/envs/gsw_fastgs_experimental',
  [string]$BasePython = 'D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe',
  [string]$Cuda = 'D:/Apps/CUDA/11.8',
  [string]$Artifacts = 'D:/Apps/GSW/validation/fused-loss-experimental-oct14'
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
New-Item -ItemType Directory -Path $Artifacts -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Artifacts 'tmp') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Artifacts 'wheels') -Force | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $Runtime 'Scripts/python.exe'))) {
  & $BasePython -B -m venv --system-site-packages $Runtime
  if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated loss extension overlay.' }
}
$python = Join-Path $Runtime 'Scripts/python.exe'
$env:TEMP = Join-Path $Artifacts 'tmp'
$env:TMP = $env:TEMP
$env:PIP_CACHE_DIR = 'D:/CodexData/pip-cache'
$env:TORCH_EXTENSIONS_DIR = Join-Path $Artifacts 'torch-extensions'
$env:PYTHONUTF8 = '1'
$env:CUDA_HOME = $Cuda
$env:CUDA_PATH = $Cuda
$env:TORCH_CUDA_ARCH_LIST = '8.9'
$env:DISTUTILS_USE_SDK = '1'
$env:MAX_JOBS = '2'
$env:PATH = (Join-Path $Cuda 'bin') + ';' +
  (Join-Path (Split-Path -Parent $BasePython) 'Library/bin') + ';' + $env:PATH
$setup = & $env:ComSpec /d /c 'call D:\Apps\PortableMSVC-2022\msvc\setup_x64.bat >nul && set'
foreach ($line in $setup) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
  }
}
& $python -B -m pip wheel --no-deps --no-build-isolation `
  --wheel-dir (Join-Path $Artifacts 'wheels') (Join-Path $repository 'gaussian-splatting/gsw_loss_cuda')
if ($LASTEXITCODE -ne 0) { throw 'Fused loss compilation failed; keep stock loss.' }
$wheel = Get-ChildItem -LiteralPath (Join-Path $Artifacts 'wheels') -Filter '*gsw_training_loss_cuda*.whl' |
  Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $wheel) { throw 'The fused loss wheel was not produced.' }
& $python -B -m pip install --no-deps $wheel.FullName
if ($LASTEXITCODE -ne 0) { throw 'Isolated fused loss installation failed.' }
Write-Output 'Numerical forward/backward and both-backend tests remain required before activation.'
