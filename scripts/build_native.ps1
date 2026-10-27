param(
  [ValidateSet("Debug", "RelWithDebInfo", "Release")]
  [string]$Configuration = "RelWithDebInfo",
  [string]$QtRoot = "",
  [string]$CMakeRoot = "",
  [string]$MsvcEnvironmentScript = "",
  [string]$BuildDirectory = "",
  [string]$PackageDirectoryName = "Gaussian-Scene-Workbench-0.3.1-native-preview-win-x64",
  [ValidateSet("windows", "offscreen")]
  [string]$PackageSmokePlatform = "windows",
  [switch]$Clean,
  [switch]$Package,
  [string]$SigningCertificateThumbprint = "",
  [string]$SigningCertificateStoreLocation = "CurrentUser",
  [string]$SigningTimestampUrl = "",
  [string]$SignToolPath = ""
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

if ([string]::IsNullOrWhiteSpace($SigningCertificateThumbprint)) {
  $SigningCertificateThumbprint = $env:GSW_WINDOWS_SIGNING_CERTIFICATE_THUMBPRINT
}
if (-not $PSBoundParameters.ContainsKey("SigningCertificateStoreLocation") -and
    -not [string]::IsNullOrWhiteSpace($env:GSW_WINDOWS_SIGNING_CERTIFICATE_STORE_LOCATION)) {
  $SigningCertificateStoreLocation = $env:GSW_WINDOWS_SIGNING_CERTIFICATE_STORE_LOCATION
}
if ([string]::IsNullOrWhiteSpace($SigningTimestampUrl)) {
  $SigningTimestampUrl = $env:GSW_WINDOWS_SIGNING_TIMESTAMP_URL
}
if ([string]::IsNullOrWhiteSpace($SignToolPath)) {
  $SignToolPath = $env:GSW_WINDOWS_SIGNTOOL_PATH
}
if ([string]::IsNullOrWhiteSpace($CMakeRoot)) {
  $CMakeRoot = $env:GSW_NATIVE_CMAKE_ROOT
}
if ([string]::IsNullOrWhiteSpace($MsvcEnvironmentScript)) {
  $MsvcEnvironmentScript = $env:GSW_NATIVE_MSVC_SETUP
}
if ($MsvcEnvironmentScript -and
    -not (Test-Path -LiteralPath $MsvcEnvironmentScript -PathType Leaf)) {
  throw "MSVC environment script not found: $MsvcEnvironmentScript"
}
if ($SigningCertificateStoreLocation -notin @("CurrentUser", "LocalMachine")) {
  throw "SigningCertificateStoreLocation must be CurrentUser or LocalMachine."
}
$SigningEnabled = -not [string]::IsNullOrWhiteSpace($SigningCertificateThumbprint)

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
  $MsvcEnvironmentScript,
  (Join-Path $DriveRoot "vsi\Common7\Tools\VsDevCmd.bat"),
  "E:\vsi\Common7\Tools\VsDevCmd.bat",
  "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }
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
    $(if ($QtRoot) { Join-Path $QtRoot "python.exe" }),
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

  $PackageBin = [IO.Path]::GetFullPath((Split-Path -Parent $Executable))
  $SmokeSettingsRoot = [IO.Path]::GetFullPath((Join-Path $PackageBin 'test-settings'))
  # Snapshot ownership before starting the one process this gate owns. Existing
  # QA data, including an empty directory, must never be adopted for cleanup.
  $SmokeSettingsRootExisted = Test-Path -LiteralPath $SmokeSettingsRoot
  if (-not [IO.Path]::GetDirectoryName($SmokeSettingsRoot).Equals(
        $PackageBin.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar),
        [StringComparison]::OrdinalIgnoreCase) -or
      [IO.Path]::GetFileName($SmokeSettingsRoot) -cne 'test-settings') {
    throw 'Invalid fixed smoke-settings child path.'
  }
  $PackageBinInfo = Get-Item -LiteralPath $PackageBin -Force
  if (-not $PackageBinInfo.PSIsContainer -or
      ($PackageBinInfo.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Package launch requires a regular application-bin directory.'
  }
  $Process = $null
  $StdoutRead = $null
  $StderrRead = $null
  try {
    $IsolatedPath = @(
      $PackageBin,
      (Join-Path $env:SystemRoot "System32"),
      $env:SystemRoot,
      (Join-Path $env:SystemRoot "System32\Wbem")
    ) -join ";"
    # Offscreen is an explicit local packaging/dependency check, not a claim of
    # real-window/OpenGL acceptance. Keep Windows as the existing CI default.
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $Executable
    $StartInfo.Arguments = '--smoke-test'
    $StartInfo.WorkingDirectory = $PackageBin
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    # Configure only this child. Never mutate the packaging process's Qt/PATH
    # environment (including the distinction between missing and empty values).
    $StartInfo.EnvironmentVariables['PATH'] = $IsolatedPath
    $StartInfo.EnvironmentVariables['QT_OPENGL'] = 'software'
    $StartInfo.EnvironmentVariables['QT_QPA_PLATFORM'] = $PackageSmokePlatform
    $StartInfo.EnvironmentVariables.Remove('QT_PLUGIN_PATH')
    $StartInfo.EnvironmentVariables.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')
    # GUI Qt otherwise sends diagnostics only to the Windows debugger. Drain
    # both streams concurrently so a failed/verbose startup cannot deadlock.
    $StartInfo.EnvironmentVariables['QT_FORCE_STDERR_LOGGING'] = '1'
    $Process = [System.Diagnostics.Process]::Start($StartInfo)
    $StdoutRead = $Process.StandardOutput.ReadToEndAsync()
    $StderrRead = $Process.StandardError.ReadToEndAsync()
    if (-not $Process.WaitForExit(60000)) {
      Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
      $Process.WaitForExit(5000) | Out-Null
      throw "Native application launch smoke test timed out: $Executable"
    }
    $Process.WaitForExit()
    if ($Process.ExitCode -ne 0) {
      $UnsignedExitCode = [BitConverter]::ToUInt32(
        [BitConverter]::GetBytes([int]$Process.ExitCode),
        0
      )
      throw "Native application launch smoke test failed with exit code $($Process.ExitCode) (0x$($UnsignedExitCode.ToString('X8'))): $Executable"
    }
  } finally {
    try {
      if ($null -ne $Process -and -not $Process.HasExited) {
          Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
          $Process.WaitForExit(5000) | Out-Null
      }
      $LogRoot = Join-Path $BuildDirectory 'packaged-startup'
      New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
      $LogStem = [IO.Path]::GetFileName((Split-Path -Parent $PackageBin)) + '-' + [guid]::NewGuid().ToString('N')
      foreach ($Capture in @(@('stdout', $StdoutRead), @('stderr', $StderrRead))) {
        $Task = $Capture[1]
        if ($null -ne $Task -and $Task.Wait(5000)) {
          [IO.File]::WriteAllText((Join-Path $LogRoot ($LogStem + '.' + $Capture[0] + '.log')),
            $Task.Result, [Text.UTF8Encoding]::new($false))
        }
      }
    } finally {
      try {
        # QTemporaryDir removes the per-run settings folder, but not the empty
        # parent created by the smoke bootstrap. Do not ship that generated
        # parent in an installer payload. This is deliberately non-recursive:
        # pre-existing roots and every remaining entry are retained unchanged.
        if (-not $SmokeSettingsRootExisted -and $null -ne $Process -and $Process.HasExited -and
            (Test-Path -LiteralPath $SmokeSettingsRoot)) {
          $CurrentBin = Get-Item -LiteralPath $PackageBin -Force
          $SettingsItem = Get-Item -LiteralPath $SmokeSettingsRoot -Force
          if ($CurrentBin.PSIsContainer -and $SettingsItem.PSIsContainer -and
              -not ($CurrentBin.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
              -not ($SettingsItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
              [IO.Path]::GetDirectoryName($SettingsItem.FullName).Equals(
                  $PackageBin.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar),
                  [StringComparison]::OrdinalIgnoreCase) -and
              [IO.Directory]::GetFileSystemEntries($SmokeSettingsRoot).Length -eq 0) {
            [IO.Directory]::Delete($SmokeSettingsRoot, $false)
          }
        }
      } catch {
        # Preserve the process's real exit diagnostic and any uncertain data.
        # The package/installer boundary still rejects generated leftovers.
        Write-Warning 'Smoke-settings cleanup was unavailable; existing data was preserved.'
      } finally {
        if ($null -ne $Process) { $Process.Dispose() }
      }
    }
  }
}

function Stage-CondaDependencyNotices {
  param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$Executable
  )
  # This exact-owner collector applies only to the local Conda distribution.
  # Official Qt SDK packaging has different provenance and is not silently
  # certified or blocked by a fabricated Conda metadata fallback.
  if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'conda-meta') -PathType Container)) { return }
  $NoticeFiles = @('COPYING', 'LICENSE', 'native/LICENSE.md') | ForEach-Object {
    $Source = Join-Path $Root $_
    [ordered]@{
      source = $Source
      name = $_
      sha256 = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  $KnownOwners = [ordered]@{
    schema = 'gsw.runtime_license_known_owners/1'
    owners = @(@('Gaussian Scene Workbench.exe', 'Gaussian Scene Workbench Launcher.exe') | ForEach-Object { [ordered]@{
      binary = $_
      sha256 = (Get-FileHash -LiteralPath (Join-Path (Split-Path -Parent $Executable) $_) -Algorithm SHA256).Hash.ToLowerInvariant()
      owner = 'Gaussian Scene Workbench'
      version = [string]$BuildManifest.version
      license_expression = 'GPL-3.0-or-later'
      source_urls = @('https://github.com/fubukisama/Gaussian-Scene-Workbench')
      license_files = @($NoticeFiles)
    }})
  }
  $OwnerPath = Join-Path $BuildDirectory 'runtime-license-known-owners.json'
  [IO.File]::WriteAllText($OwnerPath, ($KnownOwners | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
  $Output = Join-Path $PackageRoot 'licenses/runtime-dependencies'
  $LicenseJson = & $CheckPython -I -B (Join-Path $Root 'scripts/stage_native_dependency_licenses.py') `
    --qt-root $QtRoot --package-bin (Split-Path -Parent $Executable) --output-root $Output --known-owners $OwnerPath
  if ($LASTEXITCODE -ne 0) { throw ('Native dependency notice staging failed: ' + ($LicenseJson -join "`n")) }
  $Report = ($LicenseJson -join "`n") | ConvertFrom-Json
  Write-Host "Runtime notices: $($Report.binary_count) exact PE bindings; $($Report.package_count) original notice packages."
}

function Invoke-WindowsArtifactSigning {
  param(
    [Parameter(Mandatory = $true)][string[]]$Artifacts,
    [Parameter(Mandatory = $true)][string]$Purpose
  )

  if (-not $SigningEnabled) { return }

  $SigningScript = Join-Path $Root "scripts\sign_windows_artifacts.ps1"
  if (-not (Test-Path -LiteralPath $SigningScript -PathType Leaf)) {
    throw "Windows signing script was not found: $SigningScript"
  }

  Write-Host "Signing $Purpose with the configured enterprise certificate."
  $SigningParameters = @{
    Path = $Artifacts
    CertificateThumbprint = $SigningCertificateThumbprint
    CertificateStoreLocation = $SigningCertificateStoreLocation
    Confirm = $false
  }
  if (-not [string]::IsNullOrWhiteSpace($SigningTimestampUrl)) {
    $SigningParameters.TimestampUrl = $SigningTimestampUrl
  }
  if (-not [string]::IsNullOrWhiteSpace($SignToolPath)) {
    $SigningParameters.SignToolPath = $SignToolPath
  }

  & $SigningScript @SigningParameters
}

Import-VisualStudioEnvironment -ScriptPath $VsDevCmd

if (-not [string]::IsNullOrWhiteSpace($CMakeRoot)) {
  $ExplicitCMakeBin = if (Test-Path -LiteralPath (Join-Path $CMakeRoot "bin\cmake.exe") -PathType Leaf) {
    Join-Path $CMakeRoot "bin"
  } elseif (Test-Path -LiteralPath (Join-Path $CMakeRoot "cmake.exe") -PathType Leaf) {
    $CMakeRoot
  } else {
    throw "CMakeRoot does not contain bin\cmake.exe or cmake.exe: $CMakeRoot"
  }
  $CMakeBinCandidates = @($ExplicitCMakeBin)
} else {
  $CMakeBinCandidates = @(
    (Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "CMake\bin"),
    (Join-Path $QtRoot "Library\bin")
  )
}
$CMakeBin = $CMakeBinCandidates | Where-Object {
  (Test-Path -LiteralPath (Join-Path $_ "cmake.exe") -PathType Leaf) -and
  (Test-Path -LiteralPath (Join-Path $_ "ctest.exe") -PathType Leaf)
} | Select-Object -First 1
if (-not $CMakeBin) {
  throw "A CMake tool directory containing both cmake.exe and ctest.exe was not found. Set GSW_NATIVE_CMAKE_ROOT or pass -CMakeRoot."
}
$CMake = Join-Path $CMakeBin "cmake.exe"
$CTest = Join-Path $CMakeBin "ctest.exe"
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

$Executable = Join-Path $BuildDirectory "Gaussian Scene Workbench.exe"
$BuildExecutables = @(
  $Executable,
  (Join-Path $BuildDirectory "gsw_native_tests.exe"),
  (Join-Path $BuildDirectory "gsw_dataset_import_plan_tests.exe"),
  (Join-Path $BuildDirectory "gsw_process_output_fixture.exe"),
  (Join-Path $BuildDirectory "gsw_process_supervisor_tests.exe"),
  (Join-Path $BuildDirectory "gsw_training_telemetry_tests.exe"),
  (Join-Path $BuildDirectory "gsw_workspace_scene_format_tests.exe"),
  (Join-Path $BuildDirectory "gsw_backend_locator_tests.exe")
)
$MissingBuildExecutables = $BuildExecutables | Where-Object {
  -not (Test-Path -LiteralPath $_ -PathType Leaf)
}
if ($MissingBuildExecutables) {
  throw "Native build did not produce the expected executables: $($MissingBuildExecutables -join ', ')"
}
Invoke-WindowsArtifactSigning `
  -Artifacts $BuildExecutables `
  -Purpose "native application and test executables"

& $CTest --test-dir $BuildDirectory --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Native tests failed with exit code $LASTEXITCODE." }

$CheckPython = Find-PythonForNativeChecks
& $CheckPython -I -B (Join-Path $Root 'scripts/verify_native_backend.py') --root $Root
if ($LASTEXITCODE -ne 0) { throw "Native source boundary verification failed." }
$PreviousBytecodeSetting = $env:PYTHONDONTWRITEBYTECODE
$env:PYTHONDONTWRITEBYTECODE = "1"
Push-Location $Root
try {
  & $CheckPython -B -m unittest native.worker.test_gsw_worker native.worker.test_mesh_generation native.worker.test_mesh_material_preview native.worker.test_import_preflight native.worker.test_gpu_preview_publisher native.worker.test_training_preview native.worker.test_training_checkpoint native.worker.test_two_dgs_adapter native.worker.test_three_dgs_preview_adapter native.worker.test_training_density_control native.worker.test_training_summary native.worker.test_reconstruction_quality native.worker.test_training_reconstruction_summary native.worker.test_training_preflight native.backend.tests.test_training_telemetry native.backend.tests.test_training_backends native.backend.tests.test_path_security native.backend.tests.test_command_security native.backend.tests.test_texture_collection native.backend.tests.test_scene_workspace
  if ($LASTEXITCODE -ne 0) {
    throw "Native worker tests failed with exit code $LASTEXITCODE."
  }
} finally {
  Pop-Location
  $env:PYTHONDONTWRITEBYTECODE = $PreviousBytecodeSetting
}

Write-Host "Native build completed:"
Write-Host $Executable

if ($Package) {
  if (-not (Test-Path -LiteralPath $WinDeployQt)) {
    throw "windeployqt was not found: $WinDeployQt"
  }
  if ($PackageDirectoryName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    throw "PackageDirectoryName must be a single safe directory name."
  }
  $PackageRoot = Join-Path (Join-Path $NativeRoot "dist") $PackageDirectoryName
  if (Test-Path -LiteralPath $PackageRoot) {
    Remove-DirectoryWithRetry -Path $PackageRoot -AllowedParent (Join-Path $NativeRoot "dist")
  }
  & $CMake --install $BuildDirectory --prefix $PackageRoot
  if ($LASTEXITCODE -ne 0) { throw "Native install failed with exit code $LASTEXITCODE." }
  $PackagedExecutable = Join-Path $PackageRoot "bin\Gaussian Scene Workbench.exe"
  $DeployConfigurationFlag = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
  # QtWidgets uses native OpenGL. Do not copy this PC's optional system/RHI
  # shader compilers into a portable package; they are not linked dependencies.
  & $WinDeployQt $DeployConfigurationFlag --no-translations --compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler $PackagedExecutable
  if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
  if ($PackageSmokePlatform -eq "offscreen") {
    # windeployqt selects qwindows for a Windows GUI. The explicitly requested
    # background startup test must also be self-contained, not borrow a plugin
    # from the development environment or fall back to a visible window.
    $OffscreenPlugin = @(
      (Join-Path $QtRoot "Library\lib\qt6\plugins\platforms\qoffscreen.dll"),
      (Join-Path $QtRoot "Library\plugins\platforms\qoffscreen.dll")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $OffscreenPlugin) { throw "Requested offscreen package smoke requires the matching qoffscreen.dll." }
    $PlatformDirectory = Join-Path (Split-Path -Parent $PackagedExecutable) "platforms"
    New-Item -ItemType Directory -Path $PlatformDirectory -Force | Out-Null
    Copy-Item -LiteralPath $OffscreenPlugin -Destination (Join-Path $PlatformDirectory "qoffscreen.dll") -Force
  }
  $RuntimeSearchRoots = @(
    (Join-Path $QtRoot "Library\bin"),
    $(if ($env:VCToolsRedistDir) {
      Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT"
    })
  ) | Where-Object { $_ }
  Copy-AppLocalDependencies -PackageBin (Split-Path -Parent $PackagedExecutable) -SearchRoots $RuntimeSearchRoots
  Invoke-WindowsArtifactSigning `
    -Artifacts @($PackagedExecutable) `
    -Purpose "packaged native application"
  Invoke-NativeLaunchSmokeTest -Executable $PackagedExecutable
  & $CheckPython -I -B (Join-Path $Root 'scripts/stage_native_readme.py') --source-root $Root --package-root $PackageRoot
  if ($LASTEXITCODE -ne 0) { throw "Native package README/document link staging failed." }
  Copy-Item -LiteralPath (Join-Path $NativeRoot "build_manifest.json") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\NATIVE_MIGRATION.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\NATIVE_PARITY.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\COLMAP_SETUP.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\WINDOWS_APPLICATION_CONTROL.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "COPYING") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $NativeRoot "LICENSE.md") -Destination (Join-Path $PackageRoot "native/LICENSE.md") -Force
  & (Join-Path $Root 'scripts/write_native_source_manifest.ps1') -PackageRoot $PackageRoot
  Copy-Item -LiteralPath (Join-Path $Root "THIRD_PARTY_LICENSES.md") -Destination $PackageRoot -Force
  Stage-CondaDependencyNotices -PackageRoot $PackageRoot -Executable $PackagedExecutable
  Copy-Item -LiteralPath (Join-Path $Root "docs\LOCALIZATION.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\RESOURCE_BUDGETS.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\CONTINUOUS_TRAINING_PREVIEW.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\STUDIO_COMPARISON.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\TRAINING_RESUME.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\GENERATION_PIPELINES.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\MESH_GENERATION.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\RECONSTRUCTION_QUALITY.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\ORIGINAL_RESOLUTION_TRAINING.md") -Destination $PackageRoot -Force
  Copy-Item -LiteralPath (Join-Path $Root "docs\QUALITY_PLAN.md") -Destination $PackageRoot -Force
  $QualityDocumentationRoot = Join-Path $PackageRoot "quality_upgrade"
  New-Item -ItemType Directory -Path $QualityDocumentationRoot -Force | Out-Null
  foreach ($QualityDocument in @("CAPABILITY_MATRIX.json", "ACCEPTANCE_REPORT.json", "PAPER_VERSIONS.json")) {
    Copy-Item -LiteralPath (Join-Path (Join-Path $Root "docs\quality_upgrade") $QualityDocument) `
      -Destination $QualityDocumentationRoot -Force
  }
  Copy-Item -LiteralPath (Join-Path $Root "docs\WINDOWS_NATIVE_RUNTIME.md") -Destination $PackageRoot -Force
  & (Join-Path $Root "scripts\stage_native_backend.ps1") `
    -SourceRoot $Root `
    -DestinationRoot $PackageRoot `
    -PythonExecutable $CheckPython
  Invoke-PythonSyntaxGate -Python $CheckPython -SourceRoot $PackageRoot
  $RequiredBackendFiles = @(
    "native\worker\gsw_worker.py",
    "native\worker\import_preflight.py",
    "native\worker\training_preflight.py",
    "native\worker\gpu_preview_publisher.py",
    "native\worker\training_preview.py",
    "native\worker\training_preview_policy.py",
    "native\worker\training_reconstruction_summary.py",
    "native\worker\training_checkpoint.py",
    "native\worker\training_density_control.py",
    "native\worker\training_summary.py",
    "native\worker\two_dgs_train.py",
    "native\worker\generation_capabilities.py",
    "native\worker\mesh_generation.py",
    "native\worker\mesh_material_preview.py",
    "native\worker\licenses\2dgs-LICENSE.md",
    "native\backend\__init__.py",
    "native\backend\compute.py",
    "native\backend\reconstruction_quality.py",
    "native\backend\video_extract.py",
    "scripts\check_3dgs_env.ps1",
    "scripts\requirements-native-3dgs.txt",
    "scripts\requirements-native-2dgs.txt",
    "scripts\requirements-native-sugar.txt",
    "scripts\requirements-native-gs2mesh.txt",
    "scripts\patches\pytorch3d-0.7.4-windows-cub.patch",
    "scripts\patches\sugar-windows-stage-paths.patch",
    "scripts\sign_windows_artifacts.ps1",
    "gaussian-splatting\train.py",
    "training_kit\apply_local_fixes.bat",
    "WINDOWS_APPLICATION_CONTROL.md",
    "backend_manifest.json"
  ) | ForEach-Object { Join-Path $PackageRoot $_ }
  $MissingBackendFiles = $RequiredBackendFiles | Where-Object {
    -not (Test-Path -LiteralPath $_)
  }
  if ($MissingBackendFiles) {
    throw "Native package is missing backend files: $($MissingBackendFiles -join ', ')"
  }
  # Exercise the installed entrypoint in isolation: repository PYTHONPATH must
  # not hide missing imports in the staged compute backend or its sibling modules.
  & $CheckPython -I -B (Join-Path $PackageRoot "native\worker\import_preflight.py") `
    --backend-root $PackageRoot
  if ($LASTEXITCODE -ne 0) {
    throw "Packaged media-import preflight failed with exit code $LASTEXITCODE."
  }
  Write-Host "Native package directory:"
  Write-Host $PackageRoot
}
