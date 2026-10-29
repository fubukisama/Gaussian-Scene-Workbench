#requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet('Prepare','Run')][string]$Mode)

# Diagnostic only. No retries, product rebuild, local execution, model input,
# assertion/timeout override, installation or managed-pointer mutation.
$ErrorActionPreference='Stop'
Set-StrictMode -Version 3.0
$repository='fubukisama/Gaussian-Scene-Workbench'
$productCommit='ac5c77b4ab22d5ec3de444e41bb723e1cba68c4f'
$failedRun=40800786447L
$artifactId=12866153944L
$archiveBytes=123884830L
$archiveSha='695ab8f56ead7df4555bee70dff84432a74dbf89545daba5eeb212097349bbc9'
function Require([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function NewJson([string]$Path,$Value){
  $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 10))
  $stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try{$stream.Write($bytes);$stream.Flush($true)}finally{$stream.Dispose()}
}
function RequireX64Pe([string]$Path){
  $stream=[IO.File]::OpenRead($Path);$reader=[IO.BinaryReader]::new($stream)
  try{
    Require ($stream.Length -ge 64 -and $reader.ReadUInt16() -eq 0x5a4d) 'Selected CRT is not a PE file.'
    $stream.Position=0x3c;$offset=$reader.ReadInt32()
    Require ($offset -ge 64 -and $offset -le $stream.Length-6) 'Selected CRT has an invalid PE header.'
    $stream.Position=$offset
    Require ($reader.ReadUInt32() -eq 0x4550 -and $reader.ReadUInt16() -eq 0x8664) 'Selected CRT must be x64 AMD64.'
  }finally{$reader.Dispose();$stream.Dispose()}
}
Require ($IsWindows -and $env:GITHUB_ACTIONS -ceq 'true' -and $env:GITHUB_EVENT_NAME -ceq 'workflow_dispatch') 'Remote manual Windows diagnostic only.'
Require ([Environment]::Is64BitProcess) 'The diagnostic must use the native x64 Windows process/runtime view.'
Require ($env:GITHUB_REPOSITORY -ceq $repository -and $env:GITHUB_REF -ceq 'refs/heads/main') 'Unexpected diagnostic repository/ref.'
Require ($env:RUNNER_TEMP -match '^D:[\\/]') 'An owned D-drive runner.temp is required; no local/system-drive fallback.'
$root=Join-Path ([IO.Path]::GetFullPath($env:RUNNER_TEMP)) 'gsw-grid-timeout-once'
$archive=Join-Path $root 'original-diagnostic-artifact.zip'
$payload=Join-Path $root 'payload'
$receipt=Join-Path $root 'verified-artifact.json'
$exe=Join-Path $payload 'bin/Gaussian Scene Workbench.exe'
$requiredRuntime=@('bin/Qt6Core.dll','bin/Qt6Gui.dll','bin/Qt6Widgets.dll','bin/Qt6Network.dll',
  'bin/Qt6OpenGL.dll','bin/Qt6OpenGLWidgets.dll','bin/Qt6Svg.dll','bin/platforms/qwindows.dll')
$crtNames=@('MSVCP140.dll','VCRUNTIME140.dll','VCRUNTIME140_1.dll')
$systemDirectory=[Environment]::SystemDirectory

if($Mode -ceq 'Prepare'){
  Require (-not(Test-Path -LiteralPath $root)) 'Evidence root must be fresh; do not overwrite/replay a previous attempt.'
  [IO.Directory]::CreateDirectory($root)|Out-Null
  $ownedTemporary=Join-Path $root 'tmp';[IO.Directory]::CreateDirectory($ownedTemporary)|Out-Null
  $env:TEMP=$ownedTemporary;$env:TMP=$ownedTemporary
  Require (-not [string]::IsNullOrEmpty($env:GITHUB_ENV)) 'Runner environment file unavailable.'
  [IO.File]::AppendAllText($env:GITHUB_ENV,"TEMP=$ownedTemporary`nTMP=$ownedTemporary`n",[Text.UTF8Encoding]::new($false))
  Require (-not [string]::IsNullOrEmpty($env:GH_TOKEN)) 'Read-only Actions API token unavailable.'
  $headers=@{Authorization=('Bearer '+$env:GH_TOKEN);Accept='application/vnd.github+json';'User-Agent'='GSW-one-grid-diagnostic'}
  $api="https://api.github.com/repos/$repository/actions"
  $run=Invoke-RestMethod -Uri "$api/runs/$failedRun" -Headers $headers -TimeoutSec 30
  $artifact=Invoke-RestMethod -Uri "$api/artifacts/$artifactId" -Headers $headers -TimeoutSec 30
  Require ($run.id -eq $failedRun -and $run.head_sha -ceq $productCommit -and $run.head_branch -ceq 'main' -and
    $run.repository.full_name -ceq $repository) 'Failed run source identity mismatch.'
  Require ($artifact.id -eq $artifactId -and $artifact.name -ceq 'native-windows-failed-run-diagnostics' -and
    $artifact.size_in_bytes -eq $archiveBytes -and $artifact.expired -eq $false -and
    $artifact.digest -ceq ('sha256:'+$archiveSha) -and $artifact.workflow_run.id -eq $failedRun -and
    $artifact.workflow_run.head_sha -ceq $productCommit) 'Failed artifact identity/digest mismatch.'
  # Git blob IDs bind both setup adapters to the original failed source without
  # fetching another branch or silently changing the diagnostic driver recipe.
  foreach($binding in @(@{path='scripts/setup_native_ci_display.ps1';blob='94f2ef1ce52bb709ed066282afc8913873c4cf34'},
      @{path='scripts/setup_native_ci_graphics.ps1';blob='6a4253dfa5c2e6e52f4dc2772ff30d2c72c3a187'})){
    $actual=(& git hash-object "--path=$($binding.path)" $binding.path).Trim()
    Require ($LASTEXITCODE -eq 0 -and $actual -ceq $binding.blob) 'Display/graphics adapter differs from the failed source.'
  }
  # The API redirect is authenticated; the redirected signed asset request is
  # made by a separate client with no Authorization header. Never log its URL.
  $handler=[Net.Http.HttpClientHandler]::new();$handler.AllowAutoRedirect=$false
  $client=[Net.Http.HttpClient]::new($handler);$client.Timeout=[TimeSpan]::FromSeconds(30)
  try{
    $client.DefaultRequestHeaders.Authorization=[Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer',$env:GH_TOKEN)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('GSW-one-grid-diagnostic/1')
    $response=$client.GetAsync("$api/artifacts/$artifactId/zip").GetAwaiter().GetResult()
    try{Require ([int]$response.StatusCode -eq 302 -and $null -ne $response.Headers.Location) 'Artifact redirect unavailable.';$location=$response.Headers.Location}
    finally{$response.Dispose()}
  }finally{$client.Dispose();$handler.Dispose();$headers=$null}
  $download=[Net.Http.HttpClient]::new();$cancel=[Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(180))
  try{
    $response=$download.GetAsync($location,[Net.Http.HttpCompletionOption]::ResponseHeadersRead,$cancel.Token).GetAwaiter().GetResult()
    try{
      $response.EnsureSuccessStatusCode()|Out-Null
      $sourceStream=$response.Content.ReadAsStreamAsync($cancel.Token).GetAwaiter().GetResult()
      $output=[IO.FileStream]::new($archive,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
      try{$sourceStream.CopyToAsync($output,$cancel.Token).GetAwaiter().GetResult();$output.Flush($true)}finally{$output.Dispose();$sourceStream.Dispose()}
    }finally{$response.Dispose()}
  }finally{$cancel.Dispose();$download.Dispose();$location=$null}
  Require ((Get-Item -LiteralPath $archive).Length -eq $archiveBytes -and (Hash $archive) -ceq $archiveSha) 'Downloaded original archive bytes/digest mismatch.'
  # Retain the independently verified download even if extraction/dependency
  # prechecks refuse execution. The previous attempt started no app process.
  NewJson (Join-Path $root 'downloaded-archive.json') ([ordered]@{schema='gsw.one_grid_replay_download/1';
    productSourceCommit=$productCommit;failedRunId=$failedRun;artifactId=$artifactId;archiveBytes=$archiveBytes;
    archiveSha256=$archiveSha;harnessSourceCommit=$env:GITHUB_SHA;applicationExecuted=$false;
    dependencyPrecheck='NOT_YET_COMPLETED';verifiedUtc=[DateTime]::UtcNow.ToString('o')})
  $zip=[IO.Compression.ZipFile]::OpenRead($archive)
  try{
    Require ($zip.Entries.Count -gt 0 -and $zip.Entries.Count -lt 20000) 'Diagnostic archive file-count bound exceeded.'
    $total=0L
    foreach($entry in $zip.Entries){
      $relative=$entry.FullName.Replace('\','/');$total+=$entry.Length
      Require ($relative -notmatch '^[/\\]|:|(^|/)\.\.?(/|$)' -and
        (($entry.ExternalAttributes -shr 16) -band 0xf000) -ne 0xa000 -and $entry.Length -le 268435456) 'Unsafe/oversized diagnostic archive entry.'
    }
    Require ($total -le 2147483648L) 'Diagnostic archive expansion bound exceeded.'
  }finally{$zip.Dispose()}
  Expand-Archive -LiteralPath $archive -DestinationPath $payload
  foreach($relative in @('bin/Gaussian Scene Workbench.exe')+$requiredRuntime){
    Require (Test-Path -LiteralPath (Join-Path $payload $relative) -PathType Leaf) "Original diagnostic runtime file missing: $relative; no replacement will be installed."
  }
  $runtime=@($requiredRuntime|ForEach-Object{@{path=$_;sha256=(Hash (Join-Path $payload $_))}})
  $crt=@(foreach($name in $crtNames){
    $selected=Join-Path $payload ('bin/'+$name);$source='archive'
    if(-not(Test-Path -LiteralPath $selected -PathType Leaf)){
      $selected=Join-Path $systemDirectory $name;$source='preinstalled_system32'
    }
    Require (Test-Path -LiteralPath $selected -PathType Leaf) "Required CRT is absent from both the original archive and preinstalled System32: $name; nothing will be installed."
    RequireX64Pe $selected
    $version=(Get-Item -LiteralPath $selected).VersionInfo.FileVersion
    Require (-not [string]::IsNullOrWhiteSpace($version)) "Selected CRT version metadata unavailable: $name."
    @{name=$name;source=$source;absolutePath=[IO.Path]::GetFullPath($selected);sha256=(Hash $selected);
      fileVersion=$version;machine='AMD64';originalCrtVersion='NOT_RECORDED'}
  })
  NewJson $receipt ([ordered]@{schema='gsw.one_grid_replay_artifact/1';productSourceCommit=$productCommit;
    failedRunId=$failedRun;artifactId=$artifactId;archiveBytes=$archiveBytes;archiveSha256=$archiveSha;
    harnessSourceCommit=$env:GITHUB_SHA;executableSha256=(Hash $exe);runtime=$runtime;crt=$crt;
    originalCrtVersion='NOT_RECORDED';byteIdenticalEnvironment=$false;
    crtPolicy='Reuse archive CRT when present, otherwise existing x64 System32; no install/copy/replace';
    productRebuilt=$false;applicationExecuted=$false;archiveMayLackHiddenSource=$true;
    scope='Original failed diagnostic binary only; not product/package acceptance'})
  Write-Output 'Exact failed artifact verified and extracted; no application executed.'
  exit 0
}

Require (Test-Path -LiteralPath $receipt -PathType Leaf) 'Verified original artifact receipt unavailable.'
$identity=Get-Content -LiteralPath $receipt -Raw -Encoding utf8|ConvertFrom-Json
Require ($identity.productSourceCommit -ceq $productCommit -and $identity.artifactId -eq $artifactId -and
  $identity.archiveSha256 -ceq $archiveSha -and (Hash $archive) -ceq $archiveSha -and
  (Hash $exe) -ceq $identity.executableSha256) 'Original binary/archive changed after verification.'
foreach($row in $identity.runtime){Require ((Hash (Join-Path $payload $row.path)) -ceq $row.sha256) 'Original runtime bytes changed.'}
Require ($identity.crt.Count -eq $crtNames.Count -and $identity.originalCrtVersion -ceq 'NOT_RECORDED' -and
  $identity.byteIdenticalEnvironment -eq $false) 'CRT provenance/limitations unavailable.'
foreach($name in $crtNames){
  $rows=@($identity.crt|Where-Object {$_.name -ceq $name});Require ($rows.Count -eq 1) 'Unambiguous selected CRT identity required.'
  $row=$rows[0];Require ($row.source -cin @('archive','preinstalled_system32')) 'Unexpected CRT source.'
  $expected=if($row.source -ceq 'archive'){Join-Path $payload ('bin/'+$name)}else{Join-Path $systemDirectory $name}
  Require ([IO.Path]::GetFullPath($expected).Equals($row.absolutePath,[StringComparison]::OrdinalIgnoreCase) -and
    (Hash $expected) -ceq $row.sha256) 'Selected CRT path/bytes changed after verification.'
  RequireX64Pe $expected
}
Require (-not(Test-Path -LiteralPath (Join-Path $root 'process-started.json'))) 'This diagnostic already started its one application process.'
Require (-not [string]::IsNullOrEmpty($env:QT_OPENGL_DLL) -and (Test-Path -LiteralPath $env:QT_OPENGL_DLL -PathType Leaf)) 'Pinned CI Mesa runtime unavailable.'
$ctest=(Get-Command ctest.exe -CommandType Application -ErrorAction Stop).Source
$harness=Join-Path $root 'harness';$temporary=Join-Path $root 'tmp'
[IO.Directory]::CreateDirectory($harness)|Out-Null;[IO.Directory]::CreateDirectory($temporary)|Out-Null
$cmakeExe=$exe.Replace('\','/');$cmakeWork=$harness.Replace('\','/')
$cmakePlugins=(Join-Path $payload 'bin').Replace('\','/')
$test="add_test(native_infinite_reference_grid `"$cmakeExe`" `"--smoke-test-infinite-grid`")`n"+
  "set_tests_properties(native_infinite_reference_grid PROPERTIES TIMEOUT 15 WORKING_DIRECTORY `"$cmakeWork`" ENVIRONMENT `"QT_OPENGL=software;QT_PLUGIN_PATH=$cmakePlugins;QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=`")`n"
[IO.File]::WriteAllText((Join-Path $harness 'CTestTestfile.cmake'),$test,[Text.UTF8Encoding]::new($false))
$env:TEMP=$temporary;$env:TMP=$temporary;$env:GSW_UI_QA_TRACE='1';$env:QT_FORCE_STDERR_LOGGING='1';$env:QT_LOGGING_TO_CONSOLE='1'
$env:PATH=(Join-Path $payload 'bin')+';'+$env:PATH
foreach($name in @('GSW_SMOKE_BACKGROUND','GSW_UI_SURFACE_EVIDENCE','GSW_RENDER_DIAGNOSTICS_DIR','GSW_RENDER_DIAGNOSTICS_SMOKE','GSW_UI_QA_GRAYSCALE_TEXT')){
  Remove-Item -LiteralPath ('Env:'+$name) -ErrorAction SilentlyContinue
}
NewJson (Join-Path $root 'process-started.json') ([ordered]@{productSourceCommit=$productCommit;
  harnessSourceCommit=$env:GITHUB_SHA;command=@($exe,'--smoke-test-infinite-grid');timeoutSeconds=15;
  originalTestName='native_infinite_reference_grid';maximumApplicationProcesses=1;repeat=$false;
  executableSha256=(Hash $exe);mesaDllSha256=(Hash $env:QT_OPENGL_DLL);temporaryDirectory=$temporary;
  crt=$identity.crt;originalCrtVersion='NOT_RECORDED';byteIdenticalEnvironment=$false;
  trace='GSW_UI_QA_TRACE=1';diagnosticOnly=$true;startedUtc=[DateTime]::UtcNow.ToString('o')})
$PSNativeCommandUseErrorActionPreference=$false
& $ctest --test-dir $harness --no-tests=error --verbose --output-on-failure --output-log (Join-Path $harness 'grid-replay.log') --output-junit (Join-Path $harness 'grid-replay.xml') -R '^native_infinite_reference_grid$'
$code=$LASTEXITCODE
$images=@(Get-ChildItem -LiteralPath $temporary -File -Filter 'gsw-infinite-grid-*.png'|ForEach-Object{@{path=$_.Name;bytes=$_.Length;sha256=(Hash $_.FullName)}})
NewJson (Join-Path $root 'one-process-result.json') ([ordered]@{schema='gsw.one_grid_replay_result/1';
  productSourceCommit=$productCommit;harnessSourceCommit=$env:GITHUB_SHA;ctestExitCode=$code;
  diagnosticVerdict=$(if($code -eq 0){'NOT_REPRODUCED_IN_THIS_ONE_RUN'}else{'REPLAY_FAILED_INSPECT_TRACE'});
  originalFailurePreserved=$true;productRebuilt=$false;packageAcceptance=$false;performanceComparison=$false;
  crt=$identity.crt;originalCrtVersion='NOT_RECORDED';byteIdenticalEnvironment=$false;
  timeoutSeconds=15;maximumApplicationProcesses=1;repeat=$false;images=$images;completedUtc=[DateTime]::UtcNow.ToString('o')})
exit $code
