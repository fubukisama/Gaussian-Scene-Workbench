param(
  [switch]$NonInteractive,
  [switch]$Interactive,
  [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Log = Join-Path $Root "setup_3dgs_editor.log"

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
  Invoke-WebRequest -Uri $url -OutFile $destination -Headers @{ "User-Agent" = "3DGS-Editor-Setup" }
}

function Add-SetupPath($path) {
  if ((Test-Path -LiteralPath $path) -and (($env:PATH -split [regex]::Escape([IO.Path]::PathSeparator)) -notcontains $path)) {
    $env:PATH = "$path$([IO.Path]::PathSeparator)$env:PATH"
  }
}

function Install-Miniforge {
  $condaBat = Join-Path $env:USERPROFILE "miniforge3\condabin\conda.bat"
  if (Test-Path -LiteralPath $condaBat) {
    Write-Step "SKIP: Miniforge already installed -> $condaBat"
    return $condaBat
  }
  Mark-Missing "Miniforge" $condaBat
  if ($CheckOnly) { return $null }
  if (-not (Confirm-Step "Install Miniforge automatically now?")) { return $null }
  $installer = Join-Path $Root "third_party\downloads\Miniforge3-Windows-x86_64.exe"
  Invoke-Download "https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Windows-x86_64.exe" $installer
  $target = Join-Path $env:USERPROFILE "miniforge3"
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
  $release = Invoke-RestMethod -Uri "https://api.github.com/repos/git-for-windows/git/releases/latest" -Headers @{ "User-Agent" = "3DGS-Editor-Setup" }
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

function Install-NodeRuntime {
  if (Test-Command "npm") {
    Write-Step "SKIP: Node/npm already installed -> $((Get-Command npm).Source)"
    return
  }
  Mark-Missing "Node.js/npm"
  if ($CheckOnly) { return }
  if (-not (Confirm-Step "Install local Node.js runtime for helper dependencies now?")) { return }
  $nodeRoot = Join-Path $Root "third_party\node"
  $nodeExe = Join-Path $nodeRoot "node.exe"
  if (Test-Path -LiteralPath $nodeExe) {
    Add-SetupPath $nodeRoot
    return
  }
  $downloadDir = Join-Path $Root "third_party\downloads"
  $extractDir = Join-Path $Root "third_party\node_extract"
  New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
  if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
  $index = Invoke-RestMethod -Uri "https://nodejs.org/dist/index.json" -Headers @{ "User-Agent" = "3DGS-Editor-Setup" }
  $release = @($index) | Where-Object { $_.lts -and ($_.files -contains "win-x64-zip") } | Select-Object -First 1
  if (-not $release) { throw "Could not find a Node.js LTS win-x64 zip." }
  $zipName = "node-$($release.version)-win-x64.zip"
  $zip = Join-Path $downloadDir $zipName
  Invoke-Download "https://nodejs.org/dist/$($release.version)/$zipName" $zip
  Expand-Archive -LiteralPath $zip -DestinationPath $extractDir -Force
  $found = Get-ChildItem -LiteralPath $extractDir -Directory | Select-Object -First 1
  if (-not $found) { throw "Node.js archive extracted no directory." }
  if (Test-Path -LiteralPath $nodeRoot) { Remove-Item -LiteralPath $nodeRoot -Recurse -Force }
  Copy-Item -LiteralPath $found.FullName -Destination $nodeRoot -Recurse -Force
  Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
  Add-SetupPath $nodeRoot
  if (-not (Test-Command "npm")) { throw "Node.js was installed locally, but npm is still unavailable." }
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

function Get-LocalColmapExe {
  $candidates = @(
    (Join-Path $Root "third_party\colmap\bin\colmap.exe"),
    (Join-Path $Root "tools\colmap\bin\colmap.exe"),
    (Join-Path $Root "colmap\bin\colmap.exe"),
    (Join-Path $env:USERPROFILE "Downloads\colmap-x64-windows-cuda\bin\colmap.exe")
  )
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return $candidates[0]
}

function Install-Colmap {
  $targetRoot = Join-Path $Root "third_party\colmap"
  $targetExe = Join-Path $targetRoot "bin\colmap.exe"
  if (Test-Path -LiteralPath $targetExe) {
    Write-Step "SKIP: bundled COLMAP already installed -> $targetExe"
    return $targetExe
  }
  Mark-Missing "COLMAP" $targetExe
  if ($CheckOnly) { return $null }
  if (-not (Confirm-Step "Download and install COLMAP into this app folder now?")) {
    Write-Step "Skipped COLMAP install."
    return $null
  }

  $downloadDir = Join-Path $Root "third_party\downloads"
  $extractDir = Join-Path $Root "third_party\colmap_extract"
  New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
  if (Test-Path -LiteralPath $extractDir) {
    Remove-Item -LiteralPath $extractDir -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

  Write-Step "Querying COLMAP releases..."
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/colmap/colmap/releases" -Headers @{ "User-Agent" = "3DGS-Editor-Setup" }
  $asset = $null
  foreach ($release in $releases) {
    $asset = @($release.assets) | Where-Object { $_.name -match "(?i)windows.*cuda.*\.zip$|colmap.*cuda.*windows.*\.zip$|colmap-x64-windows-cuda.*\.zip$" } | Select-Object -First 1
    if ($asset) { break }
  }
  if (-not $asset) {
    foreach ($release in $releases) {
      $asset = @($release.assets) | Where-Object { $_.name -match "(?i)windows.*\.zip$|win.*x64.*\.zip$" } | Select-Object -First 1
      if ($asset) { break }
    }
  }
  if (-not $asset) {
    throw "Could not find a Windows COLMAP zip in GitHub releases."
  }

  $zip = Join-Path $downloadDir $asset.name
  Write-Step "Downloading COLMAP: $($asset.browser_download_url)"
  Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers @{ "User-Agent" = "3DGS-Editor-Setup" }
  Write-Step "Extracting COLMAP..."
  Expand-Archive -LiteralPath $zip -DestinationPath $extractDir -Force

  $foundExe = Get-ChildItem -LiteralPath $extractDir -Recurse -Filter "colmap.exe" | Select-Object -First 1
  if (-not $foundExe) {
    throw "COLMAP download extracted, but colmap.exe was not found."
  }
  $copyRoot = $foundExe.Directory
  if ($copyRoot.Name -ieq "bin") {
    $copyRoot = $copyRoot.Parent
  }
  if (Test-Path -LiteralPath $targetRoot) {
    Remove-Item -LiteralPath $targetRoot -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $targetRoot) | Out-Null
  Copy-Item -LiteralPath $copyRoot.FullName -Destination $targetRoot -Recurse -Force
  if (-not (Test-Path -LiteralPath $targetExe)) {
    throw "COLMAP install did not create expected executable: $targetExe"
  }
  Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue
  Write-Step "COLMAP installed: $targetExe"
  Clear-ComponentIssue "COLMAP"
  return $targetExe
}

function Invoke-CondaRun($arguments) {
  $condaExe = Join-Path $env:USERPROFILE "miniforge3\Scripts\conda.exe"
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

Write-Step "3DGS Editor setup started."
Write-Step "Root: $Root"
New-Item -ItemType Directory -Force -Path (Join-Path $Root "desktop_app") | Out-Null

$MiniforgeConda = Join-Path $env:USERPROFILE "miniforge3\condabin\conda.bat"
$EnvPython = Join-Path $env:USERPROFILE "miniforge3\envs\gaussian_splatting\python.exe"
$ColmapExe = if ($env:COLMAP_EXE) { $env:COLMAP_EXE } else { Get-LocalColmapExe }

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
    & cmd.exe /d /s /c "`"$CreateEnv`""
  }
}

Repair-GaussianEnvironment
Repair-PytorchRuntime
Repair-PillowRuntime
Ensure-3DGSSubmodulePaths

$CropPackage = Join-Path $Root "crop_editor\package.json"
if ((Test-Path -LiteralPath $CropPackage) -and -not (Test-Path -LiteralPath (Join-Path $Root "crop_editor\node_modules\.bin\splat-transform.cmd"))) {
  Mark-Missing "crop_editor Node helper dependencies" (Join-Path $Root "crop_editor\node_modules")
  Install-NodeRuntime
  if ($CheckOnly) {
    Write-Step "CHECK ONLY: skipping crop_editor npm install."
  } elseif (Test-Command "npm") {
    if (Confirm-Step "Install crop_editor Node dependencies with npm?") {
      Push-Location (Join-Path $Root "crop_editor")
      try {
        npm install
      } finally {
        Pop-Location
      }
    }
  } else {
    Write-Step "npm is not installed. Splat transform helper dependencies were not installed."
  }
}

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
