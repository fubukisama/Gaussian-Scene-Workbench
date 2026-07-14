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
$BuildManifestPath = Join-Path $NativeRoot "build_manifest.json"
if (-not (Test-Path -LiteralPath $BuildManifestPath)) {
  throw "Native build manifest not found: $BuildManifestPath"
}
$BuildManifest = Get-Content -LiteralPath $BuildManifestPath -Raw | ConvertFrom-Json
$ReleaseDate = [string]$BuildManifest.releaseDate
if ($ReleaseDate -notmatch '^\d{4}-\d{2}-\d{2}$') {
  throw "Native build manifest contains an invalid releaseDate: $ReleaseDate"
}

if ([string]::IsNullOrWhiteSpace($QtRoot)) {
  $QtCandidates = @(
    $env:GSW_NATIVE_QT_ROOT,
    $env:CONDA_PREFIX,
    (Join-Path $env:USERPROFILE "miniforge3\envs\gsw_native"),
    (Join-Path $env:USERPROFILE "miniconda3\envs\gsw_native"),
    (Join-Path $env:USERPROFILE "anaconda3\envs\gsw_native"),
    (Join-Path $DriveRoot "conda\envs\gsw_native"),
    "E:\conda\envs\gsw_native"
  ) | Where-Object {
    $_ -and (Test-Path -LiteralPath ([IO.Path]::Combine(
      [string]$_,
      "Library\lib\cmake\Qt6\Qt6Config.cmake"
    )))
  }
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

function Resolve-SafeRemovalDirectory {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$AllowedParent
  )

  $TargetItem = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
  $ParentItem = Get-Item -LiteralPath $AllowedParent -Force -ErrorAction Stop
  if (-not $TargetItem.PSIsContainer) {
    throw "Refusing to recursively remove a non-directory path: $($TargetItem.FullName)"
  }
  if (-not $ParentItem.PSIsContainer) {
    throw "Removal boundary is not a directory: $($ParentItem.FullName)"
  }
  if (($ParentItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing to remove through a reparse-point boundary: $($ParentItem.FullName)"
  }

  $ResolvedTarget = [IO.Path]::GetFullPath($TargetItem.FullName).TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar
  )
  $ResolvedParent = [IO.Path]::GetFullPath($ParentItem.FullName).TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar
  )
  $ParentPrefix = $ResolvedParent + [IO.Path]::DirectorySeparatorChar
  if (-not $ResolvedTarget.StartsWith($ParentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove a directory outside the allowed parent '$ResolvedParent': $ResolvedTarget"
  }

  $Cursor = $TargetItem
  while (-not $Cursor.FullName.Equals($ResolvedParent, [StringComparison]::OrdinalIgnoreCase)) {
    if (($Cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
      throw "Refusing to recursively remove a reparse point or junction: $($Cursor.FullName)"
    }
    $Cursor = $Cursor.Parent
    if ($null -eq $Cursor) {
      throw "Unable to verify removal boundary for: $ResolvedTarget"
    }
  }

  # Enumerate one directory level at a time so junctions are detected before
  # they can be traversed. Remove-Item -Recurse is only used after the complete
  # target tree has been proven free of reparse points.
  $PendingDirectories = New-Object System.Collections.Stack
  $PendingDirectories.Push($TargetItem)
  while ($PendingDirectories.Count -gt 0) {
    $Directory = $PendingDirectories.Pop()
    foreach ($Child in Get-ChildItem -LiteralPath $Directory.FullName -Force -ErrorAction Stop) {
      if (($Child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to recursively remove a tree containing a reparse point or junction: $($Child.FullName)"
      }
      if ($Child.PSIsContainer) {
        $PendingDirectories.Push($Child)
      }
    }
  }
  return $ResolvedTarget
}

function Remove-DirectoryWithRetry {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$AllowedParent,
    [int]$Attempts = 6
  )

  $SafePath = Resolve-SafeRemovalDirectory -Path $Path -AllowedParent $AllowedParent
  for ($Attempt = 1; $Attempt -le $Attempts; $Attempt++) {
    try {
      Remove-Item -LiteralPath $SafePath -Recurse -Force -ErrorAction Stop
      return
    } catch {
      if ($Attempt -eq $Attempts) {
        throw "Unable to remove directory after $Attempts attempts: $SafePath. $($_.Exception.Message)"
      }
      Start-Sleep -Milliseconds (350 * $Attempt)
    }
  }
}

function Find-PythonForNativeChecks {
  $PathCommand = Get-Command python.exe -ErrorAction SilentlyContinue
  $Candidates = @(
    $(if ($PathCommand) { $PathCommand.Source }),
    $(if ($env:GAUSSIAN_SPLATTING_CONDA_PREFIX) { Join-Path $env:GAUSSIAN_SPLATTING_CONDA_PREFIX "python.exe" }),
    $(if ($env:GS_CONDA_PREFIX) { Join-Path $env:GS_CONDA_PREFIX "python.exe" }),
    $(if ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX "python.exe" })
  ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
  foreach ($Candidate in $Candidates | Select-Object -Unique) {
    & $Candidate -B -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
    if ($LASTEXITCODE -eq 0) {
      return (Resolve-Path -LiteralPath $Candidate).Path
    }
  }
  throw "Python 3.10 or newer was not found; native worker tests and syntax gates cannot run."
}

function Invoke-PythonSyntaxGate {
  param(
    [Parameter(Mandatory = $true)][string]$Python,
    [Parameter(Mandatory = $true)][string]$SourceRoot
  )

  $CompileScript = @'
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
paths = sorted(root.rglob("*.py"))
if not paths:
    raise SystemExit("No staged Python files were found under {}".format(root))
for path in paths:
    compile(path.read_bytes(), str(path), "exec")
print("Compiled {} staged Python files without bytecode output.".format(len(paths)))
'@
  $CompileScript | & $Python -B - $SourceRoot
  if ($LASTEXITCODE -ne 0) {
    throw "Staged Python syntax gate failed with exit code $LASTEXITCODE."
  }
}

function Test-NinjaDependencyListContainsPath {
  param(
    [Parameter(Mandatory = $true)][object[]]$DependencyLines,
    [Parameter(Mandatory = $true)][string]$Directory,
    [Parameter(Mandatory = $true)][string]$ExpectedPath
  )

  $ExpectedFullPath = [IO.Path]::GetFullPath($ExpectedPath)
  foreach ($DependencyLine in $DependencyLines) {
    $Candidate = $DependencyLine.ToString().Trim()
    if ([string]::IsNullOrWhiteSpace($Candidate) -or $Candidate.Contains(': #deps ')) {
      continue
    }
    try {
      $CandidateFullPath = if ([IO.Path]::IsPathRooted($Candidate)) {
        [IO.Path]::GetFullPath($Candidate)
      } else {
        [IO.Path]::GetFullPath((Join-Path $Directory $Candidate))
      }
      if ($CandidateFullPath.Equals($ExpectedFullPath, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
      }
    } catch {
      continue
    }
  }
  return $false
}

function Test-NativeBuildNeedsDependencyReset {
  param(
    [Parameter(Mandatory = $true)][string]$Ninja,
    [Parameter(Mandatory = $true)][string]$Directory,
    [Parameter(Mandatory = $true)][string]$ObjectPath,
    [Parameter(Mandatory = $true)][string]$ExpectedHeader
  )

  if (-not (Test-Path -LiteralPath (Join-Path $Directory $ObjectPath))) {
    return $false
  }

  $DependencyLines = @(& $Ninja -C $Directory -t deps $ObjectPath)
  if ($LASTEXITCODE -ne 0) {
    return $true
  }

  return -not (Test-NinjaDependencyListContainsPath `
    -DependencyLines $DependencyLines `
    -Directory $Directory `
    -ExpectedPath $ExpectedHeader)
}

function Get-MsvcShowIncludesPrefix {
  $ProbeName = "gsw-showincludes-$([Guid]::NewGuid().ToString('N'))"
  $ProbeHeader = Join-Path ([IO.Path]::GetTempPath()) "$ProbeName.h"
  $ProbeSource = Join-Path ([IO.Path]::GetTempPath()) "$ProbeName.cpp"
  $OriginalConsoleEncoding = [Console]::OutputEncoding
  $OriginalPipelineEncoding = $OutputEncoding
  $OemEncoding = [Text.Encoding]::GetEncoding(
    [Globalization.CultureInfo]::CurrentUICulture.TextInfo.OEMCodePage
  )

  try {
    [IO.File]::WriteAllText(
      $ProbeHeader,
      "#pragma once`r`n",
      [Text.UTF8Encoding]::new($false)
    )
    $PortableHeaderPath = $ProbeHeader.Replace('\', '/')
    [IO.File]::WriteAllText(
      $ProbeSource,
      "#include `"$PortableHeaderPath`"`r`n",
      [Text.UTF8Encoding]::new($false)
    )

    [Console]::OutputEncoding = $OemEncoding
    $OutputEncoding = $OemEncoding
    $CompilerOutput = @(& cl.exe /nologo /showIncludes /EP $ProbeSource 2>&1)
    if ($LASTEXITCODE -ne 0) {
      throw "MSVC /showIncludes probe failed with exit code $LASTEXITCODE."
    }

    foreach ($OutputLine in $CompilerOutput) {
      $Text = $OutputLine.ToString()
      if (-not $Text.EndsWith("$ProbeName.h", [StringComparison]::OrdinalIgnoreCase)) {
        continue
      }
      $PathStart = [regex]::Match($Text, '[A-Za-z]:[\\/]').Index
      if ($PathStart -gt 0) {
        return $Text.Substring(0, $PathStart)
      }
    }
    throw "MSVC /showIncludes probe did not emit a recognizable header dependency line."
  } finally {
    [Console]::OutputEncoding = $OriginalConsoleEncoding
    $OutputEncoding = $OriginalPipelineEncoding
    [IO.File]::Delete($ProbeSource)
    [IO.File]::Delete($ProbeHeader)
  }
}

function Assert-NinjaHeaderDependency {
  param(
    [Parameter(Mandatory = $true)][string]$Ninja,
    [Parameter(Mandatory = $true)][string]$Directory,
    [Parameter(Mandatory = $true)][string]$ObjectPath,
    [Parameter(Mandatory = $true)][string]$ExpectedHeader
  )

  $DependencyLines = @(& $Ninja -C $Directory -t deps $ObjectPath)
  if ($LASTEXITCODE -ne 0) {
    throw "Unable to inspect Ninja header dependencies for $ObjectPath."
  }

  $ExpectedPath = [IO.Path]::GetFullPath($ExpectedHeader)
  $HasExpectedHeader = Test-NinjaDependencyListContainsPath `
    -DependencyLines $DependencyLines `
    -Directory $Directory `
    -ExpectedPath $ExpectedPath
  if (-not $HasExpectedHeader) {
    throw "Native build did not record the required header dependency '$ExpectedPath' for '$ObjectPath'. Refusing to package a potentially ABI-inconsistent executable."
  }
}

function Invoke-NativeLaunchSmokeTest {
  param([Parameter(Mandatory = $true)][string]$Executable)

  $PackageBin = Split-Path -Parent $Executable
  $LaunchEnvironment = @{
    PATH = [Environment]::GetEnvironmentVariable("PATH", "Process")
    QT_OPENGL = [Environment]::GetEnvironmentVariable("QT_OPENGL", "Process")
    QT_PLUGIN_PATH = [Environment]::GetEnvironmentVariable("QT_PLUGIN_PATH", "Process")
    QT_QPA_PLATFORM = [Environment]::GetEnvironmentVariable("QT_QPA_PLATFORM", "Process")
    QT_QPA_PLATFORM_PLUGIN_PATH = [Environment]::GetEnvironmentVariable(
      "QT_QPA_PLATFORM_PLUGIN_PATH",
      "Process"
    )
  }
  $Process = $null
  try {
    $IsolatedPath = @(
      $PackageBin,
      (Join-Path $env:SystemRoot "System32"),
      $env:SystemRoot,
      (Join-Path $env:SystemRoot "System32\Wbem")
    ) -join ";"
    [Environment]::SetEnvironmentVariable("PATH", $IsolatedPath, "Process")
    [Environment]::SetEnvironmentVariable("QT_OPENGL", "software", "Process")
    [Environment]::SetEnvironmentVariable("QT_PLUGIN_PATH", $null, "Process")
    [Environment]::SetEnvironmentVariable("QT_QPA_PLATFORM", "windows", "Process")
    [Environment]::SetEnvironmentVariable("QT_QPA_PLATFORM_PLUGIN_PATH", $null, "Process")

    $Process = Start-Process `
      -FilePath $Executable `
      -ArgumentList "--smoke-test" `
      -WorkingDirectory $PackageBin `
      -WindowStyle Hidden `
      -PassThru `
      -ErrorAction Stop
    if (-not $Process.WaitForExit(15000)) {
      Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
      $Process.WaitForExit(5000) | Out-Null
      throw "Native application launch smoke test timed out: $Executable"
    }
    if ($Process.ExitCode -ne 0) {
      $UnsignedExitCode = [BitConverter]::ToUInt32(
        [BitConverter]::GetBytes([int]$Process.ExitCode),
        0
      )
      throw "Native application launch smoke test failed with exit code $($Process.ExitCode) (0x$($UnsignedExitCode.ToString('X8'))): $Executable"
    }
  } finally {
    if ($null -ne $Process) {
      if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit(5000) | Out-Null
      }
      $Process.Dispose()
    }
    foreach ($Name in $LaunchEnvironment.Keys) {
      [Environment]::SetEnvironmentVariable($Name, $LaunchEnvironment[$Name], "Process")
    }
  }
}

Import-VisualStudioEnvironment -ScriptPath $VsDevCmd

$CMake = Join-Path $QtRoot "Library\bin\cmake.exe"
$CTest = Join-Path $QtRoot "Library\bin\ctest.exe"
$Ninja = Join-Path $QtRoot "Library\bin\ninja.exe"
$WinDeployQtCandidates = @(
  (Join-Path $QtRoot "Library\bin\windeployqt.exe"),
  (Join-Path $QtRoot "Library\bin\windeployqt6.exe"),
  (Join-Path $QtRoot "Library\lib\qt6\bin\windeployqt.exe")
) | Where-Object { Test-Path -LiteralPath $_ }
$WinDeployQt = $WinDeployQtCandidates | Select-Object -First 1
foreach ($tool in @($CMake, $CTest, $Ninja)) {
  if (-not (Test-Path -LiteralPath $tool)) {
    throw "Required native build tool not found: $tool"
  }
}
$MsvcShowIncludesPrefix = Get-MsvcShowIncludesPrefix
$Utf8ConsoleEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $Utf8ConsoleEncoding
$OutputEncoding = $Utf8ConsoleEncoding

$QtRuntimePaths = @(
  $QtRoot,
  (Join-Path $QtRoot "Library\bin"),
  (Join-Path $QtRoot "Library\lib\qt6\bin"),
  (Join-Path $QtRoot "Scripts")
) -join ";"
$env:Path = "$QtRuntimePaths;$env:Path"

if ($Clean -and (Test-Path -LiteralPath $BuildDirectory)) {
  Remove-DirectoryWithRetry -Path $BuildDirectory -AllowedParent $NativeRoot
} elseif ((Test-Path -LiteralPath $BuildDirectory) -and
          (Test-NativeBuildNeedsDependencyReset `
            -Ninja $Ninja `
            -Directory $BuildDirectory `
            -ObjectPath "CMakeFiles/GaussianSceneWorkbench.dir/src/main.cpp.obj" `
            -ExpectedHeader (Join-Path $NativeRoot "src\MainWindow.h"))) {
  Write-Host "Resetting native build cache to repair localized MSVC header dependency tracking."
  Remove-DirectoryWithRetry -Path $BuildDirectory -AllowedParent $NativeRoot
}

& $CMake `
  -S $NativeRoot `
  -B $BuildDirectory `
  -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$Ninja" `
  "-DCMAKE_PREFIX_PATH=$(Join-Path $QtRoot 'Library')" `
  "-DCMAKE_BUILD_TYPE=$Configuration" `
  "-DGSW_RELEASE_DATE=$ReleaseDate" `
  "-DGSW_MSVC_SHOWINCLUDES_PREFIX=$($MsvcShowIncludesPrefix.TrimEnd())" `
  "-DBUILD_TESTING=ON"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

& $CMake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed with exit code $LASTEXITCODE." }

Assert-NinjaHeaderDependency `
  -Ninja $Ninja `
  -Directory $BuildDirectory `
  -ObjectPath "CMakeFiles/GaussianSceneWorkbench.dir/src/main.cpp.obj" `
  -ExpectedHeader (Join-Path $NativeRoot "src\MainWindow.h")

& $CTest --test-dir $BuildDirectory --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed with exit code $LASTEXITCODE." }

$CheckPython = Find-PythonForNativeChecks
$PreviousBytecodeSetting = $env:PYTHONDONTWRITEBYTECODE
$env:PYTHONDONTWRITEBYTECODE = "1"
Push-Location $Root
try {
  & $CheckPython -B -m unittest native.worker.test_gsw_worker native.worker.test_import_preflight
  if ($LASTEXITCODE -ne 0) {
    throw "Native worker tests failed with exit code $LASTEXITCODE."
  }
} finally {
  Pop-Location
  $env:PYTHONDONTWRITEBYTECODE = $PreviousBytecodeSetting
}

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
    Remove-DirectoryWithRetry -Path $PackageRoot -AllowedParent (Join-Path $NativeRoot "dist")
  }
  & $CMake --install $BuildDirectory --prefix $PackageRoot
  if ($LASTEXITCODE -ne 0) { throw "Native install failed with exit code $LASTEXITCODE." }
  $PackagedExecutable = Join-Path $PackageRoot "bin\Gaussian Scene Workbench.exe"
  $DeployConfigurationFlag = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
  & $WinDeployQt $DeployConfigurationFlag --no-translations --compiler-runtime $PackagedExecutable
  if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
  $RuntimeSearchRoots = @(
    (Join-Path $QtRoot "Library\bin"),
    (Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT")
  )
  Copy-AppLocalDependencies -PackageBin (Split-Path -Parent $PackagedExecutable) -SearchRoots $RuntimeSearchRoots
  Invoke-NativeLaunchSmokeTest -Executable $PackagedExecutable
  Copy-Item -LiteralPath (Join-Path $NativeRoot "README.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $NativeRoot "build_manifest.json") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\NATIVE_MIGRATION.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\NATIVE_PARITY.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "THIRD_PARTY_LICENSES.md") -Destination $PackageRoot -Force
  & (Join-Path $Root "scripts\stage_native_backend.ps1") `
    -SourceRoot $Root `
    -DestinationRoot $PackageRoot
  Invoke-PythonSyntaxGate -Python $CheckPython -SourceRoot $PackageRoot
  $RequiredBackendFiles = @(
    "native\worker\gsw_worker.py",
    "native\worker\import_preflight.py",
    "crop_editor\server.py",
    "crop_editor\video_extract.py",
    "scripts\check_3dgs_env.ps1",
    "gaussian-splatting\train.py",
    "training_kit\apply_local_fixes.bat",
    "backend_manifest.json"
  ) | ForEach-Object { Join-Path $PackageRoot $_ }
  $MissingBackendFiles = $RequiredBackendFiles | Where-Object {
    -not (Test-Path -LiteralPath $_)
  }
  if ($MissingBackendFiles) {
    throw "Native package is missing backend files: $($MissingBackendFiles -join ', ')"
  }
  Write-Host "Native package directory:"
  Write-Host $PackageRoot
}
