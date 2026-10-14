param([Parameter(Mandatory)][int]$TrainerPid)
$ErrorActionPreference = 'Stop'
while (Get-Process -Id $TrainerPid -ErrorAction SilentlyContinue) {
    $process = Get-Process -Id $TrainerPid -ErrorAction SilentlyContinue
    if (-not $process) { break }
    $dedicated = 0L
    $shared = 0L
    try {
        $counters = Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
            Where-Object { $_.Name -like "pid_${TrainerPid}_*" }
        foreach ($counter in $counters) {
            $dedicated += [long]$counter.DedicatedUsage
            $shared += [long]$counter.SharedUsage
        }
        @{ pid=$TrainerPid; timestamp=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();
            workingSetBytes=$process.WorkingSet64; privateBytes=$process.PrivateMemorySize64;
            observedDedicatedBytes=$dedicated; observedSharedBytes=$shared } | ConvertTo-Json -Compress
    } catch {
        @{ pid=$TrainerPid; timestamp=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();
            memoryProbeUnavailable=$true } | ConvertTo-Json -Compress
    }
    Start-Sleep -Milliseconds 1000
}
