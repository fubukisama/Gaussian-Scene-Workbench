[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Inventory only. Never run this probe on the user's desktop, even with a hidden
# window. It does not change display modes, OS scale, topology, drivers or power.
if ($env:GITHUB_ACTIONS -ne 'true' -or
    $env:GITHUB_REPOSITORY -ne 'fubukisama/Gaussian-Scene-Workbench' -or
    -not [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::Windows)) {
    throw 'This read-only capability probe is restricted to the authorized Windows CI repository.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'The capability probe requires 64-bit PowerShell.'
}

$fullOutput = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetPathRoot($fullOutput) -ine 'D:\' -or
    [IO.Path]::GetExtension($fullOutput) -ine '.json' -or
    [IO.File]::Exists($fullOutput) -or [IO.Directory]::Exists($fullOutput)) {
    throw 'Use a fresh JSON output file on D:.'
}
$outputDirectory = [IO.DirectoryInfo][IO.Path]::GetDirectoryName($fullOutput)
if (-not $outputDirectory.Exists) { throw 'The owned output directory must already exist.' }
for ($directory = $outputDirectory; $null -ne $directory; $directory = $directory.Parent) {
    if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Reparse points are not allowed in the capability output path.'
    }
}

function Limit-Text([object]$Value, [int]$Length = 200) {
    if ($null -eq $Value) { return $null }
    $text = ([string]$Value) -replace '[\x00-\x1F\x7F]', ''
    return $text.Substring(0, [Math]::Min($text.Length, $Length))
}

$warnings = [Collections.Generic.List[string]]::new()
$display = [ordered]@{ status = 'unavailable'; monitors = @(); displayMonitorCount = $null }
try {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace GswCiCapabilities {
  public sealed class MonitorRecord {
    public int localIndex;
    public bool primary;
    public int[] physicalRect;
    public int[] workRect;
    public uint? ownedHiddenWindowDpi;
    public int? ownedHiddenWindowDpiAwareness;
  }
  public static class NativeDisplay {
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int l, t, r, b; }
    [StructLayout(LayoutKind.Sequential)] struct MonitorInfo {
      public int size; public Rect monitor, work; public uint flags;
    }
    delegate bool MonitorCallback(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorCallback callback, IntPtr data);
    [DllImport("user32.dll")] static extern bool GetMonitorInfoW(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetWindowDpiAwarenessContext(IntPtr window);
    [DllImport("user32.dll")] static extern int GetAwarenessFromDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowExW(
      uint extendedStyle, string className, string title, uint style, int x, int y,
      int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    public static MonitorRecord[] Read() {
      var records = new List<MonitorRecord>();
      // This changes only this probe thread's awareness, not Windows display
      // scaling. No ShowWindow/SetForegroundWindow or visible styles are used.
      IntPtr previous = SetThreadDpiAwarenessContext(new IntPtr(-4));
      if (previous == IntPtr.Zero) throw new InvalidOperationException();
      try {
        MonitorCallback callback = delegate(IntPtr monitor, IntPtr hdc, IntPtr rect, IntPtr data) {
          var info = new MonitorInfo { size = Marshal.SizeOf(typeof(MonitorInfo)) };
          if (!GetMonitorInfoW(monitor, ref info)) return true;
          var item = new MonitorRecord {
            localIndex = records.Count, primary = (info.flags & 1) != 0,
            physicalRect = new[] {info.monitor.l, info.monitor.t, info.monitor.r, info.monitor.b},
            workRect = new[] {info.work.l, info.work.t, info.work.r, info.work.b}
          };
          IntPtr window = CreateWindowExW(0x08000080, "STATIC", "", 0x80000000,
            info.monitor.l + 1, info.monitor.t + 1, 1, 1,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
          if (window != IntPtr.Zero) {
            try {
              uint dpi = GetDpiForWindow(window);
              if (dpi != 0) item.ownedHiddenWindowDpi = dpi;
              item.ownedHiddenWindowDpiAwareness = GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
            } finally { DestroyWindow(window); }
          }
          records.Add(item);
          return records.Count < 32;
        };
        if (!EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero)) {
          throw new InvalidOperationException();
        }
        GC.KeepAlive(callback);
      } finally { SetThreadDpiAwarenessContext(previous); }
      return records.ToArray();
    }
  }
}
'@
    $monitors = @([GswCiCapabilities.NativeDisplay]::Read())
    $display = [ordered]@{
        status = 'ok'
        source = 'EnumDisplayMonitors/GetMonitorInfoW; owned hidden per-monitor-aware HWND/GetDpiForWindow'
        displayMonitorCount = [GswCiCapabilities.NativeDisplay]::GetSystemMetrics(80)
        enumeratedMonitorCount = $monitors.Count
        enumerationLimit = 32
        mayIncludeMirroringPseudoMonitors = $true
        monitors = @($monitors | ForEach-Object {
            [ordered]@{
                localIndex = $_.localIndex; primary = $_.primary
                physicalRect = $_.physicalRect; workRect = $_.workRect
                ownedHiddenWindowDpi = $_.ownedHiddenWindowDpi
                ownedHiddenWindowDpiAwareness = $_.ownedHiddenWindowDpiAwareness
                actualOsScalePercent = $(if ($_.ownedHiddenWindowDpiAwareness -eq 2 -and
                    $null -ne $_.ownedHiddenWindowDpi) { [Math]::Round($_.ownedHiddenWindowDpi * 100.0 / 96.0, 3) } else { $null })
            }
        })
        applicationCrossMonitorTransition = 'not_tested'
        osDisplayScaleChanged = $false
    }
} catch { $warnings.Add('DISPLAY_OBSERVATION_UNAVAILABLE') }

$adapters = [ordered]@{ status = 'unavailable'; candidates = @(); runtimeUsableDevices = $null }
try {
    $controllers = @(Get-CimInstance -ClassName Win32_VideoController -OperationTimeoutSec 3 |
        Select-Object -First 32 -Property Name, DriverVersion)
    $adapters.status = 'ok'
    $adapters.candidates = @($controllers | ForEach-Object {
        [ordered]@{ model = (Limit-Text $_.Name); driverVersion = (Limit-Text $_.DriverVersion 64)
            runtimeCompatibility = 'not_tested'; devicePermission = 'not_tested' }
    })
} catch { $warnings.Add('VIDEO_CONTROLLER_QUERY_UNAVAILABLE') }

$nvidia = [ordered]@{ status = 'not_found'; candidates = @(); runtimeUsableDevices = $null }
$nvidiaCommand = Get-Command nvidia-smi.exe -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -ne $nvidiaCommand) {
    $process = [Diagnostics.Process]::new()
    try {
        $process.StartInfo.FileName = $nvidiaCommand.Source
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.ArgumentList.Add('--query-gpu=index,name,memory.total,driver_version')
        $process.StartInfo.ArgumentList.Add('--format=csv,noheader,nounits')
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(5000)) {
            $process.Kill()
            $nvidia.status = 'timeout'
        } else {
            $raw = $stdout.GetAwaiter().GetResult()
            [void]$stderr.GetAwaiter().GetResult()
            if ($process.ExitCode -ne 0) { $nvidia.status = 'error' }
            elseif ($raw.Length -gt 16384) { $nvidia.status = 'truncated' }
            else {
                $parsed = @($raw | ConvertFrom-Csv -Header index, model, memoryMiB, driver)
                if ($parsed.Count -gt 32) { $nvidia.status = 'truncated' }
                else {
                    $nvidia.candidates = @($parsed | ForEach-Object {
                        $index = 0; $memory = [double]0
                        if (-not [int]::TryParse($_.index.Trim(), [ref]$index)) { throw 'Invalid query index.' }
                        $memoryBytes = $null
                        if ([double]::TryParse($_.memoryMiB.Trim(), [Globalization.NumberStyles]::Float,
                            [Globalization.CultureInfo]::InvariantCulture, [ref]$memory) -and
                            [double]::IsFinite($memory) -and $memory -ge 0) {
                            $memoryBytes = [Math]::Floor($memory * 1MB)
                        }
                        [ordered]@{
                            vendor = 'nvidia'; localIndex = $index
                            model = (Limit-Text $_.model.Trim())
                            driverVersion = (Limit-Text $_.driver.Trim() 64)
                            memory = @{ model = 'device_reported_pool'; reportedTotalBytes = $memoryBytes
                                dedicatedTotalBytes = $null; availableForApplicationBytes = $null }
                            managementQuery = 'nvidia-smi selective read-only query'
                            runtimeCompatibility = 'not_tested'; devicePermission = 'not_tested'
                        }
                    })
                    $nvidia.status = 'ok'
                }
            }
        }
    } catch { $nvidia.status = 'parse_or_query_error'; $nvidia.candidates = @() }
    finally { $process.Dispose() }
}
if ($nvidia.status -ne 'ok') { $warnings.Add('NVIDIA_RUNTIME_AND_MEMORY_REMAIN_UNKNOWN') }

$memory = [ordered]@{ status = 'unavailable'; hostTotalBytes = $null; hostAvailableBytes = $null
    effectiveAllocationBytes = $null }
try {
    $os = Get-CimInstance -ClassName Win32_OperatingSystem -OperationTimeoutSec 3 |
        Select-Object -First 1 -Property TotalVisibleMemorySize, FreePhysicalMemory
    if ($null -eq $os -or $null -eq $os.TotalVisibleMemorySize -or
        $null -eq $os.FreePhysicalMemory -or $os.TotalVisibleMemorySize -le 0) {
        throw 'Host memory fields are unavailable.'
    }
    $memory.hostTotalBytes = [uint64]$os.TotalVisibleMemorySize * 1KB
    $memory.hostAvailableBytes = [uint64]$os.FreePhysicalMemory * 1KB
    $memory.status = 'ok'
} catch { $warnings.Add('HOST_MEMORY_QUERY_UNAVAILABLE') }

$commit = $(if ($env:GITHUB_SHA -match '^[0-9a-f]{40}$') { $env:GITHUB_SHA } else { $null })
$snapshot = [ordered]@{
    schema = 'gsw.remote_ci_capabilities/1'
    snapshotKind = 'read_only_remote_inventory_not_product_acceptance'
    observedUtc = [DateTime]::UtcNow.ToString('o')
    sourceCommit = $commit
    probeSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    platform = @{ family = 'Windows'; architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        osVersion = [Environment]::OSVersion.Version.ToString() }
    privacy = @{ hostnameOmitted = $true; deviceIdentifiersOmitted = $true; pathsOmitted = $true
        credentialsOmitted = $true; environmentDumpOmitted = $true; rawExceptionsAndStderrOmitted = $true }
    cpu = @{ processVisibleLogical = [Environment]::ProcessorCount; effectiveCapacityCores = $null
        semantics = 'process-visible count; not physical cores or a measured entitlement' }
    memory = $memory
    display = $display
    videoControllerInventory = $adapters
    nvidiaManagementInventory = $nvidia
    notTested = @('GSW or Qt runtime', 'OpenGL provider', 'CUDA runtime', 'actual resident VRAM',
        'OS scale transitions 100/125/150/175/200%', 'application cross-monitor transitions')
    safety = @{ applicationLaunched = $false; visibleWindowShown = $false; displayModeChanged = $false
        osScaleChanged = $false; driversChanged = $false; workloadsOrStressStarted = $false }
    warnings = @($warnings | Sort-Object -Unique)
}
$json = $snapshot | ConvertTo-Json -Depth 12
$stream = [IO.File]::Open($fullOutput, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + [Environment]::NewLine)
    $stream.Write($bytes, 0, $bytes.Length)
} finally { $stream.Dispose() }
Write-Output 'Read-only remote capability snapshot saved. No product acceptance is implied.'
