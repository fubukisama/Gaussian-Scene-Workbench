param(
  [ValidateSet("Photo", "Video")]
  [string]$MediaMode = "Photo",
  [ValidateRange(8, 64)]
  [int]$Views = 16,
  [ValidateRange(1000, 30000)]
  [int]$Iterations = 1000,
  [ValidateSet(1, 2, 4, 8)]
  [int]$Resolution = 8,
  [string]$WorkRoot,
  [string]$NativeExecutable,
  [switch]$SkipNativeLoad
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$RepositoryDrive = Split-Path -Qualifier $RepositoryRoot
$Utf8NoBom = [Text.UTF8Encoding]::new($false)

function Write-JsonFile {
  param(
    [Parameter(Mandatory = $true)]$Value,
    [Parameter(Mandatory = $true)][string]$Path
  )

  $json = $Value | ConvertTo-Json -Depth 12
  [IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $Utf8NoBom)
}

function Find-GaussianEnvironment {
  $candidates = @(
    $env:GAUSSIAN_SPLATTING_CONDA_PREFIX,
    $env:GS_CONDA_PREFIX,
    (Join-Path $RepositoryDrive "conda\envs\gaussian_splatting"),
    (Join-Path $RepositoryDrive "miniforge3\envs\gaussian_splatting"),
    (Join-Path $RepositoryDrive "anaconda\envs\gaussian_splatting"),
    (Join-Path $env:USERPROFILE "miniforge3\envs\gaussian_splatting"),
    (Join-Path $env:USERPROFILE "miniconda3\envs\gaussian_splatting")
  ) | Where-Object { $_ }

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $candidate "python.exe") -PathType Leaf) {
      return (Resolve-Path $candidate).Path
    }
  }
  throw "Could not find a gaussian_splatting environment."
}

function Find-ColmapExecutable {
  $candidates = [Collections.Generic.List[string]]::new()
  foreach ($configured in @($env:COLMAP_EXE, $env:COLMAP_PATH)) {
    if (-not $configured) {
      continue
    }
    if (Test-Path -LiteralPath $configured -PathType Container) {
      $candidates.Add((Join-Path $configured "colmap.exe"))
    } else {
      $candidates.Add($configured)
    }
  }
  $candidates.Add((Join-Path $RepositoryDrive "COLMAP\bin\colmap.exe"))

  $versionedRoot = Join-Path $RepositoryDrive "Tools\COLMAP"
  if (Test-Path -LiteralPath $versionedRoot -PathType Container) {
    $versioned = Get-ChildItem -LiteralPath $versionedRoot -Directory |
      Sort-Object {
        try { [version]$_.Name } catch { [version]"0.0" }
      } -Descending
    foreach ($directory in $versioned) {
      $candidates.Add((Join-Path $directory.FullName "bin\colmap.exe"))
    }
  }

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path $candidate).Path
    }
  }
  throw "Could not find colmap.exe."
}

function Invoke-CheckedPython {
  param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

  & $script:PythonExecutable @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Python command failed with exit code $LASTEXITCODE."
  }
}

$GaussianEnvironment = Find-GaussianEnvironment
$PythonExecutable = Join-Path $GaussianEnvironment "python.exe"
$ColmapExecutable = Find-ColmapExecutable
$ColmapDirectory = Split-Path -Parent $ColmapExecutable
$env:CONDA_PREFIX = $GaussianEnvironment
$env:GAUSSIAN_SPLATTING_CONDA_PREFIX = $GaussianEnvironment
$env:GS_CONDA_PREFIX = $GaussianEnvironment
$env:COLMAP_EXE = $ColmapExecutable
$env:COLMAP_PATH = $ColmapExecutable
$env:PYTHONUTF8 = "1"
$env:Path = [string]::Join(";", @(
    $ColmapDirectory,
    $GaussianEnvironment,
    (Join-Path $GaussianEnvironment "Library\bin"),
    (Join-Path $GaussianEnvironment "Library\usr\bin"),
    (Join-Path $GaussianEnvironment "Scripts"),
    $env:Path
  ))

if (-not $WorkRoot) {
  $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $WorkRoot = Join-Path $RepositoryRoot ".tools\training-e2e\$timestamp-$($MediaMode.ToLowerInvariant())"
}
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $WorkRoot) {
  $entries = @(Get-ChildItem -Force -LiteralPath $WorkRoot)
  if ($entries.Count -gt 0) {
    throw "WorkRoot must be new or empty: $WorkRoot"
  }
}

$MediaRoot = Join-Path $WorkRoot "media"
$ProjectRoot = Join-Path $WorkRoot "project"
$DatasetRoot = Join-Path $ProjectRoot "datasets"
$OutputRoot = Join-Path $ProjectRoot "output"
$JobsRoot = Join-Path $ProjectRoot "jobs"
foreach ($directory in @($MediaRoot, $DatasetRoot, $OutputRoot, $JobsRoot)) {
  New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

$Generator = Join-Path $PSScriptRoot "generate_colmap_training_fixture.py"
Invoke-CheckedPython $Generator "--output-dir" $MediaRoot "--views" "$Views"
$Photos = @(Get-ChildItem -LiteralPath (Join-Path $MediaRoot "photos") -File -Filter "*.jpg" | Sort-Object Name)
if ($Photos.Count -ne $Views) {
  throw "Fixture generator produced $($Photos.Count) photos; expected $Views."
}

$Scene = "e2e-$($MediaMode.ToLowerInvariant())"
$ImportFiles = @()
if ($MediaMode -eq "Video") {
  $Ffmpeg = Join-Path $GaussianEnvironment "Library\bin\ffmpeg.exe"
  if (-not (Test-Path -LiteralPath $Ffmpeg -PathType Leaf)) {
    throw "Video smoke test requires FFmpeg: $Ffmpeg"
  }
  $VideoPath = Join-Path $MediaRoot "e2e-source.mp4"
  & $Ffmpeg -hide_banner -loglevel error -y -framerate 2 -start_number 0 `
    -i (Join-Path $MediaRoot "photos\view_%03d.jpg") -frames:v $Views `
    -c:v libx264 -pix_fmt yuv420p $VideoPath
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $VideoPath -PathType Leaf)) {
    throw "Could not create the video import fixture."
  }
  $ImportFiles = @([ordered]@{
      path = $VideoPath
      name = "e2e-source.mp4"
      relativePath = "e2e-source.mp4"
      type = "video/mp4"
    })
} else {
  $ImportFiles = @($Photos | ForEach-Object {
      [ordered]@{
        path = $_.FullName
        name = $_.Name
        relativePath = $_.Name
        type = "image/jpeg"
      }
    })
}

$ImportConfigPath = Join-Path $WorkRoot "import.json"
$ImportConfig = [ordered]@{
  task = "import"
  repositoryRoot = $RepositoryRoot
  projectRoot = $ProjectRoot
  datasetRoot = $DatasetRoot
  scene = $Scene
  overwrite = $false
  fps = 2
  files = $ImportFiles
}
Write-JsonFile $ImportConfig $ImportConfigPath

$Worker = Join-Path $RepositoryRoot "native\worker\gsw_worker.py"
Invoke-CheckedPython $Worker "--config" $ImportConfigPath
$DatasetPath = Join-Path $DatasetRoot $Scene
$ImportedImages = @(Get-ChildItem -LiteralPath (Join-Path $DatasetPath "images") -File)
if ($ImportedImages.Count -ne $Views) {
  throw "Media import produced $($ImportedImages.Count) training images; expected $Views."
}

$OutputScene = "$Scene-3dgs"
$TrainingConfigPath = Join-Path $WorkRoot "training.json"
$TrainingConfig = [ordered]@{
  task = "training"
  repositoryRoot = $RepositoryRoot
  datasetPath = $DatasetPath
  outputRoot = $OutputRoot
  outputScene = $OutputScene
  jobStore = $JobsRoot
  scene = $Scene
  backend = "3dgs"
  quality = "quick"
  runColmap = $true
  overwrite = $false
  colmapOptions = [ordered]@{
    preset = "robust"
    matching = "exhaustive"
    camera_model = "PINHOLE"
    single_camera = $true
    use_gpu = $true
    reset = $true
  }
  trainOptions = [ordered]@{
    iterations = $Iterations
    resolution = $Resolution
  }
}
Write-JsonFile $TrainingConfig $TrainingConfigPath
Invoke-CheckedPython $Worker "--config" $TrainingConfigPath

$ModelRoot = Join-Path $OutputRoot $OutputScene
$PlyPath = Join-Path $ModelRoot "point_cloud\iteration_$Iterations\point_cloud.ply"
$CheckpointPath = Join-Path $ModelRoot "chkpnt$Iterations.pth"
$ValidationPath = Join-Path $WorkRoot "validation.json"
$Validator = Join-Path $PSScriptRoot "validate_gaussian_training_output.py"
Invoke-CheckedPython $Validator $PlyPath "--checkpoint" $CheckpointPath `
  "--minimum-vertices" "1000" "--json-output" $ValidationPath

$ProjectPath = Join-Path $WorkRoot "trained-output.gsw.json"
$Project = [ordered]@{
  schemaVersion = 1
  application = "Gaussian Scene Workbench"
  projectName = "3DGS $MediaMode E2E Smoke"
  rootPath = $ProjectRoot
  datasetPath = $DatasetPath
  scenePath = $PlyPath
  updatedUtc = [DateTime]::UtcNow.ToString("o")
}
Write-JsonFile $Project $ProjectPath

$NativeLoaded = $false
if (-not $SkipNativeLoad) {
  if (-not $NativeExecutable) {
    $candidate = Join-Path $RepositoryRoot "native\build\Gaussian Scene Workbench.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      $NativeExecutable = $candidate
    }
  }
  if ($NativeExecutable) {
    # --smoke-test asserts an empty workspace, not a loaded model. Windows
    # GUI executables also do not reliably update PowerShell's LASTEXITCODE.
    # Wait for the renderer-specific check and read that process's exit code.
    $NativeProcess = Start-Process -FilePath $NativeExecutable `
      -ArgumentList ('--smoke-test-gaussian-performance --smoke-scene "{0}"' -f $PlyPath) `
      -WindowStyle Hidden -Wait -PassThru
    if ($NativeProcess.ExitCode -ne 0) {
      throw "Native model load smoke test failed with exit code $($NativeProcess.ExitCode)."
    }
    $NativeLoaded = $true
  } else {
    Write-Warning "Native executable was not found; PLY validation passed but UI load was skipped."
  }
}

$Validation = Get-Content -Raw -LiteralPath $ValidationPath | ConvertFrom-Json
$Summary = [ordered]@{
  version = 1
  valid = $true
  mediaMode = $MediaMode
  importedImages = $ImportedImages.Count
  iterations = $Iterations
  resolution = $Resolution
  gaussianVertices = $Validation.vertices
  ply = $PlyPath
  checkpoint = $CheckpointPath
  project = $ProjectPath
  nativeLoaded = $NativeLoaded
  workRoot = $WorkRoot
}
$SummaryPath = Join-Path $WorkRoot "summary.json"
Write-JsonFile $Summary $SummaryPath
Write-Output "3DGS end-to-end smoke test passed."
Write-Output ($Summary | ConvertTo-Json -Depth 6)
