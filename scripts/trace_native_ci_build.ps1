#Requires -Version 7.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$BuildDirectory,
  [Parameter(Mandatory = $true)][string]$TraceDirectory,
  [string]$CMakeExecutable = 'cmake',
  [string]$Configuration = 'Release',
  [ValidateRange(1, 300)][int]$SampleIntervalSeconds = 30
)

# Observation only: no build flags, scheduling, deadlines, or process termination
# are changed here. The caller/CI remains responsible for its original timeout.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$TraceDirectory = [IO.Path]::GetFullPath($TraceDirectory)
$cmake = (Get-Command $CMakeExecutable -ErrorAction Stop).Source
[IO.Directory]::CreateDirectory($TraceDirectory) | Out-Null
$tracePath = Join-Path $TraceDirectory 'build-trace.jsonl'
$stream = [IO.File]::Open($tracePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true
$clock = [Diagnostics.Stopwatch]::StartNew()
$knownProcesses = [Collections.Generic.Dictionary[int, long]]::new()
$objectState = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
$artifactBaselineEstablished = $false
$process = $null
$buildExit = $null
$completed = $false

function Read-OwnedProcesses {
  $observed = [Collections.Generic.List[object]]::new()
  try {
    # Deliberately never request CommandLine, ExecutablePath or environment data.
    $snapshot = @(Get-CimInstance -ClassName Win32_Process -Property Name, ProcessId, ParentProcessId, CreationDate, KernelModeTime, UserModeTime, WorkingSetSize -ErrorAction Stop)
    $live = [Collections.Generic.Dictionary[int, object]]::new()
    foreach ($item in $snapshot) { $live[[int]$item.ProcessId] = $item }
    $owned = [Collections.Generic.Dictionary[int, long]]::new()
    foreach ($entry in $script:knownProcesses.GetEnumerator()) {
      if ($live.ContainsKey($entry.Key) -and $null -ne $live[$entry.Key].CreationDate) {
        $birth = $live[$entry.Key].CreationDate.ToUniversalTime().Ticks
        if ([Math]::Abs($birth - $entry.Value) -lt 10000) { $owned[$entry.Key] = $birth }
      }
    }
    if ($live.ContainsKey($script:process.Id) -and $null -ne $live[$script:process.Id].CreationDate) {
      $birth = $live[$script:process.Id].CreationDate.ToUniversalTime().Ticks
      if ([Math]::Abs($birth - $script:rootBirth) -lt 10000) { $owned[$script:process.Id] = $birth }
    }
    do {
      $added = $false
      foreach ($item in $snapshot) {
        $id = [int]$item.ProcessId
        $parent = [int]$item.ParentProcessId
        if ($owned.ContainsKey($id) -or -not $owned.ContainsKey($parent) -or $null -eq $item.CreationDate) { continue }
        $birth = $item.CreationDate.ToUniversalTime().Ticks
        if ($birth -ge $owned[$parent]) { $owned[$id] = $birth; $added = $true }
      }
    } while ($added)
    foreach ($entry in $owned.GetEnumerator()) {
      $script:knownProcesses[$entry.Key] = $entry.Value
      $item = $live[$entry.Key]
      $name = ([string]$item.Name).ToLowerInvariant() -replace '\.exe$', ''
      if ($name -notin @('cmake', 'msbuild', 'cl', 'clang-cl', 'clang', 'clang++', 'gcc', 'g++', 'link', 'lld-link', 'tracker', 'extractor', 'ninja', 'mspdbsrv')) { continue }
      $observed.Add([ordered]@{
        name = $name
        pid = [int]$item.ProcessId
        parent_pid = [int]$item.ParentProcessId
        creation_utc_ticks = $entry.Value
        cpu_seconds = [Math]::Round(([double]$item.KernelModeTime + [double]$item.UserModeTime) / 10000000, 3)
        rss_bytes = [uint64]$item.WorkingSetSize
      })
    }
    return @{ available = $true; items = @($observed.ToArray() | Sort-Object { $_.pid }) }
  } catch {
    # Diagnostic failure must not replace the real build result or leak raw errors.
    return @{ available = $false; items = @() }
  }
}

function Read-ObjectProgress {
  $count = 0L; $bytes = 0L; $updated = 0L; $updatedBytes = 0L
  $sources = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  try {
    $options = [IO.EnumerationOptions]::new()
    $options.RecurseSubdirectories = $true
    $options.IgnoreInaccessible = $true
    $options.AttributesToSkip = [IO.FileAttributes]::ReparsePoint
    if ([IO.Directory]::Exists($script:BuildDirectory)) {
      foreach ($path in [IO.Directory]::EnumerateFiles($script:BuildDirectory, '*', $options)) {
        if ([IO.Path]::GetExtension($path) -notin @('.obj', '.o')) { continue }
        try {
          $file = [IO.FileInfo]::new($path)
          $length = $file.Length
          $stamp = "$($file.LastWriteTimeUtc.Ticks):$length"
          $count++; $bytes += $length
          if ($script:artifactBaselineEstablished -and (-not $script:objectState.ContainsKey($path) -or $script:objectState[$path] -ne $stamp)) {
            $updated++; $updatedBytes += $length
            # Only a source basename may leave the process; never full paths.
            $source = $file.Name -replace '\.(obj|o)$', ''
            if ($source -cmatch '^[A-Za-z0-9_+.-]{1,160}\.(cpp|cxx|cc|c)$') { [void]$sources.Add($source) }
          }
          $script:objectState[$path] = $stamp
        } catch { continue }
      }
    }
    $script:artifactBaselineEstablished = $true
    return [ordered]@{ available = $true; total_objects = $count; total_object_bytes = $bytes; updated_objects = $updated; updated_object_bytes = $updatedBytes; source_basenames = @($sources | Sort-Object | Select-Object -First 12) }
  } catch {
    return [ordered]@{ available = $false; total_objects = $count; total_object_bytes = $bytes; updated_objects = $updated; updated_object_bytes = $updatedBytes; source_basenames = @() }
  }
}

function Write-TraceEvent([string]$Event, [Nullable[int]]$ExitCode = $null) {
  $owned = Read-OwnedProcesses
  $objects = Read-ObjectProgress
  $record = [ordered]@{
    schema = 'gsw.ci_build_trace/1'
    event = $Event
    utc = [DateTime]::UtcNow.ToString('o')
    elapsed_seconds = [Math]::Round($script:clock.Elapsed.TotalSeconds, 3)
    root_pid = $script:process.Id
    process_observation_available = [bool]$owned.available
    processes = @($owned.items)
    objects = $objects
  }
  if ($Event -eq 'finish') { $record.exit_code = $ExitCode }
  try { $script:writer.WriteLine(($record | ConvertTo-Json -Depth 7 -Compress)) }
  catch { Write-Warning 'CI build trace write unavailable; the actual build result remains authoritative.' }
  Write-Host ("[native-build-trace] {0}: elapsed={1}s processes={2} objects={3} updated={4}" -f $Event, $record.elapsed_seconds, $record.processes.Count, $objects.total_objects, $objects.updated_objects)
}

function Forward-BuildOutput {
  foreach ($channel in @('stdout', 'stderr')) {
    $task = if ($channel -eq 'stdout') { $script:stdoutTask } else { $script:stderrTask }
    while ($null -ne $task -and $task.IsCompleted) {
      $line = $task.GetAwaiter().GetResult()
      if ($null -eq $line) { $task = $null; break }
      if ($channel -eq 'stdout') {
        [Console]::Out.WriteLine($line)
        $task = $script:process.StandardOutput.ReadLineAsync()
      } else {
        [Console]::Error.WriteLine($line)
        $task = $script:process.StandardError.ReadLineAsync()
      }
    }
    if ($channel -eq 'stdout') { $script:stdoutTask = $task } else { $script:stderrTask = $task }
  }
}

try {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $cmake
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  # Forward build output without copying it into the diagnostic artifact.
  # The environment is inherited without being inspected or recorded.
  foreach ($argument in @('--build', $BuildDirectory, '--config', $Configuration, '--parallel')) { $start.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  if (-not $process.Start()) { throw 'CMake did not start.' }
  $rootBirth = $process.StartTime.ToUniversalTime().Ticks
  $stdoutTask = $process.StandardOutput.ReadLineAsync()
  $stderrTask = $process.StandardError.ReadLineAsync()
  Write-TraceEvent 'start'
  $nextSample = $clock.Elapsed.TotalSeconds + $SampleIntervalSeconds
  while (-not $process.WaitForExit(200)) {
    Forward-BuildOutput
    if ($clock.Elapsed.TotalSeconds -ge $nextSample) {
      Write-TraceEvent 'sample'
      $nextSample = $clock.Elapsed.TotalSeconds + $SampleIntervalSeconds
    }
  }
  $process.WaitForExit()
  # A detached descendant may retain a pipe after cmake exits. Observation must
  # not wait indefinitely after the authoritative build process has completed.
  $drain = [Diagnostics.Stopwatch]::StartNew()
  while (($null -ne $stdoutTask -or $null -ne $stderrTask) -and $drain.ElapsedMilliseconds -lt 2000) {
    Forward-BuildOutput
    if ($null -ne $stdoutTask -or $null -ne $stderrTask) { [Threading.Thread]::Sleep(10) }
  }
  $buildExit = $process.ExitCode
  Write-TraceEvent 'finish' $buildExit
  $completed = $true
} finally {
  if (-not $completed -and $null -ne $process) {
    # Cancellation/timeout is never turned into success. Do not terminate any
    # process: the caller/runner retains ownership and cancellation policy.
    try { Write-TraceEvent 'interrupted' } catch { }
  }
  try { $writer.Dispose() } catch { Write-Warning 'CI build trace close unavailable; the actual build result remains authoritative.' }
  if ($null -ne $process) { $process.Dispose() }
}
exit $buildExit
