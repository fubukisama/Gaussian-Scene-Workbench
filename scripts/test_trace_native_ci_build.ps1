[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$TestRoot,
  [string]$CMakeExecutable = 'cmake',
  [string]$NinjaExecutable = 'ninja'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$TestRoot = [IO.Path]::GetFullPath($TestRoot)
if (Test-Path -LiteralPath $TestRoot) { throw 'TestRoot must be a new, owned directory.' }
[IO.Directory]::CreateDirectory($TestRoot) | Out-Null
$wrapper = Join-Path $PSScriptRoot 'trace_native_ci_build.ps1'
$pwsh = (Get-Process -Id $PID).Path
$cmake = (Get-Command $CMakeExecutable -ErrorAction Stop).Source
$ninjaCommand = Get-Command $NinjaExecutable -ErrorAction SilentlyContinue
if ($null -ne $ninjaCommand) {
  $NinjaExecutable = $ninjaCommand.Source
} elseif ($NinjaExecutable -eq 'ninja' -and (Test-Path -LiteralPath 'D:/Apps/GSW/runtime/envs/gsw_native/Library/bin/ninja.exe')) {
  $NinjaExecutable = 'D:/Apps/GSW/runtime/envs/gsw_native/Library/bin/ninja.exe'
} else { throw 'Provide an existing Ninja executable with -NinjaExecutable.' }
$results = [Collections.Generic.List[object]]::new()

function Assert-Condition([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Invoke-Recorded([string]$Executable, [string[]]$Arguments, [string]$Name) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $Executable
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  try {
    if (-not $process.Start()) { throw 'Fixture process did not start.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(50000)) {
      throw 'Owned fixture exceeded 50 seconds; no process was force-terminated.'
    }
    [IO.File]::WriteAllText((Join-Path $TestRoot "$Name.stdout.log"), $stdout.GetAwaiter().GetResult())
    [IO.File]::WriteAllText((Join-Path $TestRoot "$Name.stderr.log"), $stderr.GetAwaiter().GetResult())
    return $process.ExitCode
  } finally { $process.Dispose() }
}

$source = Join-Path $TestRoot 'source with spaces_日本語'
[IO.Directory]::CreateDirectory($source) | Out-Null
$cmakeSource = @'
cmake_minimum_required(VERSION 3.24)
project(NativeBuildTraceFixture NONE)
add_custom_target(trace_fixture ALL
  COMMAND "${CMAKE_COMMAND}" -E echo "fixture public build output"
  COMMAND "${CMAKE_COMMAND}" -E sleep 2.2
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/fixture.cpp.obj"
  COMMAND "${CMAKE_COMMAND}" -E sleep 1.2
  COMMAND "${CMAKE_COMMAND}" -E ${FIXTURE_RESULT}
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/fixture.finished"
  VERBATIM)
'@
[IO.File]::WriteAllText((Join-Path $source 'CMakeLists.txt'), $cmakeSource)
$build = Join-Path $TestRoot 'build success_日本語'
$configure = Invoke-Recorded $cmake @('-S', $source, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$NinjaExecutable", '-DFIXTURE_RESULT=true') 'configure-success'
Assert-Condition ($configure -eq 0) 'Real fixture configuration must succeed.'
$direct = Invoke-Recorded $cmake @('--build', $build, '--config', 'Release', '--parallel') 'direct-success'
Assert-Condition ($direct -eq 0) 'Real direct fixture build must succeed.'
$trace = Join-Path $TestRoot 'success-trace'
$env:GSW_TRACE_PRIVATE_SENTINEL = 'GSW_PRIVATE_SENTINEL_20261022_NOT_FOR_ARTIFACT'
$wrapped = Invoke-Recorded $pwsh @('-NoProfile', '-File', $wrapper, '-BuildDirectory', $build, '-TraceDirectory', $trace, '-CMakeExecutable', $cmake, '-SampleIntervalSeconds', '1') 'wrapped-success'
Assert-Condition ($wrapped -eq $direct) 'Wrapper must preserve successful cmake exit code.'
$events = @(Get-Content -LiteralPath (Join-Path $trace 'build-trace.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
Assert-Condition ($events[0].event -eq 'start') 'Trace starts with a start event.'
Assert-Condition ($events[-1].event -eq 'finish' -and $events[-1].exit_code -eq 0) 'Trace records the actual successful exit.'
Assert-Condition (@($events | Where-Object event -eq 'sample').Count -ge 2) 'Long-running fixture must produce repeated samples.'
Assert-Condition ((Get-Content -LiteralPath (Join-Path $TestRoot 'wrapped-success.stdout.log') -Raw).Contains('fixture public build output')) 'Actual build output must remain visible.'
$results.Add([pscustomobject]@{ name = 'real_success_unicode_repeated_samples'; pass = $true; direct_exit = $direct; wrapper_exit = $wrapped })

Assert-Condition (@($events | Where-Object { $_.objects.updated_objects -gt 0 -and 'fixture.cpp' -in $_.objects.source_basenames }).Count -ge 1) 'Trace must observe a real source-object update.'
$observations = @($events | ForEach-Object { $_.processes } | Where-Object { $null -ne $_ })
Assert-Condition (@($observations | Where-Object { $_.name -eq 'cmake' -and $_.pid -gt 0 -and $_.rss_bytes -gt 0 -and $_.cpu_seconds -ge 0 }).Count -ge 1) 'Owned real cmake PID, CPU and RSS must be observable.'
$raw = Get-Content -LiteralPath (Join-Path $trace 'build-trace.jsonl') -Raw
Assert-Condition (-not $raw.Contains($env:GSW_TRACE_PRIVATE_SENTINEL) -and -not $raw.Contains($TestRoot) -and -not $raw.Contains($cmake)) 'Diagnostic JSONL must not collect environment secrets or full paths.'
foreach ($event in $events) {
  foreach ($property in $event.PSObject.Properties.Name) { Assert-Condition ($property -in @('schema', 'event', 'utc', 'elapsed_seconds', 'root_pid', 'process_observation_available', 'processes', 'objects', 'exit_code')) 'Event must contain only allowed fields.' }
  foreach ($item in $event.processes) {
    Assert-Condition ($item.name -in @('cmake', 'msbuild', 'cl', 'clang-cl', 'clang', 'clang++', 'gcc', 'g++', 'link', 'lld-link', 'tracker', 'extractor', 'ninja', 'mspdbsrv')) 'Only allowlisted process names may be recorded.'
    foreach ($property in $item.PSObject.Properties.Name) { Assert-Condition ($property -in @('name', 'pid', 'parent_pid', 'creation_utc_ticks', 'cpu_seconds', 'rss_bytes')) 'Process must contain only allowed fields.' }
  }
  foreach ($property in $event.objects.PSObject.Properties.Name) { Assert-Condition ($property -in @('available', 'total_objects', 'total_object_bytes', 'updated_objects', 'updated_object_bytes', 'source_basenames')) 'Object progress must contain only allowed fields.' }
  foreach ($name in $event.objects.source_basenames) { Assert-Condition ($name -cmatch '^[A-Za-z0-9_+.-]{1,160}\.(cpp|cxx|cc|c)$') 'Only safe source basenames may be recorded.' }
}
$results.Add([pscustomobject]@{ name = 'owned_process_object_progress_and_private_allowlist'; pass = $true })

$failedBuild = Join-Path $TestRoot 'build failure_日本語'
$configure = Invoke-Recorded $cmake @('-S', $source, '-B', $failedBuild, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$NinjaExecutable", '-DFIXTURE_RESULT=false') 'configure-failure'
Assert-Condition ($configure -eq 0) 'Failing build fixture must still configure.'
$directFailure = Invoke-Recorded $cmake @('--build', $failedBuild, '--config', 'Release', '--parallel') 'direct-failure'
Assert-Condition ($directFailure -ne 0) 'Real failing target must produce a failure exit code.'
$failureTrace = Join-Path $TestRoot 'failure-trace'
$wrappedFailure = Invoke-Recorded $pwsh @('-NoProfile', '-File', $wrapper, '-BuildDirectory', $failedBuild, '-TraceDirectory', $failureTrace, '-CMakeExecutable', $cmake, '-SampleIntervalSeconds', '1') 'wrapped-failure'
Assert-Condition ($wrappedFailure -eq $directFailure) 'Wrapper must preserve the actual failing cmake exit code.'
$failureEvents = @(Get-Content -LiteralPath (Join-Path $failureTrace 'build-trace.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
Assert-Condition ($failureEvents[-1].event -eq 'finish' -and $failureEvents[-1].exit_code -eq $directFailure) 'Failed build must remain failed in trace.'
$results.Add([pscustomobject]@{ name = 'real_failure_exit_code_preserved'; pass = $true; direct_exit = $directFailure; wrapper_exit = $wrappedFailure })

$missingBuild = Join-Path $TestRoot 'not configured_日本語'
$directMissing = Invoke-Recorded $cmake @('--build', $missingBuild, '--config', 'Release', '--parallel') 'direct-missing'
$wrappedMissing = Invoke-Recorded $pwsh @('-NoProfile', '-File', $wrapper, '-BuildDirectory', $missingBuild, '-TraceDirectory', (Join-Path $TestRoot 'missing-trace'), '-CMakeExecutable', $cmake) 'wrapped-missing'
Assert-Condition ($directMissing -ne 0 -and $wrappedMissing -eq $directMissing) 'Invalid cmake build directory must preserve its real failure.'
Assert-Condition ((Get-Content -LiteralPath (Join-Path $TestRoot 'wrapped-missing.stderr.log') -Raw).Contains('Error')) 'Actual cmake stderr must remain visible.'
$results.Add([pscustomobject]@{ name = 'real_cmake_error_stderr_preserved'; pass = $true; direct_exit = $directMissing; wrapper_exit = $wrappedMissing })

$cancelBuild = Join-Path $TestRoot 'build cancelled observer_日本語'
$configure = Invoke-Recorded $cmake @('-S', $source, '-B', $cancelBuild, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$NinjaExecutable", '-DFIXTURE_RESULT=true') 'configure-cancel'
Assert-Condition ($configure -eq 0) 'Cancellation fixture must configure.'
$cancelTrace = Join-Path $TestRoot 'cancel-trace'
$cancelStart = [Diagnostics.ProcessStartInfo]::new()
$cancelStart.FileName = $pwsh
$cancelStart.UseShellExecute = $false
$cancelStart.CreateNoWindow = $true
$cancelStart.RedirectStandardOutput = $true
$cancelStart.RedirectStandardError = $true
foreach ($argument in @('-NoProfile', '-File', $wrapper, '-BuildDirectory', $cancelBuild, '-TraceDirectory', $cancelTrace, '-CMakeExecutable', $cmake, '-SampleIntervalSeconds', '1')) { $cancelStart.ArgumentList.Add($argument) }
$cancelProcess = [Diagnostics.Process]::new()
$cancelProcess.StartInfo = $cancelStart
try {
  Assert-Condition ($cancelProcess.Start()) 'Owned cancellation wrapper must start.'
  $outTask = $cancelProcess.StandardOutput.ReadToEndAsync()
  $errTask = $cancelProcess.StandardError.ReadToEndAsync()
  $wait = [Diagnostics.Stopwatch]::StartNew()
  $cancelPath = Join-Path $cancelTrace 'build-trace.jsonl'
  $sampleReady = $false
  while (-not $cancelProcess.HasExited -and $wait.ElapsedMilliseconds -lt 6000) {
    if (Test-Path -LiteralPath $cancelPath) {
      $sampleReady = @(Select-String -LiteralPath $cancelPath -Pattern '"event":"sample"' -SimpleMatch).Count -gt 0
    }
    if ($sampleReady) { break }
    [Threading.Thread]::Sleep(50)
  }
  Assert-Condition ($sampleReady -and -not $cancelProcess.HasExited) 'Real observer must be running before owned external cancellation.'
  # Cancel only the exact Process instance created above, never a name or tree.
  $cancelProcess.Kill()
  Assert-Condition ($cancelProcess.WaitForExit(5000)) 'Cancelled wrapper must exit.'
  Assert-Condition ($cancelProcess.ExitCode -ne 0) 'External cancellation must not become success.'
  [IO.File]::WriteAllText((Join-Path $TestRoot 'cancel.stdout.log'), $outTask.GetAwaiter().GetResult())
  [IO.File]::WriteAllText((Join-Path $TestRoot 'cancel.stderr.log'), $errTask.GetAwaiter().GetResult())
  $naturalCompletion = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path -LiteralPath (Join-Path $cancelBuild 'fixture.finished')) -and $naturalCompletion.ElapsedMilliseconds -lt 10000) { [Threading.Thread]::Sleep(50) }
  Assert-Condition (Test-Path -LiteralPath (Join-Path $cancelBuild 'fixture.finished')) 'Observer cancellation must not terminate the owned build child; it completes naturally.'
  # Abrupt process termination may leave the final JSONL line incomplete.
  Assert-Condition (@(Select-String -LiteralPath $cancelPath -Pattern '"event":"finish"' -SimpleMatch).Count -eq 0) 'Abruptly cancelled observer must not fabricate build completion.'
  $results.Add([pscustomobject]@{ name = 'owned_external_cancel_nonzero_no_child_termination'; pass = $true; wrapper_exit = $cancelProcess.ExitCode })
} finally { $cancelProcess.Dispose() }

$before = (Get-FileHash -LiteralPath (Join-Path $trace 'build-trace.jsonl') -Algorithm SHA256).Hash
$repeat = Invoke-Recorded $pwsh @('-NoProfile', '-File', $wrapper, '-BuildDirectory', $build, '-TraceDirectory', $trace, '-CMakeExecutable', $cmake) 'repeat-refused'
Assert-Condition ($repeat -ne 0) 'An existing trace must never be overwritten.'
Assert-Condition ((Get-FileHash -LiteralPath (Join-Path $trace 'build-trace.jsonl') -Algorithm SHA256).Hash -eq $before) 'Refused repeat must preserve prior trace bytes.'
$results.Add([pscustomobject]@{ name = 'existing_trace_preserved'; pass = $true })
[IO.File]::WriteAllText((Join-Path $TestRoot 'RESULTS.json'), ($results | ConvertTo-Json -Depth 6))
Write-Host 'CI build trace public success/failure/privacy fixtures passed.'
