#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../native/.deps/mesa-ci')
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Native CI graphics requires Windows x64.' }
if (-not [Environment]::Is64BitProcess) { throw 'Native CI graphics requires an x64 process.' }

# CI-only OpenGL implementation; never replace system or application DLLs.
# Release digest is supplied by the upstream GitHub release asset API.
$version = '26.2.4'
$digest = '351FC8C8B695878FFB3EAA044B3EAD08672A48B1A045E3C3E3975811DF0F6695'
$asset = "mesa3d-$version-release-msvc.7z"
$root = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $root | Out-Null
$archive = Join-Path $root $asset
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri "https://github.com/pal1000/mesa-dist-win/releases/download/$version/$asset" -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $digest) {
    throw "Mesa $version release checksum mismatch; refusing extraction."
}
# Windows' bundled bsdtar does not support the LZMA codec used by this .7z
# release on every hosted image. Use the runner's existing real 7-Zip CLI;
# never install a new extractor or silently fall back to a partial extraction.
$sevenZip = $null
$candidates = @()
if ($env:ProgramFiles) {
    $candidates += Join-Path $env:ProgramFiles '7-Zip/7z.exe'
}
$candidates += @(
    Get-Command 7z.exe, 7z -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source
)
foreach ($candidate in $candidates) {
    if ([IO.Path]::IsPathRooted($candidate) -and
        [IO.Path]::GetExtension($candidate) -ieq '.exe' -and
        (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $sevenZip = [IO.Path]::GetFullPath($candidate)
        break
    }
}
if (-not $sevenZip) {
    throw 'Mesa CI extraction requires an already installed 7-Zip 7z.exe. No extractor will be installed, and native graphics tests will not be skipped.'
}
$unpacked = Join-Path $root "mesa-$version"
New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
Write-Output "Mesa CI extractor: $sevenZip"
& $sevenZip x -y "-o$unpacked" $archive x64/opengl32.dll x64/libgallium_wgl.dll readme.txt
if ($LASTEXITCODE -ne 0) { throw 'Mesa CI extraction failed.' }
$dll = Join-Path $unpacked 'x64/opengl32.dll'
$driver = Join-Path $unpacked 'x64/libgallium_wgl.dll'
if (-not (Test-Path -LiteralPath $dll) -or -not (Test-Path -LiteralPath $driver)) {
    throw 'Mesa CI OpenGL runtime files are missing.'
}
$bin = Join-Path $unpacked 'x64'
$env:QT_OPENGL_DLL = $dll
$env:GALLIUM_DRIVER = 'llvmpipe'
$env:PATH = "$bin;$env:PATH"
if ($env:GITHUB_ACTIONS -eq 'true') {
    if (-not $env:GITHUB_ENV -or -not $env:GITHUB_PATH) { throw 'GitHub runner environment files are missing.' }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::AppendAllText($env:GITHUB_ENV, "QT_OPENGL_DLL=$dll`nGALLIUM_DRIVER=llvmpipe`n", $utf8)
    [IO.File]::AppendAllText($env:GITHUB_PATH, "$bin`n", $utf8)
}
Write-Output "CI software OpenGL: Mesa $version llvmpipe; SHA256=$digest; QT_OPENGL_DLL=$dll"
Write-Output 'No system-wide deployment, version override, or application-package runtime replacement.'
