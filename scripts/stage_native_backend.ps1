param(
  [Parameter(Mandatory = $true)]
  [string]$SourceRoot,
  [Parameter(Mandatory = $true)]
  [string]$DestinationRoot
)

$ErrorActionPreference = "Stop"
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
if (-not (Test-Path -LiteralPath $DestinationRoot)) {
  New-Item -ItemType Directory -Path $DestinationRoot -Force | Out-Null
}
$DestinationRoot = (Resolve-Path -LiteralPath $DestinationRoot).Path

$ExcludedDirectories = @(
  ".git",
  ".pytest_cache",
  "__pycache__",
  "build",
  "dist",
  "doc",
  "docs"
)
$ExcludedExtensions = @(".css", ".html", ".js", ".map", ".pyc")

function Copy-FilteredTree {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination
  )

  $ResolvedSource = (Resolve-Path -LiteralPath $Source).Path.TrimEnd("\", "/")
  Get-ChildItem -LiteralPath $ResolvedSource -Recurse -File | ForEach-Object {
    $RelativePath = $_.FullName.Substring($ResolvedSource.Length).TrimStart("\", "/")
    $Segments = $RelativePath -split "[\\/]"
    if ($Segments | Where-Object { $ExcludedDirectories -contains $_ }) {
      return
    }
    if ($ExcludedExtensions -contains $_.Extension.ToLowerInvariant()) {
      return
    }
    $Target = Join-Path $Destination $RelativePath
    $TargetDirectory = Split-Path -Parent $Target
    if (-not (Test-Path -LiteralPath $TargetDirectory)) {
      New-Item -ItemType Directory -Path $TargetDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $_.FullName -Destination $Target -Force
  }
}

$StaticFiles = @(
  @{ Source = "native\worker\gsw_worker.py"; Destination = "native\worker\gsw_worker.py" },
  @{ Source = "native\worker\import_preflight.py"; Destination = "native\worker\import_preflight.py" },
  @{ Source = "native\worker\training_preflight.py"; Destination = "native\worker\training_preflight.py" },
  @{ Source = "crop_editor\server.py"; Destination = "crop_editor\server.py" },
  @{ Source = "crop_editor\video_extract.py"; Destination = "crop_editor\video_extract.py" },
  @{ Source = "scripts\check_3dgs_env.ps1"; Destination = "scripts\check_3dgs_env.ps1" },
  @{ Source = "scripts\install_colmap.ps1"; Destination = "scripts\install_colmap.ps1" },
  @{ Source = "scripts\sign_windows_artifacts.ps1"; Destination = "scripts\sign_windows_artifacts.ps1" }
)
foreach ($Entry in $StaticFiles) {
  $Source = Join-Path $SourceRoot $Entry.Source
  if (-not (Test-Path -LiteralPath $Source)) {
    throw "Required native backend file is missing: $Source"
  }
  $Destination = Join-Path $DestinationRoot $Entry.Destination
  $DestinationDirectory = Split-Path -Parent $Destination
  New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
  Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

Copy-FilteredTree `
  -Source (Join-Path $SourceRoot "gaussian-splatting") `
  -Destination (Join-Path $DestinationRoot "gaussian-splatting")
Copy-FilteredTree `
  -Source (Join-Path $SourceRoot "training_kit") `
  -Destination (Join-Path $DestinationRoot "training_kit")

$Manifest = [ordered]@{
  schemaVersion = 1
  purpose = "Native compute backend"
  runtime = "External Python/Conda environment required"
  components = @(
    "native/worker/gsw_worker.py",
    "native/worker/import_preflight.py",
    "native/worker/training_preflight.py",
    "crop_editor/server.py",
    "crop_editor/video_extract.py",
    "gaussian-splatting",
    "training_kit",
    "scripts/check_3dgs_env.ps1",
    "scripts/install_colmap.ps1",
    "scripts/sign_windows_artifacts.ps1"
  )
  forbiddenWebRuntime = @("html", "js", "css", "electron", "node")
}
$ManifestPath = Join-Path $DestinationRoot "backend_manifest.json"
[System.IO.File]::WriteAllText(
  $ManifestPath,
  ($Manifest | ConvertTo-Json -Depth 5),
  [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Native compute backend staged at:"
Write-Host $DestinationRoot
