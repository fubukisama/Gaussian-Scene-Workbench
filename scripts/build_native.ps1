param(
  [ValidateSet("Debug", "RelWithDebInfo", "Release")]
  [string]$Configuration = "RelWithDebInfo",
  [string]$QtRoot = "",
  [string]$BuildDirectory = "",
  [switch]$Clean,
  [switch]$Package
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$NativeRoot = Join-Path $Root "native"
$DriveRoot = Split-Path -Qualifier $Root

if ([string]::IsNullOrWhiteSpace($QtRoot)) {
  $QtCandidates = @(
    $env:GSW_NATIVE_QT_ROOT,
    (Join-Path $DriveRoot "conda\envs\gsw_native"),
    "E:\conda\envs\gsw_native"
  ) | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ "Library\lib\cmake\Qt6\Qt6Config.cmake")) }
  $QtRoot = $QtCandidates | Select-Object -First 1
}

if (-not $QtRoot -or -not (Test-Path -LiteralPath (Join-Path $QtRoot "Library\lib\cmake\Qt6\Qt6Config.cmake"))) {
  throw "Qt 6 environment not found. Set GSW_NATIVE_QT_ROOT or pass -QtRoot."
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
  $BuildDirectory = Join-Path $NativeRoot "build"
}

$VsDevCmdCandidates = @(
  (Join-Path $DriveRoot "vsi\Common7\Tools\VsDevCmd.bat"),
  "E:\vsi\Common7\Tools\VsDevCmd.bat",
  "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
) | Where-Object { Test-Path -LiteralPath $_ }
$VsDevCmd = $VsDevCmdCandidates | Select-Object -First 1
if (-not $VsDevCmd) {
  throw "Visual Studio C++ build environment not found."
}

function Import-VisualStudioEnvironment {
  param([Parameter(Mandatory = $true)][string]$ScriptPath)

  $command = '"{0}" -no_logo -arch=x64 && set' -f $ScriptPath
  $environmentLines = & $env:ComSpec /d /s /c $command
  if ($LASTEXITCODE -ne 0) {
    throw "VsDevCmd failed with exit code $LASTEXITCODE."
  }
  foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
  }
}

function Copy-AppLocalDependencies {
  param(
    [Parameter(Mandatory = $true)][string]$PackageBin,
    [Parameter(Mandatory = $true)][string[]]$SearchRoots
  )

  $DumpBin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
  $available = @{}
  foreach ($searchRoot in $SearchRoots) {
    if (-not $searchRoot -or -not (Test-Path -LiteralPath $searchRoot)) { continue }
    Get-ChildItem -LiteralPath $searchRoot -Filter *.dll -File -ErrorAction SilentlyContinue | ForEach-Object {
      if (-not $available.ContainsKey($_.Name)) {
        $available[$_.Name] = $_.FullName
      }
    }
  }

  $present = @{}
  Get-ChildItem -LiteralPath $PackageBin -Recurse -File | Where-Object { $_.Extension -in ".dll", ".exe" } | ForEach-Object {
    $present[$_.Name] = $_.FullName
  }

  $queue = [System.Collections.Generic.Queue[string]]::new()
  foreach ($path in $present.Values) { $queue.Enqueue($path) }
  $processed = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

  while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    if (-not $processed.Add($binary)) { continue }
    $dependencyOutput = & $DumpBin /nologo /dependents $binary 2>$null
    foreach ($line in $dependencyOutput) {
      if ($line -notmatch '^\s+([A-Za-z0-9_.+\-]+\.dll)\s*$') { continue }
      $dependency = $Matches[1]
      if ($present.ContainsKey($dependency) -or -not $available.ContainsKey($dependency)) { continue }
      $destination = Join-Path $PackageBin $dependency
      Copy-Item -LiteralPath $available[$dependency] -Destination $destination -Force
      $present[$dependency] = $destination
      $queue.Enqueue($destination)
      Write-Host "Adding app-local dependency $dependency"
    }
  }
}

function Remove-DirectoryWithRetry {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Attempts = 6
  )

  for ($Attempt = 1; $Attempt -le $Attempts; $Attempt++) {
    try {
      Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
      return
    } catch {
      if ($Attempt -eq $Attempts) {
        throw "Unable to remove package directory after $Attempts attempts: $Path. $($_.Exception.Message)"
      }
      Start-Sleep -Milliseconds (350 * $Attempt)
    }
  }
}

Import-VisualStudioEnvironment -ScriptPath $VsDevCmd

$CMake = Join-Path $QtRoot "Library\bin\cmake.exe"
$Ninja = Join-Path $QtRoot "Library\bin\ninja.exe"
$WinDeployQtCandidates = @(
  (Join-Path $QtRoot "Library\bin\windeployqt.exe"),
  (Join-Path $QtRoot "Library\bin\windeployqt6.exe"),
  (Join-Path $QtRoot "Library\lib\qt6\bin\windeployqt.exe")
) | Where-Object { Test-Path -LiteralPath $_ }
$WinDeployQt = $WinDeployQtCandidates | Select-Object -First 1
foreach ($tool in @($CMake, $Ninja)) {
  if (-not (Test-Path -LiteralPath $tool)) {
    throw "Required native build tool not found: $tool"
  }
}

$QtRuntimePaths = @(
  $QtRoot,
  (Join-Path $QtRoot "Library\bin"),
  (Join-Path $QtRoot "Library\lib\qt6\bin"),
  (Join-Path $QtRoot "Scripts")
) -join ";"
$env:Path = "$QtRuntimePaths;$env:Path"

if ($Clean -and (Test-Path -LiteralPath $BuildDirectory)) {
  $resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
  $resolvedNative = (Resolve-Path -LiteralPath $NativeRoot).Path
  if (-not $resolvedBuild.StartsWith($resolvedNative, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean a build directory outside native/: $resolvedBuild"
  }
  Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

& $CMake `
  -S $NativeRoot `
  -B $BuildDirectory `
  -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$Ninja" `
  "-DCMAKE_PREFIX_PATH=$(Join-Path $QtRoot 'Library')" `
  "-DCMAKE_BUILD_TYPE=$Configuration" `
  "-DBUILD_TESTING=ON"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

& $CMake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE." }

& $CMake --build $BuildDirectory --target test
if ($LASTEXITCODE -ne 0) { throw "Native tests failed with exit code $LASTEXITCODE." }

$Executable = Join-Path $BuildDirectory "Gaussian Scene Workbench.exe"
if (-not (Test-Path -LiteralPath $Executable)) {
  throw "Native executable was not produced: $Executable"
}

Write-Host "Native build completed:"
Write-Host $Executable

if ($Package) {
  if (-not (Test-Path -LiteralPath $WinDeployQt)) {
    throw "windeployqt was not found: $WinDeployQt"
  }
  $PackageRoot = Join-Path $NativeRoot "dist\Gaussian-Scene-Workbench-0.3.0-native-preview-win-x64"
  if (Test-Path -LiteralPath $PackageRoot) {
    $ResolvedPackage = (Resolve-Path -LiteralPath $PackageRoot).Path
    $ResolvedDist = (Resolve-Path -LiteralPath (Join-Path $NativeRoot "dist")).Path
    if (-not $ResolvedPackage.StartsWith($ResolvedDist + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to remove a package directory outside native/dist: $ResolvedPackage"
    }
    Remove-DirectoryWithRetry -Path $ResolvedPackage
  }
  & $CMake --install $BuildDirectory --prefix $PackageRoot
  if ($LASTEXITCODE -ne 0) { throw "Native install failed with exit code $LASTEXITCODE." }
  $PackagedExecutable = Join-Path $PackageRoot "bin\Gaussian Scene Workbench.exe"
  & $WinDeployQt --release --no-translations --compiler-runtime $PackagedExecutable
  if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
  $RuntimeSearchRoots = @(
    (Join-Path $QtRoot "Library\bin"),
    (Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT")
  )
  Copy-AppLocalDependencies -PackageBin (Split-Path -Parent $PackagedExecutable) -SearchRoots $RuntimeSearchRoots
  Copy-Item -LiteralPath (Join-Path $NativeRoot "README.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $NativeRoot "build_manifest.json") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\NATIVE_MIGRATION.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\NATIVE_PARITY.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "THIRD_PARTY_LICENSES.md") -Destination $PackageRoot -Force
  & (Join-Path $Root "scripts\stage_native_backend.ps1") `
    -SourceRoot $Root `
    -DestinationRoot $PackageRoot
  Write-Host "Native package directory:"
  Write-Host $PackageRoot
}
