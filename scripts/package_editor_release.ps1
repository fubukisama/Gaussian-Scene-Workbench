param(
  [string]$Version = "",
  [switch]$SkipElectronPackage,
  [switch]$IncludeDatasets,
  [switch]$IncludeOutputs,
  [switch]$IncludeAdvancedMeshBackends
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$DesktopApp = Join-Path $Root "desktop_app"
$PackageDir = Join-Path $DesktopApp "dist\3DGS Editor-win32-x64"
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

if (-not $SkipElectronPackage) {
  Push-Location $DesktopApp
  try {
    npm install
    npm run package:win
  } finally {
    Pop-Location
  }
}

if (-not (Test-Path -LiteralPath (Join-Path $PackageDir "3DGS Editor.exe"))) {
  throw "Packaged executable not found: $PackageDir"
}

$excludeCommon = @(
  ".git", "__pycache__", ".pytest_cache", ".preview", "dist", "release",
  "desktop_editor.log", "crop_editor_server_upgrade.log", "crop_editor_server_upgrade.err.log"
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

Copy-Item -LiteralPath (Join-Path $Root "Setup 3DGS Editor.cmd") -Destination (Join-Path $PackageDir "Setup 3DGS Editor.cmd") -Force
Copy-Item -LiteralPath (Join-Path $Root "Check 3DGS Editor Environment.cmd") -Destination (Join-Path $PackageDir "Check 3DGS Editor Environment.cmd") -Force
Copy-Item -LiteralPath (Join-Path $Root "README_RELEASE.md") -Destination (Join-Path $PackageDir "README_RELEASE.md") -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination (Join-Path $PackageDir "README.md") -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $ReleaseRoot | Out-Null
$zip = Join-Path $ReleaseRoot ("3DGS-Editor-{0}-win-x64.zip" -f $Version)
if (Test-Path -LiteralPath $zip) {
  Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $zip -Force

Write-Host "Release package created:"
Write-Host $zip
