param(
  [Parameter(Mandatory = $true)]
  [string]$SourceRoot,
  [Parameter(Mandatory = $true)]
  [string]$DestinationRoot,
  [string]$PythonExecutable = ""
)

$ErrorActionPreference = "Stop"
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
if ([string]::IsNullOrWhiteSpace($PythonExecutable)) {
  $PathPython = Get-Command python.exe, python -ErrorAction SilentlyContinue |
    Where-Object { $_.Source -notmatch '[\\/]WindowsApps[\\/]' } |
    Select-Object -First 1
  $PythonCandidates = @(
    $env:QA_PYTHON,
    $env:GSW_NATIVE_CHECK_PYTHON,
    $(if ($env:GAUSSIAN_SPLATTING_CONDA_PREFIX) { Join-Path $env:GAUSSIAN_SPLATTING_CONDA_PREFIX 'python.exe' }),
    $(if ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX 'python.exe' }),
    $(if ($PathPython) { $PathPython.Source })
  ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
  $PythonExecutable = $PythonCandidates | Select-Object -First 1
}
if (-not $PythonExecutable) {
  throw "Python is required to verify the native backend. Pass -PythonExecutable with the QA interpreter."
}
& $PythonExecutable -I -B (Join-Path $SourceRoot 'scripts/verify_native_backend.py') --root $SourceRoot
if ($LASTEXITCODE -ne 0) { throw "Native source boundary verification failed." }
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
$ExcludedExtensions = @(".css", ".htm", ".html", ".js", ".mjs", ".cjs", ".asar", ".wasm", ".map", ".pyc", ".pyd")

function Copy-FilteredTree {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination
  )

  $ResolvedSource = (Resolve-Path -LiteralPath $Source).Path.TrimEnd("\", "/")
  Get-ChildItem -LiteralPath $ResolvedSource -Recurse -File | ForEach-Object {
    $RelativePath = $_.FullName.Substring($ResolvedSource.Length).TrimStart("\", "/")
    $Segments = $RelativePath -split "[\\/]"
    if ($Segments | Where-Object { $ExcludedDirectories -contains $_ -or $_ -like '*.egg-info' }) {
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
  @{ Source = "native\worker\gpu_preview_publisher.py"; Destination = "native\worker\gpu_preview_publisher.py" },
  @{ Source = "native\worker\training_preview.py"; Destination = "native\worker\training_preview.py" },
  @{ Source = "native\worker\training_preview_policy.py"; Destination = "native\worker\training_preview_policy.py" },
  @{ Source = "native\worker\training_reconstruction_summary.py"; Destination = "native\worker\training_reconstruction_summary.py" },
  @{ Source = "native\worker\training_checkpoint.py"; Destination = "native\worker\training_checkpoint.py" },
  @{ Source = "native\worker\training_density_control.py"; Destination = "native\worker\training_density_control.py" },
  @{ Source = "native\worker\training_summary.py"; Destination = "native\worker\training_summary.py" },
  @{ Source = "native\worker\training_runtime.py"; Destination = "native\worker\training_runtime.py" },
  @{ Source = "native\worker\training_image_pipeline.py"; Destination = "native\worker\training_image_pipeline.py" },
  @{ Source = "native\worker\training_camera_lookahead.py"; Destination = "native\worker\training_camera_lookahead.py" },
  @{ Source = "native\worker\training_capacity_storage.py"; Destination = "native\worker\training_capacity_storage.py" },
  @{ Source = "native\worker\training_quality.py"; Destination = "native\worker\training_quality.py" },
  @{ Source = "native\worker\surfel_render_adapter.py"; Destination = "native\worker\surfel_render_adapter.py" },
  @{ Source = "native\worker\training_resource_policy.py"; Destination = "native\worker\training_resource_policy.py" },
  @{ Source = "native\worker\training_step_profile.py"; Destination = "native\worker\training_step_profile.py" },
  @{ Source = "native\worker\training_acceleration.py"; Destination = "native\worker\training_acceleration.py" },
  @{ Source = "native\worker\training_loss.py"; Destination = "native\worker\training_loss.py" },
  @{ Source = "native\worker\surfel_regularization.py"; Destination = "native\worker\surfel_regularization.py" },
  @{ Source = "native\worker\two_dgs_train.py"; Destination = "native\worker\two_dgs_train.py" },
  @{ Source = "native\worker\generation_capabilities.py"; Destination = "native\worker\generation_capabilities.py" },
  @{ Source = "native\worker\mesh_generation.py"; Destination = "native\worker\mesh_generation.py" },
  @{ Source = "native\worker\mesh_material_preview.py"; Destination = "native\worker\mesh_material_preview.py" },
  @{ Source = "native\worker\licenses\2dgs-LICENSE.md"; Destination = "native\worker\licenses\2dgs-LICENSE.md" },
  @{ Source = "native\backend\__init__.py"; Destination = "native\backend\__init__.py" },
  @{ Source = "native\backend\compute.py"; Destination = "native\backend\compute.py" },
  @{ Source = "native\backend\reconstruction_quality.py"; Destination = "native\backend\reconstruction_quality.py" },
  @{ Source = "native\backend\video_extract.py"; Destination = "native\backend\video_extract.py" },
  @{ Source = "scripts\check_3dgs_env.ps1"; Destination = "scripts\check_3dgs_env.ps1" },
  @{ Source = "scripts\compare_training_quality.py"; Destination = "scripts\compare_training_quality.py" },
  @{ Source = "scripts\requirements-native-3dgs.txt"; Destination = "scripts\requirements-native-3dgs.txt" },
  @{ Source = "scripts\requirements-native-2dgs.txt"; Destination = "scripts\requirements-native-2dgs.txt" },
  @{ Source = "scripts\requirements-native-sugar.txt"; Destination = "scripts\requirements-native-sugar.txt" },
  @{ Source = "scripts\requirements-native-gs2mesh.txt"; Destination = "scripts\requirements-native-gs2mesh.txt" },
  @{ Source = "scripts\patches\pytorch3d-0.7.4-windows-cub.patch"; Destination = "scripts\patches\pytorch3d-0.7.4-windows-cub.patch" },
  @{ Source = "scripts\patches\sugar-windows-stage-paths.patch"; Destination = "scripts\patches\sugar-windows-stage-paths.patch" },
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
    "native/worker/gpu_preview_publisher.py",
    "native/worker/training_preview.py",
    "native/worker/training_preview_policy.py",
    "native/worker/training_reconstruction_summary.py",
    "native/worker/training_checkpoint.py",
    "native/worker/training_summary.py",
    "native/worker/training_runtime.py",
    "native/worker/training_image_pipeline.py",
    "native/worker/training_camera_lookahead.py",
    "native/worker/training_capacity_storage.py",
    "native/worker/training_quality.py",
    "native/worker/surfel_render_adapter.py",
    "native/worker/training_resource_policy.py",
    "native/worker/training_step_profile.py",
    "native/worker/training_acceleration.py",
    "native/worker/training_loss.py",
    "native/worker/surfel_regularization.py",
    "native/worker/training_density_control.py",
    "native/worker/two_dgs_train.py",
    "native/worker/generation_capabilities.py",
    "native/worker/mesh_generation.py",
    "native/worker/mesh_material_preview.py",
    "native/worker/licenses/2dgs-LICENSE.md",
    "native/backend/__init__.py",
    "native/backend/compute.py",
    "native/backend/reconstruction_quality.py",
    "native/backend/video_extract.py",
    "gaussian-splatting",
    "training_kit",
    "scripts/check_3dgs_env.ps1",
    "scripts/compare_training_quality.py",
    "scripts/requirements-native-3dgs.txt",
    "scripts/requirements-native-2dgs.txt",
    "scripts/requirements-native-sugar.txt",
    "scripts/requirements-native-gs2mesh.txt",
    "scripts/patches/pytorch3d-0.7.4-windows-cub.patch",
    "scripts/patches/sugar-windows-stage-paths.patch",
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

& $PythonExecutable -I -B (Join-Path $SourceRoot 'scripts/verify_native_backend.py') --root $DestinationRoot --package
if ($LASTEXITCODE -ne 0) { throw "Staged native package boundary verification failed." }

Write-Host "Native compute backend staged at:"
Write-Host $DestinationRoot
