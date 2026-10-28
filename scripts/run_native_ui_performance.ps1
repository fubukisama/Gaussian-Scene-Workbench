#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceRoot)

$ErrorActionPreference='Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or -not $IsWindows) { throw 'Remote owned Windows runner only; no local GUI fallback.' }
$root=[IO.Path]::GetFullPath($EvidenceRoot)
$sources=Get-Content -LiteralPath (Join-Path $root 'sources.json') -Raw|ConvertFrom-Json
$fixtures=Join-Path $root 'shared-fixtures'
New-Item -ItemType Directory -Path $fixtures | Out-Null
$env:PATH="$env:QT_ROOT_DIR/bin;$env:PATH"
$env:QT_PLUGIN_PATH="$env:QT_ROOT_DIR/plugins"
$env:QT_QPA_PLATFORM='windows'
$env:QT_OPENGL='software'
$env:GSW_SWAP_INTERVAL='1'
Remove-Item Env:GSW_RENDER_DIAGNOSTICS_DIR -ErrorAction SilentlyContinue
Remove-Item Env:GSW_RENDER_DIAGNOSTICS_SMOKE -ErrorAction SilentlyContinue
$processes=@()
$watch=[Diagnostics.Stopwatch]::StartNew()
$groups=@(@{id='empty-dpr1';scenario='empty';dpr=1},@{id='empty-dpr2';scenario='empty';dpr=2},
  @{id='light-dpr1';scenario='light';dpr=1},@{id='medium-dpr1';scenario='medium';dpr=1})
foreach($group in $groups) {
  foreach($round in 1..3) {
    $order=if($round -eq 2){@('B','A')}else{@('A','B')}
    foreach($arm in $order) {
      if($watch.Elapsed.TotalMinutes -ge 12) { throw 'Frozen12minute observation wall budget expired; retain partial INCONCLUSIVE evidence.' }
      $identity=$sources.arms|Where-Object arm -eq $arm
      if(@($identity).Count -ne 1){throw 'Ambiguous executable arm.'}
      if((Get-FileHash -LiteralPath $identity.executable).Hash.ToLowerInvariant() -cne $identity.executableSha256){throw 'Executable changed after build.'}
      $case=Join-Path $root ("$($group.scenario)-d$($group.dpr)-r$round-$arm")
      New-Item -ItemType Directory -Path $case | Out-Null
      $start=[Diagnostics.ProcessStartInfo]::new()
      $start.FileName=$identity.executable
      $start.UseShellExecute=$false
      $start.CreateNoWindow=$true
      $start.RedirectStandardOutput=$true
      $start.RedirectStandardError=$true
      $start.ArgumentList.Add('--smoke-test-ui-performance')
      $start.ArgumentList.Add('--theme')
      $start.ArgumentList.Add('light')
      $start.Environment['GSW_UI_PERF_OUTPUT_DIR']=$case
      $start.Environment['GSW_UI_PERF_FIXTURE_DIR']=$fixtures
      $start.Environment['GSW_UI_PERF_ARM']=$arm
      $start.Environment['GSW_UI_PERF_SOURCE_COMMIT']=$identity.sourceCommit
      $start.Environment['GSW_UI_PERF_RUNNER_ID']=[string]$sources.runId
      $start.Environment['GSW_UI_PERF_ROUND']=[string]$round
      $start.Environment['GSW_UI_PERF_SCENARIO']=$group.scenario
      $start.Environment['QT_SCALE_FACTOR']=[string]$group.dpr
      $p=[Diagnostics.Process]::Start($start)
      $stdout=$p.StandardOutput.ReadToEndAsync()
      $stderr=$p.StandardError.ReadToEndAsync()
      $limit=if($group.scenario -eq 'medium'){120000}else{60000}
      $timedOut=$false
      $code=$null
      try {
        if(-not $p.WaitForExit($limit)){$timedOut=$true;$p.Kill($true);$p.WaitForExit()}
        $p.WaitForExit()
        $code=$p.ExitCode
      } finally {
        try {
          if($stdout.Wait(5000)){[IO.File]::WriteAllText((Join-Path $case 'stdout.log'),$stdout.Result,[Text.UTF8Encoding]::new($false))}
          if($stderr.Wait(5000)){[IO.File]::WriteAllText((Join-Path $case 'stderr.log'),$stderr.Result,[Text.UTF8Encoding]::new($false))}
          @{exitCode=$code;timedOut=$timedOut;wallLimitMilliseconds=$limit}|ConvertTo-Json|
            Set-Content -LiteralPath (Join-Path $case 'process.json') -Encoding utf8
        } finally {$p.Dispose()}
      }
      $result=Join-Path $case 'result.json'
      $digest=if(Test-Path -LiteralPath $result){(Get-FileHash -LiteralPath $result).Hash.ToLowerInvariant()}else{''}
      $processes+=@{arm=$arm;round=$round;group=$group.id;path=$result;sha256=$digest;exitCode=$code;timedOut=$timedOut}
      [ordered]@{schema='gsw.native_ui_performance_manifest/1';sources=(Join-Path $root 'sources.json');
        runnerId=[string]$sources.runId;baselineSourceCommit=($sources.arms|Where-Object arm -eq 'A').sourceCommit;
        candidateSourceCommit=($sources.arms|Where-Object arm -eq 'B').sourceCommit;processes=$processes;
        expectedGroups=$groups;
        elapsedSeconds=$watch.Elapsed.TotalSeconds}|ConvertTo-Json -Depth 8|
        Set-Content -LiteralPath (Join-Path $root 'manifest.json') -Encoding utf8
      if($timedOut){throw "Observation timeout; INCONCLUSIVE raw data retained: $case"}
      if($code -ne 0 -and $code -ne 3){throw "Structural performance contract failed: $case exit$code"}
    }
  }
}
if($processes.Count -ne 24){throw 'Incomplete frozen paired process matrix.'}
