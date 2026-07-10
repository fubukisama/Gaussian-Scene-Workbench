param(
  [string]$Version = "",
  [switch]$SkipElectronPackage,
  [switch]$IncludeDatasets,
  [switch]$IncludeOutputs,
  [switch]$IncludeAdvancedMeshBackends
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$DesktopApp = Join-Path $Root "resources\app"
if (-not (Test-Path -LiteralPath (Join-Path $DesktopApp "package.json"))) {
  $DesktopApp = Join-Path $Root "desktop_app"
}
$PackageDir = Join-Path $DesktopApp "dist\Gaussian Scene Workbench-win32-x64"
$ReleaseRoot = Join-Path $Root "release"

if ([string]::IsNullOrWhiteSpace($Version)) {
  $PackageJson = Get-Content (Join-Path $DesktopApp "package.json") -Raw | ConvertFrom-Json
  $Version = $PackageJson.version
}

function Copy-TreeFiltered($Source, $Destination, [string[]]$ExcludeNames = @()) {
  if (-not (Test-Path -LiteralPath $Source)) { return }
  if (Test-Path -LiteralPath $Destination) {
    Remove-Item -LiteralPath $Destination -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  $sourcePath = (Resolve-Path -LiteralPath $Source).Path
  Get-ChildItem -LiteralPath $sourcePath -Force | ForEach-Object {
    if ($ExcludeNames -contains $_.Name) { return }
    $target = Join-Path $Destination $_.Name
    if ($_.PSIsContainer) {
      Copy-TreeFiltered $_.FullName $target $ExcludeNames
    } else {
      Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
  }
}

function Get-Sha256Hex($Path) {
  if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  $stream = [System.IO.File]::OpenRead($Path)
  try {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
      $bytes = $sha.ComputeHash($stream)
      return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
    } finally {
      $sha.Dispose()
    }
  } finally {
    $stream.Dispose()
  }
}

if (-not $SkipElectronPackage) {
  Push-Location $DesktopApp
  try {
    npm install
    npm run package:win
  } finally {
    Pop-Location
  }
}

if (-not (Test-Path -LiteralPath (Join-Path $PackageDir "Gaussian Scene Workbench.exe"))) {
  throw "Packaged executable not found: $PackageDir"
}

$excludeCommon = @(
  ".git", "__pycache__", ".pytest_cache", ".preview", "dist", "release",
  "desktop_editor.log", "gaussian_scene_workbench.log", "crop_editor_server_upgrade.log", "crop_editor_server_upgrade.err.log"
)

Copy-TreeFiltered (Join-Path $Root "crop_editor") (Join-Path $PackageDir "crop_editor") @("__pycache__", ".pytest_cache", ".preview", ".jobs")
Copy-TreeFiltered (Join-Path $Root "scripts") (Join-Path $PackageDir "scripts") @("__pycache__", ".pytest_cache")
Copy-TreeFiltered (Join-Path $Root "training_kit") (Join-Path $PackageDir "training_kit") @("__pycache__", ".pytest_cache")
Copy-TreeFiltered (Join-Path $Root "gaussian-splatting") (Join-Path $PackageDir "gaussian-splatting") @(".git", "output", "data_device", "__pycache__")

if ($IncludeAdvancedMeshBackends -and (Test-Path -LiteralPath (Join-Path $Root "SuGaR"))) {
  Copy-TreeFiltered (Join-Path $Root "SuGaR") (Join-Path $PackageDir "SuGaR") @(".git", "output", "__pycache__")
}
if ($IncludeAdvancedMeshBackends -and (Test-Path -LiteralPath (Join-Path $Root "gs2mesh"))) {
  Copy-TreeFiltered (Join-Path $Root "gs2mesh") (Join-Path $PackageDir "gs2mesh") @(".git", "output", "__pycache__")
}
if ($IncludeAdvancedMeshBackends -and (Test-Path -LiteralPath (Join-Path $Root "openMVS\build\bin"))) {
  Copy-TreeFiltered (Join-Path $Root "openMVS") (Join-Path $PackageDir "openMVS") @(".git", "buildtrees", "downloads", "packages")
}

if ($IncludeDatasets -and (Test-Path -LiteralPath (Join-Path $Root "datasets"))) {
  Copy-TreeFiltered (Join-Path $Root "datasets") (Join-Path $PackageDir "datasets") @("__pycache__")
}
if ($IncludeOutputs -and (Test-Path -LiteralPath (Join-Path $Root "output"))) {
  Copy-TreeFiltered (Join-Path $Root "output") (Join-Path $PackageDir "output") @("__pycache__")
}

Copy-Item -LiteralPath (Join-Path $Root "Setup Gaussian Scene Workbench.cmd") -Destination (Join-Path $PackageDir "Setup Gaussian Scene Workbench.cmd") -Force
Copy-Item -LiteralPath (Join-Path $Root "Check Gaussian Scene Workbench Environment.cmd") -Destination (Join-Path $PackageDir "Check Gaussian Scene Workbench Environment.cmd") -Force
Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination (Join-Path $PackageDir "LICENSE") -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath (Join-Path $Root "THIRD_PARTY_LICENSES.md") -Destination (Join-Path $PackageDir "THIRD_PARTY_LICENSES.md") -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath (Join-Path $Root "README_RELEASE.md") -Destination (Join-Path $PackageDir "README_RELEASE.md") -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination (Join-Path $PackageDir "README.md") -Force -ErrorAction SilentlyContinue
$ReleaseNotes = Join-Path $Root ("release_notes_v{0}.md" -f $Version)
if (Test-Path -LiteralPath $ReleaseNotes) {
  Copy-Item -LiteralPath $ReleaseNotes -Destination (Join-Path $PackageDir (Split-Path -Leaf $ReleaseNotes)) -Force
}
Copy-Item -LiteralPath (Join-Path $Root "version") -Destination (Join-Path $PackageDir "version") -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath (Join-Path $Root "build_manifest.json") -Destination (Join-Path $PackageDir "build_manifest.json") -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $ReleaseRoot | Out-Null
$zip = Join-Path $ReleaseRoot ("Gaussian-Scene-Workbench-{0}-win-x64.zip" -f $Version)
if (Test-Path -LiteralPath $zip) {
  Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $zip -Force
$hash = Get-Sha256Hex $zip
$shaPath = "$zip.sha256"
("{0}  {1}" -f $hash, (Split-Path -Leaf $zip)) | Set-Content -LiteralPath $shaPath -Encoding ASCII

Write-Host "Release package created:"
Write-Host $zip
Write-Host "SHA256:"
Write-Host $shaPath
