param(
  [string]$Version = "",
  [string]$PackageVersion = "",
  [ValidateSet("patch", "minor", "major", "none")]
  [string]$Bump = "patch",
  [string]$Note = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VersionFile = Join-Path $Root "version"
$PackageJsonPath = Join-Path $Root "resources\app\package.json"
$ManifestPath = Join-Path $Root "build_manifest.json"

function Write-Utf8NoBom([string]$Path, [string]$Text) {
  $encoding = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Get-VersionParts([string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) {
    return @(0, 0, 0)
  }
  $parts = $Value.Trim().Split(".") | ForEach-Object {
    $parsed = 0
    if ([int]::TryParse($_, [ref]$parsed)) { $parsed } else { 0 }
  }
  while ($parts.Count -lt 3) {
    $parts += 0
  }
  return @($parts[0], $parts[1], $parts[2])
}

function Bump-Version([string]$Value, [string]$Mode) {
  $parts = Get-VersionParts $Value
  if ($Mode -eq "major") {
    $parts[0] += 1; $parts[1] = 0; $parts[2] = 0
  } elseif ($Mode -eq "minor") {
    $parts[1] += 1; $parts[2] = 0
  } elseif ($Mode -eq "patch") {
    $parts[2] += 1
  }
  return "{0}.{1}.{2}" -f $parts[0], $parts[1], $parts[2]
}

function Get-GitValue([string[]]$GitArgs) {
  try {
    $value = & git -C $Root @GitArgs 2>$null
    if ($LASTEXITCODE -eq 0) { return ($value -join "").Trim() }
  } catch {
  }
  return ""
}

if (-not (Test-Path -LiteralPath $VersionFile)) {
  Write-Utf8NoBom $VersionFile "0.0.0`n"
}
if (-not (Test-Path -LiteralPath $PackageJsonPath)) {
  throw "Missing package.json: $PackageJsonPath"
}

$currentVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
$packageRaw = Get-Content -LiteralPath $PackageJsonPath -Raw
$packageJson = $packageRaw | ConvertFrom-Json
$currentPackageVersion = [string]$packageJson.version
$appName = "Gaussian Scene Workbench (Legacy HTML)"
if (Test-Path -LiteralPath $ManifestPath) {
  try {
    $currentManifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if (-not [string]::IsNullOrWhiteSpace([string]$currentManifest.appName)) {
      $appName = [string]$currentManifest.appName
    }
  } catch {
  }
}

if ([string]::IsNullOrWhiteSpace($Version)) {
  $Version = if ($Bump -eq "none") { $currentVersion } else { Bump-Version $currentVersion $Bump }
}
if ([string]::IsNullOrWhiteSpace($PackageVersion)) {
  $PackageVersion = if ($Bump -eq "none") { $currentPackageVersion } else { Bump-Version $currentPackageVersion $Bump }
}

$packageRaw = [regex]::Replace($packageRaw, '("version"\s*:\s*")[^"]+(")', "`${1}$PackageVersion`${2}", 1)
Write-Utf8NoBom $PackageJsonPath $packageRaw
Write-Utf8NoBom $VersionFile "$Version`n"

$updatedAt = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
$gitCommit = Get-GitValue -GitArgs @("rev-parse", "--short", "HEAD")
$gitBranch = Get-GitValue -GitArgs @("branch", "--show-current")
$dirty = Get-GitValue -GitArgs @("status", "--short")

$manifest = [ordered]@{
  appName = $appName
  sourceVersion = $Version
  packageVersion = $PackageVersion
  releaseTag = "v$PackageVersion"
  packageName = "Gaussian-Scene-Workbench-$PackageVersion-win-x64.zip"
  updatedAt = $updatedAt
  updatedBy = $env:USERNAME
  machine = $env:COMPUTERNAME
  gitBranch = $gitBranch
  gitCommit = $gitCommit
  gitDirty = -not [string]::IsNullOrWhiteSpace($dirty)
  note = $Note
}

Write-Utf8NoBom $ManifestPath (($manifest | ConvertTo-Json -Depth 10) + "`n")
Write-Host "Updated source version: $Version"
Write-Host "Updated package version: $PackageVersion"
Write-Host "Updated manifest: $ManifestPath"
