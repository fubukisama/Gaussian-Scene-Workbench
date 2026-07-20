param(
  [string]$Root = "",
  [string]$OpenMvsDir = "",
  [string]$VcpkgRoot = "",
  [string]$Triplet = "x64-windows-release",
  [switch]$UseCuda,
  [switch]$UsePython,
  [switch]$BuildViewer,
  [switch]$ConfigureOnly
)

$ErrorActionPreference = "Stop"

if (-not $Root) {
  if ($env:GS_EDITOR_WORKSPACE_ROOT) {
    $Root = [System.IO.Path]::GetFullPath($env:GS_EDITOR_WORKSPACE_ROOT)
  } else {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
  }
}

if (-not $OpenMvsDir) {
  $OpenMvsDir = Join-Path $Root "openMVS"
}

function Find-CommandPath([string]$Name) {
  $cmd = Get-Command $Name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}

$git = Find-CommandPath "git.exe"
if (-not $git) {
  throw "git.exe was not found in PATH."
}

if (-not (Test-Path $OpenMvsDir)) {
  & $git clone --recursive https://github.com/cdcseacave/openMVS.git $OpenMvsDir
} else {
  Push-Location $OpenMvsDir
  try {
    & $git submodule update --init --recursive
  } finally {
    Pop-Location
  }
}

$cmake = Find-CommandPath "cmake.exe"
if (-not $cmake) {
  $localCmake = Get-ChildItem -Path (Join-Path $Root "third_party\tools") -Filter "cmake.exe" -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*\bin\cmake.exe" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
  if ($localCmake) { $cmake = $localCmake.FullName }
}
if (-not $cmake) {
  Write-Host "OpenMVS source is ready at $OpenMvsDir"
  Write-Host "cmake.exe was not found. Install Visual Studio Build Tools + CMake, then rerun this script."
  exit 0
}

if (-not $VcpkgRoot) {
  if ($env:VCPKG_ROOT) {
    $VcpkgRoot = $env:VCPKG_ROOT
  } else {
    $candidate = Join-Path $Root "third_party\vcpkg"
    if (Test-Path $candidate) { $VcpkgRoot = $candidate }
  }
}

if (-not $VcpkgRoot -or -not (Test-Path (Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"))) {
  Write-Host "OpenMVS source is ready at $OpenMvsDir"
  Write-Host "vcpkg was not found. Set VCPKG_ROOT or pass -VcpkgRoot before building OpenMVS dependencies."
  exit 0
}

$buildDir = Join-Path $OpenMvsDir "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$configureArgs = @(
  "-S", $OpenMvsDir,
  "-B", $buildDir,
  "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake')",
  "-DVCPKG_TARGET_TRIPLET=$Triplet",
  "-DBUILD_SHARED_LIBS=ON",
  "-DOpenMVS_USE_CUDA=$(if ($UseCuda) { 'ON' } else { 'OFF' })",
  "-DOpenMVS_USE_SIFTGPU=$(if ($UseCuda) { 'ON' } else { 'OFF' })",
  "-DOpenMVS_USE_PYTHON=$(if ($UsePython) { 'ON' } else { 'OFF' })",
  "-DOpenMVS_BUILD_VIEWER=$(if ($BuildViewer) { 'ON' } else { 'OFF' })",
  "-DOpenMVS_ENABLE_TESTS=OFF"
)

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
  throw "OpenMVS CMake configure failed with exit code $LASTEXITCODE."
}

if ($ConfigureOnly) {
  Write-Host "OpenMVS configured at $buildDir"
  exit 0
}

& $cmake --build $buildDir --config Release --target InterfaceCOLMAP TextureMesh
if ($LASTEXITCODE -ne 0) {
  throw "OpenMVS build failed with exit code $LASTEXITCODE."
}

$releaseBin = Join-Path $buildDir "bin\x64\Release"
if (-not (Test-Path (Join-Path $releaseBin "InterfaceCOLMAP.exe"))) {
  $releaseBin = Join-Path $buildDir "bin\Release"
}

Write-Host "OpenMVS build finished."
Write-Host "Set OPENMVS_BIN=$releaseBin if the app does not find the binaries automatically."
