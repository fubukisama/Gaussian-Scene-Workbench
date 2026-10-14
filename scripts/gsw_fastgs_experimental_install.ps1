param(
  [string]$Source = 'D:/Codex/Research/FasterGS-c60ea269-20261014',
  [string]$Runtime = 'D:/Apps/GSW/runtime/envs/gsw_fastgs_experimental',
  [string]$BasePython = 'D:/Apps/GSW/runtime/envs/gaussian_splatting/python.exe',
  [string]$Cuda = 'D:/Apps/CUDA/11.8',
  [string]$Artifacts = 'D:/Apps/GSW/validation/fastgs-experimental-oct14'
)
$ErrorActionPreference = 'Stop'
$PinnedCommit = 'c60ea269b7cf3ebda527d640b6fdd31462f91b41'
$resolvedSource = (Resolve-Path -LiteralPath $Source).Path
$sourceHead = (& git -C $resolvedSource rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceHead -ne $PinnedCommit) {
  throw "Experimental Faster-GS source must be pinned to $PinnedCommit."
}
if (-not (Test-Path -LiteralPath (Join-Path $Cuda 'bin/nvcc.exe'))) {
  throw 'CUDA compiler is unavailable; the stock training environment remains unchanged.'
}
New-Item -ItemType Directory -Path $Artifacts -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Artifacts 'tmp') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Artifacts 'wheels') -Force | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $Runtime 'Scripts/python.exe'))) {
  # The overlay reads the unchanged stock Torch binary. Experimental packages
  # are installed here only; never replace diff_gaussian_rasterization globally.
  & $BasePython -B -m venv --system-site-packages $Runtime
  if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated extension overlay.' }
}
$python = Join-Path $Runtime 'Scripts/python.exe'
$env:TEMP = Join-Path $Artifacts 'tmp'
$env:TMP = $env:TEMP
$env:PIP_CACHE_DIR = 'D:/CodexData/pip-cache'
$env:TORCH_EXTENSIONS_DIR = Join-Path $Artifacts 'torch-extensions'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
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
  --wheel-dir (Join-Path $Artifacts 'wheels') (Join-Path $resolvedSource 'FasterGSCudaBackend')
if ($LASTEXITCODE -ne 0) {
  throw 'Faster-GS compilation failed; do not advertise the experimental renderer as available.'
}
$wheel = Get-ChildItem -LiteralPath (Join-Path $Artifacts 'wheels') -Filter '*FasterGSCudaBackend*.whl' |
  Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $wheel) { throw 'The expected experimental wheel was not produced.' }
& $python -B -m pip install --no-deps $wheel.FullName
if ($LASTEXITCODE -ne 0) { throw 'Experimental overlay installation failed.' }
Write-Output "Built pinned Faster-GS in $Runtime. Kernel/training probes are still required."
