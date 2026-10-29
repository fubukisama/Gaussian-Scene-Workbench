param(
  [string]$InstallRoot = "",
  [switch]$NonInteractive,
  [switch]$Interactive,
  [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Log = Join-Path $Root "setup_gaussian_scene_workbench.log"

function Write-Step($message) {
  $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $message
  Write-Host $line
  Add-Content -LiteralPath $Log -Value $line
}

$script:MissingComponents = New-Object System.Collections.Generic.List[string]
$script:FailedComponents = New-Object System.Collections.Generic.List[string]

function Mark-Missing($name, $detail = "") {
  $text = if ([string]::IsNullOrWhiteSpace($detail)) { $name } else { "$name - $detail" }
  $script:MissingComponents.Add($text) | Out-Null
}

function Mark-Failed($name, $detail = "") {
  $text = if ([string]::IsNullOrWhiteSpace($detail)) { $name } else { "$name - $detail" }
  $script:FailedComponents.Add($text) | Out-Null
}

function Clear-ComponentIssue($needle) {
  for ($i = $script:MissingComponents.Count - 1; $i -ge 0; $i--) {
    if ($script:MissingComponents[$i].Contains($needle)) {
      $script:MissingComponents.RemoveAt($i)
    }
  }
  for ($i = $script:FailedComponents.Count - 1; $i -ge 0; $i--) {
    if ($script:FailedComponents[$i].Contains($needle)) {
      $script:FailedComponents.RemoveAt($i)
    }
  }
}

function Confirm-Step($message) {
  if ($NonInteractive -or -not $Interactive) { return $true }
  $answer = Read-Host "$message [Y/n]"
  return [string]::IsNullOrWhiteSpace($answer) -or $answer.Trim().ToLowerInvariant().StartsWith("y")
}

function Test-Command($name) {
  return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

function Invoke-WingetInstall($id, $name, $extraArgs = @()) {
  if (-not (Test-Command "winget")) {
    Write-Step "winget is unavailable. Install $name manually: $id"
    return
  }
  if (-not (Confirm-Step "Install $name with winget?")) {
    Write-Step "Skipped $name install."
    return
  }
  Write-Step "Installing $name..."
  & winget install --id $id --exact --accept-package-agreements --accept-source-agreements @extraArgs
}

function Invoke-Download($url, $destination) {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
  Write-Step "Downloading: $url"
  Invoke-WebRequest -Uri $url -OutFile $destination -Headers @{ "User-Agent" = "Gaussian-Scene-Workbench-Setup" }
}

function Add-SetupPath($path) {
  if ((Test-Path -LiteralPath $path) -and (($env:PATH -split [regex]::Escape([IO.Path]::PathSeparator)) -notcontains $path)) {
    $env:PATH = "$path$([IO.Path]::PathSeparator)$env:PATH"
  }
}

function Get-InstallDriveRoot {
  return ([IO.Path]::GetPathRoot($Root)).TrimEnd("\")
}

function Get-DefaultInstallRoot {
  $drive = Get-InstallDriveRoot
  $current = Join-Path $drive "Gaussian-Scene-Workbench-Runtime"
  $legacy = Join-Path $drive "3DGS-Editor-Runtime"
  if ((Test-Path -LiteralPath $legacy) -and -not (Test-Path -LiteralPath $current)) {
    return $legacy
  }
  return $current
}

function Resolve-RuntimeRoot {
  if (-not [string]::IsNullOrWhiteSpace($InstallRoot)) {
    return [IO.Path]::GetFullPath($InstallRoot)
  }
  if (-not [string]::IsNullOrWhiteSpace($env:GS_EDITOR_RUNTIME_ROOT)) {
    return [IO.Path]::GetFullPath($env:GS_EDITOR_RUNTIME_ROOT)
  }
  return Get-DefaultInstallRoot
}

function Get-CondaRootCandidates {
  $drive = Get-InstallDriveRoot
  $items = @(
    $env:MINIFORGE_ROOT,
    $env:CONDA_ROOT,
    $env:MAMBA_ROOT_PREFIX,
    (Join-Path $script:RuntimeRoot "miniforge3"),
    (Join-Path $drive "miniforge3"),
    (Join-Path $drive "conda"),
    (Join-Path $drive "anaconda"),
    (Join-Path $env:USERPROFILE "miniforge3")
  )
  return @($items | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
}

function Find-CondaBat {
  foreach ($rootCandidate in Get-CondaRootCandidates) {
    $candidate = Join-Path $rootCandidate "condabin\conda.bat"
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return Join-Path $script:MiniforgeRoot "condabin\conda.bat"
}

function Find-CondaExe {
  foreach ($rootCandidate in Get-CondaRootCandidates) {
    $candidate = Join-Path $rootCandidate "Scripts\conda.exe"
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return Join-Path $script:MiniforgeRoot "Scripts\conda.exe"
}

function Find-GaussianEnvPython {
  $drive = Get-InstallDriveRoot
  $candidates = @(
    $env:GAUSSIAN_SPLATTING_CONDA_PREFIX,
    $env:GS_CONDA_PREFIX,
    (Join-Path $script:MiniforgeRoot "envs\gaussian_splatting"),
    (Join-Path $drive "miniforge3\envs\gaussian_splatting"),
    (Join-Path $drive "conda\envs\gaussian_splatting"),
    (Join-Path $drive "anaconda\envs\gaussian_splatting"),
    (Join-Path $env:USERPROFILE "miniforge3\envs\gaussian_splatting")
  )
  foreach ($candidateRoot in @($candidates | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)) {
    $candidate = Join-Path $candidateRoot "python.exe"
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return Join-Path $script:MiniforgeRoot "envs\gaussian_splatting\python.exe"
}

function Install-Miniforge {
  $condaBat = Find-CondaBat
  if (Test-Path -LiteralPath $condaBat) {
    Write-Step "SKIP: Miniforge already installed -> $condaBat"
    return $condaBat
  }
  Mark-Missing "Miniforge" $condaBat
  if ($CheckOnly) { return $null }
  if (-not (Confirm-Step "Install Miniforge automatically now?")) { return $null }
  $installer = Join-Path $Root "third_party\downloads\Miniforge3-Windows-x86_64.exe"
  Invoke-Download "https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Windows-x86_64.exe" $installer
  $target = $script:MiniforgeRoot
  Write-Step "Installing Miniforge to $target"
  $proc = Start-Process -FilePath $installer -ArgumentList @("/InstallationType=JustMe", "/RegisterPython=0", "/S", "/D=$target") -Wait -PassThru -WindowStyle Hidden
  if ($proc.ExitCode -ne 0) { throw "Miniforge installer failed with exit code $($proc.ExitCode)." }
  if (-not (Test-Path -LiteralPath $condaBat)) { throw "Miniforge install finished but conda.bat was not found: $condaBat" }
  Clear-ComponentIssue "Miniforge"
  return $condaBat
}

function Install-Git {
  if (Test-Command "git") {
    Write-Step "SKIP: Git already installed -> $((Get-Command git).Source)"
    return
  }
  Mark-Missing "Git"
  if ($CheckOnly) { return }
  if (-not (Confirm-Step "Install Git automatically now?")) { return }
  $downloadDir = Join-Path $Root "third_party\downloads"
  New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
  Write-Step "Querying Git for Windows releases..."
  $release = Invoke-RestMethod -Uri "https://api.github.com/repos/git-for-windows/git/releases/latest" -Headers @{ "User-Agent" = "Gaussian-Scene-Workbench-Setup" }
  $asset = @($release.assets) | Where-Object { $_.name -match "64-bit\.exe$" -and $_.name -notmatch "portable|minGit" } | Select-Object -First 1
  if (-not $asset) {
    Write-Step "Could not find Git installer asset; falling back to winget."
    Invoke-WingetInstall "Git.Git" "Git"
    return
  }
  $installer = Join-Path $downloadDir $asset.name
  Invoke-Download $asset.browser_download_url $installer
  Write-Step "Installing Git..."
  $proc = Start-Process -FilePath $installer -ArgumentList @("/VERYSILENT", "/NORESTART", "/NOCANCEL") -Wait -PassThru -WindowStyle Hidden
  if ($proc.ExitCode -ne 0) { throw "Git installer failed with exit code $($proc.ExitCode)." }
  Add-SetupPath (Join-Path $env:ProgramFiles "Git\cmd")
}

function Install-VsBuildTools {
  if ($CheckOnly) {
    Mark-Missing "Visual Studio 2022 C++ Build Tools"
    return
  }
  if (-not (Confirm-Step "Install Visual Studio 2022 C++ Build Tools automatically now?")) { return }
  $installer = Join-Path $Root "third_party\downloads\vs_BuildTools.exe"
  Invoke-Download "https://aka.ms/vs/17/release/vs_BuildTools.exe" $installer
  Write-Step "Installing Visual Studio 2022 C++ Build Tools. Windows may request administrator approval."
  $args = @(
    "--quiet", "--wait", "--norestart",
    "--add", "Microsoft.VisualStudio.Workload.VCTools",
    "--includeRecommended"
  )
  $proc = Start-Process -FilePath $installer -ArgumentList $args -Wait -PassThru
  if ($proc.ExitCode -ne 0 -and $proc.ExitCode -ne 3010) {
    throw "Visual Studio Build Tools installer failed with exit code $($proc.ExitCode)."
  }
}

function Test-PathReport($path, $name) {
  if (Test-Path -LiteralPath $path) {
    Write-Step "OK: $name -> $path"
    return $true
  }
  Write-Step "MISSING: $name -> $path"
  Mark-Missing $name $path
  return $false
}

function Get-VersionedColmapExe {
  $installRoot = Join-Path (Get-InstallDriveRoot) "Tools\COLMAP"
  if (-not (Test-Path -LiteralPath $installRoot -PathType Container)) {
    return $null
  }
  $versioned = Get-ChildItem -LiteralPath $installRoot -Directory |
    Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName "bin\colmap.exe" } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
  if ($versioned) { return $versioned }
  $direct = Join-Path $installRoot "bin\colmap.exe"
  if (Test-Path -LiteralPath $direct -PathType Leaf) { return $direct }
  return $null
}

function Get-LocalColmapExe {
  $versioned = Get-VersionedColmapExe
  $candidates = @(
    $env:COLMAP_EXE,
    $env:COLMAP_PATH,
    $versioned,
    (Join-Path $Root "third_party\colmap\bin\colmap.exe"),
    (Join-Path $Root "tools\colmap\bin\colmap.exe"),
    (Join-Path $Root "colmap\bin\colmap.exe"),
    (Join-Path $env:USERPROFILE "Downloads\colmap-x64-windows-cuda\bin\colmap.exe")
  ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  foreach ($candidate in @($candidates | Select-Object -Unique)) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
  }
  return Join-Path (Get-InstallDriveRoot) "Tools\COLMAP\bin\colmap.exe"
}

function Install-Colmap {
  $existing = Get-LocalColmapExe
  if (Test-Path -LiteralPath $existing -PathType Leaf) {
    Write-Step "SKIP: COLMAP already installed -> $existing"
    return $existing
  }
  $targetRoot = Join-Path (Get-InstallDriveRoot) "Tools\COLMAP"
  Mark-Missing "COLMAP" $targetRoot
  if ($CheckOnly) { return $null }
  if (-not (Confirm-Step "Download and install CUDA COLMAP into $targetRoot now?")) {
    Write-Step "Skipped COLMAP install."
    return $null
  }

  $installer = Join-Path $Root "scripts\install_colmap.ps1"
  if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Missing COLMAP installer: $installer"
  }
  $installedPaths = @(& $installer -InstallRoot $targetRoot -Variant cuda -Version latest)
  $targetExe = $installedPaths |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -Last 1
  if (-not $targetExe) {
    throw "COLMAP installer completed without returning a valid executable."
  }
  Write-Step "COLMAP installed: $targetExe"
  Clear-ComponentIssue "COLMAP"
  return $targetExe
}

function Invoke-CondaRun($arguments) {
  $condaExe = Find-CondaExe
  if (Test-Path -LiteralPath $condaExe) {
    & $condaExe @arguments
    return $LASTEXITCODE
  }
  if (Test-Path -LiteralPath $MiniforgeConda) {
    & cmd.exe /d /s /c "`"$MiniforgeConda`" $($arguments -join ' ')"
    return $LASTEXITCODE
  }
  throw "Conda was not found."
}

function Test-EnvPythonImport($code, $name) {
  if (-not (Test-Path -LiteralPath $EnvPython)) { return $false }
  $envRoot = Split-Path -Parent $EnvPython
  Add-SetupPath $envRoot
  Add-SetupPath (Join-Path $envRoot "Library\bin")
  Add-SetupPath (Join-Path $envRoot "Library\usr\bin")
  Add-SetupPath (Join-Path $envRoot "Scripts")
  $env:CONDA_PREFIX = $envRoot
  & $EnvPython -c $code | Out-Host
  if ($LASTEXITCODE -eq 0) {
    Write-Step "OK: $name"
    return $true
  }
  Write-Step "MISSING/FAILED: $name"
  return $false
}

function Repair-GaussianEnvironment {
  if (-not (Test-Path -LiteralPath $EnvPython)) {
    Write-Step "Cannot repair gaussian_splatting because python is missing: $EnvPython"
    Mark-Missing "gaussian_splatting Python" $EnvPython
    return
  }
  $envRoot = Split-Path -Parent $EnvPython
  Add-SetupPath $envRoot
  Add-SetupPath (Join-Path $envRoot "Library\bin")
  Add-SetupPath (Join-Path $envRoot "Library\usr\bin")
  Add-SetupPath (Join-Path $envRoot "Scripts")
  $env:CONDA_PREFIX = $envRoot
  $ffmpegExe = Join-Path $envRoot "Library\bin\ffmpeg.exe"
  Write-Step "Repairing gaussian_splatting runtime packages..."
  if (Test-Path -LiteralPath $ffmpegExe) {
    Write-Step "SKIP: ffmpeg already present -> $ffmpegExe"
  } else {
    Mark-Missing "ffmpeg.exe" $ffmpegExe
    if ($CheckOnly) {
      Write-Step "CHECK ONLY: skipping ffmpeg installation."
    } else {
    $condaCode = Invoke-CondaRun -arguments @("install", "-n", "gaussian_splatting", "-y", "-c", "conda-forge", "ffmpeg")
    if ($condaCode -ne 0) {
      Write-Step "WARNING: conda ffmpeg install failed; falling back to imageio-ffmpeg."
    }
    }
  }
  $videoPackagesOk = Test-EnvPythonImport "import cv2, imageio, imageio_ffmpeg; print('OpenCV', cv2.__version__); print('imageio-ffmpeg', imageio_ffmpeg.get_ffmpeg_exe())" "OpenCV/imageio video packages"
  if (-not $videoPackagesOk) {
    Mark-Missing "OpenCV/imageio Python packages"
    if ($CheckOnly) {
      Write-Step "CHECK ONLY: skipping pip package installation."
    } else {
      & $EnvPython -m pip install --upgrade opencv-python==4.8.1.78 imageio imageio-ffmpeg
      if ($LASTEXITCODE -ne 0) { throw "Failed to install OpenCV/video Python packages." }
      $videoPackagesOk = Test-EnvPythonImport "import cv2, imageio, imageio_ffmpeg; print('OpenCV', cv2.__version__); print('imageio-ffmpeg', imageio_ffmpeg.get_ffmpeg_exe())" "OpenCV/imageio video packages"
      if (-not $videoPackagesOk) { throw "OpenCV import still fails after repair." }
      Clear-ComponentIssue "OpenCV/imageio"
    }
  } else {
    Write-Step "SKIP: OpenCV/imageio packages already import correctly."
    Clear-ComponentIssue "OpenCV/imageio"
  }
  if (-not (Test-Path -LiteralPath $ffmpegExe)) {
    if ($CheckOnly) {
      Write-Step "CHECK ONLY: ffmpeg.exe is missing and was not copied."
      return
    }
    Write-Step "Copying imageio-ffmpeg binary to conda environment..."
    $copyScript = @"
import imageio_ffmpeg
import pathlib
import shutil
dst = pathlib.Path(r"$ffmpegExe")
dst.parent.mkdir(parents=True, exist_ok=True)
src = pathlib.Path(imageio_ffmpeg.get_ffmpeg_exe())
shutil.copy2(src, dst)
print(dst)
"@
    & $EnvPython -c $copyScript
    if ($LASTEXITCODE -ne 0) { throw "Failed to copy imageio-ffmpeg executable into the conda environment." }
  }
  if (-not (Test-Path -LiteralPath $ffmpegExe)) {
    Mark-Failed "ffmpeg.exe" $ffmpegExe
    throw "ffmpeg.exe is still missing after repair: $ffmpegExe"
  }
  Write-Step "ffmpeg OK: $ffmpegExe"
  Clear-ComponentIssue "ffmpeg.exe"
}

function Repair-PytorchRuntime {
  if (-not (Test-Path -LiteralPath $EnvPython)) { return }
  $torchOk = Test-EnvPythonImport "import torch; print('torch', torch.__version__); print('cuda available:', torch.cuda.is_available())" "PyTorch runtime"
  if ($torchOk) {
    Write-Step "SKIP: PyTorch runtime already imports correctly."
    return
  }
  if ($CheckOnly) {
    Mark-Missing "PyTorch 1.12 compatible MKL runtime"
    Write-Step "CHECK ONLY: skipping PyTorch MKL runtime repair."
    return
  }
  Write-Step "Installing PyTorch 1.12-compatible MKL runtime..."
  $code = Invoke-CondaRun -arguments @(
    "install", "-n", "gaussian_splatting", "-y",
    "defaults::mkl=2021.4",
    "defaults::mkl-service=2.4",
    "defaults::intel-openmp=2021.4"
  )
  if ($code -ne 0) { throw "Failed to install PyTorch-compatible MKL runtime." }
  $torchOk = Test-EnvPythonImport "import torch; print('torch', torch.__version__); print('cuda available:', torch.cuda.is_available())" "PyTorch runtime"
  if (-not $torchOk) {
    Mark-Failed "PyTorch runtime" "torch import failed after MKL repair"
    throw "PyTorch import still fails after MKL runtime repair."
  }
}

function Repair-PillowRuntime {
  if (-not (Test-Path -LiteralPath $EnvPython)) { return }
  $pillowOk = Test-EnvPythonImport "from PIL import Image; print('Pillow', Image.__version__)" "Pillow runtime"
  if ($pillowOk) {
    Write-Step "SKIP: Pillow runtime already imports correctly."
    return
  }
  if ($CheckOnly) {
    Mark-Missing "Pillow DLL runtime"
    Write-Step "CHECK ONLY: skipping Pillow DLL runtime repair."
    return
  }
  Write-Step "Installing Pillow-compatible image DLL runtime..."
  $code = Invoke-CondaRun -arguments @(
    "install", "-n", "gaussian_splatting", "-y",
    "defaults::pillow=9.4.0",
    "defaults::libtiff",
    "defaults::libdeflate",
    "defaults::zlib",
    "defaults::jpeg",
    "defaults::libjpeg-turbo"
  )
  if ($code -ne 0) { throw "Failed to install Pillow-compatible image DLL runtime." }
  $pillowOk = Test-EnvPythonImport "from PIL import Image; print('Pillow', Image.__version__)" "Pillow runtime"
  if (-not $pillowOk) {
    Mark-Failed "Pillow runtime" "PIL.Image import failed after DLL repair"
    throw "Pillow import still fails after image DLL runtime repair."
  }
}

function Ensure-3DGSSubmodulePaths {
  if (-not (Test-Path -LiteralPath $EnvPython)) { return }
  $envRoot = Split-Path -Parent $EnvPython
  $sitePackages = Join-Path $envRoot "Lib\site-packages"
  if (-not (Test-Path -LiteralPath $sitePackages)) {
    Mark-Missing "Python site-packages" $sitePackages
    return
  }
  $submodulePaths = @(
    (Join-Path $Root "gaussian-splatting\submodules\diff-gaussian-rasterization\build\lib.win-amd64-cpython-37"),
    (Join-Path $Root "gaussian-splatting\submodules\simple-knn\build\lib.win-amd64-cpython-37"),
    (Join-Path $Root "gaussian-splatting\submodules\fused-ssim\build\lib.win-amd64-cpython-37")
  )
  $missing = @($submodulePaths | Where-Object { -not (Test-Path -LiteralPath $_) })
  if ($missing.Count -gt 0) {
    foreach ($path in $missing) {
      Mark-Missing "prebuilt 3DGS CUDA extension path" $path
      Write-Step "MISSING: prebuilt 3DGS CUDA extension path -> $path"
    }
    return
  }
  $pth = Join-Path $sitePackages "3dgs_editor_submodules.pth"
  if ($CheckOnly) {
    if (Test-Path -LiteralPath $pth) {
      Write-Step "OK: 3DGS extension path file -> $pth"
    } else {
      Mark-Missing "3DGS extension path file" $pth
      Write-Step "MISSING: 3DGS extension path file -> $pth"
    }
    return
  }
  Set-Content -LiteralPath $pth -Value $submodulePaths -Encoding ASCII
  Write-Step "OK: 3DGS extension path file -> $pth"
}

Write-Step "Gaussian Splatting Workbench setup started."
Write-Step "Root: $Root"
$script:RuntimeRoot = Resolve-RuntimeRoot
$script:MiniforgeRoot = Join-Path $script:RuntimeRoot "miniforge3"
New-Item -ItemType Directory -Force -Path $script:RuntimeRoot | Out-Null
Write-Step "Runtime install root: $script:RuntimeRoot"

$MiniforgeConda = Find-CondaBat
$EnvPython = Find-GaussianEnvPython
$env:MINIFORGE_ROOT = Split-Path -Parent (Split-Path -Parent $MiniforgeConda)
$env:CONDA_ROOT = $env:MINIFORGE_ROOT
$env:GAUSSIAN_SPLATTING_CONDA_PREFIX = Split-Path -Parent $EnvPython
$env:GS_CONDA_PREFIX = $env:GAUSSIAN_SPLATTING_CONDA_PREFIX
$ColmapExe = Get-LocalColmapExe

if (-not (Test-PathReport $MiniforgeConda "Miniforge conda")) {
  $installedConda = Install-Miniforge
  if ($installedConda) {
    $MiniforgeConda = $installedConda
  } else {
    Invoke-WingetInstall "CondaForge.Miniforge3" "Miniforge3"
  }
}

if (-not (Test-Command "git")) {
  Install-Git
}

$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$HasBuildTools = $false
if (Test-Path -LiteralPath $VsWhere) {
  $VsInstall = & $VsWhere -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath
  $HasBuildTools = -not [string]::IsNullOrWhiteSpace($VsInstall)
}
if (-not $HasBuildTools) {
  Install-VsBuildTools
}

if (-not (Test-PathReport $ColmapExe "COLMAP executable")) {
  $installedColmap = Install-Colmap
  if ($installedColmap) {
    $ColmapExe = $installedColmap
  }
}

if (-not (Test-Path -LiteralPath $EnvPython)) {
  Mark-Missing "gaussian_splatting conda environment" $EnvPython
  $CreateEnv = Join-Path $Root "scripts\create_3dgs_env.bat"
  if (-not (Test-Path -LiteralPath $CreateEnv)) {
    throw "Missing environment creation script: $CreateEnv"
  }
  if ($CheckOnly) {
    Write-Step "CHECK ONLY: skipping conda environment creation."
  } elseif (Confirm-Step "Create/update the gaussian_splatting conda environment now?") {
    Write-Step "Running $CreateEnv"
    $env:MINIFORGE_ROOT = Split-Path -Parent (Split-Path -Parent $MiniforgeConda)
    $env:CONDA_ROOT = $env:MINIFORGE_ROOT
    & cmd.exe /d /s /c "`"$CreateEnv`""
    $EnvPython = Find-GaussianEnvPython
    $env:GAUSSIAN_SPLATTING_CONDA_PREFIX = Split-Path -Parent $EnvPython
    $env:GS_CONDA_PREFIX = $env:GAUSSIAN_SPLATTING_CONDA_PREFIX
  }
}

Repair-GaussianEnvironment
Repair-PytorchRuntime
Repair-PillowRuntime
Ensure-3DGSSubmodulePaths

if (Test-Path -LiteralPath $EnvPython) {
  Write-Step "Verifying Python imports..."
  $runtimeOk = Test-EnvPythonImport "import torch, cv2; from PIL import Image; import diff_gaussian_rasterization, simple_knn; print('3DGS runtime OK; cuda=', torch.cuda.is_available())" "3DGS runtime imports"
  if (-not $runtimeOk) {
    Mark-Failed "3DGS runtime imports" "torch/cv2/PIL/diff_gaussian_rasterization/simple_knn"
    if (-not $CheckOnly) { throw "3DGS runtime import verification failed." }
  }
}

Write-Step "Environment check summary:"
if ($script:MissingComponents.Count -eq 0 -and $script:FailedComponents.Count -eq 0) {
  Write-Step "ALL OK: no missing local components detected."
} else {
  foreach ($item in ($script:MissingComponents | Select-Object -Unique)) {
    Write-Step "MISSING SUMMARY: $item"
  }
  foreach ($item in ($script:FailedComponents | Select-Object -Unique)) {
    Write-Step "FAILED SUMMARY: $item"
  }
}
Write-Step "Setup finished. Log: $Log"
Write-Host ""
if ($Interactive -and -not $NonInteractive) {
  Write-Host "Press Enter to close."
  Read-Host | Out-Null
}
