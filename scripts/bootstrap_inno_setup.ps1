[CmdletBinding()]
param(
  [string]$InstallRoot = '',
  [string]$CacheRoot = ''
)
$ErrorActionPreference='Stop'
$version='7.1.0'
$expectedHash='0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f'
if (-not $InstallRoot) {
  $InstallRoot=if(Test-Path 'D:/'){'D:/Apps/InnoSetup/7.1.0'}else{Join-Path $env:LOCALAPPDATA 'Programs/InnoSetup/7.1.0'}
}
if (-not $CacheRoot) {
  $CacheRoot=if(Test-Path 'D:/'){'D:/CodexData/Downloads/InnoSetup/7.1.0'}else{Join-Path $env:LOCALAPPDATA 'GSW-build-cache/InnoSetup/7.1.0'}
}
$compiler=Join-Path $InstallRoot 'ISCC.exe'
if(Test-Path -LiteralPath $compiler){Write-Output $compiler;return}
New-Item -ItemType Directory -Path $CacheRoot -Force|Out-Null
$download=Join-Path $CacheRoot 'innosetup-7.1.0-x64.exe'
if(-not(Test-Path -LiteralPath $download)){
  Invoke-WebRequest -Uri 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe' -OutFile $download
}
if((Get-Item -LiteralPath $download).Length -ne 14304168 -or
   (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash){throw 'Inno Setup official asset checksum differs.'}
$signature=Get-AuthenticodeSignature -LiteralPath $download
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Pyrsys B\.V\.'){
  throw "Inno Setup publisher verification failed: $($signature.Status)"
}
$temporary=Join-Path $CacheRoot 'temporary'
New-Item -ItemType Directory -Path $temporary -Force|Out-Null
$oldTemp=$env:TEMP;$oldTmp=$env:TMP
try{
  $env:TEMP=$temporary;$env:TMP=$temporary
  $arguments=@('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-','/CURRENTUSER','/NOICONS',
    "/DIR=`"$InstallRoot`"","/LOG=`"$(Join-Path $CacheRoot 'install.log')`"")
  $process=Start-Process -FilePath $download -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
  if($process.ExitCode -ne 0){throw "Inno Setup bootstrap failed: $($process.ExitCode)"}
}finally{$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
if(-not(Test-Path -LiteralPath $compiler -PathType Leaf)){throw 'ISCC.exe missing after bootstrap.'}
Write-Output $compiler
